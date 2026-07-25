/* SPDX-License-Identifier: BSD-2-Clause */

/* coredump.c — bounded ELF (ET_CORE) coredump over raw Ethernet. See
 * coredump.h for the on-wire framing and the receiver contract. */
#include <stdint.h>
#include "coredump.h"
#include "exceptions.h"

/* Raw-frame sender (emac.c). Returns 1 if queued to TX DMA, 0 if dropped
 * (link down / TX ring busy past its bounded wait). Non-blocking beyond that
 * same bounded wait. */
extern int  emac_send_frame(uint16_t ethertype, const uint8_t *payload,
                            uint16_t len);
/* Service TX completions between frames so the 8-deep TX ring drains. */
extern void emac_poll(void);

/* A64 DRAM window we are willing to read from. Kernel @0x42000000, U-Boot
 * stack near DRAM top, breadcrumbs @0x50000000. 0x40000000..0x80000000 covers
 * the 1 GiB the board has; anything outside is refused (never fault reading). */
#define DRAM_LO 0x40000000ULL
#define DRAM_HI 0x80000000ULL

/* ROADMAP B3 fix: translate a guest EL1 virtual address to a physical one
 * before it is handed to region_clamp()/stream_mem() below (which only ever
 * understand flat physical addresses in [DRAM_LO, DRAM_HI)). Exactly the
 * same AT S1E1R / PAR_EL1 idiom backtrace.c's bt_read64() already uses for
 * the guest frame-pointer walk, with the identical MMU-off fallback
 * (treat the VA as already-physical if translation faults — correct for an
 * early-boot guest with SCTLR_EL1.M==0, where VA==PA by construction).
 * `at`/`isb`/`mrs` never themselves fault, so this is safe to call
 * unconditionally from the fault path. See coredump_send()'s use below: the
 * automatic "stack window" region used to be built from frame->sp_at_entry,
 * which is EL2's OWN exception stack pointer (see exceptions.S's frame
 * layout comment: "sp_at_entry: SP before we pushed the frame" — that's
 * SP_EL2, never anything the guest owns) — i.e. every coredump ever sent
 * streamed a window of the HYPERVISOR's stack, not the panicking GUEST's,
 * and stamped that same wrong value into NT_PRSTATUS's sp register too. */
static uint64_t gva_to_pa(uint64_t va)
{
	uint64_t par, pa;

	__asm__ volatile("at s1e1r, %0" :: "r"(va) : "memory");
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("mrs %0, par_el1" : "=r"(par));

	if (!(par & 1ull)) {
		pa = (par & 0x000ffffffffff000ull) | (va & 0xfffull);
		return pa;
	}
	return va;   /* MMU-off guest (VA==PA), or untranslatable: flat fallback */
}

/* ---- ELF64 constants (only what we need) ---- */
#define ET_CORE       4
#define EM_AARCH64    183
#define EV_CURRENT    1
#define PT_LOAD       1
#define PT_NOTE       4
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define NT_PRSTATUS   1

#define EHDR_SZ   64u
#define PHDR_SZ   56u

/* AArch64 elf_gregset_t == struct user_pt_regs: x0..x30, sp, pc, pstate. */
#define GREG_N        34u                 /* 31 + sp + pc + pstate */
#define GREG_SZ       (GREG_N * 8u)       /* 272 */
/* elf_prstatus: pr_reg (the gregset) sits at offset 112 on LP64 AArch64; the
 * struct is 392 bytes with pr_fpvalid + tail padding. gdb only needs pr_reg. */
#define PRSTATUS_REGOFF 112u
#define PRSTATUS_SZ     392u

/* PT_NOTE payload: Nhdr(12) + "CORE\0"pad(8) + prstatus(392). */
#define NOTE_NAME_SZ  8u
#define NOTE_SZ       (12u + NOTE_NAME_SZ + PRSTATUS_SZ)   /* 412 */

