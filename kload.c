/* SPDX-License-Identifier: BSD-2-Clause */

/* kload.c — FreeBSD/arm64 kernel loader + boot handoff. See kload.h for the
 * full API contract and the FreeBSD arm64 boot-protocol writeup (modinfo
 * record layout, every MODINFO_ / MODINFOMD_ tag value + confidence notes).
 *
 * ------------------------------------------------------------------------
 * HONEST SCOPE: this is expected to get the real FreeBSD kernel's locore.S
 * EXECUTING under our EL2 hypervisor, not to fully boot it. initarm() will
 * very likely go on to probe a UART/GIC/timer/DTB in ways this milestone
 * does not emulate or pass through, and fault or hang there — that's a
 * separate, later lane (device pass-through / emulation), and is exactly
 * why the integration lane's EL2 trap handler (el2_exc.c/exceptions.S)
 * staying live and reporting ESR/FAR/ELR on the resulting trap is the real
 * deliverable here, on top of "the copy + metadata + eret are all correct".
 *
 * PHYSICAL LOAD MODEL (corrected from an initial physical-link-address
 * assumption after inspecting the real kernel ELF at
 * /opt/bzdos/tftpboot/kernel): this kernel's PT_LOAD program headers have
 * BOTH p_vaddr and p_paddr equal to 0xffff000000000000 + offset (the
 * FreeBSD arm64 fixed KVA "kernbase" — NOT a physical address in either
 * field). FreeBSD's own loader/locore.S convention is: the loader places
 * the kernel at a physical base of ITS choosing (not the ELF's p_paddr),
 * and position-independent code at the front of locore.S computes the
 * running phys<->virt delta itself before building page tables. We do the
 * same: the integration lane supplies pa_base (a physical load address),
 * and every PT_LOAD segment is placed at
 *     dest_pa = pa_base + (p_vaddr - kernbase)
 * where kernbase = the lowest p_vaddr across all PT_LOAD entries (this
 * preserves the kernel's internal relative layout exactly as linked). The
 * raw ELF p_paddr field is read but never trusted/used as an address for
 * this reason — see struct kload_seg's p_paddr_pa field naming in kload.h,
 * which is the RESOLVED physical destination, not a copy of p_paddr.
 *
 * MODINFOMD_KERNEND — the SP-in-rodata fix. A live hypervisor debugger
 * caught the running guest looping forever at VBAR_EL1+0x200 (curEL/SPx
 * SYNC) in a nested data-abort: ESR_EL1=0x9600004f (EC=0x25 data abort,
 * DFSC=0x0f permission fault L3), SP_EL1 pointing into the kernel's RODATA
 * segment (KVA 0xffff000000a90000, R-only) — so the exception handler's
 * context push permission-faulted, forever. Root cause: our first cut used
 * the WRONG modinfo subtypes (DTBP as MI 0x0004 = ESYM, KERNEND as MI
 * 0x0007 = ENVP), so the kernel never actually got a KERNEND. initarm()
 * uses KERNEND as the VIRTUAL cursor its bootstrap allocator starts from,
 * and thread0's initial kernel stack is carved from there; with no/garbage
 * KERNEND the kstack landed in rodata.
 *
 * The fix (this revision): emit MODINFOMD_KERNEND=0x0009 (MI) and
 * MODINFOMD_DTBP=0x1002 (arm64 MD) — see kload.h for the authoritative
 * values + why 0x0009/0x1002 are locked by the target's verified enum
 * (ESYM=0x0004, ENVP=0x0007). KERNEND is emitted as a KVA covering
 * EVERYTHING placed, rounded up to 2 MiB:
 *     kernend_va = kernbase + roundup2(max_pa - pa_base, 2MiB)
 *     max_pa = max(kernel_end_pa, dtb_pa+dtb_totalsize, scratch_pa+blob_len)
 * The DTB size is read from its own FDT header (magic 0xd00dfeed and a
 * big-endian totalsize at dtb+4), so kernend genuinely sits past the DTB
 * and the modinfo blob and the kernel won't allocate over live loader
 * data. This is a VA (not physical) because initarm() dereferences kernend
 * in KVA space after pmap_bootstrap maps [KERNBASE, kernend).
 *
 * EARLY-MAP REACHABILITY (the recursive-exception-storm fix). Static
 * analysis of this kernel's locore.S create_pagetables (entry 0xb00) shows
 * the ONLY memory the kernel can touch before pmap_bootstrap is what that
 * routine maps: for TTBR1 it maps [pa_base, phys(modulep) + ~6 MiB) (the
 * upper bound is literally (modulep - KERNBASE) + 0x5fffff, >>21, i.e. the
 * modulep offset rounded up ~three 2 MiB blocks) when modulep is a KVA; for
 * TTBR0 it maps just the single 2 MiB block containing kernbase. initarm()
 * dereferences BOTH modulep (parse_boot_param -> fdt_check_header) and the
 * DTBP value (OF_init) while only that early map is live, so BOTH must sit
 * inside it. We therefore (1) hand the kernel a VIRTUAL modulep so the map
 * is sized to cover it, (2) relocate the DTB from wherever U-Boot left it
 * (dtb_src_pa) to dtb_dst_pa just above the kernel image and emit DTBP as
 * the matching KVA, and (3) keep dtb_dst_pa BELOW scratch_pa so both land
 * under the modulep+6 MiB extent. INTEGRATION NOTE: place dtb_dst_pa and
 * scratch_pa just above kload_kernel_end_pa() (a couple of 2 MiB blocks up,
 * clear of the live page tables that sit between __bss_end and _end) and
 * well below dtb_src_pa — the VA<->PA delta is the uniform (kernbase -
 * pa_base) our placement model guarantees, which locore recomputes from
 * where it actually runs, so physical P maps to kernbase + (P - pa_base).
 *
 * X0 CONVENTION CONFIDENCE: HIGH, now confirmed empirically — the guest
 * reached its OWN C code and set VBAR_EL1 from this blob, which only
 * happens if locore successfully walked the x0 = modinfo blob. FreeBSD
 * arm64's initarm() (machdep.c) calls preload_search_by_type("elf kernel")
 * against x0; there is no alternate "x0 = DTB" arm64 convention to hedge,
 * so kload_enter() always sets x0 = the modinfo blob pointer, no dual-mode
 * flag. That pointer is now the blob's KVA (kload_build_modinfo() returns
 * it): a virtual modulep is what steers create_pagetables into sizing the
 * early TTBR1 map to cover the blob (see EARLY-MAP REACHABILITY below); a
 * physical modulep would instead be taken as a raw DTB and left unmapped.
 *
 * Freestanding: <stdint.h> only; memcpy/memset come from this tree's
 * libmin.c (already linked elsewhere, no separate declaration header — we
 * declare the two prototypes we use locally, matching libmin.c's exact
 * signatures, same as how other translation units in this tree consume it
 * implicitly via -nostdlib without a shared string.h).
 */
