/* SPDX-License-Identifier: BSD-2-Clause */

/* test_zload2_parsing.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the pure ELF-parsing / segment-placement logic in zload2.c
 * (dual-guest milestone: the Zephyr-on-CPU3 loader).
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "zload2.c"` WITH STUBS
 * ============================================================================
 * Same reasoning as test_kload_modinfo.c (read in full before writing this
 * file, used as the direct template): zload2.c's public entry point
 * (zload2_parse_and_place()) is pure address/index/bounds arithmetic with NO
 * real hardware access, but it is not ISOLATED pure logic in the source --
 * it calls zload2_cache_clean_inval(), which is raw ARMv8 inline asm
 * ("dc cvac"/"ic ivau"/"dsb"/"isb") that plain x86_64 gcc rejects outright.
 * So this file hand-transcribes ONLY the pure parsing/placement functions,
 * verbatim in control flow, with an exact zload2.c/zload2.h line-number-free
 * citation on each one (this mirror was written and last checked against
 * zload2.c as committed alongside it in the same change -- see that file for
 * the authoritative source). The one thing intentionally NOT mirrored is the
 * cache-maintenance call itself (a side effect that changes neither the
 * parsed struct fields nor the bytes actually written) -- mirrored functions
 * memcpy/memset exactly as zload2.c does (this test's own g_mem stand-in for
 * "physical DRAM", same technique test_kload_modinfo.c / test_vblk_ring.c use).
 *
 * WHAT THIS FILE IS AIMED AT: zload2.c is NEW code (no live-hardware history
 * to regression-guard yet, unlike kload.c's MODINFOMD story), so these tests
 * instead pin down the CONTRACT zload2.h documents and that zguest_cpu3.c's
 * safety depends on:
 *   1. the identity-copy degenerate case (pa_base == zbase) that
 *      main_zephyr.c's header comment says the general relocation formula
 *      collapses to for the CURRENT zephyr-guest bpi_m64_hv board image;
 *   2. genuine relocation (pa_base != zbase) for a FUTURE image relinked at
 *      a different base, since the formula is the same one kload.c uses for
 *      FreeBSD's KVA-vs-PA split, just simpler (no modinfo/DTB/symtab);
 *   3. the SAFETY BOUNDS CHECK that has no equivalent in kload.c at all
 *      (kload.c places FreeBSD anywhere in its own DRAM gigabyte with no
 *      independent ceiling) -- zload2.h's whole reason to exist is feeding
 *      an ISOLATED 32 MiB slice, so a segment that would land outside
 *      [pa_base, pa_base+ZLOAD2_MAX_IMAGE_SIZE) must be rejected, and
 *      rejected ATOMICALLY (nothing copied at all, not a partial copy) --
 *      this is the single most safety-critical property in this file and is
 *      the one most worth a regression test with no board involved.
 *
 * Build: gcc -o test_zload2_parsing test_zload2_parsing.c && ./test_zload2_parsing
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mock "physical DRAM": a pa/elf_addr is a plain byte offset into this flat
 * array, exactly like test_kload_modinfo.c's g_mem -- zload2.c treats every
 * address it touches (elf_addr, pa_base, resolved dest) as a plain EL2
 * pointer under the identity map, so a byte-array stand-in is faithful.
 * ------------------------------------------------------------------ */