/* ---- header assembly buffer ---- */
/* Ehdr + (1 PT_NOTE + up to REGION_MAX PT_LOAD) phdrs + the note. */
#define HDR_MAX (EHDR_SZ + PHDR_SZ * (1u + COREDUMP_REGION_MAX) + NOTE_SZ)

static uint8_t  hdrbuf[HDR_MAX];
static uint8_t  framebuf[12u + COREDUMP_DATA_MAX];

/* ---- little-endian stores into a byte buffer ---- */
static void put16(uint8_t *b, uint32_t off, uint16_t v)
{
	b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *b, uint32_t off, uint32_t v)
{
	b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)(v >> 16); b[off + 3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *b, uint32_t off, uint64_t v)
{
	put32(b, off, (uint32_t)v);
	put32(b, off + 4, (uint32_t)(v >> 32));
}

/* ---- streaming state: buffer bytes into frames of COREDUMP_DATA_MAX ---- */
struct stream {
	uint32_t seq;        /* next frame index                        */
	uint32_t total;      /* precomputed total frame count           */
	uint32_t fill;       /* bytes currently in framebuf data area    */
	uint32_t sent_bytes; /* bytes emitted so far (for the hard cap)  */
};

/* Emit the current frame (with optional last flag), then reset the fill. */
static void frame_flush(struct stream *s, int last)
{
	int i;

	put32(framebuf, 0, COREDUMP_MAGIC);
	put16(framebuf, 4, (uint16_t)s->seq);
	put16(framebuf, 6, (uint16_t)s->total);
	put16(framebuf, 8, (uint16_t)(last ? 1u : 0u));
	put16(framebuf, 10, (uint16_t)s->fill);

	/* Bounded retry: give the TX ring a few chances to drain, then drop. */
	for (i = 0; i < 2000; i++) {
		if (emac_send_frame(COREDUMP_ETHERTYPE, framebuf,
		                    (uint16_t)(12u + s->fill)))
			break;
		emac_poll();
	}
	emac_poll();             /* reap this frame's TX completion */

	s->seq++;
	s->fill = 0;
}

static void stream_byte(struct stream *s, uint8_t v)
{
	if (s->sent_bytes >= COREDUMP_MAX_TOTAL)
		return;              /* hard cap reached — silently truncate */
	framebuf[12u + s->fill] = v;
	s->fill++;
	s->sent_bytes++;
	if (s->fill >= COREDUMP_DATA_MAX)
		frame_flush(s, 0);
}

static void stream_buf(struct stream *s, const uint8_t *p, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		stream_byte(s, p[i]);
}

/* Read `n` bytes from guest/kernel memory into the stream, defensively: only
 * DRAM-range addresses are dereferenced; anything else streams as zero so the
 * segment stays the declared size and the fault path never faults on a read. */
static void stream_mem(struct stream *s, uint64_t addr, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint64_t a = addr + i;
		uint8_t  b = 0;

		if (a >= DRAM_LO && a < DRAM_HI)
			b = *(volatile uint8_t *)(uintptr_t)a;
		stream_byte(s, b);
	}
}

/* ---- region set: [addr,len] pairs, clamped into DRAM and to a size cap ---- */
struct region { uint64_t addr; uint32_t len; };

/* Clamp a requested region to DRAM and to a per-region size cap. Returns 0 if
 * nothing usable remains. */
static int region_clamp(uint64_t addr, uint64_t len, struct region *out)
{
	uint64_t end;

	if (len == 0 || addr >= DRAM_HI || addr < DRAM_LO)
		return 0;
	if (len > (COREDUMP_MAX_TOTAL / 2u))
		len = COREDUMP_MAX_TOTAL / 2u;
	end = addr + len;
	if (end > DRAM_HI)
		end = DRAM_HI;
	out->addr = addr;
	out->len  = (uint32_t)(end - addr);
	return out->len ? 1 : 0;
}