#include <stdint.h>
#include "kload.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);

/* ------------------------------------------------------------------ *
 * Breadcrumb window: 0x50000d00 ("KLD1"). Distinct from every other
 * window in the tree (MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300,
 * EL2 exceptions 0x50000400, jitter/TIMR 0x50000500, RING 0x50000600,
 * ALLOC 0x50000700, GIC timer 0x50000800, SCHED 0x50000a00, GUEST
 * 0x50000b00, STAGE2 0x50000c00, this file 0x50000d00).
 *
 *   [0]  magic          0x4B4C4431 ("KLD1")
 *   [1]  elf_valid      1 if kload_parse_elf() accepted the header, else 0
 *   [2]  e_entry_lo     raw ELF e_entry, low 32 bits (a KVA, NOT physical —
 *                       see [10] for the physical address actually used)
 *   [3]  n_seg          number of PT_LOAD segments recorded
 *   [4]  kernel_end_lo  kernel_end_pa (physical), low 32 bits
 *   [5]  modinfo_pa_lo  modinfo blob physical address, low 32 bits
 *   [6]  bytes_copied   total bytes memcpy'd across all PT_LOAD segments
 *                       during kload_place_segments()
 *   [7]  first_dest_lo  resolved physical destination of PT_LOAD[0], low
 *                       32 bits (pa_base + (p_vaddr[0] - kernbase))
 *   [8]  pa_base_lo     pa_base as supplied to kload_place_segments(),
 *                       low 32 bits
 *   [9]  kernbase_lo    kernbase (lowest PT_LOAD p_vaddr), low 32 bits —
 *                       high bits are always 0xffff0000 for a stock
 *                       FreeBSD arm64 kernel, so the low word is the
 *                       useful/varying part
 *   [10] entry_pa_lo    physical entry address kload_enter() was/will be
 *                       given: pa_base + (e_entry - kernbase), low 32 bits
 *   [11] kernend_va_lo  MODINFOMD_KERNEND value emitted (a KVA), low 32
 *                       bits — high word is 0xffff0000. This is the VA
 *                       cursor the FreeBSD bootstrap allocator (thread0
 *                       kstack included) starts from; the SP-in-rodata bug
 *                       was this being absent/garbage in the first cut.
 *   [12] dtbp_va_lo     MODINFOMD_DTBP value emitted, low 32 bits. Now a
 *                       KVA (high word 0xffff0000) pointing at the DTB
 *                       relocated into the early-mapped window — 0 if the
 *                       source had no valid FDT magic.
 *   [13] modulep_va_lo  modulep (x0) actually handed to the kernel, low 32
 *                       bits — a KVA (high word 0xffff0000), the return
 *                       value of kload_build_modinfo(). Confirms the blob
 *                       is handed virtually so locore's map covers it.
 *   [14] symtab_valid   ROADMAP B3 crash-forensics: 1 if kload_parse_elf()
 *                       located a .symtab + paired .strtab section pair in
 *                       this ELF, 0 if not (stripped kernel, or malformed/
 *                       absent section headers — never fatal to the parse).
 *   [15] symtab_count   number of Elf64_Sym entries found (0 if [14]==0).
 *                       See kload_symtab_info()/kload.h for the consumer
 *                       contract (ksym.c's on-board symbol resolver).
 * ------------------------------------------------------------------ */
#define KLOAD_BC_BASE  0x50000d00UL
#define KLOAD_BC_MAGIC 0x4B4C4431u   /* "KLD1" */

enum {
	KBC_MAGIC = 0,
	KBC_ELF_VALID,
	KBC_E_ENTRY_LO,
	KBC_N_SEG,
	KBC_KERNEL_END_LO,
	KBC_MODINFO_PA_LO,
	KBC_BYTES_COPIED,
	KBC_FIRST_DEST_LO,
	KBC_PA_BASE_LO,
	KBC_KERNBASE_LO,
	KBC_ENTRY_PA_LO,
	KBC_KERNEND_VA_LO,
	KBC_DTBP_VA_LO,      /* [12] MODINFOMD_DTBP value emitted (now a KVA), low 32 */
	KBC_MODULEP_VA_LO,   /* [13] modulep handed to the kernel (KVA), low 32 */
	KBC_SYMTAB_VALID,    /* [14] ROADMAP B3: 1 if a .symtab/.strtab pair was located */
	KBC_SYMTAB_COUNT,    /* [15] ROADMAP B3: symtab_count (0 if symtab_valid==0) */
};

static inline void
kload_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(KLOAD_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Minimal local ELF64/aarch64 definitions — no dependency on any system
 * or toolchain <elf.h>, matching this tree's "freestanding, define what
 * you need" style (see guest.c/stage2.h doing the same for system
 * registers rather than pulling in an SDK header).
 * ------------------------------------------------------------------ */
