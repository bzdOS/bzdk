/* test_stage2_tables.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the stage-2 page-table-builder logic in stage2.c.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "stage2.c"` WITH STUBS
 * ============================================================================
 * stage2.c was read in full before writing this file. Unlike vblk_emmc.c, the
 * bulk of it genuinely IS separable: the descriptor-bit-construction helpers
 * (stage2_block_desc/stage2_table_desc/stage2_page_desc/stage2_l2_block_desc)
 * and the table-topology builders (stage2_build_mmio_tables(),
 * stage2_unmap_guest_vector()'s L1/L2/L3 split, the index-derivation #defines)
 * are pure bit arithmetic over plain uint64_t arrays with no I/O at all. But
 * the FILE as a whole still is not includable as-is on x86_64:
 *   - stg2_bc()                      "dc civac, %0\n\tdsb sy"   (line 61)
 *   - read/write_vtcr_el2/vttbr_el2/hcr_el2   "mrs"/"msr ..., isb" (482-530)
 *   - stage2_tlb_flush()              "tlbi vmalls12e1; dsb ish; isb" (545)
 *   - stage2_at_s12e1r()              "at s12e1r, ...; isb; mrs par_el1" (700-707)
 *   - read_id_aa64mmfr0_el1()          "mrs %0, id_aa64mmfr0_el1"   (486)
 * every one of these is a raw ARMv8 system-register/AT-instruction mnemonic
 * the assembler rejects on x86_64, and they are threaded through
 * stage2_init()/stage2_enable()/stage2_selftest() alongside the table-building
 * calls, not confined to one helper. Stubbing them all away via macros would
 * again mean neutering register I/O throughout the file (the PARange read
 * that picks VTCR.PS, the enable/disable read-modify-write, the TLB flush) —
 * more invasive than "cache maintenance ops", and it would still require
 * editing stage2.c to make the system-register accessors overridable, which
 * is forbidden (read-only reference).
 *
 * So: this file hand-transcribes the pure descriptor-bit and table-topology
 * functions verbatim, each with an exact stage2.c line-number citation, and
 * mirrors the *derivation arithmetic* (VEC_L1_IDX/VEC_L2_IDX/VEC_L3_IDX,
 * UART_L2_IDX/UART_L3_IDX) rather than the register plumbing around it.
 *
 * Build: gcc -o test_stage2_tables test_stage2_tables.c && ./test_stage2_tables
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Constants mirrored from stage2.h (identity-map region + probe addresses).
 * ------------------------------------------------------------------ */
#define STAGE2_MMIO_BASE   0x00000000UL
#define STAGE2_MMIO_SIZE   0x40000000UL
#define STAGE2_DRAM_BASE   0x40000000UL
#define STAGE2_DRAM_SIZE   0x40000000UL
#define UART0_BASE         0x01C28000UL
#define GUEST_VECTOR_IPA   0x46927000UL
#define STAGE2_SELFTEST_IPA 0x42000000UL

/* VBLK_MMIO_BASE mirrored from vblk_emmc.h:70 (the trapped virtio-mmio
 * window stage2_build_mmio_tables() must leave invalid). */
#define VBLK_MMIO_BASE     0x0A000000UL

/* ------------------------------------------------------------------ *
 * Mirrors stage2.c:81-88 table-geometry constants.
 * ------------------------------------------------------------------ */
#define STAGE2_L1_ENTRIES   512u
#define STAGE2_L1_TABLES    2u
#define STAGE2_BLOCK_SIZE   0x40000000UL
#define STAGE2_BLOCK_SHIFT  30

/* NOTE: the __attribute__((aligned(...))) here mirrors stage2.c:86-87/
 * 200-203/344-347 exactly and is NOT cosmetic — stage2_table_desc() masks
 * the next-table pointer to STAGE2_TABLE_ADDR_MASK (bits[47:12]), i.e. it
 * silently assumes/requires every table is naturally aligned to its own
 * size. Without this alignment a plain `static uint64_t foo[512]` only gets
 * the compiler's default 8-byte alignment, the low 12 bits of &foo[0] are
 * nonzero, and stage2_table_desc() truncates them away — corrupting the
 * table-descriptor's output address. (Caught by actually running this
 * suite: omitting these attributes here reproduces exactly that failure.) */
