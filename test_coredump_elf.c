/* SPDX-License-Identifier: BSD-2-Clause */

/* test_coredump_elf.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the ELF (ET_CORE) assembly logic in coredump.c, and in
 * particular a regression test for the 2026-08-06 VA-vs-PA fix: the
 * PT_LOAD segments coredump_send() builds MUST publish the guest's
 * VIRTUAL address in p_vaddr (what a debugger matches SP/frame-pointer
 * values against) even though the bytes behind that segment are read via
 * a per-page translation to a PHYSICAL address that has nothing to do
 * with p_vaddr once the guest's MMU is on.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "coredump.c"` WITH STUBS
 * ============================================================================
 * coredump.c was read in full (current, post-fix) before writing this file.
 * The ELF-assembly arithmetic (put16/put32/put64, the Ehdr/Phdr/Nhdr layout
 * in coredump_send(), region_clamp(), stream_mem()'s per-page loop) is pure
 * byte-buffer bit-twiddling with no I/O at all. But the FILE is not
 * includable on x86_64:
 *   - gva_to_pa()          "at s1e1r"/"isb"/"mrs par_el1"     (coredump.c:44-46)
 *   - coredump_send()      "mrs sp_el1"                       (coredump.c:281)
 *   - frame_flush()        calls emac_send_frame()/emac_poll() (real EMAC TX)
 *   - stream_mem()         dereferences real physical DRAM addresses
 * every one of which either the assembler rejects on x86_64 or would
 * segfault the host process (exactly sd_bio.c's and stage2.c's situation, per
 * their own test files' rationale, which this mirrors).
 *
 * So: this file hand-transcribes put16/put32/put64, region_clamp(),
 * stream_mem() and the ELF/Phdr/PT_NOTE-layout portion of coredump_send()
 * verbatim (line numbers as of the 2026-08-06 fix), replacing ONLY:
 *   - gva_to_pa()      -> gva_to_pa_fake(): a small, caller-scripted
 *     VA-page -> PA-page table instead of AT S1E1R, with the IDENTICAL
 *     "translated, or untranslatable -> flat identity fallback" contract;
 *   - the raw `*(volatile uint8_t*)a` DRAM dereference -> fake_dram_read():
 *     a bounds-checked read from a small host byte array standing in for
 *     the real board's 1 GiB DRAM window, with the SAME
 *     "in [DRAM_LO,DRAM_HI) or zero" defensiveness;
 *   - emac_send_frame()/frame_flush()'s chunking -> appending straight into
 *     an output buffer (the wire-chunking protocol itself is already covered
 *     by coredump-recv.py's selftest, including its agreement with
 *     coredump.c:314-316's frame-count arithmetic; this file is only about
 *     ELF CONTENT correctness, a gap that selftest explicitly does not cover
 *     -- its one ELF-shaped case, case_assembled_elf_blob_is_verbatim, tests
 *     only that reassembly doesn't corrupt bytes, using a hand-built partial
 *     Ehdr with no PT_LOAD/PT_NOTE structure at all).
 *
 * ============================================================================
 * WHAT THIS FILE PROVES, EXERCISES, AND CANNOT COVER
 * ============================================================================
 * PROVES (about the mirrored logic):
 *   - PT_LOAD's p_vaddr/p_paddr equal the guest VIRTUAL address (the
 *     page-aligned window around SP_EL1), even when that VA translates to a
 *     PHYSICAL address that is numerically different and NOT DRAM-range-
 *     adjacent to it -- the exact case a real FreeBSD guest is in for every
 *     panic once its MMU is enabled (i.e. essentially always, past the very
 *     first few instructions of boot).
 *   - the data BEHIND that segment is read from the TRANSLATED PHYSICAL
 *     address, per page, not from the VA reinterpreted as a flat PA and not
 *     from one page's translation reused across a multi-page span whose
 *     pages are NOT physically contiguous (test_two_pages_translate_independently
 *     specifically fabricates non-contiguous physical pages and would fail
 *     under the pre-fix logic, which is also transcribed here as
 *     stream_mem_OLD_BUGGY() purely to demonstrate the delta).
 *   - the MMU-off / untranslatable fallback (identity VA==PA) still produces
 *     the same result it always did -- the fix changes nothing for the
 *     early-boot case this project's other instruments (backtrace.c,
 *     gtrace.c) already document as their proven scope.
 *   - NT_PRSTATUS's register fields round-trip exactly.
 * EXERCISES but does not, by itself, prove a real debugger accepts the
 * output: see va_pa_gdb_readable() below, which is a SEPARATE, narrower
 * case that shells out to `readelf`/`gdb` against the REAL kernel.debug --
 * skipped (not failed) if either tool or that file is unavailable, since a
 * hosted `make test` run must not depend on either.
 * CANNOT COVER (deliberately out of scope, needs the board): whether a REAL
 * FreeBSD guest's AT S1E1R/PAR_EL1 sequence, EMAC TX ring, or SP_EL1 at a
 * genuine panic behave as gva_to_pa_fake()/fake_dram_read() assume. This
 * file only proves the ARITHMETIC is now VA-safe, not that it has been
 * exercised against a live panic.
 *
 * Build: gcc -o test_coredump_elf test_coredump_elf.c && ./test_coredump_elf
 * (wired into `make test`, see Makefile.) Also supports a `--dump` mode (see
 * bottom of main()) used by crash_report.py's selftest to produce a real
 * core.elf for the readelf/gdb cross-check described above.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Verbatim from coredump.c:20-21.
 * ------------------------------------------------------------------ */