typedef struct {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} kload_elf64_ehdr_t;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} kload_elf64_phdr_t;

#define ELF_PT_LOAD     1u
#define ELF_ELFCLASS64  2u
#define ELF_ELFDATA2LSB 1u
#define ELF_EM_AARCH64  183u
#define ELF_ET_EXEC     2u
#define ELF_ET_DYN      3u

/* Elf64_Shdr — ROADMAP B3 (crash forensics): the ONLY new field this loader
 * consults beyond what it already used for PT_LOAD placement. Read purely to
 * LOCATE (never to read the actual symbol bytes here — that stays in
 * ksym.c) a SHT_SYMTAB + its sh_link-paired SHT_STRTAB section, so a
 * backtrace address can later be resolved to "symbol+offset" on-board
 * instead of only ever host-side via addr2line. Never required for the
 * primary boot mission (a stripped kernel still places/enters fine — see
 * the scan in kload_parse_elf() below, which never fails the parse). */
typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint64_t sh_flags;
	uint64_t sh_addr;
	uint64_t sh_offset;
	uint64_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint64_t sh_addralign;
	uint64_t sh_entsize;
} kload_elf64_shdr_t;

#define ELF_SHT_SYMTAB       2u
#define ELF_SHT_STRTAB       3u
#define ELF_SYM_ENTSIZE      24u  /* sizeof(Elf64_Sym): fixed by the ELF64 spec */

/* Sanity cap on section headers we're willing to scan looking for .symtab —
 * mirrors KLOAD_MAX_PHDR's role for program headers: generous headroom (a
 * real FreeBSD arm64 GENERIC kernel has on the order of 30-40 sections), not
 * a tight fit, and purely defensive against a corrupt/hostile e_shnum. */
#define KLOAD_MAX_SHDR 512u

/* MODINFO_ and MODINFOMD_ tag values live in kload.h now (authoritative,
 * with per-value source-header citations). See the big protocol comment
 * there for the full derivation/confidence discussion. */

/* 2 MiB kernend rounding boundary — matches the L2 block granularity
 * FreeBSD arm64 pmap_bootstrap uses when mapping [KERNBASE, kernend). */
#define KLOAD_KERNEND_ALIGN 0x200000ull

/* ------------------------------------------------------------------ *
 * Cache maintenance: clean D-cache to PoC ("dc cvac") + invalidate
 * I-cache to PoU ("ic ivau") over every 64-byte line in [addr, addr+len).
 * Exact idiom already used by repl.c's "rx"/"c" hot-reload path and
 * fb.c's flush path in this tree — copied here rather than shared via a
 * header because this file must stay disjoint (only kload.c/kload.h are
 * ours to create/edit).
 * ------------------------------------------------------------------ */
#define KLOAD_CACHE_LINE 64ul

static void
kload_cache_clean_inval(uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~(KLOAD_CACHE_LINE - 1);
	uint64_t end = addr + len;
	uint64_t o;

	for (o = start; o < end; o += KLOAD_CACHE_LINE)
		__asm__ volatile("dc cvac, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish");
	for (o = start; o < end; o += KLOAD_CACHE_LINE)
		__asm__ volatile("ic ivau, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish\n\tisb");
}

/* ------------------------------------------------------------------ *
 * EL2/EL1 handoff register helpers — same registers/asm as guest.c's
 * guest_enter(), duplicated locally (not shared via guest.h) so this file
 * stays a self-contained, disjoint addition.
 * ------------------------------------------------------------------ */
#define KLOAD_SPSR_EL1h (0x5ull)
#define KLOAD_SPSR_D    (1ull << 9)
#define KLOAD_SPSR_A    (1ull << 8)
#define KLOAD_GUEST_SPSR_EL2 (KLOAD_SPSR_EL1h | KLOAD_SPSR_D | KLOAD_SPSR_A)

static inline void
kload_write_sp_el1(uint64_t v)
{
	__asm__ volatile("msr sp_el1, %0" :: "r"(v));
}

static inline void
kload_write_spsr_el2(uint64_t v)
{
	__asm__ volatile("msr spsr_el2, %0" :: "r"(v) : "memory");
}

static inline void
kload_write_elr_el2(uint64_t v)
{
	__asm__ volatile("msr elr_el2, %0" :: "r"(v) : "memory");
}

/* ------------------------------------------------------------------ *
 * Parser/placer state. One in-flight kernel at a time (matches the
 * integration model: parse -> place -> build_modinfo -> enter, serially,
 * for a single boot attempt).
 * ------------------------------------------------------------------ */
static struct {
	int      valid;       /* kload_parse_elf() accepted the header */
	int      placed;      /* kload_place_segments() has run successfully */
	uint64_t elf_addr;     /* ELF image base, as given to kload_parse_elf() */
	uint64_t e_entry;      /* raw ELF entry point (KVA) */
	uint64_t kernbase;     /* lowest PT_LOAD p_vaddr */
	uint64_t kernend_va;   /* max(p_vaddr+p_memsz) across every PT_LOAD — a
	                        * pure VA quantity, known right after parse,
	                        * unlike kernel_end_pa below which needs pa_base */
	int      n_seg;
	struct kload_seg seg[KLOAD_MAX_PHDR];

	uint64_t pa_base;      /* physical load base, as given to place_segments */
	uint64_t entry_pa;     /* pa_base + (e_entry - kernbase) */
	uint64_t kernel_end_pa;/* pa_base + (max(p_vaddr+p_memsz) - kernbase) */