static uint64_t stage2_l1[STAGE2_L1_TABLES][STAGE2_L1_ENTRIES]
	__attribute__((aligned(STAGE2_L1_TABLES * STAGE2_L1_ENTRIES * 8u)));

/* Mirrors stage2.c:121-134 descriptor bit-field constants. */
#define S2_DESC_VALID_BLOCK   0x1ull
#define S2_MEMATTR_DEVICE_nGnRE  0x1u
#define S2_MEMATTR_NORMAL_WB     0xFu
#define S2AP_RW   0x3u
#define S2_SH_OUTER   0x2u
#define S2_SH_INNER   0x3u
#define S2_AF_BIT     (1ull << 10)
#define S2_XN_BIT     (1ull << 54)
#define STAGE2_BLOCK_ADDR_MASK  (~(uint64_t)(STAGE2_BLOCK_SIZE - 1u))

/* Mirrors stage2.c:136-148 stage2_block_desc() verbatim. */
static uint64_t
stage2_block_desc(uint64_t pa, unsigned memattr, unsigned sh, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_BLOCK;
	d |= (uint64_t)(memattr & 0xFu) << 2;
	d |= (uint64_t)(S2AP_RW)        << 6;
	d |= (uint64_t)(sh & 0x3u)      << 8;
	d |= S2_AF_BIT;
	d |= (pa & STAGE2_BLOCK_ADDR_MASK);
	if (xn)
		d |= S2_XN_BIT;
	return d;
}

/* Mirrors stage2.c:170-184 L2/L3 geometry constants + UART index derivation. */
#define STAGE2_L2_ENTRIES     512u
#define STAGE2_L2_BLOCK_SIZE  0x200000UL
#define STAGE2_L2_BLOCK_SHIFT 21
#define STAGE2_L2_ADDR_MASK   (~(uint64_t)(STAGE2_L2_BLOCK_SIZE - 1u))

#define STAGE2_L3_ENTRIES     512u
#define STAGE2_L3_PAGE_SIZE   0x1000UL
#define STAGE2_L3_PAGE_SHIFT  12
#define STAGE2_L3_ADDR_MASK   (~(uint64_t)(STAGE2_L3_PAGE_SIZE - 1u))

#define STAGE2_TABLE_ADDR_MASK  0x0000FFFFFFFFF000ULL

#define UART_L2_IDX  ((unsigned)((UART0_BASE >> STAGE2_L2_BLOCK_SHIFT) % STAGE2_L2_ENTRIES))
#define UART_L3_IDX  ((unsigned)((UART0_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))

static uint64_t stage2_l2_mmio[STAGE2_L2_ENTRIES]
	__attribute__((aligned(STAGE2_L2_ENTRIES * 8u)));
static uint64_t stage2_l3_uart[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));

#define S2_DESC_VALID_TABLE   0x3ull

/* Mirrors stage2.c:212-216 stage2_table_desc() verbatim. */
static uint64_t
stage2_table_desc(uint64_t next_table_pa)
{
	return S2_DESC_VALID_TABLE | (next_table_pa & STAGE2_TABLE_ADDR_MASK);
}

/* Mirrors stage2.c:222-234 stage2_page_desc() verbatim. */
static uint64_t
stage2_page_desc(uint64_t pa, unsigned memattr, unsigned sh, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_TABLE;
	d |= (uint64_t)(memattr & 0xFu) << 2;
	d |= (uint64_t)(S2AP_RW)        << 6;
	d |= (uint64_t)(sh & 0x3u)      << 8;
	d |= S2_AF_BIT;
	d |= (pa & STAGE2_L3_ADDR_MASK);
	if (xn)
		d |= S2_XN_BIT;
	return d;
}