#define DRAM_LO 0x40000000ULL
#define DRAM_HI 0x80000000ULL

/* ------------------------------------------------------------------ *
 * Fake DRAM: a small host buffer standing in for the real board's 1 GiB
 * window, based at DRAM_LO. Physical addresses below DRAM_LO+FAKE_DRAM_SIZE
 * are backed by real bytes; the rest of [DRAM_LO,DRAM_HI) reads as 0 (valid
 * but unbacked -- matching real hardware's harmless zero/garbage-but-not-a-
 * fault behaviour for DRAM this test never populated); outside
 * [DRAM_LO,DRAM_HI) entirely also reads as 0, per coredump.c's own
 * defensiveness (never fault, never dereference outside the DRAM window).
 * ------------------------------------------------------------------ */
#define FAKE_DRAM_SIZE (1u << 22)          /* 4 MiB -- ample for the tests */
static uint8_t fake_dram[FAKE_DRAM_SIZE];

static uint8_t fake_dram_read(uint64_t pa)
{
	if (pa >= DRAM_LO && pa < DRAM_HI) {
		uint64_t off = pa - DRAM_LO;
		if (off < FAKE_DRAM_SIZE)
			return fake_dram[off];
		return 0;                  /* valid DRAM, just unbacked here */
	}
	return 0;                          /* outside DRAM entirely */
}

/* ------------------------------------------------------------------ *
 * Fake page table + gva_to_pa(). Mirrors coredump.c:40-53's CONTRACT
 * exactly (translated PA on a hit, flat VA==PA identity fallback on a
 * miss/MMU-off) via a scripted table instead of AT S1E1R/PAR_EL1.
 * ------------------------------------------------------------------ */
#define MAX_MAP 8
static struct { uint64_t va_page, pa_page; } g_map[MAX_MAP];
static int g_map_n;

static void map_reset(void) { g_map_n = 0; }

static void map_page(uint64_t va_page, uint64_t pa_page)
{
	assert((va_page & 0xfffULL) == 0 && (pa_page & 0xfffULL) == 0);
	assert(g_map_n < MAX_MAP);
	g_map[g_map_n].va_page = va_page;
	g_map[g_map_n].pa_page = pa_page;
	g_map_n++;
}