#define MEM_SIZE (48u << 20)  /* 48 MiB: room for fixtures + placement targets,
                                * including the exactly-32-MiB boundary test
                                * below (ZLOAD2_MAX_IMAGE_SIZE itself). Note:
                                * unlike test_kload_modinfo.c's kernbase
                                * (a pure KVA, never used as a g_mem index),
                                * every address a test below actually
                                * DEREFERENCES through g_mem[] here is kept
                                * small/mock-realistic (well under this
                                * array's size) even where the comment names a
                                * real production constant for context -- an
                                * address used ONLY in delta arithmetic (never
                                * as an index) is free to be a realistic-sized
                                * value like 0xBE000000. */
static uint8_t g_mem[MEM_SIZE];

static void reset_mem(void)
{
	memset(g_mem, 0xAA, sizeof(g_mem));   /* poison, catch stray reads */
}

/* ------------------------------------------------------------------ *
 * Mirrored types/constants -- verbatim field layout + values from
 * zload2.c's local ELF64/AArch64 definitions.
 * ------------------------------------------------------------------ */
struct elf64_ehdr {
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
};

struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

#define ZLOAD2_PT_LOAD     1u
#define ZLOAD2_ELFCLASS64  2u
#define ZLOAD2_ELFDATA2LSB 1u
#define ZLOAD2_EM_AARCH64  183u
#define ZLOAD2_ET_EXEC     2u
#define ZLOAD2_ET_DYN      3u

#define ZLOAD2_MAX_PHDR 8u                    /* zload2.c */
#define ZLOAD2_MAX_IMAGE_SIZE 0x02000000UL    /* zload2.h, == ZSTAGE2_DRAM_SIZE */

/* Mirrors zload2.c's file-scope `zls` parser/placer state (only the fields
 * the mirrored functions below read/write). */
struct zload2_seg {
	uint64_t p_vaddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_offset;
};

static struct {
	int      valid;
	uint64_t elf_addr;
	uint64_t e_entry;
	uint64_t zbase;
	int      n_seg;
	struct zload2_seg seg[ZLOAD2_MAX_PHDR];

	uint64_t pa_base;
	uint64_t entry_pa;
} zls;

static void reset_zls(void)
{
	memset(&zls, 0, sizeof(zls));
}

/* ------------------------------------------------------------------ *
 * zload2_parse_elf() -- mirrors zload2.c's static function of the same
 * name verbatim in control flow, minus the zld2_bc() breadcrumb calls
 * (pure side-channel telemetry, cannot affect parsed state).
 * ------------------------------------------------------------------ */
static int
zload2_parse_elf(uint64_t elf_addr)
{
	const struct elf64_ehdr *eh = (const struct elf64_ehdr *)&g_mem[elf_addr];
	const struct elf64_phdr *ph;
	uint32_t i;

	zls.valid = 0;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return 0;
	if (eh->e_ident[4] != ZLOAD2_ELFCLASS64)
		return 0;
	if (eh->e_ident[5] != ZLOAD2_ELFDATA2LSB)
		return 0;
	if (eh->e_machine != ZLOAD2_EM_AARCH64)
		return 0;
	if (eh->e_type != ZLOAD2_ET_EXEC && eh->e_type != ZLOAD2_ET_DYN)
		return 0;
	if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(struct elf64_phdr))
		return 0;
	if (eh->e_phnum > 64u)
		return 0;

	zls.elf_addr = elf_addr;
	zls.e_entry = eh->e_entry;
	zls.n_seg = 0;
	zls.zbase = ~0ull;

	ph = (const struct elf64_phdr *)&g_mem[elf_addr + eh->e_phoff];
	for (i = 0; i < eh->e_phnum; i++) {
		struct zload2_seg *s;

		if (ph[i].p_type != ZLOAD2_PT_LOAD)
			continue;
		if (zls.n_seg >= (int)ZLOAD2_MAX_PHDR)
			return 0;

		s = &zls.seg[zls.n_seg++];
		s->p_vaddr  = ph[i].p_vaddr;
		s->p_filesz = ph[i].p_filesz;
		s->p_memsz  = ph[i].p_memsz;
		s->p_offset = ph[i].p_offset;

		if (ph[i].p_vaddr < zls.zbase)
			zls.zbase = ph[i].p_vaddr;
	}
	if (zls.n_seg == 0)
		return 0;

	zls.valid = 1;
	return 1;
}

/* ------------------------------------------------------------------ *
 * zload2_parse_and_place() -- mirrors zload2.c's public function verbatim
 * in control flow, minus zld2_bc() breadcrumb calls and
 * zload2_cache_clean_inval() (asm, see this file's header comment).
 * ------------------------------------------------------------------ */
static int
zload2_parse_and_place(uint64_t elf_addr, uint64_t pa_base,
                        uint64_t *entry_pa_out)
{
	uint64_t total_bytes = 0;
	int i;

	if (!zload2_parse_elf(elf_addr) || pa_base == 0)
		return 0;

	if (zls.n_seg <= 0 || zls.n_seg > (int)ZLOAD2_MAX_PHDR)
		return 0;

	for (i = 0; i < zls.n_seg; i++) {
		struct zload2_seg *s = &zls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - zls.zbase);
		uint64_t dest_end = dest + s->p_memsz;

		if (dest < pa_base || dest_end < dest ||
		    dest_end > pa_base + ZLOAD2_MAX_IMAGE_SIZE)
			return 0;
	}

	for (i = 0; i < zls.n_seg; i++) {
		struct zload2_seg *s = &zls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - zls.zbase);

		if (s->p_filesz)
			memcpy(&g_mem[dest], &g_mem[elf_addr + s->p_offset],
			       (size_t)s->p_filesz);
		if (s->p_memsz > s->p_filesz)
			memset(&g_mem[dest + s->p_filesz], 0,
			       (size_t)(s->p_memsz - s->p_filesz));

		total_bytes += s->p_memsz;
	}

	zls.pa_base = pa_base;
	zls.entry_pa = pa_base + (zls.e_entry - zls.zbase);

	if (entry_pa_out)
		*entry_pa_out = zls.entry_pa;
	return 1;
}