void coredump_send(struct el2_frame *frame, uint64_t *regions, int nregions)
{
	struct region reg[COREDUMP_REGION_MAX];
	struct stream s;
	uint8_t  prstatus[PRSTATUS_SZ];
	uint32_t nreg = 0, hlen, note_off, data_off, seg_off;
	uint32_t total_bytes, i, j;
	uint64_t sp1, sp_pa, sbase;

	if (!frame)
		return;

	/* Region 0: a bounded window around the GUEST's own stack pointer (2
	 * pages, SP kept inside) — SP_EL1, read directly, NOT frame->sp_at_entry
	 * (that field is EL2's OWN exception stack pointer; see the big comment
	 * on gva_to_pa() above for why that was wrong for a guest post-mortem).
	 * coredump_send() is only ever invoked for a genuine GUEST (lower-EL)
	 * fault today — el2_exc.c's B3 call site gates on (kind>>2)==2u before
	 * calling this — so SP_EL1 here is exactly the panicking kernel's own
	 * live stack pointer, same register el2_ss_handle() already treats as
	 * "the guest's SP" elsewhere in this tree. sp1 (the VA) is what's
	 * recorded into NT_PRSTATUS below, matching every other captured
	 * register (x0-x30/elr/spsr are also raw, untranslated guest values);
	 * sp_pa (translated via gva_to_pa()) is only for the actual memory
	 * region, since region_clamp()/stream_mem() work in flat physical
	 * addresses. */
	__asm__ volatile("mrs %0, sp_el1" : "=r"(sp1));
	sp_pa = gva_to_pa(sp1);
	sbase = (sp_pa > 0x1000ULL) ? ((sp_pa - 0x1000ULL) & ~0xFFFULL) : sp_pa;
	if (region_clamp(sbase, 0x2000ULL, &reg[nreg]))
		nreg++;

	/* Caller-supplied (addr,len) pairs. */
	for (i = 0; regions && (int)i < nregions && nreg < COREDUMP_REGION_MAX; i++)
		if (region_clamp(regions[2u * i], regions[2u * i + 1u], &reg[nreg]))
			nreg++;

	/* ---- build the prstatus note payload (register set) ---- */
	for (i = 0; i < PRSTATUS_SZ; i++)
		prstatus[i] = 0;
	for (i = 0; i < 31u; i++)                               /* x0..x30 */
		put64(prstatus, PRSTATUS_REGOFF + i * 8u, frame->x[i]);
	put64(prstatus, PRSTATUS_REGOFF + 31u * 8u, sp1);                /* sp (guest SP_EL1) */
	put64(prstatus, PRSTATUS_REGOFF + 32u * 8u, frame->elr);         /* pc */
	put64(prstatus, PRSTATUS_REGOFF + 33u * 8u, frame->spsr);        /* pstate */

	/* ---- lay out the file: Ehdr | phdrs | note | seg0 | seg1 | ... ---- */
	hlen     = EHDR_SZ + PHDR_SZ * (1u + nreg);   /* Ehdr + all phdrs   */
	note_off = hlen;                              /* PT_NOTE contents   */
	data_off = note_off + NOTE_SZ;                /* first PT_LOAD data */

	/* Ehdr */
	for (i = 0; i < HDR_MAX; i++)
		hdrbuf[i] = 0;
	hdrbuf[0] = 0x7f; hdrbuf[1] = 'E'; hdrbuf[2] = 'L'; hdrbuf[3] = 'F';
	hdrbuf[4] = 2;    /* ELFCLASS64 */
	hdrbuf[5] = 1;    /* ELFDATA2LSB */
	hdrbuf[6] = EV_CURRENT;
	put16(hdrbuf, 16, ET_CORE);
	put16(hdrbuf, 18, EM_AARCH64);
	put32(hdrbuf, 20, EV_CURRENT);
	put64(hdrbuf, 24, 0);              /* e_entry  */
	put64(hdrbuf, 32, EHDR_SZ);        /* e_phoff  */
	put64(hdrbuf, 40, 0);              /* e_shoff  */
	put32(hdrbuf, 48, 0);              /* e_flags  */
	put16(hdrbuf, 52, EHDR_SZ);        /* e_ehsize */
	put16(hdrbuf, 54, PHDR_SZ);        /* e_phentsize */
	put16(hdrbuf, 56, (uint16_t)(1u + nreg)); /* e_phnum */
	put16(hdrbuf, 58, 0);              /* e_shentsize */
	put16(hdrbuf, 60, 0);              /* e_shnum */
	put16(hdrbuf, 62, 0);              /* e_shstrndx */

	/* PT_NOTE phdr (first) */
	{
		uint32_t p = EHDR_SZ;
		put32(hdrbuf, p + 0, PT_NOTE);
		put32(hdrbuf, p + 4, 0);                  /* p_flags */
		put64(hdrbuf, p + 8,  note_off);          /* p_offset */
		put64(hdrbuf, p + 16, 0);                 /* p_vaddr */
		put64(hdrbuf, p + 24, 0);                 /* p_paddr */
		put64(hdrbuf, p + 32, NOTE_SZ);           /* p_filesz */
		put64(hdrbuf, p + 40, 0);                 /* p_memsz */
		put64(hdrbuf, p + 48, 4);                 /* p_align */
	}

	/* PT_LOAD phdrs */
	seg_off = data_off;
	for (j = 0; j < nreg; j++) {
		uint32_t p = EHDR_SZ + PHDR_SZ * (1u + j);
		put32(hdrbuf, p + 0, PT_LOAD);
		put32(hdrbuf, p + 4, PF_R | PF_W | PF_X);
		put64(hdrbuf, p + 8,  seg_off);           /* p_offset */
		put64(hdrbuf, p + 16, reg[j].addr);       /* p_vaddr */
		put64(hdrbuf, p + 24, reg[j].addr);       /* p_paddr */
		put64(hdrbuf, p + 32, reg[j].len);        /* p_filesz */
		put64(hdrbuf, p + 40, reg[j].len);        /* p_memsz */
		put64(hdrbuf, p + 48, 8);                 /* p_align */
		seg_off += reg[j].len;
	}

	/* PT_NOTE contents at note_off: Nhdr + name + prstatus */
	{
		uint32_t p = note_off;
		put32(hdrbuf, p + 0, 5);           /* n_namesz ("CORE\0") */
		put32(hdrbuf, p + 4, PRSTATUS_SZ); /* n_descsz */
		put32(hdrbuf, p + 8, NT_PRSTATUS); /* n_type   */
		hdrbuf[p + 12] = 'C'; hdrbuf[p + 13] = 'O';
		hdrbuf[p + 14] = 'R'; hdrbuf[p + 15] = 'E';
		hdrbuf[p + 16] = 0;                /* + name padding to 8 (already 0) */
		for (i = 0; i < PRSTATUS_SZ; i++)
			hdrbuf[p + 20u + i] = prstatus[i];
	}

	/* ---- compute total frame count for the wire header ---- */
	total_bytes = hlen + NOTE_SZ;
	for (j = 0; j < nreg; j++)
		total_bytes += reg[j].len;
	if (total_bytes > COREDUMP_MAX_TOTAL)
		total_bytes = COREDUMP_MAX_TOTAL;

	s.seq = 0;
	s.fill = 0;
	s.sent_bytes = 0;
	s.total = (total_bytes + COREDUMP_DATA_MAX - 1u) / COREDUMP_DATA_MAX;
	if (s.total == 0)
		s.total = 1;

	/* ---- stream: header block (Ehdr+phdrs+note), then each segment ---- */
	stream_buf(&s, hdrbuf, data_off);            /* through end of note */
	for (j = 0; j < nreg; j++)
		stream_mem(&s, reg[j].addr, reg[j].len);

	frame_flush(&s, 1);                          /* final frame, last flag */
}