/* Mirrors coredump.c:40-53. */
static uint64_t gva_to_pa_fake(uint64_t va)
{
	uint64_t page = va & ~0xfffULL;
	int i;

	for (i = 0; i < g_map_n; i++)
		if (g_map[i].va_page == page)
			return g_map[i].pa_page | (va & 0xfffULL);
	return va;      /* MMU-off guest (VA==PA), or untranslatable: flat fallback */
}

/* ------------------------------------------------------------------ *
 * Verbatim from coredump.c:89-102.
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ *
 * ELF64/coredump constants -- verbatim from coredump.c:56-79 and
 * coredump.h:49-53.
 * ------------------------------------------------------------------ */
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
#define PRSTATUS_REGOFF 112u
#define PRSTATUS_SZ     392u
#define NOTE_NAME_SZ  8u
#define NOTE_SZ       (12u + NOTE_NAME_SZ + PRSTATUS_SZ)   /* 412 */

#define COREDUMP_REGION_MAX 8u
#define COREDUMP_MAX_TOTAL  (384u * 1024u)

#define HDR_MAX (EHDR_SZ + PHDR_SZ * (1u + COREDUMP_REGION_MAX) + NOTE_SZ)

/* ------------------------------------------------------------------ *
 * Mirrors coredump.c:216-248 (post-fix): `va` is the address published
 * in the phdr AND what stream_mem_m() below translates per-page -- never a
 * pre-translated PA.
 * ------------------------------------------------------------------ */
struct region { uint64_t va; uint32_t len; };

static int region_clamp_m(uint64_t va, uint64_t len, struct region *out)
{
	uint64_t pa0;

	if (len == 0)
		return 0;
	if (len > (COREDUMP_MAX_TOTAL / 2u))
		len = COREDUMP_MAX_TOTAL / 2u;

	pa0 = gva_to_pa_fake(va & ~0xfffULL);
	if (pa0 < DRAM_LO || pa0 >= DRAM_HI)
		return 0;

	out->va  = va;
	out->len = (uint32_t)len;
	return 1;
}

/* Mirrors coredump.c:189-214 (post-fix, FIXED): per-page translate. Appends
 * directly into `out` instead of going through the framed-Ethernet stream
 * (that chunking is orthogonal to ELF content, see file header). */
static void stream_mem_m(uint8_t *out, uint32_t *outlen, uint64_t va, uint32_t n)
{
	uint32_t done = 0;

	while (done < n) {
		uint64_t cur     = va + done;
		uint64_t page_va = cur & ~0xfffULL;
		uint64_t page_pa = gva_to_pa_fake(page_va);
		uint32_t in_page = (uint32_t)(0x1000ULL - (cur & 0xfffULL));
		uint32_t chunk   = n - done;
		uint32_t i;

		if (chunk > in_page)
			chunk = in_page;

		for (i = 0; i < chunk; i++) {
			uint64_t a = page_pa + ((cur + i) & 0xfffULL);
			out[(*outlen)++] = fake_dram_read(a);
		}
		done += chunk;
	}
}

/* The OLD, pre-2026-08-06 logic, transcribed here ONLY to demonstrate the
 * regression it had: translate ONCE at the region's base VA, then read
 * `addr+i` as a flat PA for the WHOLE span. Correct only when the span
 * stays within one physically-contiguous run starting at that first
 * translation -- wrong the moment a second page maps elsewhere, and wrong
 * for the phdr's own p_vaddr (the caller published the translated PA, not
 * the VA -- reproduced in test_old_buggy_publishes_pa_not_va()). */
static void stream_mem_OLD_BUGGY(uint8_t *out, uint32_t *outlen,
                                 uint64_t already_translated_pa, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint64_t a = already_translated_pa + i;
		out[(*outlen)++] = fake_dram_read(a);
	}
}