/* ==================================================================== *
 * Fixture builder -- same technique as test_kload_modinfo.c's
 * build_elf_fixture(): writes a synthetic ELF64/AArch64 EXEC directly into
 * g_mem at `elf_addr`.
 * ==================================================================== */
struct fixture_seg { uint64_t vaddr; uint32_t filesz; uint32_t memsz; uint8_t fill; };

static void
build_elf_fixture(uint64_t elf_addr, uint64_t entry,
                   const struct fixture_seg *segs, int n_segs)
{
	struct elf64_ehdr eh;
	memset(&eh, 0, sizeof(eh));
	eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
	eh.e_ident[4] = ZLOAD2_ELFCLASS64;
	eh.e_ident[5] = ZLOAD2_ELFDATA2LSB;
	eh.e_type = ZLOAD2_ET_EXEC;
	eh.e_machine = ZLOAD2_EM_AARCH64;
	eh.e_entry = entry;
	eh.e_phoff = sizeof(eh);
	eh.e_phentsize = sizeof(struct elf64_phdr);
	eh.e_phnum = (uint16_t)n_segs;

	memcpy(&g_mem[elf_addr], &eh, sizeof(eh));

	uint64_t file_cursor = sizeof(eh) + (uint64_t)n_segs * sizeof(struct elf64_phdr);
	for (int i = 0; i < n_segs; i++) {
		struct elf64_phdr ph;
		memset(&ph, 0, sizeof(ph));
		ph.p_type = ZLOAD2_PT_LOAD;
		ph.p_offset = file_cursor;
		ph.p_vaddr = segs[i].vaddr;
		ph.p_paddr = segs[i].vaddr;   /* zload2.c never trusts this field */
		ph.p_filesz = segs[i].filesz;
		ph.p_memsz = segs[i].memsz;
		memcpy(&g_mem[elf_addr + sizeof(eh) + (uint64_t)i * sizeof(ph)], &ph, sizeof(ph));

		memset(&g_mem[elf_addr + file_cursor], segs[i].fill, segs[i].filesz);
		file_cursor += segs[i].filesz;
	}
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* Degenerate identity-copy case (main_zephyr.c's documented Zephyr
 * placement model): pa_base == the ELF's own lowest p_vaddr -> dest ==
 * p_vaddr for every segment, i.e. a plain copy with no relocation. */
static void test_identity_copy_when_pa_base_equals_zbase(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x200000;
	/* Stand-in for the bpi_m64_hv board's real link address (0x51000000) --
	 * kept small here purely so g_mem[zbase+...] below stays in bounds; the
	 * formula under test does not care about the literal magnitude. */
	uint64_t zbase = 0x50000ull;
	struct fixture_seg segs[1] = { { zbase, 0x1000, 0x1000, 0x77 } };
	build_elf_fixture(elf_addr, zbase + 0x0c, segs, 1);

	uint64_t entry_pa = 0;
	int rc = zload2_parse_and_place(elf_addr, zbase, &entry_pa);
	assert(rc == 1);
	assert(zls.zbase == zbase);
	assert(zls.seg[0].p_vaddr == zbase);

	/* Identity: the destination IS the ELF's own p_vaddr. */
	uint64_t dest = zbase + (zls.seg[0].p_vaddr - zls.zbase);
	assert(dest == zbase);
	assert(g_mem[zbase] == 0x77 && g_mem[zbase + 0xFFF] == 0x77);
	assert(entry_pa == zbase + 0x0c);
}

/* Genuine relocation: pa_base != zbase -> dest = pa_base + (p_vaddr -
 * zbase) for every segment, same formula kload.c uses for FreeBSD's
 * KVA-vs-PA split, just without any modinfo/DTB bookkeeping on top. */
static void test_relocation_when_pa_base_differs_from_zbase(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x200000;
	uint64_t zbase = 0x1000000ull;    /* an arbitrary link address */
	uint64_t pa_base = 0x600000ull;   /* placed somewhere else entirely */
	struct fixture_seg segs[2] = {
		{ zbase,          0x800, 0x1000, 0x11 },   /* 0x800 bss tail */
		{ zbase + 0x2000, 0x400, 0x400,  0x22 },
	};
	build_elf_fixture(elf_addr, zbase + 0x10, segs, 2);

	uint64_t entry_pa = 0;
	int rc = zload2_parse_and_place(elf_addr, pa_base, &entry_pa);
	assert(rc == 1);

	uint64_t dest0 = pa_base;                 /* delta 0: seg0 IS zbase */
	uint64_t dest1 = pa_base + 0x2000;         /* delta 0x2000 */

	assert(g_mem[dest0] == 0x11 && g_mem[dest0 + 0x7FF] == 0x11);
	/* bss tail (filesz=0x800, memsz=0x1000) zeroed, not left as fixture
	 * fill or 0xAA poison. */
	assert(g_mem[dest0 + 0x800] == 0);
	assert(g_mem[dest0 + 0xFFF] == 0);

	assert(g_mem[dest1] == 0x22 && g_mem[dest1 + 0x3FF] == 0x22);

	assert(entry_pa == pa_base + 0x10);
}

/* Multi-segment placement is fully generic, not hard-coded to one segment
 * (zguest_cpu3.h documents that a future Zephyr image is not assumed to
 * have exactly one PT_LOAD, even though today's does). */
static void test_multi_segment_placement(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x300000;
	/* Stand-in for stage2_zephyr.h's own slice base (0xBE000000) -- kept
	 * small so the direct g_mem[zbase+...] indexing below stays in bounds. */
	uint64_t zbase = 0x60000ull;
	struct fixture_seg segs[3] = {
		{ zbase,          0x1000, 0x1000, 0xA1 },
		{ zbase + 0x1000, 0x1000, 0x1000, 0xA2 },
		{ zbase + 0x2000, 0x0200, 0x1000, 0xA3 },
	};
	build_elf_fixture(elf_addr, zbase + 0x2004, segs, 3);

	uint64_t entry_pa = 0;
	int rc = zload2_parse_and_place(elf_addr, zbase, &entry_pa);
	assert(rc == 1);
	assert(zls.n_seg == 3);
	assert(g_mem[zbase + 0x0000] == 0xA1);
	assert(g_mem[zbase + 0x1000] == 0xA2);
	assert(g_mem[zbase + 0x2000] == 0xA3);
	assert(g_mem[zbase + 0x2200] == 0);   /* seg2's bss tail zeroed */
	assert(entry_pa == zbase + 0x2004);
}

static void test_rejects_bad_magic(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x400000;
	struct fixture_seg segs[1] = { { 0x1000, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0x1000, segs, 1);
	g_mem[elf_addr + 1] = 'X';   /* corrupt "ELF" -> "EXF" */

	uint64_t entry_pa = 0;
	assert(zload2_parse_and_place(elf_addr, 0x1000, &entry_pa) == 0);
	assert(zls.valid == 0);
}

static void test_rejects_wrong_machine(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x410000;
	struct fixture_seg segs[1] = { { 0x1000, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0x1000, segs, 1);
	uint16_t wrong_machine = 0x3e;   /* EM_X86_64, not EM_AARCH64 */
	memcpy(&g_mem[elf_addr + 18], &wrong_machine, 2);

	uint64_t entry_pa = 0;
	assert(zload2_parse_and_place(elf_addr, 0x1000, &entry_pa) == 0);
}

static void test_rejects_zero_phnum(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x420000;
	struct fixture_seg segs[1] = { { 0x1000, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0x1000, segs, 1);
	uint16_t zero = 0;
	memcpy(&g_mem[elf_addr + 54], &zero, 2);   /* e_phnum offset */

	uint64_t entry_pa = 0;
	assert(zload2_parse_and_place(elf_addr, 0x1000, &entry_pa) == 0);
}

static void test_rejects_too_many_segments(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x430000;
	struct fixture_seg segs[ZLOAD2_MAX_PHDR + 1];
	for (unsigned i = 0; i < ZLOAD2_MAX_PHDR + 1; i++) {
		segs[i].vaddr = 0x1000ull + (uint64_t)i * 0x1000u;
		segs[i].filesz = 0x10;
		segs[i].memsz = 0x10;
		segs[i].fill = (uint8_t)i;
	}
	build_elf_fixture(elf_addr, 0x1000, segs, (int)(ZLOAD2_MAX_PHDR + 1));

	uint64_t entry_pa = 0;
	assert(zload2_parse_and_place(elf_addr, 0x1000, &entry_pa) == 0);
	assert(zls.valid == 0);
}

static void test_rejects_pa_base_zero(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x440000;
	struct fixture_seg segs[1] = { { 0x1000, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0x1000, segs, 1);

	uint64_t entry_pa = 0;
	assert(zload2_parse_and_place(elf_addr, 0, &entry_pa) == 0);
}

/* THE SAFETY-CRITICAL TEST: a segment whose resolved destination would land
 * outside [pa_base, pa_base+ZLOAD2_MAX_IMAGE_SIZE) is rejected, and
 * rejected BEFORE any byte is copied -- not a partial placement. This is
 * the property zguest_cpu3.c's caller depends on (see zload2.h): a
 * malformed/oversized image must never partially land in Zephyr's 32 MiB
 * slice and then get "entered" anyway. */
static void test_rejects_segment_outside_safety_window_atomically(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x450000;
	uint64_t pa_base = 0x600000ull;
	/* seg0 fits; seg1's delta (ZLOAD2_MAX_IMAGE_SIZE) pushes its destination
	 * exactly one byte past the window. zbase itself is only ever used in
	 * delta arithmetic (never as a direct g_mem index -- only pa_base and
	 * pa_base-derived dest0 are), so its magnitude is unconstrained by
	 * MEM_SIZE. */
	uint64_t zbase = 0x2000000ull;
	struct fixture_seg segs[2] = {
		{ zbase,                              0x100, 0x100, 0x55 },
		{ zbase + ZLOAD2_MAX_IMAGE_SIZE, 0x100, 0x100, 0x66 },
	};
	build_elf_fixture(elf_addr, zbase, segs, 2);

	/* Sentinel: pre-fill the WOULD-BE destination of seg0 with a distinct
	 * poison value so we can prove it was never written (atomic rejection),
	 * not just that the return code is 0. */
	g_mem[pa_base] = 0xEE;

	uint64_t entry_pa = 0xdeadbeefull;
	int rc = zload2_parse_and_place(elf_addr, pa_base, &entry_pa);
	assert(rc == 0);
	/* Nothing copied -- seg0's destination still holds the sentinel, not
	 * the fixture's 0x55 fill. */
	assert(g_mem[pa_base] == 0xEE);
	/* Output parameter untouched on failure (zload2.h's documented
	 * contract: "leaves *entry_pa_out untouched" on failure). */
	assert(entry_pa == 0xdeadbeefull);
}

/* A segment that fits EXACTLY at the top edge of the safety window (dest +
 * memsz == pa_base + ZLOAD2_MAX_IMAGE_SIZE) is accepted -- the check is a
 * half-open range, not off-by-one in the conservative direction. */
static void test_accepts_segment_exactly_at_window_edge(void)
{
	reset_mem(); reset_zls();
	uint64_t elf_addr = 0x480000;
	uint64_t pa_base = 0x1000000ull;   /* leaves 32 MiB of headroom below MEM_SIZE */
	uint64_t zbase = pa_base;
	uint32_t memsz = (uint32_t)ZLOAD2_MAX_IMAGE_SIZE;   /* fills the whole window */
	struct fixture_seg segs[1] = { { zbase, 0x100, memsz, 0x33 } };
	build_elf_fixture(elf_addr, zbase, segs, 1);

	uint64_t entry_pa = 0;
	int rc = zload2_parse_and_place(elf_addr, pa_base, &entry_pa);
	assert(rc == 1);
	assert(g_mem[pa_base] == 0x33);
}

/* ==================================================================== *
 * Driver
 * ==================================================================== */
struct zl_test { const char *name; void (*fn)(void); };

static struct zl_test k_tests[] = {
	{ "identity_copy_when_pa_base_equals_zbase",        test_identity_copy_when_pa_base_equals_zbase },
	{ "relocation_when_pa_base_differs_from_zbase",     test_relocation_when_pa_base_differs_from_zbase },
	{ "multi_segment_placement",                        test_multi_segment_placement },
	{ "rejects_bad_magic",                               test_rejects_bad_magic },
	{ "rejects_wrong_machine",                           test_rejects_wrong_machine },
	{ "rejects_zero_phnum",                              test_rejects_zero_phnum },
	{ "rejects_too_many_segments",                       test_rejects_too_many_segments },
	{ "rejects_pa_base_zero",                            test_rejects_pa_base_zero },
	{ "rejects_segment_outside_safety_window_atomically", test_rejects_segment_outside_safety_window_atomically },
	{ "accepts_segment_exactly_at_window_edge",          test_accepts_segment_exactly_at_window_edge },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- zload2 parsing tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
