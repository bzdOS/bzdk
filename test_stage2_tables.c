/* SPDX-License-Identifier: BSD-2-Clause */

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
 * ============================================================================
 * 2026-08-05: A1 ISOLATION + PERMISSIONS, AND THREE MIRROR DRIFTS FIXED
 * ============================================================================
 * This file was audited against the current stage2.c and had drifted in three
 * places, each of which made a green assertion meaningless. All three are the
 * SAME root cause: the A1 guest/HV isolation milestone changed what the builder
 * produces and the mirror was never updated (precisely the failure mode
 * REVIEW-2026-07-24.md:243-245 warned about, and it happened here):
 *
 *   1. stage2_init_tables() did not mirror the DRAM-block split at all, so
 *      test_init_tables_l1_layout() asserted the DRAM L1 entry was a plain
 *      1 GiB BLOCK. Since A1 it is a TABLE — HVIMG_BASE is inside that very
 *      block — so the test pinned pre-A1 behaviour while the source did
 *      something else.
 *   2. STAGE2_SELFTEST_IPA was 0x42000000 here vs 0x60000000 in stage2.h:154.
 *      0x42000000 is HVIMG_BASE, i.e. the mirror was verifying the identity
 *      property at an address the real system deliberately refuses to
 *      translate for the guest.
 *   3. stage2_selfcheck_ipa() mirrored only the pre-A1 BLOCK arm, using a
 *      single-bit valid test that is also true for a TABLE descriptor — the
 *      exact mis-resolution stage2.c:806-812 says it fixed.
 *
 * Added on top, so these become measured rather than assumed:
 *   - the A1 carve itself (exactly two invalid L2 entries, boundary-checked
 *     either side of both windows, every surviving translation verified to be
 *     true identity). Until now A1 was only ever checked on real hardware.
 *   - s2_walk(), a software stage-2 walker (this file's own instrument, not a
 *     mirror), so assertions can be made on RESOLVED permissions the way the
 *     MMU would answer them, not on one descriptor level at a time.
 *   - W^X: measured, and measured to be ABSENT. Guest DRAM is RWX everywhere.
 *     Read test_guest_dram_permissions_are_rwx_wx_not_enforced()'s comment for
 *     why that is a measurement and not a claim, and why stage2.c cannot fix
 *     it alone (it has no guest code/data map; kload.c owns that).
 *   - the enforced half of W^X: no MMIO leaf is executable, anywhere.
 *   - the vgic self-test breadcrumb collision found the same day (a
 *     guest-written window that lands inside the hv-scratch carve).
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
/* DRIFT FIX, 2026-08-05: this said 0x42000000 — which is HVIMG_BASE, the
 * hv-image window A1 now deliberately leaves UNMAPPED in the guest's stage-2
 * map. So the self-check assertion below was verifying the identity property
 * at an address the real system refuses to translate for the guest at all.
 * stage2.h:154 has said 0x60000000 since A1 landed (chosen precisely to fall
 * on a plain L2 block, never an excluded or further-split index). */
#define STAGE2_SELFTEST_IPA 0x60000000UL

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

/* ------------------------------------------------------------------ *
 * A1 GUEST/HV ISOLATION: the DRAM-block split.
 *
 * ADDED 2026-08-05, and the reason it had to be added is itself the finding:
 * this file's stage2_init_tables() mirror was written BEFORE the A1 milestone
 * carved the HV's own DRAM out of the guest's stage-2 map, and was never
 * updated. The real stage2_init() (stage2.c:740-758) now asks
 * stage2_dram_block_needs_split() whether a 1 GiB DRAM block contains the
 * hv-image or hv-scratch window and, if so, installs a TABLE descriptor down
 * to stage2_build_dram_table()'s level-2 table with those windows left
 * INVALID — instead of the flat 1 GiB BLOCK this file used to mirror. Since
 * HVIMG_BASE (0x42000000) is inside the FIRST DRAM block, that is not a
 * corner case: it is what every build does on every boot.
 *
 * So test_init_tables_l1_layout()'s old assertion
 *     assert((dram_desc & 0x3ull) == 0x1ull);   // plain BLOCK
 * asserted the OPPOSITE of what the source has been doing since A1, and
 * passed only because the mirror never grew the split. Exactly the mirror
 * drift REVIEW-2026-07-24.md:243-245 warned about, in this very file. The
 * assertion is corrected below, and the A1 exclusions are now mirrored and
 * asserted directly — which also means the guest/HV memory partition, until
 * now checked only on real hardware (AT S12E1W selfcheck / ISOL breadcrumb),
 * is measured board-free too.
 * ------------------------------------------------------------------ */