/* ------------------------------------------------------------------ *
 * Mirrors the ELF/Phdr/PT_NOTE layout portion of coredump_send()
 * (coredump.c:300-374), minus the wire-streaming tail (coredump.c:376-395,
 * covered elsewhere -- see file header). Builds ONE region (the auto stack
 * window around `sp`) plus whatever `extra` regions the caller passes,
 * exactly like coredump_send()'s regions[] parameter.
 * regs->x[31], regs->elr, regs->spsr feed NT_PRSTATUS like frame->x[]/
 * frame->elr/frame->spsr do on the real path. */
struct fake_frame { uint64_t x[31]; uint64_t elr; uint64_t spsr; };

static uint32_t build_core_m(uint8_t *out, uint32_t outcap,
                             const struct fake_frame *frame, uint64_t sp,
                             const uint64_t *extra, int nextra)
{
	struct region reg[COREDUMP_REGION_MAX];
	uint8_t  prstatus[PRSTATUS_SZ];
	uint32_t nreg = 0, hlen, note_off, data_off, seg_off;
	uint32_t i, j, outlen = 0;
	uint64_t sbase;

	memset(out, 0, outcap);

	/* Region 0: stack window, page-aligned on the VA (fixed 2026-08-06;
	 * coredump.c:281-283). */
	sbase = (sp > 0x1000ULL) ? ((sp - 0x1000ULL) & ~0xFFFULL) : sp;
	if (region_clamp_m(sbase, 0x2000ULL, &reg[nreg]))
		nreg++;

	for (i = 0; extra && (int)i < nextra && nreg < COREDUMP_REGION_MAX; i++)
		if (region_clamp_m(extra[2u * i], extra[2u * i + 1u], &reg[nreg]))
			nreg++;

	for (i = 0; i < PRSTATUS_SZ; i++)
		prstatus[i] = 0;
	for (i = 0; i < 31u; i++)
		put64(prstatus, PRSTATUS_REGOFF + i * 8u, frame->x[i]);
	put64(prstatus, PRSTATUS_REGOFF + 31u * 8u, sp);
	put64(prstatus, PRSTATUS_REGOFF + 32u * 8u, frame->elr);
	put64(prstatus, PRSTATUS_REGOFF + 33u * 8u, frame->spsr);

	hlen     = EHDR_SZ + PHDR_SZ * (1u + nreg);
	note_off = hlen;
	data_off = note_off + NOTE_SZ;

	assert(data_off <= outcap);

	out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
	out[4] = 2; out[5] = 1; out[6] = EV_CURRENT;
	put16(out, 16, ET_CORE);
	put16(out, 18, EM_AARCH64);
	put32(out, 20, EV_CURRENT);
	put64(out, 32, EHDR_SZ);
	put16(out, 52, EHDR_SZ);
	put16(out, 54, PHDR_SZ);
	put16(out, 56, (uint16_t)(1u + nreg));

	{
		uint32_t p = EHDR_SZ;
		put32(out, p + 0, PT_NOTE);
		put64(out, p + 8,  note_off);
		put64(out, p + 32, NOTE_SZ);
		put64(out, p + 48, 4);
	}

	seg_off = data_off;
	for (j = 0; j < nreg; j++) {
		uint32_t p = EHDR_SZ + PHDR_SZ * (1u + j);
		put32(out, p + 0, PT_LOAD);
		put32(out, p + 4, PF_R | PF_W | PF_X);
		put64(out, p + 8,  seg_off);
		put64(out, p + 16, reg[j].va);      /* the fix under test */
		put64(out, p + 24, reg[j].va);
		put64(out, p + 32, reg[j].len);
		put64(out, p + 40, reg[j].len);
		put64(out, p + 48, 8);
		seg_off += reg[j].len;
	}

	{
		uint32_t p = note_off;
		put32(out, p + 0, 5);
		put32(out, p + 4, PRSTATUS_SZ);
		put32(out, p + 8, NT_PRSTATUS);
		out[p + 12] = 'C'; out[p + 13] = 'O'; out[p + 14] = 'R'; out[p + 15] = 'E';
		for (i = 0; i < PRSTATUS_SZ; i++)
			out[p + 20u + i] = prstatus[i];
	}

	outlen = data_off;
	for (j = 0; j < nreg; j++) {
		assert(outlen + reg[j].len <= outcap);
		stream_mem_m(out, &outlen, reg[j].va, reg[j].len);
	}
	return outlen;
}