	/* ROADMAP B3: located (not read) .symtab/.strtab, see kload_parse_elf().
	 * symtab_valid==0 means "no symbol table" (e.g. stripped kernel, or no
	 * successful parse yet) — every other symtab_* field is meaningless. */
	int      symtab_valid;
	uint64_t symtab_addr;  /* elf_addr + sh_offset of the SHT_SYMTAB section */
	uint32_t symtab_count; /* sh_size / ELF_SYM_ENTSIZE */
	uint64_t strtab_addr;  /* elf_addr + sh_offset of the paired SHT_STRTAB */
	uint64_t strtab_size;
} kls;

int
kload_parse_elf(uint64_t elf_addr)
{
	const kload_elf64_ehdr_t *eh = (const kload_elf64_ehdr_t *)elf_addr;
	const kload_elf64_phdr_t *ph;
	uint32_t i;

	kls.valid = 0;
	kls.placed = 0;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		goto fail;
	if (eh->e_ident[4] != ELF_ELFCLASS64)
		goto fail;
	if (eh->e_ident[5] != ELF_ELFDATA2LSB)
		goto fail;
	if (eh->e_machine != ELF_EM_AARCH64)
		goto fail;
	if (eh->e_type != ELF_ET_EXEC && eh->e_type != ELF_ET_DYN)
		goto fail;
	if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(kload_elf64_phdr_t))
		goto fail;
	if (eh->e_phnum > 64)   /* sanity cap on TOTAL headers (not just PT_LOAD) */
		goto fail;

	kls.elf_addr = elf_addr;
	kls.e_entry = eh->e_entry;
	kls.n_seg = 0;
	kls.kernbase = ~0ull;
	kls.kernend_va = 0;

	ph = (const kload_elf64_phdr_t *)(elf_addr + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		struct kload_seg *s;
		uint64_t vend;

		if (ph[i].p_type != ELF_PT_LOAD)
			continue;
		if (kls.n_seg >= KLOAD_MAX_PHDR)
			goto fail;   /* more PT_LOAD than we track -> refuse rather than truncate silently */

		s = &kls.seg[kls.n_seg++];
		s->p_offset = ph[i].p_offset;
		s->p_vaddr  = ph[i].p_vaddr;
		s->p_filesz = ph[i].p_filesz;
		s->p_memsz  = ph[i].p_memsz;
		s->p_paddr_pa = 0;   /* resolved by kload_place_segments() */

		if (ph[i].p_vaddr < kls.kernbase)
			kls.kernbase = ph[i].p_vaddr;

		/* ROADMAP B3: track the VA high-water mark directly from the ELF
		 * (max p_vaddr+p_memsz) — this is what kload_kernel_end_va() hands
		 * a symbol resolver for its cheap "is this VA even in the kernel
		 * image" range check. Pure VA math, no pa_base involved, so it's
		 * valid immediately after parse, unlike kernel_end_pa. */
		vend = ph[i].p_vaddr + ph[i].p_memsz;
		if (vend > kls.kernend_va)
			kls.kernend_va = vend;
	}
	if (kls.n_seg == 0)
		goto fail;

	/* ROADMAP B3: locate (never read here) an optional .symtab/.strtab pair
	 * via the ELF's SECTION headers (e_shoff/e_shnum — previously parsed
	 * into kload_elf64_ehdr_t but never consulted, since PT_LOAD placement
	 * only needed the program headers). Best-effort and NEVER fatal: a
	 * missing/malformed section-header table, or a stripped kernel with no
	 * SHT_SYMTAB at all, just leaves symtab_valid==0 — the core placement
	 * contract above is already satisfied and returns 1 regardless. */
	kls.symtab_valid = 0;
	kls.symtab_addr = 0;
	kls.symtab_count = 0;
	kls.strtab_addr = 0;
	kls.strtab_size = 0;
	if (eh->e_shoff != 0 && eh->e_shnum > 0 && eh->e_shnum <= KLOAD_MAX_SHDR &&
	    eh->e_shentsize == sizeof(kload_elf64_shdr_t)) {
		const kload_elf64_shdr_t *sh =
			(const kload_elf64_shdr_t *)(elf_addr + eh->e_shoff);
		uint32_t si;

		for (si = 0; si < eh->e_shnum; si++) {
			uint32_t link;

			if (sh[si].sh_type != ELF_SHT_SYMTAB)
				continue;
			/* Paired string table via sh_link — NOT by matching section
			 * NAMES against .shstrtab (that string table can itself be
			 * absent/stripped independently; sh_link is the authoritative,
			 * always-present ELF linkage for SHT_SYMTAB -> its strings). */
			link = sh[si].sh_link;
			if (link >= eh->e_shnum || sh[link].sh_type != ELF_SHT_STRTAB)
				break;   /* malformed linkage: leave symtab_valid==0 */
			if (sh[si].sh_entsize != ELF_SYM_ENTSIZE || sh[si].sh_size == 0)
				break;   /* not a real ELF64 symtab: leave symtab_valid==0 */

			kls.symtab_addr  = elf_addr + sh[si].sh_offset;
			kls.symtab_count = (uint32_t)(sh[si].sh_size / ELF_SYM_ENTSIZE);
			kls.strtab_addr  = elf_addr + sh[link].sh_offset;
			kls.strtab_size  = sh[link].sh_size;
			kls.symtab_valid = 1;
			break;   /* first SHT_SYMTAB wins — a kernel image has exactly one */
		}
	}

	kls.valid = 1;
	kload_bc(KBC_MAGIC, KLOAD_BC_MAGIC);
	kload_bc(KBC_ELF_VALID, 1u);
	kload_bc(KBC_E_ENTRY_LO, (uint32_t)kls.e_entry);
	kload_bc(KBC_N_SEG, (uint32_t)kls.n_seg);
	kload_bc(KBC_KERNBASE_LO, (uint32_t)kls.kernbase);
	kload_bc(KBC_SYMTAB_VALID, (uint32_t)kls.symtab_valid);
	kload_bc(KBC_SYMTAB_COUNT, kls.symtab_count);
	return 1;