#define HVIMG_BASE      0x42000000UL   /* stage2.c:435 — DTB hv-image   */
#define HVSCR_BASE      0x50000000UL   /* stage2.c:436 — DTB hv-scratch */

/* Mirrors stage2.c:438-439. */
#define HVIMG_L2_IDX  ((unsigned)((HVIMG_BASE - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))
#define HVSCR_L2_IDX  ((unsigned)((HVSCR_BASE - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))

/* Mirrors stage2.c:464-482 stage2_build_dram_table() verbatim. The HV_HDMI
 * framebuffer arm (stage2.c:470-472) is deliberately NOT mirrored: it is
 * #ifdef'd out of every target except `dbg -DHV_HDMI`, and mirroring a
 * conditional carve would assert something most builds do not do. Noted here
 * rather than silently omitted — if HV_HDMI ever becomes the default, this
 * mirror is wrong and this comment is the trail. */
static uint64_t
stage2_build_dram_table(uint64_t block_base)
{
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (block_base == STAGE2_DRAM_BASE &&
		    (i == HVIMG_L2_IDX || i == HVSCR_L2_IDX)) {
			stage2_l2_dram[i] = 0;   /* INVALID: HV image / scratch */
			continue;
		}
		uint64_t pa = block_base + (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_dram[i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	}
	return (uint64_t)(uintptr_t)&stage2_l2_dram[0];
}

/* Mirrors stage2.c:491-499 stage2_dram_block_needs_split() verbatim. */
static int
stage2_dram_block_needs_split(uint64_t block_base)
{
	uint64_t block_end = block_base + STAGE2_BLOCK_SIZE;

	if (HVIMG_BASE >= block_base && HVIMG_BASE < block_end)
		return 1;
	if (HVSCR_BASE >= block_base && HVSCR_BASE < block_end)
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * A software stage-2 walker, for asserting on RESOLVED PERMISSIONS rather
 * than on descriptor words one level at a time. This is not a mirror of
 * anything in stage2.c (the source only ever walks its own tables for the
 * single STAGE2_SELFTEST_IPA self-check, stage2.c:797-825); it is the test's
 * own instrument, and it is what lets the W^X question below be asked of an
 * arbitrary guest IPA the way the MMU would answer it.
 * ------------------------------------------------------------------ */
struct s2_leaf {
	int      mapped;    /* 0 => translation fault at some level */
	uint64_t pa;        /* resolved output address (mapped only) */
	unsigned level;     /* 1 = 1 GiB block, 2 = 2 MiB block, 3 = 4 KiB page */
	unsigned memattr;   /* descriptor bits[5:2]  */
	unsigned s2ap;      /* descriptor bits[7:6]  */
	unsigned xn;        /* descriptor bit[54]    */
};

static struct s2_leaf
s2_walk(uint64_t ipa)
{
	struct s2_leaf r;
	memset(&r, 0, sizeof(r));

	unsigned l1i = (unsigned)(ipa >> STAGE2_BLOCK_SHIFT);
	if (l1i >= STAGE2_L1_ENTRIES * STAGE2_L1_TABLES)
		return r;
	uint64_t d = stage2_l1[l1i / STAGE2_L1_ENTRIES][l1i % STAGE2_L1_ENTRIES];

	if ((d & 0x3ull) == S2_DESC_VALID_BLOCK) {
		r.mapped  = 1;
		r.level   = 1;
		r.pa      = (d & STAGE2_BLOCK_ADDR_MASK) | (ipa & (STAGE2_BLOCK_SIZE - 1u));
		r.memattr = (unsigned)((d >> 2) & 0xFu);
		r.s2ap    = (unsigned)((d >> 6) & 0x3u);
		r.xn      = (d & S2_XN_BIT) ? 1u : 0u;
		return r;
	}
	if ((d & 0x3ull) != S2_DESC_VALID_TABLE)
		return r;   /* translation fault, level 1 */

	const uint64_t *l2 = (const uint64_t *)(uintptr_t)(d & STAGE2_TABLE_ADDR_MASK);
	unsigned l2i = (unsigned)((ipa >> STAGE2_L2_BLOCK_SHIFT) % STAGE2_L2_ENTRIES);
	uint64_t d2 = l2[l2i];

	if ((d2 & 0x3ull) == S2_DESC_VALID_BLOCK) {
		r.mapped  = 1;
		r.level   = 2;
		r.pa      = (d2 & STAGE2_L2_ADDR_MASK) | (ipa & (STAGE2_L2_BLOCK_SIZE - 1u));
		r.memattr = (unsigned)((d2 >> 2) & 0xFu);
		r.s2ap    = (unsigned)((d2 >> 6) & 0x3u);
		r.xn      = (d2 & S2_XN_BIT) ? 1u : 0u;
		return r;
	}
	if ((d2 & 0x3ull) != S2_DESC_VALID_TABLE)
		return r;   /* translation fault, level 2 */

	const uint64_t *l3 = (const uint64_t *)(uintptr_t)(d2 & STAGE2_TABLE_ADDR_MASK);
	unsigned l3i = (unsigned)((ipa & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT);
	uint64_t d3 = l3[l3i];

	if ((d3 & 0x3ull) != S2_DESC_VALID_TABLE)
		return r;   /* translation fault, level 3 (page descriptor is 0b11) */

	r.mapped  = 1;
	r.level   = 3;
	r.pa      = (d3 & STAGE2_TABLE_ADDR_MASK) | (ipa & (STAGE2_L3_PAGE_SIZE - 1u));
	r.memattr = (unsigned)((d3 >> 2) & 0xFu);
	r.s2ap    = (unsigned)((d3 >> 6) & 0x3u);
	r.xn      = (d3 & S2_XN_BIT) ? 1u : 0u;
	return r;
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
		/* Mirrors stage2.c:744-757, INCLUDING the A1 split arm that this
		 * mirror was missing until 2026-08-05 (see the A1 comment block
		 * further up for why that omission mattered and what it hid). */
		unsigned nblocks = (unsigned)(STAGE2_DRAM_SIZE / STAGE2_BLOCK_SIZE);
		unsigned base_idx = (unsigned)(STAGE2_DRAM_BASE >> STAGE2_BLOCK_SHIFT);
		for (unsigned b = 0; b < nblocks; b++) {
			uint64_t pa = STAGE2_DRAM_BASE + (uint64_t)b * STAGE2_BLOCK_SIZE;
			if (stage2_dram_block_needs_split(pa)) {
				uint64_t l2_pa = stage2_build_dram_table(pa);
				stage2_l1[0][base_idx + b] = stage2_table_desc(l2_pa);
			} else {
				stage2_l1[0][base_idx + b] = stage2_block_desc(pa,
					S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
			}
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
	/* DRIFT FIX, 2026-08-05: this used to mirror only the pre-A1 BLOCK arm,
	 * and tested `(desc & S2_DESC_VALID_BLOCK) != S2_DESC_VALID_BLOCK` — a
	 * single-bit test that is true for a TABLE descriptor too, which is
	 * exactly the mis-resolution stage2.c:806-812's own comment says it
	 * fixed. stage2.c:797-825 now checks both descriptor types and walks one
	 * more level for the TABLE case; mirrored in full here. With the
	 * corrected STAGE2_SELFTEST_IPA (0x60000000) the TABLE arm is the one
	 * actually taken, since that block is split by A1. */
	unsigned idx = (unsigned)(ipa >> STAGE2_BLOCK_SHIFT);
	uint64_t desc = stage2_l1[0][idx];
	uint64_t desc_type = desc & 0x3ull;

	if (desc_type == S2_DESC_VALID_BLOCK) {
		uint64_t block_pa = desc & STAGE2_BLOCK_ADDR_MASK;
		uint64_t offset = ipa & (STAGE2_BLOCK_SIZE - 1u);
		return ((block_pa | offset) == ipa) ? 1 : 0;
	}
	if (desc_type == S2_DESC_VALID_TABLE) {
		uint64_t l2_table_pa = desc & STAGE2_TABLE_ADDR_MASK;
		const uint64_t *l2 = (const uint64_t *)(uintptr_t)l2_table_pa;
		unsigned l2_idx = (unsigned)((ipa >> STAGE2_L2_BLOCK_SHIFT)
			% STAGE2_L2_ENTRIES);
		uint64_t l2_desc = l2[l2_idx];
		if ((l2_desc & 0x3ull) == S2_DESC_VALID_BLOCK) {
			uint64_t block_pa = l2_desc & STAGE2_L2_ADDR_MASK;
			uint64_t offset = ipa & (STAGE2_L2_BLOCK_SIZE - 1u);
			return ((block_pa | offset) == ipa) ? 1 : 0;
		}
	}
	return 0;
}

/* ==================================================================== *
 * W^X checker mirror (2026-08-11): stage2.c gained a REAL, general W^X
 * scanner (stage2_leaf_is_wx()/stage2_wx_scan()/stage2_wx_selfcheck(), see
 * stage2.c's own long comment above stage2_wx_selfcheck() for the full
 * rationale) as part of closing the ROADMAP v1 gate's last isolation item.
 * Mirrored verbatim here, same discipline as every other function in this
 * file (exact stage2.c citation, no register/breadcrumb I/O — stage2_wx_
 * selfcheck() itself is NOT mirrored, only the two pure functions under it).
 * ==================================================================== */

/* Mirrors stage2.c's stage2_wx_violation struct (added alongside
 * stage2_wx_scan(), just above it). */
struct stage2_wx_violation {
	uint64_t ipa;
	unsigned level;
};

/* Mirrors stage2.c's stage2_leaf_is_wx() verbatim. */
static inline int
stage2_leaf_is_wx(unsigned s2ap, unsigned xn)
{
	return ((s2ap & 0x2u) != 0u) && (xn == 0u);
}

/* Mirrors stage2.c's stage2_wx_scan() verbatim (the general table walk; the
 * breadcrumb-writing stage2_wx_selfcheck() wrapper around it is EL2-register
 * territory in spirit only insofar as it's a live self-check entry point —
 * it does no register I/O itself either, but is deliberately left unmirrored
 * since the tests below call stage2_wx_scan() directly, which is the part
 * with anything to prove). */
static uint32_t
stage2_wx_scan(struct stage2_wx_violation *first_violation)
{
	uint32_t violations = 0;

	for (unsigned t = 0; t < STAGE2_L1_TABLES; t++) {
		for (unsigned i = 0; i < STAGE2_L1_ENTRIES; i++) {
			uint64_t d1 = stage2_l1[t][i];
			uint64_t ipa1 = ((uint64_t)t * STAGE2_L1_ENTRIES + i)
				<< STAGE2_BLOCK_SHIFT;

			if ((d1 & 0x3ull) == S2_DESC_VALID_BLOCK) {
				unsigned s2ap = (unsigned)((d1 >> 6) & 0x3u);
				unsigned xn   = (d1 & S2_XN_BIT) ? 1u : 0u;
				if (stage2_leaf_is_wx(s2ap, xn)) {
					if (violations == 0 && first_violation) {
						first_violation->ipa = ipa1;
						first_violation->level = 1;
					}
					violations++;
				}
				continue;
			}
			if ((d1 & 0x3ull) != S2_DESC_VALID_TABLE)
				continue;

			const uint64_t *l2 = (const uint64_t *)(uintptr_t)
				(d1 & STAGE2_TABLE_ADDR_MASK);
			for (unsigned j = 0; j < STAGE2_L2_ENTRIES; j++) {
				uint64_t d2 = l2[j];
				uint64_t ipa2 = ipa1 + ((uint64_t)j << STAGE2_L2_BLOCK_SHIFT);

				if ((d2 & 0x3ull) == S2_DESC_VALID_BLOCK) {
					unsigned s2ap = (unsigned)((d2 >> 6) & 0x3u);
					unsigned xn   = (d2 & S2_XN_BIT) ? 1u : 0u;
					if (stage2_leaf_is_wx(s2ap, xn)) {
						if (violations == 0 && first_violation) {
							first_violation->ipa = ipa2;
							first_violation->level = 2;
						}
						violations++;
					}
					continue;
				}
				if ((d2 & 0x3ull) != S2_DESC_VALID_TABLE)
					continue;

				const uint64_t *l3 = (const uint64_t *)(uintptr_t)
					(d2 & STAGE2_TABLE_ADDR_MASK);
				for (unsigned k = 0; k < STAGE2_L3_ENTRIES; k++) {
					uint64_t d3 = l3[k];
					uint64_t ipa3 = ipa2 + ((uint64_t)k << STAGE2_L3_PAGE_SHIFT);

					if ((d3 & 0x3ull) != S2_DESC_VALID_TABLE)
						continue;

					unsigned s2ap = (unsigned)((d3 >> 6) & 0x3u);
					unsigned xn   = (d3 & S2_XN_BIT) ? 1u : 0u;
					if (stage2_leaf_is_wx(s2ap, xn)) {
						if (violations == 0 && first_violation) {
							first_violation->ipa = ipa3;
							first_violation->level = 3;
						}
						violations++;
					}
				}
			}
		}
	}
	return violations;
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
 * hierarchy), index 1 -> also a TABLE since A1 (down to the DRAM hierarchy
 * with the hv-image/hv-scratch windows carved out), everything else
 * untouched/invalid, and ndesc == 2.
 *
 * DRIFT FIX, 2026-08-05: this test used to assert the DRAM entry was a plain
 * 1 GiB BLOCK. That has been false since the A1 milestone — HVIMG_BASE
 * (0x42000000) is inside this very block, so stage2_init() installs a TABLE.
 * The assertion passed only because the mirror never grew the split, i.e. the
 * test was pinning pre-A1 behaviour while the source did something else. */
static void test_init_tables_l1_layout(void)
{
	uint32_t ndesc = stage2_init_tables();
	assert(ndesc == 2u);   /* 1 MMIO table entry + 1 DRAM entry (1 GiB size) */

	uint64_t mmio_desc = stage2_l1[0][0];
	assert((mmio_desc & 0x3ull) == 0x3ull);  /* TABLE, not a block */

	unsigned dram_idx = (unsigned)(STAGE2_DRAM_BASE >> STAGE2_BLOCK_SHIFT);
	assert(dram_idx == 1u);
	uint64_t dram_desc = stage2_l1[0][dram_idx];
	assert((dram_desc & 0x3ull) == 0x3ull);  /* TABLE: split by A1 */
	assert((dram_desc & STAGE2_TABLE_ADDR_MASK) ==
	       (uint64_t)(uintptr_t)&stage2_l2_dram[0]);

	/* The guest-visible attributes now live on the L2 leaves, so check one
	 * there instead: still Normal-WB and still executable (xn=0). */
	struct s2_leaf leaf = s2_walk(STAGE2_DRAM_BASE + STAGE2_L2_BLOCK_SIZE);
	assert(leaf.mapped && leaf.level == 2u);
	assert(leaf.memattr == S2_MEMATTR_NORMAL_WB);
	assert(leaf.xn == 0u);

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
 * A1 guest/HV isolation, measured board-free (2026-08-05)
 * ==================================================================== */

/* The two DTB reserved-memory windows that ARE the A1 partition must be
 * unmapped for the guest, and NOTHING ELSE may be: exactly two invalid L2
 * entries, every other one a plain identity 2 MiB block. Until now this
 * property was only ever checked on real hardware (the AT S12E1W selfcheck /
 * ISOL breadcrumb); this asserts it on the tables themselves, so a future
 * edit that widens or drops a carve fails here first.
 *
 * Note the asymmetry this pins: 2 MiB granularity means the carve is
 * 0x42000000..0x421FFFFF and 0x50000000..0x501FFFFF, i.e. the guest loses a
 * full 2 MiB per window regardless of how much the HV actually occupies. That
 * is a deliberate consequence of using L2 blocks rather than L3 pages, and it
 * is what the vgic self-test breadcrumb collision (below) fell into. */
static void test_a1_hv_windows_are_unmapped_for_the_guest(void)
{
	stage2_init_tables();

	assert(HVIMG_L2_IDX == 16u);    /* (0x42000000-0x40000000)>>21 */
	assert(HVSCR_L2_IDX == 128u);   /* (0x50000000-0x40000000)>>21 */

	unsigned invalid = 0;
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		uint64_t d = stage2_l2_dram[i];

		if (i == HVIMG_L2_IDX || i == HVSCR_L2_IDX) {
			assert(d == 0);            /* the A1 carve */
			invalid++;
			continue;
		}
		assert((d & 0x3ull) == 0x1ull);    /* plain identity block */
		assert((d & STAGE2_TABLE_ADDR_MASK) ==
		       STAGE2_DRAM_BASE + (uint64_t)i * STAGE2_L2_BLOCK_SIZE);
	}
	assert(invalid == 2u);   /* exactly two, no more, no fewer */

	/* Resolved through a full walk: the first and last byte of each window
	 * faults, and the bytes immediately either side of each window do not.
	 * The boundary cases are the point — an off-by-one carve would still
	 * give invalid == 2 above. */
	assert(s2_walk(HVIMG_BASE).mapped == 0);
	assert(s2_walk(HVIMG_BASE + STAGE2_L2_BLOCK_SIZE - 1u).mapped == 0);
	assert(s2_walk(HVIMG_BASE - 1u).mapped == 1);
	assert(s2_walk(HVIMG_BASE + STAGE2_L2_BLOCK_SIZE).mapped == 1);

	assert(s2_walk(HVSCR_BASE).mapped == 0);
	assert(s2_walk(HVSCR_BASE + STAGE2_L2_BLOCK_SIZE - 1u).mapped == 0);
	assert(s2_walk(HVSCR_BASE - 1u).mapped == 1);
	assert(s2_walk(HVSCR_BASE + STAGE2_L2_BLOCK_SIZE).mapped == 1);

	/* Everything the walker does report mapped must be a true identity
	 * mapping — isolation is worthless if the surviving translations are
	 * wrong. Sampled across the whole DRAM block at 2 MiB steps. */
	for (uint64_t ipa = STAGE2_DRAM_BASE;
	     ipa < STAGE2_DRAM_BASE + STAGE2_BLOCK_SIZE;
	     ipa += STAGE2_L2_BLOCK_SIZE) {
		struct s2_leaf l = s2_walk(ipa + 0x1000u);
		if (!l.mapped)
			continue;
		assert(l.pa == ipa + 0x1000u);
	}
}

/* Pins the bug found on 2026-08-05 by running vgic.c's bare-metal self-test
 * guest for the first time (see main_vgic_qemu.c / vgic-qemu-ci.sh): vgic.c's
 * VGST breadcrumb window is written BY THE GUEST at EL1, but its default
 * address lands inside the hv-scratch carve above, so the payload's first
 * store takes a stage-2 translation fault (measured live under QEMU:
 * ESR=0x93810046, DFSC=0x06 translation fault level 2, FAR=0x50001d00).
 *
 * This test exists so the collision is a MEASURED fact rather than a comment,
 * and so that the same trap is caught for the HV's own image window too: any
 * future "guest writes a breadcrumb at address X" must not pick an address in
 * either carve. Project memory records a related class of error
 * ("breadcrumb window hygiene") where a window was derived from a neighbour
 * and silently overlapped. */
static void test_guest_written_breadcrumb_windows_must_avoid_the_carves(void)
{
	stage2_init_tables();

	/* vgic.c's default VGST_BC_BASE: inside hv-scratch => guest cannot write
	 * it. THIS IS THE BUG, asserted as such. */
	assert(s2_walk(0x50001d00UL).mapped == 0);

	/* vgic.c's VGIC_BC_BASE (0x50001c00) is in the same carve — harmless,
	 * because only EL2 writes that one, and EL2 is not subject to stage-2.
	 * Asserted so the distinction is explicit rather than luck. */
	assert(s2_walk(0x50001c00UL).mapped == 0);

	/* The address the vgic-qemu target overrides to must be reachable, or
	 * that gate would be testing nothing. Kept in sync with the Makefile's
	 * -DVGST_BC_BASE=0x50200000UL. */
	struct s2_leaf l = s2_walk(0x50200000UL);
	assert(l.mapped == 1);
	assert(l.pa == 0x50200000UL);
	assert(l.s2ap == S2AP_RW);
}

/* ==================================================================== *
 * W^X on guest mappings — MEASURED, and measured to be ABSENT
 * ==================================================================== */

/* HONEST NAMING, read this before trusting the name of anything below.
 *
 * ROADMAP A1's unfinished half is W^X on guest memory. This test does NOT
 * assert "guest code is RX and guest data is RW-XN", because stage2.c does not
 * do that and this suite must not claim otherwise. What it does instead is
 * measure the permission map the builder actually produces, exhaustively, and
 * pin it — so that the absence of W^X is a recorded measurement rather than an
 * assumption, and so that the day someone implements it, this test fails and
 * has to be updated deliberately.
 *
 * The measured state: every guest-DRAM leaf is Normal-WB, S2AP=RW (bits[7:6]
 * == 0b11, read AND write permitted) and XN=0 (executable). That is RWX for
 * the entire guest-visible address space — which is also why the linker warns
 * about an RWX LOAD segment. Stage-2 has exactly two permission knobs here
 * (S2AP for read/write, XN for execute) and both are wide open.
 *
 * Why stage2.c cannot simply be fixed here: the builder has no idea where the
 * guest's code is. The code/data split lives in the guest ELF's segment
 * headers, which kload.c parses (kload_parse_elf / kload_place_segments);
 * stage-2 is built before and independently of that. A real W^X
 * implementation therefore needs a new stage2 API taking (base, size,
 * executable) per placed segment and refining the DRAM L2 blocks down to L3
 * pages — plus a decision about what to do with the guest's own runtime
 * allocations, which FreeBSD maps executable for module loading. That is a
 * feature, not a test fix, and it cannot be validated board-free. */
static void test_guest_dram_permissions_are_rwx_wx_not_enforced(void)
{
	stage2_init_tables();

	unsigned checked = 0, rwx = 0;

	/* Every mapped guest-DRAM leaf, at 2 MiB granularity across the whole
	 * identity-mapped DRAM range. */
	for (uint64_t ipa = STAGE2_DRAM_BASE;
	     ipa < STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE;
	     ipa += STAGE2_L2_BLOCK_SIZE) {
		struct s2_leaf l = s2_walk(ipa);

		if (!l.mapped)
			continue;   /* an A1 carve */
		checked++;
		assert(l.memattr == S2_MEMATTR_NORMAL_WB);
		/* S2AP == 0b11: guest may read AND write. */
		assert(l.s2ap == S2AP_RW);
		/* XN == 0: guest may also EXECUTE. Hence W and X together. */
		assert(l.xn == 0u);
		rwx++;
	}

	/* Sanity: we really did look at the whole 1 GiB minus the two carves. */
	assert(checked == STAGE2_L2_ENTRIES - 2u);
	assert(rwx == checked);   /* i.e. W^X holds for ZERO guest pages */

	/* And there is no separate "code" region anywhere in the map with
	 * narrower permissions — no leaf is read-only, and no leaf is XN, in the
	 * whole guest DRAM range. If either of these ever stops being true, W^X
	 * work has begun and this test must be rewritten to assert the new
	 * intent rather than the old measurement. */
	for (uint64_t ipa = STAGE2_DRAM_BASE;
	     ipa < STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE;
	     ipa += STAGE2_L2_BLOCK_SIZE) {
		struct s2_leaf l = s2_walk(ipa);

		if (!l.mapped)
			continue;
		assert(l.s2ap != 0x1u);   /* not read-only */
		assert(l.xn == 0u);       /* not execute-never */
	}
}

/* The half of W^X that IS enforced today, and worth pinning because it is a
 * real security property the guest cannot escape: NO MMIO is executable. Every
 * leaf under the low-1 GiB device hierarchy — L2 blocks and the UART page's L3
 * pages alike — carries XN=1 and Device-nGnRE, so the guest can never fetch
 * instructions out of a device window (an aliased-device execution primitive
 * is a classic way out of a stage-2 sandbox). */
static void test_all_mmio_leaves_are_execute_never(void)
{
	stage2_init_tables();

	unsigned l2_leaves = 0, l3_leaves = 0;

	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		uint64_t base = (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		struct s2_leaf l = s2_walk(base);

		if (!l.mapped)
			continue;   /* the trapped virtio-mmio window */
		assert(l.memattr == S2_MEMATTR_DEVICE_nGnRE);
		assert(l.xn == 1u);          /* execute-never: the enforced half */
		assert(l.s2ap == S2AP_RW);   /* devices are readable+writable */
		if (l.level == 2u)
			l2_leaves++;
		else if (l.level == 3u)
			l3_leaves++;
	}
	/* 512 L2 entries: one is the trapped virtio-mmio hole (invalid), one is
	 * the UART table whose first page resolves at level 3, the rest are
	 * level-2 blocks. */
	assert(l2_leaves == STAGE2_L2_ENTRIES - 2u);
	assert(l3_leaves == 1u);

	/* Walk every page of the UART's L3 table too — the trapped UART page
	 * must fault (that is what makes vconsole.c's emulation possible) and
	 * every other page must be an XN device page. */
	uint64_t l2_block_base = (uint64_t)UART_L2_IDX << STAGE2_L2_BLOCK_SHIFT;
	unsigned faulted = 0, pages = 0;
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		uint64_t ipa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
		struct s2_leaf l = s2_walk(ipa);

		if (!l.mapped) {
			faulted++;
			continue;
		}
		assert(l.level == 3u);
		assert(l.pa == ipa);
		assert(l.xn == 1u);
		assert(l.memattr == S2_MEMATTR_DEVICE_nGnRE);
		pages++;
	}
	assert(faulted == 1u);   /* exactly the UART0 register page */
	assert(faulted + pages == STAGE2_L3_ENTRIES);

	/* The virtio-mmio window must fault for the guest — that fault is how
	 * vblk_emmc.c gets to emulate the device at all. */
	assert(s2_walk(VBLK_MMIO_BASE).mapped == 0);
	assert(s2_walk(UART0_BASE).mapped == 0);
}

/* ==================================================================== *
 * W^X checker: proving it against the real table set, AND proving it
 * actually detects a violation (2026-08-11, ROADMAP v1 gate closure)
 * ==================================================================== */

/* stage2_leaf_is_wx(): the core predicate, tested directly against all four
 * architectural (s2ap, xn) combinations before trusting it inside a
 * 512-entry table walk. Only S2AP_RW (0b11) is ever produced by this file's
 * builders, but the predicate is written to stay correct if a read-only
 * encoding is ever added (see its own comment in stage2.c), so this test
 * exercises all four rather than just the one value stage2.c happens to use
 * today. */
static void test_wx_leaf_predicate(void)
{
	assert(stage2_leaf_is_wx(S2AP_RW, /*xn=*/0) == 1);  /* RW + exec: violation */
	assert(stage2_leaf_is_wx(S2AP_RW, /*xn=*/1) == 0);  /* RW + XN: clean */
	assert(stage2_leaf_is_wx(0x1u,    /*xn=*/0) == 0);  /* read-only + exec: clean */
	assert(stage2_leaf_is_wx(0x0u,    /*xn=*/0) == 0);  /* no access + exec: clean */
	assert(stage2_leaf_is_wx(0x2u,    /*xn=*/0) == 1);  /* write-only + exec: violation */
}

/* stage2_wx_scan() against the REAL (mirrored) table set: cross-validates the
 * new general walker against the count test_guest_dram_permissions_are_rwx_
 * wx_not_enforced() already measured by probing known addresses at a 2 MiB
 * stride (STAGE2_L2_ENTRIES - 2 == 510, every one a level-2 DRAM leaf) —
 * proving the two independent methods agree, and pinning exactly which leaf
 * the general walk finds first. */
static void test_wx_scan_matches_measured_dram_state(void)
{
	stage2_init_tables();

	struct stage2_wx_violation first = { 0xFFFFFFFFFFFFFFFFULL, 99u };
	uint32_t violations = stage2_wx_scan(&first);

	/* == 510: matches the existing per-address DRAM measurement exactly. */
	assert(violations == STAGE2_L2_ENTRIES - 2u);
	assert(first.level == 2u);
	/* The lowest-IPA leaf that is not one of the two A1 carves: L2 index 0
	 * of the DRAM table, i.e. STAGE2_DRAM_BASE itself. */
	assert(first.ipa == STAGE2_DRAM_BASE);

	/* NULL first_violation must not crash and must return the same count. */
	assert(stage2_wx_scan(NULL) == violations);
}

/* THE proof the task asked for: a checker that has never seen a violation
 * outside the known/measured DRAM condition is not known to work. Poison one
 * MMIO leaf proven clean by test_all_mmio_leaves_are_execute_never() (index
 * 0: identity block, Device-nGnRE, XN=1, S2AP=RW) by clearing its XN bit —
 * simulating exactly the class of regression this checker exists to catch
 * (a future device window added/edited without xn=1). The scan must then
 * report EXACTLY one more violation than the DRAM-only baseline, and must
 * name it as the new lowest-IPA violation (0x0, level 2) — proving the
 * checker inspects the actual live bits rather than returning a hardcoded
 * DRAM-shaped answer. Restoring the descriptor must return the count to
 * exactly the original baseline. */
static void test_wx_scan_catches_a_poisoned_mmio_leaf(void)
{
	stage2_init_tables();

	uint32_t baseline = stage2_wx_scan(NULL);
	assert(baseline == STAGE2_L2_ENTRIES - 2u);   /* the known DRAM condition only */

	uint64_t clean = stage2_l2_mmio[0];
	assert((clean & 0x3ull) == 0x1ull);        /* plain identity BLOCK */
	assert(clean & S2_XN_BIT);                 /* confirmed XN=1 before poisoning */
	assert(((clean >> 6) & 0x3u) == S2AP_RW);  /* confirmed RW before poisoning */

	stage2_l2_mmio[0] = clean & ~S2_XN_BIT;    /* POISON: now RW *and* executable */

	struct stage2_wx_violation first = { 0xFFFFFFFFFFFFFFFFULL, 99u };
	uint32_t poisoned = stage2_wx_scan(&first);

	assert(poisoned == baseline + 1u);   /* exactly one NEW violation */
	assert(first.level == 2u);
	/* Lower IPA than any DRAM violation (0x40000000+), so it must now be the
	 * FIRST one the scan reports — not merely counted. */
	assert(first.ipa == 0x0u);

	/* Un-poison: the checker measures live state, not something cached. */
	stage2_l2_mmio[0] = clean;
	assert(stage2_wx_scan(NULL) == baseline);
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
	{ "a1_hv_windows_are_unmapped_for_the_guest",
	                                 test_a1_hv_windows_are_unmapped_for_the_guest },
	{ "guest_written_breadcrumb_windows_must_avoid_the_carves",
	                                 test_guest_written_breadcrumb_windows_must_avoid_the_carves },
	{ "guest_dram_permissions_are_rwx_wx_not_enforced",
	                                 test_guest_dram_permissions_are_rwx_wx_not_enforced },
	{ "all_mmio_leaves_are_execute_never",
	                                 test_all_mmio_leaves_are_execute_never },
	{ "wx_leaf_predicate",           test_wx_leaf_predicate },
	{ "wx_scan_matches_measured_dram_state",
	                                 test_wx_scan_matches_measured_dram_state },
	{ "wx_scan_catches_a_poisoned_mmio_leaf",
	                                 test_wx_scan_catches_a_poisoned_mmio_leaf },
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