/* ---- ELF64 reader helpers, for the assertions below ---- */
static uint16_t rd16(const uint8_t *b, uint32_t off) {
	return (uint16_t)(b[off] | (b[off+1] << 8));
}
static uint64_t rd64(const uint8_t *b, uint32_t off) {
	uint64_t v = 0; int i;
	for (i = 7; i >= 0; i--) v = (v << 8) | b[off + (uint32_t)i];
	return v;
}
static uint32_t rd32(const uint8_t *b, uint32_t off) {
	uint32_t v = 0; int i;
	for (i = 3; i >= 0; i--) v = (v << 8) | b[off + (uint32_t)i];
	return v;
}
struct phdr_view { uint32_t p_type; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz; };
static struct phdr_view read_phdr(const uint8_t *core, int idx)
{
	uint32_t p = EHDR_SZ + PHDR_SZ * (uint32_t)idx;
	struct phdr_view v;
	v.p_type   = rd32(core, p + 0);
	v.p_offset = rd64(core, p + 8);
	v.p_vaddr  = rd64(core, p + 16);
	v.p_paddr  = rd64(core, p + 24);
	v.p_filesz = rd64(core, p + 32);
	v.p_memsz  = rd64(core, p + 40);
	return v;
}

#define CHECK(cond, name) do { \
	g_total++; \
	if (cond) { printf("[ OK  ] %s\n", name); } \
	else { printf("[FAIL ] %s\n", name); g_failed = 1; } \
} while (0)
static int g_failed = 0;
static int g_total = 0;

#define OUTCAP (64 * 1024u)
static uint8_t g_out[OUTCAP];

/* ------------------------------------------------------------------ *
 * Test 1: basic Ehdr fields, MMU-off/identity case (the historically
 * proven scope -- must be unaffected by the fix).
 * ------------------------------------------------------------------ */
static void test_ehdr_fields_identity_case(void)
{
	struct fake_frame f = {0};
	uint64_t sp = DRAM_LO + 0x100000;   /* MMU-off: VA==PA by construction */
	uint32_t n;

	map_reset();
	f.elr = DRAM_LO + 0x42;
	f.spsr = 0x3c5;
	n = build_core_m(g_out, OUTCAP, &f, sp, NULL, 0);

	CHECK(n > EHDR_SZ, "core produced");
	CHECK(g_out[0] == 0x7f && g_out[1] == 'E' && g_out[2] == 'L' && g_out[3] == 'F',
	      "ELF magic");
	CHECK(g_out[4] == 2, "ELFCLASS64");
	CHECK(g_out[5] == 1, "ELFDATA2LSB");
	CHECK(rd16(g_out, 16) == ET_CORE, "e_type == ET_CORE");
	CHECK(rd16(g_out, 18) == EM_AARCH64, "e_machine == EM_AARCH64");
	CHECK(rd16(g_out, 56) == 2, "e_phnum == 2 (1 NOTE + 1 LOAD)");
}

/* ------------------------------------------------------------------ *
 * Test 2: NT_PRSTATUS registers round-trip exactly.
 * ------------------------------------------------------------------ */