fail:
	kload_bc(KBC_MAGIC, KLOAD_BC_MAGIC);
	kload_bc(KBC_ELF_VALID, 0u);
	return 0;
}

int
kload_place_segments(uint64_t elf_addr, uint64_t pa_base)
{
	uint64_t max_end_delta = 0;
	uint64_t total_bytes = 0;
	uint64_t first_dest = 0;
	int i;

	if (!kls.valid || elf_addr != kls.elf_addr || pa_base == 0)
		return 0;

	for (i = 0; i < kls.n_seg; i++) {
		struct kload_seg *s = &kls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - kls.kernbase);
		uint64_t end_delta;

		s->p_paddr_pa = dest;
		if (i == 0)
			first_dest = dest;

		if (s->p_filesz)
			memcpy((void *)dest, (const void *)(elf_addr + s->p_offset),
			       (unsigned long)s->p_filesz);
		if (s->p_memsz > s->p_filesz)
			memset((void *)(dest + s->p_filesz), 0,
			       (unsigned long)(s->p_memsz - s->p_filesz));
		if (s->p_memsz)
			kload_cache_clean_inval(dest, s->p_memsz);

		total_bytes += s->p_memsz;
		end_delta = (s->p_vaddr - kls.kernbase) + s->p_memsz;
		if (end_delta > max_end_delta)
			max_end_delta = end_delta;
	}

	kls.pa_base = pa_base;
	kls.kernel_end_pa = pa_base + max_end_delta;
	kls.entry_pa = pa_base + (kls.e_entry - kls.kernbase);
	kls.placed = 1;

	kload_bc(KBC_KERNEL_END_LO, (uint32_t)kls.kernel_end_pa);
	kload_bc(KBC_BYTES_COPIED, (uint32_t)total_bytes);
	kload_bc(KBC_FIRST_DEST_LO, (uint32_t)first_dest);
	kload_bc(KBC_PA_BASE_LO, (uint32_t)pa_base);
	kload_bc(KBC_ENTRY_PA_LO, (uint32_t)kls.entry_pa);
	return 1;
}

/* Append one modinfo record at *offp within [base, base+cap), advancing
 * *offp past the (type, len, data, pad-to-8) record. `data` may be NULL
 * iff len == 0 (used for the type=0/len=0 terminator). Returns 1 on
 * success, 0 if it would overflow cap (defensive — scratch regions in
 * practice are always far larger than this tiny blob). */
static int
kload_put_rec(uint8_t *base, uint64_t cap, uint64_t *offp,
              uint32_t type, const void *data, uint32_t len)
{
	uint64_t off = *offp;
	uint32_t *hdr;

	if (off + 8u + len > cap)
		return 0;

	hdr = (uint32_t *)(base + off);
	hdr[0] = type;
	hdr[1] = len;
	off += 8;
	if (len)
		memcpy(base + off, data, len);
	off += len;
	off = (off + 7u) & ~(uint64_t)7u;   /* pad to sizeof(u_long)==8 on arm64 */

	*offp = off;
	return 1;
}

/* Read the total size a flat device tree declares in its own header.
 * FDT header: big-endian u32 magic 0xd00dfeed at offset 0, big-endian u32
 * totalsize at offset 4. Returns 0 if the magic does not match (caller
 * then conservatively does not extend kernend for the DTB). */
static uint64_t
kload_dtb_totalsize(uint64_t dtb_pa)
{
	const uint8_t *p = (const uint8_t *)dtb_pa;
	uint32_t magic, total;

	if (dtb_pa == 0)
		return 0;
	magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	        ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
	if (magic != 0xd00dfeedu)
		return 0;
	total = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
	        ((uint32_t)p[6] << 8)  | (uint32_t)p[7];
	return total;
}