/* Mirrors stage2.c:241-253 stage2_l2_block_desc() verbatim. */
static uint64_t
stage2_l2_block_desc(uint64_t pa, unsigned memattr, unsigned sh, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_BLOCK;
	d |= (uint64_t)(memattr & 0xFu) << 2;
	d |= (uint64_t)(S2AP_RW)        << 6;
	d |= (uint64_t)(sh & 0x3u)      << 8;
	d |= S2_AF_BIT;
	d |= (pa & STAGE2_L2_ADDR_MASK);
	if (xn)
		d |= S2_XN_BIT;
	return d;
}

/* Mirrors stage2.c:258-316 stage2_build_mmio_tables() verbatim (minus the
 * stg2_bc() breadcrumb calls at lines 269/577/579, which are pure telemetry
 * with no effect on the tables built). */
static uint64_t
stage2_build_mmio_tables(void)
{
	uint64_t l2_block_base = (uint64_t)UART_L2_IDX << STAGE2_L2_BLOCK_SHIFT;

	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == UART_L3_IDX) {
			stage2_l3_uart[j] = 0;
			continue;
		}
		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
		stage2_l3_uart[j] = stage2_page_desc(pa,
			S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
	}

	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (i == UART_L2_IDX) {
			uint64_t l3_pa = (uint64_t)(uintptr_t)&stage2_l3_uart[0];
			stage2_l2_mmio[i] = stage2_table_desc(l3_pa);
			continue;
		}
		if (i == (unsigned)(VBLK_MMIO_BASE >> STAGE2_L2_BLOCK_SHIFT)) {
			stage2_l2_mmio[i] = 0;
			continue;
		}
		uint64_t pa = (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_mmio[i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
	}

	return (uint64_t)(uintptr_t)&stage2_l2_mmio[0];
}

/* Mirrors stage2.c:340-342 the vector-page index derivation, and the
 * L1/L2/L3 split body of stage2_unmap_guest_vector() (:354-396) /
 * stage2_map_guest_vector() (:398-411), minus stage2_tlb_flush() (a real
 * "tlbi"/"dsb"/"isb" sequence — hardware TLB maintenance, not table-content
 * logic; the tables themselves are fully built/mutated before that call in
 * the source, so omitting it does not change what we assert on the tables). */
#define VEC_L1_IDX  ((unsigned)(GUEST_VECTOR_IPA >> STAGE2_BLOCK_SHIFT))
#define VEC_L2_IDX  ((unsigned)((GUEST_VECTOR_IPA >> STAGE2_L2_BLOCK_SHIFT) % STAGE2_L2_ENTRIES))
#define VEC_L3_IDX  ((unsigned)((GUEST_VECTOR_IPA & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))

static uint64_t stage2_l2_dram[STAGE2_L2_ENTRIES]
	__attribute__((aligned(STAGE2_L2_ENTRIES * 8u)));
static uint64_t stage2_l3_vec[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));

/* Mirrors stage2.c:354-396 stage2_unmap_guest_vector(), minus stage2_tlb_flush(). */
static void
stage2_unmap_guest_vector(void)
{
	uint64_t l1_block_base = (uint64_t)VEC_L1_IDX << STAGE2_BLOCK_SHIFT;
	uint64_t l2_block_base = l1_block_base +
		((uint64_t)VEC_L2_IDX << STAGE2_L2_BLOCK_SHIFT);

	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == VEC_L3_IDX) {
			stage2_l3_vec[j] = 0;
			continue;
		}
		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
		stage2_l3_vec[j] = stage2_page_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	}

	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (i == VEC_L2_IDX) {
			uint64_t l3_pa = (uint64_t)(uintptr_t)&stage2_l3_vec[0];
			stage2_l2_dram[i] = stage2_table_desc(l3_pa);
			continue;
		}
		uint64_t pa = l1_block_base + (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_dram[i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	}

	stage2_l1[0][VEC_L1_IDX] =
		stage2_table_desc((uint64_t)(uintptr_t)&stage2_l2_dram[0]);
}