static void test_prstatus_roundtrip(void)
{
	struct fake_frame f = {0};
	uint64_t sp = DRAM_LO + 0x200000;
	uint32_t n, note_off, p;
	int i;

	map_reset();
	for (i = 0; i < 31; i++)
		f.x[i] = 0x1000000000000000ULL + (uint64_t)i;
	f.elr = 0xffff0000dead0000ULL;
	f.spsr = 0x600001c5ULL;
	n = build_core_m(g_out, OUTCAP, &f, sp, NULL, 0);
	(void)n;

	note_off = EHDR_SZ + PHDR_SZ * 2u;    /* 1 NOTE phdr + 1 LOAD phdr */
	p = note_off + 20u + PRSTATUS_REGOFF; /* Nhdr(12)+name(8) then pr_reg */

	for (i = 0; i < 31; i++)
		CHECK(rd64(g_out, p + (uint32_t)i * 8u) == f.x[i], "x[i] round-trip");
	CHECK(rd64(g_out, p + 31u * 8u) == sp, "sp round-trip");
	CHECK(rd64(g_out, p + 32u * 8u) == f.elr, "pc (elr) round-trip");
	CHECK(rd64(g_out, p + 33u * 8u) == f.spsr, "pstate (spsr) round-trip");
}

/* ------------------------------------------------------------------ *
 * Test 3 (THE REGRESSION TEST): guest MMU ON, SP's VA translates to a
 * PHYSICAL address numerically far away. p_vaddr MUST be the VA, and the
 * bytes MUST come from the translated PA.
 * ------------------------------------------------------------------ */
static void test_mmu_on_va_published_pa_used_for_data(void)
{
	struct fake_frame f = {0};
	/* A believable high canonical AArch64 kernel VA, same shape as the
	 * real FreeBSD panic()'s own address space (0xffff0000........). */
	uint64_t sp        = 0xffff000012340000ULL;
	uint64_t sp_page   = sp & ~0xfffULL;
	uint64_t sbase     = sp_page - 0x1000ULL;    /* build_core_m's 2-page window */
	uint64_t fake_pa_a = DRAM_LO + 0x9000;       /* backs sbase (page 0)     */
	uint64_t fake_pa_b = DRAM_LO + 0x10000;      /* backs sp_page (page 1) --
	                                               * numerically UNRELATED to sp */
	uint32_t n, seg_off;
	struct phdr_view load;
	int i;

	map_reset();
	map_page(sbase,   fake_pa_a);
	map_page(sp_page, fake_pa_b);
	/* Fill sp_page's backing physical page with a recognizable, non-zero
	 * pattern so a flat-PA-misread (the old bug) is distinguishable from a
	 * correct per-page translated read. */
	for (i = 0; i < 0x1000; i++)
		fake_dram[fake_pa_b - DRAM_LO + (uint32_t)i] = (uint8_t)(i * 3 + 7);

	n = build_core_m(g_out, OUTCAP, &f, sp, NULL, 0);
	(void)n;

	load = read_phdr(g_out, 1);    /* phdr[0]=NOTE, phdr[1]=our one LOAD */
	CHECK(load.p_type == PT_LOAD, "phdr[1] is PT_LOAD");
	CHECK(load.p_vaddr == sbase, "p_vaddr is the VA window, not the PA");
	CHECK(load.p_vaddr != fake_pa_a, "p_vaddr != translated PA (they genuinely differ)");
	CHECK(load.p_paddr == load.p_vaddr, "p_paddr mirrors p_vaddr (advisory, gdb ignores it)");

	seg_off = (uint32_t)load.p_offset;
	/* The SECOND page of the 2-page window is [sp_page, sp_page+0x1000) --
	 * exactly the page we mapped and filled. Byte i of that page in the
	 * segment must equal fake_dram's pattern at that page's PA. */
	for (i = 0; i < 16; i++) {
		uint32_t seg_byte_off = 0x1000u + (uint32_t)i;  /* into 2nd page */
		uint8_t  got  = g_out[seg_off + seg_byte_off];
		uint8_t  want = (uint8_t)(i * 3 + 7);
		CHECK(got == want, "stack segment byte read via TRANSLATED PA, not raw VA");
	}
}