uint64_t
kload_build_modinfo(uint64_t dtb_src_pa, uint64_t dtb_dst_pa, uint64_t scratch_pa)
{
	/* Generous cap: this blob is a handful of tiny records plus a 2-byte
	 * env, never remotely close to 4 KiB, but bound the writes defensively
	 * rather than trusting the caller's scratch region is infinite. */
	const uint64_t cap = 4096;
	uint8_t *base = (uint8_t *)scratch_pa;
	uint64_t off = 0;
	static const char mod_name[] = "kernel";
	static const char mod_type[] = "elf kernel";
	uint64_t addr_val, size_val, dtbp_val, kernend_val, howto_val, envp_val;
	uint64_t env_pa, env_va, dtb_size, dtb_end, blob_end, max_pa, max_delta;
	uint64_t modulep_va;

	if (!kls.placed || scratch_pa == 0)
		return 0;

	/* Bring the DTB INTO the early-mapped window. locore.S create_pagetables
	 * maps, for TTBR1, only [pa_base, phys(modulep)+~6 MiB) and, for TTBR0,
	 * a single 2 MiB identity block at kernbase — so the DTB the U-Boot/TFTP
	 * path parked at dtb_src_pa (0x4a000000, ~47 MiB above pa_base) is
	 * outside BOTH, and initarm()'s OF_init((void *)dtbp) would translation-
	 * fault reading it. Copy it to dtb_dst_pa (placed just above the kernel
	 * image, inside the window and BELOW the modinfo blob so it stays under
	 * the modulep+6 MiB map extent) and emit DTBP as the matching KVA. The
	 * size comes from the FDT's own header; if it's not a valid FDT we emit
	 * DTBP=0 so the kernel simply skips OF_init rather than faulting. */
	dtb_size = kload_dtb_totalsize(dtb_src_pa);
	if (dtb_size && dtb_dst_pa) {
		memcpy((void *)dtb_dst_pa, (const void *)dtb_src_pa,
		       (unsigned long)dtb_size);
		kload_cache_clean_inval(dtb_dst_pa, dtb_size);
		dtbp_val = kls.kernbase + (dtb_dst_pa - kls.pa_base);  /* KVA */
		dtb_end  = dtb_dst_pa + dtb_size;
	} else {
		dtbp_val = 0;
		dtb_end  = 0;
	}

	/* Kernel environment (MODINFOMD_ENVP): concatenated "name=value\0" entries
	 * terminated by an empty entry (trailing \0 -> double-NUL at the end). We
	 * load the kernel DIRECTLY (no FreeBSD loader), so loader.conf is NOT read —
	 * anything the guest needs at boot must be injected HERE. Critically:
	 * vfs.root.mountfrom, so the guest auto-mounts root from MMC instead of
	 * dropping to the interactive mountroot prompt (which then crashes in
	 * cngrab() calling a NULL console cn_grab). Value taken from
	 * /opt/bzdos/tftpboot/loader.conf. The C string literal's implicit final
	 * NUL supplies the empty terminating entry (sizeof includes it), giving the
	 * required double-NUL. Its VA uses the SAME uniform delta as everything else
	 * and sits inside the pmap_bootstrap-mapped region so the kernel can read it. */
	static const char kenv[] =
	    /* NOTE (2026-07-21): init_path=/rescue/sh was tried to bypass rc — it
	     * PROVED EL0 works cleanly (the static sh ran and exited 0, NO SIGSEGV,
	     * killing the old "userland faults" theory) but exiting PID 1 panics
	     * ("Going nowhere without my init") and warm-reboot-loops, so it's not
	     * usable as a persistent shell. Back to /sbin/init (kernel default —
	     * no init_path kenv needed). The real remaining blocker is the on-disk
	     * /etc/fstab naming mmcsd0pX (aw_mmc) instead of vtbd0pX (virtio); fix
	     * that in the flashed image, not here. */
	    /* (boot_single=YES was tried to reach single-user for an fstab fix —
	     * FreeBSD arm64 ignored both it and the MODINFOMD_HOWTO RB_SINGLE bit,
	     * always booting multi-user. Dropped. Instead /etc/fstab was patched
	     * directly on the eMMC via the HV (mmcsd0pX -> vtbd0p3, fsck pass 0),
	     * so multi-user rc no longer hangs on the interactive dirty-FS fsck.) */
	    /* ROOT CAUSE FOUND (2026-07-22, read straight out of the local FreeBSD
	     * checkout, sys/kern/vfs_mountroot.c): the ';'-joined value below was
	     * NEVER valid syntax. vfs_mountroot_conf0() reads this kenv with
	     * parse_token(), which splits ONLY on whitespace (parse_skipto(conf,
	     * CC_WHITESPACE), see kern_getenv("vfs.root.mountfrom") handling) — a
	     * ';' is not a delimiter anywhere in that path. So
	     * "ufs:/dev/vtbd0p3;ufs:/dev/gpt/rootfs" (no space around the ';') was
	     * read as ONE token and handed whole to parse_mount(), which parses
	     * fstype="ufs" and dev="/dev/vtbd0p3;ufs:/dev/gpt/rootfs" (everything
	     * up to the next whitespace) — a device name that can never exist.
	     * That is EXACTLY the string in every "mountroot: waiting for device
	     * /dev/vtbd0p3;ufs:/dev/gpt/rootfs..." / "failed with error 19" panic
	     * seen all session. GPT/eMMC/virtio-blk were all independently
	     * verified healthy (see memory rootmount-gpt-healthy-blocker-
	     * guestside.md) — this kenv string was the actual bug the whole time.
	     * Multiple whitespace-separated fs:dev directives ARE a valid way to
	     * list fallbacks (vfs_mountroot_conf0 emits one line per token), but a
	     * single clean device is simplest and sufficient here. */
	    "vfs.root.mountfrom=ufs:/dev/vtbd0p3\0"
	    "vfs.mountroot.timeout=20\0"              /* RETESTED (2026-07-25), hypothesis REFUTED: this
	                                               * session's vGIC Group0 fix made device-SPI
	                                               * interrupt delivery work end-to-end (vtblk0/vtnet0
	                                               * now enumerate on REAL IRQs, not polling) — a
	                                               * plausible reason the old GEOM-taste stall below
	                                               * might have been fixed as a side effect. Bumped to
	                                               * 45s and watched the console with ZERO manual
	                                               * input for 55s post-boot: vblk reads stayed flat
	                                               * (stuck at device-probe depth, same as always) —
	                                               * automatic mountroot still does not complete, it
	                                               * still needs the timeout-to-drop-to-interactive
	                                               * dance. So the root GEOM-taste stall is A SEPARATE,
	                                               * STILL-OPEN bug, not a symptom of the interrupt
	                                               * problem that's now fixed. Reverted to 20s (no
	                                               * benefit to a longer wait; auto_mount_root() still
	                                               * does the interactive nudge either way, so shorter
	                                               * = less wasted boot time per cycle). Do NOT re-run
	                                               * this exact experiment. FINDING: even a 1200s wait does NOT auto-mount —
                                               * the GPT partitions materialize only when mountroot
                                               * GIVES UP and drops to the interactive prompt (the
                                               * GEOM taste is coupled to the wait ending). So use a
                                               * SHORT 20s timeout to reach the prompt fast, then the
                                               * host auto-injects the mount over the RX ring (or
                                               * chimpd does) -> userland quickly, so we can debug
                                               * the userland reboot. Old bumped 300->1200: WORKS +
                                               * boots userland; auto-mount raced GEOM taste-settle
                                               * within 300s. Testing if longer wait catches it.
                                               * long wait: guest sits stably at "waiting for
	                                               * device" for 5 min instead of dropping to the
	                                               * cngrab-crashing prompt — lets us read the full
	                                               * boot + MMC-discovery result from the ring */
	    "kern.geom.part.check_integrity=0\0"      /* CORRECTED (2026-07-22): the theory below (stale
	                                               * ~2.5GB-image backup-GPT mismatch) is REFUTED —
	                                               * re-verified live by reading LBA1 (primary) AND
	                                               * LBA 15269887 (backup, i.e. capacity-1) directly
	                                               * via emmc_bio: BOTH are valid "EFI PART" headers,
	                                               * capacity matches, p3="rootfs" first=278562
	                                               * last=2858721 freebsd-ufs. The disk was since
	                                               * recovered to a full-disk GPT; the OLD 2.5GB-image
	                                               * backup at LBA 4955906 is stale-but-harmless (GEOM
	                                               * follows the primary header's own backup-LBA
	                                               * pointer, which is correct). The REAL error-19
	                                               * cause was the ';'-joined vfs.root.mountfrom kenv
	                                               * above, not GPT integrity — see its comment.
	                                               * Leaving check_integrity=0 in place regardless: it
	                                               * is a no-op safety net against the disk's genuinely
	                                               * unused/stale old backup header, costs nothing. */
	    "hw.aw_mmc.debug=0\0"                     /* was 0xc (INT|CMD) — used it to PROVE the
                                               * guest aw_mmc data reads all complete err 0
                                               * (CMD17/CMD18, idst=0x2 IDMAC-complete) and
                                               * that GEOM reads the correct GPT + partition
                                               * boundary blocks. So the read path is NOT the
                                               * blocker; root still ENODEV-times-out at 300s.
                                               * Turned back to 0 so the flood stops burying the
                                               * mountroot narrative + the interactive "?" GEOM
                                               * device list in the 64KB ring. Re-enable (0xc) if
                                               * a taste storm needs re-confirming. Old note:
                                               * 0xc = AW_MMC_DEBUG_INT|AW_MMC_DEBUG_CMD:
                                               * make the aw_mmc driver print EVERY command
                                               * (opcode/arg/dlen) and EVERY interrupt
                                               * (idst/imask/rint) + req_done error. The GPT
                                               * on-disk is byte-perfect (verified via EL2
                                               * emmc_bio, all FreeBSD gpt_read_hdr checks
                                               * pass) yet GEOM silently creates no mmcsd0pN
                                               * — the failing taste READ is debug-gated
                                               * silent in this driver, so unmask it. The
                                               * vconsole ring is circular (64KB) so the
                                               * late taste output survives the init spam. */
    "kern.panic_reboot_wait_time=-1\0";       /* -1 = sit at panic, DON'T auto-reboot —
	                                               * stops the panic-loop that overwrites the
	                                               * vconsole ring, so the mountroot failure +
	                                               * GEOM disk list survive for post-mortem md */
	env_pa = scratch_pa + 2048;
	for (unsigned _i = 0; _i < sizeof(kenv); _i++)
		((volatile uint8_t *)env_pa)[_i] = (uint8_t)kenv[_i];
	env_va = kls.kernbase + (env_pa - kls.pa_base);

	/* modulep handed to the kernel is the KVA of this blob, NOT its physical
	 * address. locore.S create_pagetables sizes the TTBR1 map from modulep:
	 * given a VIRTUAL modulep it maps [pa_base, phys(modulep)+~6 MiB), which
	 * is exactly what makes this blob (and the DTB below it) reachable when
	 * initarm() dereferences it; a physical modulep instead takes the "raw
	 * DTB" path and leaves x0 pointing at unmapped memory. */
	modulep_va = kls.kernbase + (scratch_pa - kls.pa_base);

	/* KERNEND (VA): cover kernel image, the relocated DTB, and this modinfo
	 * blob (incl. the env we just placed), rounded up to a 2 MiB boundary,
	 * then translated PA->VA. This is the SP-in-rodata fix — see kload.c's
	 * top comment and kload.h. */
	blob_end = env_pa + sizeof(kenv);            /* end of our own scratch use (env incl.) */
	max_pa   = kls.kernel_end_pa;
	if (dtb_end  > max_pa) max_pa = dtb_end;
	if (blob_end > max_pa) max_pa = blob_end;
	max_delta = max_pa - kls.pa_base;
	max_delta = (max_delta + (KLOAD_KERNEND_ALIGN - 1)) & ~(KLOAD_KERNEND_ALIGN - 1);

	addr_val    = kls.pa_base;                    /* MODINFO_ADDR: load base (PA) */
	size_val    = kls.kernel_end_pa - kls.pa_base;/* MODINFO_SIZE: kernel image size */
	envp_val    = env_va;                         /* MODINFOMD_ENVP: KVA of empty env */
	kernend_val = kls.kernbase + max_delta;        /* MODINFOMD_KERNEND: KVA past all */

	/* MODINFOMD_HOWTO: RB_VERBOSE for maximal boot diagnostics while this
	 * port is still being brought up. The earlier "howto doubles as
	 * lastaddr" theory (and the kernend_val-as-howto workaround it led to)
	 * was a MISDIAGNOSIS: the real bug was kload.h's MODINFOMD_ENVP/HOWTO/
	 * KERNEND tag numbers being off-by-one against the actual FreeBSD
	 * header (verified against the exact matching source, freebsd-src
	 * commit 9263fb9, releng/15.1). With the old numbering, our HOWTO
	 * record (tag MODINFO_METADATA|0x0008) was misread by the kernel's
	 * MD_FETCH(preload_kmdp, MODINFOMD_KERNEND) — which real FreeBSD also
	 * numbers 0x0008 — so whatever we put in howto_val came back verbatim
	 * as lastaddr. Now that the tag numbers are corrected in kload.h, HOWTO
	 * and KERNEND are fetched from their own distinct records again and
	 * this can go back to a normal howto value. */
	/* RB_VERBOSE (0x800) | RB_MULTIPLE (0x20000000).
	 *
	 * TWO CORRECTIONS, both verified against sys/sys/reboot.h in the matching
	 * checkout rather than from memory.
	 *
	 * 1. The old value was `0x800u | 0x1u` and its comment called 0x1
	 *    "RB_SINGLE". It is not. RB_SINGLE is 0x002; **0x001 is RB_ASKNAME**,
	 *    "force prompt of device of root filesystem". So this hypervisor was
	 *    asking the guest kernel for the interactive root prompt on every
	 *    single boot -- vfs_mountroot.c:899, `if (boothowto & RB_ASKNAME)`.
	 *
	 *    That is where the `mountroot>` prompt in every boot came from. It was
	 *    not a mount failure and not a GEOM timing problem: the kernel was
	 *    doing exactly what it was told. This tree has console-poking
	 *    automation (auto_mount_root(), see docs and the "automount must poke,
	 *    not listen" note) built to answer a prompt we were requesting.
	 *
	 *    It also explains the older note further up this file -- "FreeBSD arm64
	 *    ignored the MODINFOMD_HOWTO RB_SINGLE bit, always booting multi-user".
	 *    The bit was never RB_SINGLE, so of course single-user never happened.
	 *    arm64 honours HOWTO fine: machdep_boot.c:210,
	 *    boothowto = MD_FETCH(preload_kmdp, MODINFOMD_HOWTO, int), in the same
	 *    function that fetches the MODINFOMD_ENVP this loader already relies on.
	 *
	 * 2. RB_MULTIPLE (0x20000000) is now set, and is needed the moment anything
	 *    gives the guest a second console. With a `simple-framebuffer` DTB node
	 *    FreeBSD's simplefb(4) becomes the vt console; on a board with no
	 *    keyboard the guest then blocks in vtterm_cngetc() forever and the
	 *    serial console goes silent. RB_MULTIPLE keeps every console active, so
	 *    vt draws on the monitor while the serial keeps input.
	 *
	 *    This is the ONLY route to RB_MULTIPLE on this configuration:
	 *    `boot_multicons` as a kenv reaches boothowto only via
	 *    boot_env_to_howto(), which FreeBSD calls from x86/xen/pv.c alone, and
	 *    /chosen bootargs is parsed only `if (loader_envp == NULL)` -- and this
	 *    loader always passes MODINFOMD_ENVP. See docs/guest-display.md. */
	howto_val   = 0x800u | 0x20000000u;

	if (!kload_put_rec(base, cap, &off, MODINFO_NAME, mod_name, (uint32_t)sizeof(mod_name)))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_TYPE, mod_type, (uint32_t)sizeof(mod_type)))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_ADDR, &addr_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_SIZE, &size_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_ENVP, &envp_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_HOWTO, &howto_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_DTBP, &dtbp_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_KERNEND, &kernend_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, 0, 0, 0))   /* terminator */
		return 0;

	/* Cache-clean the whole span we touched: the record list at the front
	 * AND the env at scratch_pa+2048 (they are not contiguous). */
	kload_cache_clean_inval(scratch_pa, (env_pa + 2) - scratch_pa);

	kload_bc(KBC_MODINFO_PA_LO, (uint32_t)scratch_pa);
	kload_bc(KBC_KERNEND_VA_LO, (uint32_t)kernend_val);
	kload_bc(KBC_DTBP_VA_LO, (uint32_t)dtbp_val);
	kload_bc(KBC_MODULEP_VA_LO, (uint32_t)modulep_va);
	return modulep_va;
}