/* Mirrors stage2.c:398-411 stage2_map_guest_vector(), minus stage2_tlb_flush(). */
static void
stage2_map_guest_vector(void)
{
	uint64_t pa = GUEST_VECTOR_IPA & STAGE2_L3_ADDR_MASK;
	stage2_l3_vec[VEC_L3_IDX] = stage2_page_desc(pa,
		S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
}

/* Mirrors the table-building body of stage2.c:552-597 stage2_init(), minus
 * all register programming (VTCR/VTTBR writes, PARange read, breadcrumbs)
 * — exactly the "pure address/table-construction logic" half the task asked
 * for, versus the "pokes real MMU control registers" half it asked to leave
 * out. Returns ndesc (mirrors stage2.c's local `ndesc` counter, stg2_bc'd to
 * word 5) so tests can check it. */
static uint32_t
stage2_init_tables(void)
{
	for (unsigned t = 0; t < STAGE2_L1_TABLES; t++)
		for (unsigned i = 0; i < STAGE2_L1_ENTRIES; i++)
			stage2_l1[t][i] = 0;

	uint32_t ndesc = 0;

	{
		unsigned idx = (unsigned)(STAGE2_MMIO_BASE >> STAGE2_BLOCK_SHIFT);
		uint64_t l2_pa = stage2_build_mmio_tables();
		stage2_l1[0][idx] = stage2_table_desc(l2_pa);
		ndesc++;
	}

	{
		unsigned nblocks = (unsigned)(STAGE2_DRAM_SIZE / STAGE2_BLOCK_SIZE);
		unsigned base_idx = (unsigned)(STAGE2_DRAM_BASE >> STAGE2_BLOCK_SHIFT);
		for (unsigned b = 0; b < nblocks; b++) {
			uint64_t pa = STAGE2_DRAM_BASE + (uint64_t)b * STAGE2_BLOCK_SIZE;
			stage2_l1[0][base_idx + b] = stage2_block_desc(pa,
				S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
			ndesc++;
		}
	}

	return ndesc;
}

/* Mirrors the software-walk self-check body of stage2.c:627-639 (part of
 * stage2_init()): decode our own table entry for STAGE2_SELFTEST_IPA and
 * confirm it resolves back to the same PA. */
static int
stage2_selfcheck_ipa(uint64_t ipa)
{
	unsigned idx = (unsigned)(ipa >> STAGE2_BLOCK_SHIFT);
	uint64_t desc = stage2_l1[0][idx];
	if ((desc & S2_DESC_VALID_BLOCK) != S2_DESC_VALID_BLOCK)
		return 0;
	uint64_t block_pa = desc & STAGE2_BLOCK_ADDR_MASK;
	uint64_t offset = ipa & (STAGE2_BLOCK_SIZE - 1u);
	uint64_t resolved_pa = block_pa | offset;
	return (resolved_pa == ipa) ? 1 : 0;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* stage2_block_desc(): correct bit placement for a known DRAM 1 GiB block —
 * valid+block, MemAttr, S2AP, SH, AF, XN=0, and the PA correctly masked to
 * the 1 GiB block boundary (low 30 bits stripped even if a non-aligned PA
 * is passed in, matching the source's unconditional masking). */
static void test_block_desc_dram_bits(void)
{
	uint64_t pa = 0x40000000ULL + 0x123456ULL;  /* deliberately unaligned input */
	uint64_t d = stage2_block_desc(pa, S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);

	assert((d & 0x3ull) == 0x1ull);                 /* valid + BLOCK */
	assert(((d >> 2) & 0xFu) == S2_MEMATTR_NORMAL_WB);
	assert(((d >> 6) & 0x3u) == S2AP_RW);
	assert(((d >> 8) & 0x3u) == S2_SH_INNER);
	assert(d & S2_AF_BIT);
	assert(!(d & S2_XN_BIT));                       /* xn=0 requested */
	assert((d & STAGE2_BLOCK_ADDR_MASK) == 0x40000000ULL);  /* unaligned bits stripped */
}

/* stage2_block_desc(): MMIO-style block (Device-nGnRE, Outer-Shareable,
 * XN=1) gets the opposite bit choices, still correctly placed. */
static void test_block_desc_mmio_bits(void)
{
	uint64_t pa = 0x00000000ULL;
	uint64_t d = stage2_block_desc(pa, S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);

	assert((d & 0x3ull) == 0x1ull);
	assert(((d >> 2) & 0xFu) == S2_MEMATTR_DEVICE_nGnRE);
	assert(((d >> 8) & 0x3u) == S2_SH_OUTER);
	assert(d & S2_AF_BIT);
	assert(d & S2_XN_BIT);
}

/* stage2_table_desc()/stage2_page_desc(): valid-table encoding (bits[1:0]==
 * 0b11) at both non-leaf and leaf-at-L3 usage, and correct address masking
 * at their respective granularities. */
static void test_table_and_page_desc_bits(void)
{
	uint64_t table_pa = 0x41234000ULL;
	uint64_t td = stage2_table_desc(table_pa);
	assert((td & 0x3ull) == 0x3ull);
	assert((td & STAGE2_TABLE_ADDR_MASK) == table_pa);

	uint64_t page_pa = 0x01C28000ULL;
	uint64_t pd = stage2_page_desc(page_pa, S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, 1);
	assert((pd & 0x3ull) == 0x3ull);
	/* NOTE: verify against STAGE2_TABLE_ADDR_MASK (bits[47:12]), NOT
	 * STAGE2_L3_ADDR_MASK. STAGE2_L3_ADDR_MASK (~(0x1000-1), i.e. "every
	 * bit except the low 12") is the right mask for *building* the
	 * descriptor (pa never has bits above 47 set), but is too WIDE for
	 * *verifying* an already-built descriptor here: it does not clear
	 * bit[54] (XN), so with xn=1 comparing (pd & STAGE2_L3_ADDR_MASK)
	 * against the bare page_pa would spuriously fail. This was a bug in
	 * this test file's own assertion, not in stage2.c — caught by
	 * actually running the suite. */
	assert((pd & STAGE2_TABLE_ADDR_MASK) == page_pa);
	assert(pd & S2_XN_BIT);
}

/* Table-level split points: stage2_build_mmio_tables() must leave EXACTLY
 * one L3 entry (UART0_BASE's page) invalid, EXACTLY one L2 entry (the UART
 * block) pointing at a TABLE, and EXACTLY one L2 entry (the virtio-mmio
 * block) invalid — every other L2 entry a plain identity block, every
 * other L3 entry a plain identity page. */
static void test_mmio_table_split_points(void)
{
	memset(stage2_l2_mmio, 0xFF, sizeof(stage2_l2_mmio)); /* poison */
	memset(stage2_l3_uart, 0xFF, sizeof(stage2_l3_uart));

	uint64_t l2_pa = stage2_build_mmio_tables();
	assert(l2_pa == (uint64_t)(uintptr_t)&stage2_l2_mmio[0]);

	unsigned vblk_l2_idx = (unsigned)(VBLK_MMIO_BASE >> STAGE2_L2_BLOCK_SHIFT);
	assert(vblk_l2_idx == 80u);   /* per vblk_emmc.h's own comment: 0x0A000000>>21==80 */
	assert(UART_L2_IDX == 14u);   /* per stage2.h's own worked example */
	assert(UART_L3_IDX == 40u);   /* per stage2.h's own worked example */

	unsigned invalid_l2 = 0, table_l2 = 0, block_l2 = 0;
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		uint64_t d = stage2_l2_mmio[i];
		if (i == vblk_l2_idx) {
			assert(d == 0);   /* trapped virtio-mmio window: fully invalid */
			invalid_l2++;
		} else if (i == UART_L2_IDX) {
			assert((d & 0x3ull) == 0x3ull);  /* TABLE descriptor */
			assert((d & STAGE2_TABLE_ADDR_MASK) ==
			       (uint64_t)(uintptr_t)&stage2_l3_uart[0]);
			table_l2++;
		} else {
			assert((d & 0x3ull) == 0x1ull);  /* plain identity BLOCK */
			uint64_t expected_pa = (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
			/* Verify against the bits[47:12] address field, not the
			 * (deliberately XN-inclusive) STAGE2_L2_ADDR_MASK — see the
			 * note in test_table_and_page_desc_bits(). These entries have
			 * xn=1, so a mask that doesn't exclude bit[54] would spuriously
			 * fail here too. */
			assert((d & STAGE2_TABLE_ADDR_MASK) == expected_pa);
			block_l2++;
		}
	}
	assert(invalid_l2 == 1 && table_l2 == 1);
	assert(invalid_l2 + table_l2 + block_l2 == STAGE2_L2_ENTRIES);

	unsigned invalid_l3 = 0, page_l3 = 0;
	uint64_t l2_block_base = (uint64_t)UART_L2_IDX << STAGE2_L2_BLOCK_SHIFT;
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		uint64_t d = stage2_l3_uart[j];
		if (j == UART_L3_IDX) {
			assert(d == 0);   /* trapped UART page: fully invalid */
			invalid_l3++;
		} else {
			assert((d & 0x3ull) == 0x3ull);  /* PAGE descriptor */
			uint64_t expected_pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
			/* See the note in test_table_and_page_desc_bits(): verify with
			 * the bits[47:12] field mask, not the XN-inclusive builder mask. */
			assert((d & STAGE2_TABLE_ADDR_MASK) == expected_pa);
			page_l3++;
		}
	}
	assert(invalid_l3 == 1);
	assert(invalid_l3 + page_l3 == STAGE2_L3_ENTRIES);

	/* Sanity: UART0_BASE itself really does decode to the invalid page via
	 * the two-level walk (L2 index -> TABLE -> L3 index -> invalid). */
	uint64_t l2_entry = stage2_l2_mmio[UART_L2_IDX];
	assert((l2_entry & 0x3ull) == 0x3ull);
	uint64_t l3_table = l2_entry & STAGE2_TABLE_ADDR_MASK;
	assert(l3_table == (uint64_t)(uintptr_t)&stage2_l3_uart[0]);
	assert(stage2_l3_uart[UART_L3_IDX] == 0);
}

/* stage2_init_tables(): full L1 build. index 0 -> TABLE (down to the MMIO
 * hierarchy above), index 1 -> plain 1 GiB DRAM BLOCK (STAGE2_DRAM_SIZE ==
 * one block here), everything else untouched/invalid, and ndesc == 2. */
static void test_init_tables_l1_layout(void)
{
	uint32_t ndesc = stage2_init_tables();
	assert(ndesc == 2u);   /* 1 MMIO table entry + 1 DRAM block (1 GiB size) */

	uint64_t mmio_desc = stage2_l1[0][0];
	assert((mmio_desc & 0x3ull) == 0x3ull);  /* TABLE, not a block */

	unsigned dram_idx = (unsigned)(STAGE2_DRAM_BASE >> STAGE2_BLOCK_SHIFT);
	assert(dram_idx == 1u);
	uint64_t dram_desc = stage2_l1[0][dram_idx];
	assert((dram_desc & 0x3ull) == 0x1ull);  /* plain BLOCK */
	assert((dram_desc & STAGE2_BLOCK_ADDR_MASK) == STAGE2_DRAM_BASE);
	assert(((dram_desc >> 2) & 0xFu) == S2_MEMATTR_NORMAL_WB);
	assert(!(dram_desc & S2_XN_BIT));        /* executable guest DRAM */

	/* Every other L1 entry in table 0, and the whole of table 1
	 * (unused concatenated table), must be invalid. */
	for (unsigned i = 2; i < STAGE2_L1_ENTRIES; i++)
		assert(stage2_l1[0][i] == 0);
	for (unsigned i = 0; i < STAGE2_L1_ENTRIES; i++)
		assert(stage2_l1[1][i] == 0);

	/* Self-check must pass for STAGE2_SELFTEST_IPA, exactly like the live
	 * stage2_init() self-check breadcrumb (word 6). */
	assert(stage2_selfcheck_ipa(STAGE2_SELFTEST_IPA) == 1);
}

/* stage2_unmap_guest_vector() / stage2_map_guest_vector(): the special-case
 * function referenced from stage2.h/firstfault.c. Verifies: (1) after
 * unmap, the 1 GiB block containing GUEST_VECTOR_IPA has been re-topologised
 * into L2->L3, the vector's own L3 page is invalid, every OTHER L3 page in
 * that 2 MiB block and every OTHER L2 entry in that 1 GiB block are still
 * plain identity descriptors; (2) after map, the vector page itself becomes
 * a valid identity page again, with the correct PA/attributes, without
 * disturbing anything else. */
static void test_guest_vector_unmap_and_remap(void)
{
	/* Give it a starting L1 entry as stage2_init_tables() would have. */
	stage2_l1[0][VEC_L1_IDX] = stage2_block_desc(
		(uint64_t)VEC_L1_IDX << STAGE2_BLOCK_SHIFT,
		S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);

	assert(VEC_L1_IDX == 1u);   /* per stage2.h's own worked example */
	assert(VEC_L2_IDX == 52u);
	assert(VEC_L3_IDX == 295u);

	stage2_unmap_guest_vector();

	/* L1 entry is now a TABLE. */
	uint64_t l1d = stage2_l1[0][VEC_L1_IDX];
	assert((l1d & 0x3ull) == 0x3ull);
	assert((l1d & STAGE2_TABLE_ADDR_MASK) == (uint64_t)(uintptr_t)&stage2_l2_dram[0]);

	/* L2: exactly one TABLE entry (VEC_L2_IDX), everything else a plain
	 * identity 2 MiB block at the right PA. */
	uint64_t l1_block_base = (uint64_t)VEC_L1_IDX << STAGE2_BLOCK_SHIFT;
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		uint64_t d = stage2_l2_dram[i];
		if (i == VEC_L2_IDX) {
			assert((d & 0x3ull) == 0x3ull);
			assert((d & STAGE2_TABLE_ADDR_MASK) ==
			       (uint64_t)(uintptr_t)&stage2_l3_vec[0]);
		} else {
			assert((d & 0x3ull) == 0x1ull);
			uint64_t expected_pa = l1_block_base + (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
			assert((d & STAGE2_L2_ADDR_MASK) == expected_pa);
		}
	}

	/* L3: exactly the vector page invalid, everything else identity. */
	uint64_t l2_block_base = l1_block_base + ((uint64_t)VEC_L2_IDX << STAGE2_L2_BLOCK_SHIFT);
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		uint64_t d = stage2_l3_vec[j];
		if (j == VEC_L3_IDX) {
			assert(d == 0);
		} else {
			assert((d & 0x3ull) == 0x3ull);
			uint64_t expected_pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
			assert((d & STAGE2_L3_ADDR_MASK) == expected_pa);
		}
	}

	/* Now re-map: the vector page must come back as a valid identity page
	 * at exactly GUEST_VECTOR_IPA, Normal-WB, executable (xn=0). */
	stage2_map_guest_vector();
	uint64_t vecd = stage2_l3_vec[VEC_L3_IDX];
	assert((vecd & 0x3ull) == 0x3ull);
	assert((vecd & STAGE2_L3_ADDR_MASK) == (GUEST_VECTOR_IPA & STAGE2_L3_ADDR_MASK));
	assert(((vecd >> 2) & 0xFu) == S2_MEMATTR_NORMAL_WB);
	assert(!(vecd & S2_XN_BIT));

	/* Everything else in L3/L2 must be untouched by the re-map call. */
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == VEC_L3_IDX) continue;
		uint64_t d = stage2_l3_vec[j];
		assert((d & 0x3ull) == 0x3ull);
	}
}

/* ==================================================================== *
 * main()
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "block_desc_dram_bits",        test_block_desc_dram_bits },
	{ "block_desc_mmio_bits",        test_block_desc_mmio_bits },
	{ "table_and_page_desc_bits",    test_table_and_page_desc_bits },
	{ "mmio_table_split_points",     test_mmio_table_split_points },
	{ "init_tables_l1_layout",       test_init_tables_l1_layout },
	{ "guest_vector_unmap_and_remap", test_guest_vector_unmap_and_remap },
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

	printf("---- stage2 table tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