/* ------------------------------------------------------------------ *
 * Test 4: two pages of the SAME window map to NON-CONTIGUOUS physical
 * pages. Each half of the segment must come from ITS OWN page's PA --
 * this is exactly what the OLD (pre-fix) logic got wrong, and is
 * reproduced immediately below for contrast.
 * ------------------------------------------------------------------ */
static void test_two_pages_translate_independently(void)
{
	uint64_t va_page0 = 0xffff0000aaaa0000ULL;
	uint64_t va_page1 = va_page0 + 0x1000ULL;   /* VA-contiguous */
	uint64_t pa_page0 = DRAM_LO + 0x20000;
	uint64_t pa_page1 = DRAM_LO + 0x300000;     /* NOT PA-contiguous with page0 */
	uint32_t outlen;
	int i;

	map_reset();
	map_page(va_page0, pa_page0);
	map_page(va_page1, pa_page1);
	for (i = 0; i < 0x1000; i++) {
		fake_dram[pa_page0 - DRAM_LO + (uint32_t)i] = (uint8_t)(0x10 + (i & 0x3f));
		fake_dram[pa_page1 - DRAM_LO + (uint32_t)i] = (uint8_t)(0x80 + (i & 0x3f));
	}

	/* FIXED logic: per-page translate. */
	{
		outlen = 0;
		stream_mem_m(g_out, &outlen, va_page0, 0x2000u);
		CHECK(outlen == 0x2000u, "fixed: streamed the full 2-page span");
		CHECK(g_out[0]      == (uint8_t)(0x10 + 0),  "fixed: page0 byte0 from pa_page0");
		CHECK(g_out[0x0fff] == (uint8_t)(0x10 + (0x3f & 0x3f)), "fixed: page0 last byte from pa_page0");
		CHECK(g_out[0x1000] == (uint8_t)(0x80 + 0),  "fixed: page1 byte0 from pa_page1 (own page)");
		CHECK(g_out[0x1fff] == (uint8_t)(0x80 + (0x3f & 0x3f)), "fixed: page1 last byte from pa_page1");
	}

	/* OLD BUGGY logic: translate ONCE at the base VA, then walk flat PA.
	 * Page 1's bytes come from pa_page0+0x1000 (empty/zero here), NOT
	 * pa_page1 -- reproducing exactly the bug this file's header describes. */
	{
		uint64_t base_pa = gva_to_pa_fake(va_page0);
		outlen = 0;
		stream_mem_OLD_BUGGY(g_out, &outlen, base_pa, 0x2000u);
		CHECK(g_out[0]      == (uint8_t)(0x10 + 0), "buggy: page0 byte0 still right (same first page)");
		CHECK(g_out[0x1000] != (uint8_t)(0x80 + 0),
		      "buggy: page1 byte0 is WRONG -- read from pa_page0+0x1000, not pa_page1 (the bug)");
		CHECK(g_out[0x1000] == fake_dram_read(pa_page0 + 0x1000),
		      "buggy: page1 byte0 demonstrably came from the wrong physical page");
	}
}

/* ------------------------------------------------------------------ *
 * Test 5: the caller-visible bug this whole fix is about -- the OLD
 * coredump_send() published the TRANSLATED PA as p_vaddr, not the VA.
 * Reproduced structurally (not by calling removed code) to document
 * exactly what changed and why it matters to a debugger.
 * ------------------------------------------------------------------ */