uint64_t
kload_entry_pa(void)
{
	return kls.placed ? kls.entry_pa : 0;
}

uint64_t
kload_kernel_end_pa(void)
{
	return kls.placed ? kls.kernel_end_pa : 0;
}

uint64_t
kload_pa_base(void)
{
	return kls.placed ? kls.pa_base : 0;
}

/* ROADMAP B3 (crash forensics) accessors — see the big comment block above
 * kload_symtab_info()'s declaration in kload.h for the full contract. These
 * gate ONLY on kls.valid (a successful kload_parse_elf()), not kls.placed:
 * kernbase/kernend_va/symtab location are all pure ELF-file properties that
 * never depended on kload_place_segments() having run. */
int
kload_symtab_info(uint64_t *sym_addr, uint32_t *sym_count,
                  uint64_t *str_addr, uint64_t *str_size)
{
	if (!kls.valid || !kls.symtab_valid)
		return 0;
	if (sym_addr)  *sym_addr  = kls.symtab_addr;
	if (sym_count) *sym_count = kls.symtab_count;
	if (str_addr)  *str_addr  = kls.strtab_addr;
	if (str_size)  *str_size  = kls.strtab_size;
	return 1;
}

uint64_t
kload_kernbase(void)
{
	return kls.valid ? kls.kernbase : 0;
}

uint64_t
kload_kernel_end_va(void)
{
	return kls.valid ? kls.kernend_va : 0;
}

void
kload_enter(uint64_t entry, uint64_t modinfo_pa, uint64_t sp)
{
	kload_write_sp_el1(sp);
	kload_write_spsr_el2(KLOAD_GUEST_SPSR_EL2);
	kload_write_elr_el2(entry);

	/* x0 must hold modinfo_pa at the instant of `eret` (the FreeBSD
	 * arm64 loader->kernel x0 convention — see kload.h). Setting it via
	 * an ordinary C argument and trusting it stays in x0 across
	 * intervening codegen is fragile, so pin it with inline asm
	 * immediately followed by eret in the same asm block. */
	__asm__ volatile(
		"mov x0, %0\n\t"
		"eret"
		:
		: "r"(modinfo_pa)
		: "x0", "memory");

	__builtin_unreachable();
}