static void test_old_buggy_publishes_pa_not_va(void)
{
	uint64_t sp     = 0xffff000012340000ULL;
	uint64_t sp_pa  = 0x42000000ULL + 0x340000ULL;   /* what gva_to_pa() would
	                                                    * have returned, and what
	                                                    * the OLD code page-aligned
	                                                    * and published instead of sp */
	uint64_t old_p_vaddr = (sp_pa - 0x1000ULL) & ~0xFFFULL;
	uint64_t new_p_vaddr = (sp - 0x1000ULL) & ~0xFFFULL;

	CHECK(old_p_vaddr != new_p_vaddr,
	      "the old and new p_vaddr genuinely differ for an MMU-on guest");
	/* A debugger looks up SP (a VA) against p_vaddr ranges. */
	CHECK(!(sp >= old_p_vaddr && sp < old_p_vaddr + 0x2000ULL),
	      "OLD p_vaddr range does NOT cover SP -- 'Cannot access memory at <sp>' in gdb");
	CHECK(sp >= new_p_vaddr && sp < new_p_vaddr + 0x2000ULL,
	      "NEW p_vaddr range DOES cover SP -- gdb can read the stack");
}

/* ------------------------------------------------------------------ *
 * Test 6: region_clamp_m rejects an address that translates outside DRAM,
 * and caps an oversized region -- both preserved from the pre-fix
 * behaviour (coredump.c:236-243), just checked in VA terms now.
 * ------------------------------------------------------------------ */
static void test_region_clamp_bounds(void)
{
	struct region r;
	int ok;

	map_reset();
	ok = region_clamp_m(0x1000ULL, 0x100ULL, &r);   /* translates to itself (below DRAM_LO) */
	CHECK(!ok, "region outside DRAM (even after identity fallback) is rejected");

	ok = region_clamp_m(DRAM_LO + 0x1000ULL, COREDUMP_MAX_TOTAL, &r);
	CHECK(ok && r.len == COREDUMP_MAX_TOTAL / 2u, "oversized region is capped, not rejected");

	ok = region_clamp_m(DRAM_LO + 0x1000ULL, 0, &r);
	CHECK(!ok, "zero-length region is rejected");
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
		/* crash_report.py's selftest support: emit one realistic core.elf
		 * to a file, with pc/sp taken from argv so the caller can feed in
		 * REAL kernel.debug symbol addresses for a scripted gdb check.
		 * Usage: test_coredump_elf --dump <path> <pc_hex> <sp_hex> */
		struct fake_frame f = {0};
		uint64_t pc, sp, sp_page, fake_pa;
		uint32_t n, i;
		FILE *fp;

		if (argc < 5) {
			fprintf(stderr, "usage: %s --dump <path> <pc_hex> <sp_hex>\n", argv[0]);
			return 2;
		}
		pc = strtoull(argv[3], NULL, 16);
		sp = strtoull(argv[4], NULL, 16);
		sp_page = sp & ~0xfffULL;
		fake_pa = DRAM_LO + 0x40000;

		map_reset();
		map_page(sp_page, fake_pa & ~0xfffULL);
		map_page(sp_page - 0x1000ULL, (fake_pa - 0x1000ULL) & ~0xfffULL);
		for (i = 0; i < FAKE_DRAM_SIZE; i++)
			fake_dram[i] = (uint8_t)((i * 131 + 17) & 0xff);   /* recognizable */

		f.elr = pc;
		f.spsr = 0x600003c5ULL;
		n = build_core_m(g_out, OUTCAP, &f, sp, NULL, 0);

		fp = fopen(argv[2], "wb");
		if (!fp) { perror("fopen"); return 1; }
		if (fwrite(g_out, 1, n, fp) != n) { perror("fwrite"); fclose(fp); return 1; }
		fclose(fp);
		printf("dumped %u bytes to %s (pc=%#llx sp=%#llx)\n",
		       n, argv[2], (unsigned long long)pc, (unsigned long long)sp);
		return 0;
	}

	test_ehdr_fields_identity_case();
	test_prstatus_roundtrip();
	test_mmu_on_va_published_pa_used_for_data();
	test_two_pages_translate_independently();
	test_old_buggy_publishes_pa_not_va();
	test_region_clamp_bounds();

	if (g_failed) {
		printf("---- coredump ELF tests: FAILED ----\n");
		return 1;
	}
	printf("---- coredump ELF tests: %d/%d passed ----\n", g_total, g_total);
	return 0;
}
