/* SPDX-License-Identifier: BSD-2-Clause */

/* stage2_zephyr.c — implementation. See stage2_zephyr.h for the full API
 * contract and the isolation-boundary rationale; this file mirrors
 * stage2.c's descriptor-encoding conventions precisely (same bit layouts,
 * same VTCR_EL2 field derivation) but against a completely separate,
 * disjoint table set and this core's (CPU3's) own banked stage-2 registers.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only.
 */
#include <stdint.h>
#include "stage2_zephyr.h"
#include "hv_addrmap.h"

/* ------------------------------------------------------------------ *
 * Breadcrumb window: HVMAP_STAGE2Z_BC (0x50072000, "STGZ") — see
 * hv_addrmap.h. Mirrors stage2.c's STG2 window shape, scaled down to what
 * this simpler table set actually needs.
 *   [0] magic         0x53544758 ("STGX")
 *   [1] vtcr          VTCR_EL2 readback (low 32) after stage2_zephyr_init()
 *   [2] vttbr_lo      VTTBR_EL2 readback (low 32)
 *   [3] hcr           HCR_EL2 readback (low 32) after enable/disable
 *   [4] table_base_lo zstage2_l1[]'s base address, low 32
 *   [5] magic2        0x49534f4c ("ISOL") -- stamped by the selfcheck
 *   [6] mmio_f        AT S12E1W fault bit for IPA 0 (want 1)
 *   [7] freebsd_f     AT S12E1W fault bit for FreeBSD's DRAM base (want 1)
 *   [8] outside_f     AT S12E1W fault bit for the high-GiB "outside the
 *                      slice" control address (want 1)
 *   [9] inside_f      AT S12E1W fault bit for ZSTAGE2_DRAM_BASE (want 0)
 *   [10] pass         overall selfcheck result (1 = boundary proven)
 * ------------------------------------------------------------------ */
#define STGZ_MAGIC   0x53544758u   /* "STGX" */
#define STGZ_ISOL_MAGIC 0x49534f4cu /* "ISOL" */

enum {
	STGZ_MAGIC_IDX = 0,
	STGZ_VTCR_IDX,
	STGZ_VTTBR_IDX,
	STGZ_HCR_IDX,
	STGZ_TABLE_BASE_IDX,
	STGZ_ISOL_MAGIC_IDX,
	STGZ_MMIO_F_IDX,
	STGZ_FREEBSD_F_IDX,
	STGZ_OUTSIDE_F_IDX,
	STGZ_INSIDE_F_IDX,
	STGZ_PASS_IDX,
};

static inline void
stgz_bc(int i, uint32_t v)
{
	volatile uint32_t *p =
		(volatile uint32_t *)(HVMAP_STAGE2Z_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Table topology -- same shape as stage2.c: 4 KiB granule, 40-bit IPA
 * (T0SZ=24), start level 1 with 2 concatenated level-1 tables. Only table 0
 * is ever populated (our whole map sits far below 512 GiB); table 1 stays
 * all-zero/invalid, architecturally required to exist and be contiguous
 * but never walked in practice.
 * ------------------------------------------------------------------ */
#define ZSTAGE2_L1_ENTRIES   512u
#define ZSTAGE2_L1_TABLES    2u
#define ZSTAGE2_BLOCK_SIZE   0x40000000UL   /* 1 GiB, one level-1 block */
#define ZSTAGE2_BLOCK_SHIFT  30

static uint64_t zstage2_l1[ZSTAGE2_L1_TABLES][ZSTAGE2_L1_ENTRIES]
	__attribute__((aligned(ZSTAGE2_L1_TABLES * ZSTAGE2_L1_ENTRIES * 8u)));

/* Level-2 table for the high GiB (IPA 0x80000000-0xBFFFFFFF), installed as
 * zstage2_l1[0][ZSTAGE2_L1_HIGH_IDX]. Only the entries covering Zephyr's own
 * 32 MiB slice are valid; every other entry (the other ~992 MiB of this
 * gigabyte) stays invalid. */
static uint64_t zstage2_l2_high[512]
	__attribute__((aligned(512u * 8u)));

/* ------------------------------------------------------------------ *
 * Stage-2 descriptor bit layout -- IDENTICAL encoding to stage2.c's
 * (ARMv8-A stage-2, 4 KiB granule; see stage2.c's big comment for the full
 * per-field derivation), duplicated locally per this file's disjoint-files
 * convention (see stage2_zephyr.h's header comment).
 * ------------------------------------------------------------------ */
#define S2_DESC_VALID_BLOCK   0x1ull   /* bits[1:0] = 01 (block, level<3) */
#define S2_DESC_VALID_TABLE   0x3ull   /* bits[1:0] = 11 (table, or page @L3) */

#define S2_MEMATTR_NORMAL_WB  0xFu     /* 0b1111: Normal, Inner+Outer WB RWA */

#define S2AP_RW    0x3u    /* bits[7:6]: read+write */
#define S2_SH_INNER 0x3u   /* bits[9:8]: Inner Shareable */

#define S2_AF_BIT  (1ull << 10)
#define S2_XN_BIT  (1ull << 54)

#define ZSTAGE2_L2_ENTRIES     512u
#define ZSTAGE2_L2_BLOCK_SIZE  0x200000UL   /* 2 MiB */
#define ZSTAGE2_L2_BLOCK_SHIFT 21
#define ZSTAGE2_L2_ADDR_MASK   (~(uint64_t)(ZSTAGE2_L2_BLOCK_SIZE - 1u))

#define ZSTAGE2_TABLE_ADDR_MASK 0x0000FFFFFFFFF000ULL /* bits[47:12] */

static uint64_t
zstage2_table_desc(uint64_t next_table_pa)
{
	return S2_DESC_VALID_TABLE | (next_table_pa & ZSTAGE2_TABLE_ADDR_MASK);
}

static uint64_t
zstage2_l2_block_desc(uint64_t pa)
{
	uint64_t d = S2_DESC_VALID_BLOCK;
	d |= (uint64_t)(S2_MEMATTR_NORMAL_WB & 0xFu) << 2;
	d |= (uint64_t)(S2AP_RW)               << 6;
	d |= (uint64_t)(S2_SH_INNER & 0x3u)    << 8;
	d |= S2_AF_BIT;
	d |= (pa & ZSTAGE2_L2_ADDR_MASK);
	/* XN=0: Zephyr's own image is executable guest DRAM. */
	return d;
}

/* Which 1 GiB level-1 index covers the high GiB (0x80000000-0xBFFFFFFF)?
 * 0x80000000 >> 30 = 2 -- computed from the constant, not hard-coded, so a
 * future base-address edit stays consistent. */
#define ZSTAGE2_HIGH_GIB_BASE 0x80000000UL
#define ZSTAGE2_L1_HIGH_IDX ((unsigned)(ZSTAGE2_HIGH_GIB_BASE >> ZSTAGE2_BLOCK_SHIFT))

/* Where does Zephyr's 32 MiB slice sit WITHIN that 1 GiB block, in 2 MiB L2
 * units? (ZSTAGE2_DRAM_BASE - ZSTAGE2_HIGH_GIB_BASE) = 0xBE000000 -
 * 0x80000000 = 0x3E000000; >> 21 (2 MiB) = 496. Count = ZSTAGE2_DRAM_SIZE
 * (32 MiB) >> 21 = 16, so valid L2 indices are [496, 512) -- reaching
 * exactly to the end of the table (IPA 0x80000000 + 512*2MiB == 0xC0000000
 * == ZSTAGE2_DRAM_BASE + ZSTAGE2_DRAM_SIZE, by construction). */
#define ZSTAGE2_L2_SLICE_IDX \
	((unsigned)((ZSTAGE2_DRAM_BASE - ZSTAGE2_HIGH_GIB_BASE) >> ZSTAGE2_L2_BLOCK_SHIFT))
#define ZSTAGE2_L2_SLICE_COUNT \
	((unsigned)(ZSTAGE2_DRAM_SIZE >> ZSTAGE2_L2_BLOCK_SHIFT))

static void
zstage2_tlb_flush(void)
{
	/* Local (non-broadcast) invalidate: this core's stage-2 TLB entries are
	 * private to it (see stage2_zephyr.h's header comment on per-PE
	 * banking), so "vmalls12e1" (not the "IS" broadcast form) is correct
	 * and sufficient here, same as stage2.c's own stage2_tlb_flush(). */
	__asm__ volatile("dsb ish\n\ttlbi vmalls12e1\n\tdsb ish\n\tisb" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * VTCR_EL2 field values -- IDENTICAL to stage2.c's (see that file's big
 * comment for the full per-field rationale); duplicated locally, not
 * shared, per this file's disjoint-files convention. PS is read at runtime
 * from THIS core's own ID_AA64MMFR0_EL1 (a core-invariant field on this
 * single-cluster A64, but read fresh anyway rather than assumed).
 * ------------------------------------------------------------------ */
#define ZVTCR_T0SZ_SHIFT   0
#define ZVTCR_T0SZ_VAL     24ull

#define ZVTCR_SL0_SHIFT    6
#define ZVTCR_SL0_VAL      1ull

#define ZVTCR_IRGN0_SHIFT  8
#define ZVTCR_IRGN0_VAL    1ull

#define ZVTCR_ORGN0_SHIFT  10
#define ZVTCR_ORGN0_VAL    1ull

#define ZVTCR_SH0_SHIFT    12
#define ZVTCR_SH0_VAL      3ull

#define ZVTCR_TG0_SHIFT    14
#define ZVTCR_TG0_VAL      0ull

#define ZVTCR_PS_SHIFT     16

#define ZVTCR_RES1_BIT     (1ull << 31)

#define HCR_EL2_VM  (1ull << 0)

static inline uint64_t
zs_read_id_aa64mmfr0_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
	return v;
}

static inline uint64_t read_vtcr_el2(void)
{ uint64_t v; __asm__ volatile("mrs %0, vtcr_el2" : "=r"(v)); return v; }
static inline void write_vtcr_el2(uint64_t v)
{ __asm__ volatile("msr vtcr_el2, %0\n\tisb" :: "r"(v) : "memory"); }
static inline uint64_t read_vttbr_el2(void)
{ uint64_t v; __asm__ volatile("mrs %0, vttbr_el2" : "=r"(v)); return v; }
static inline void write_vttbr_el2(uint64_t v)
{ __asm__ volatile("msr vttbr_el2, %0\n\tisb" :: "r"(v) : "memory"); }
static inline uint64_t read_hcr_el2(void)
{ uint64_t v; __asm__ volatile("mrs %0, hcr_el2" : "=r"(v)); return v; }
static inline void write_hcr_el2(uint64_t v)
{ __asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(v) : "memory"); }

/* ------------------------------------------------------------------ *
 * Public API (see stage2_zephyr.h).
 * ------------------------------------------------------------------ */

void
stage2_zephyr_init(void)
{
	unsigned t, i;

	stgz_bc(STGZ_MAGIC_IDX, STGZ_MAGIC);

	for (t = 0; t < ZSTAGE2_L1_TABLES; t++)
		for (i = 0; i < ZSTAGE2_L1_ENTRIES; i++)
			zstage2_l1[t][i] = 0;

	/* High-GiB L2 table: every entry invalid EXCEPT the ZSTAGE2_L2_SLICE_
	 * COUNT entries starting at ZSTAGE2_L2_SLICE_IDX, which are plain
	 * identity Normal-WB executable 2 MiB blocks covering exactly
	 * [ZSTAGE2_DRAM_BASE, ZSTAGE2_DRAM_BASE + ZSTAGE2_DRAM_SIZE). This is
	 * the ONLY place this file ever writes a valid descriptor -- by
	 * construction, IPA 0x00000000-0x7FFFFFFF (all MMIO + FreeBSD's whole
	 * DRAM gigabyte) has no code path here that could ever mark it valid:
	 * zstage2_l1[] is zeroed above and the loop below only ever touches
	 * zstage2_l2_high[], never zstage2_l1[] directly for any index other
	 * than ZSTAGE2_L1_HIGH_IDX (set once, right after this loop). */
	for (i = 0; i < ZSTAGE2_L2_ENTRIES; i++) {
		if (i >= ZSTAGE2_L2_SLICE_IDX &&
		    i < ZSTAGE2_L2_SLICE_IDX + ZSTAGE2_L2_SLICE_COUNT) {
			uint64_t pa = ZSTAGE2_HIGH_GIB_BASE +
				(uint64_t)i * ZSTAGE2_L2_BLOCK_SIZE;
			zstage2_l2_high[i] = zstage2_l2_block_desc(pa);
		} else {
			zstage2_l2_high[i] = 0;   /* INVALID: not Zephyr's slice */
		}
	}

	/* Install the high-GiB L2 table as the ONLY valid level-1 entry.
	 * Level-1 index 0 (all MMIO) and index 1 (FreeBSD's DRAM gigabyte)
	 * stay exactly as the zero-fill above left them: no table pointer at
	 * all, i.e. entirely invalid -- not even a partial split, unlike
	 * stage2.c's own MMIO/UART handling. See stage2_zephyr.h's header
	 * comment: this is deliberate, not a shortcut -- Zephyr gets zero
	 * real MMIO passthrough in this design. */
	zstage2_l1[0][ZSTAGE2_L1_HIGH_IDX] =
		zstage2_table_desc((uint64_t)(uintptr_t)&zstage2_l2_high[0]);

	/* VTCR_EL2 -- identical field values to stage2.c's, PS read fresh from
	 * this core's own ID_AA64MMFR0_EL1. */
	{
		uint64_t parange = zs_read_id_aa64mmfr0_el1() & 0xFull;
		uint64_t vtcr = 0;
		vtcr |= ZVTCR_T0SZ_VAL  << ZVTCR_T0SZ_SHIFT;
		vtcr |= ZVTCR_SL0_VAL   << ZVTCR_SL0_SHIFT;
		vtcr |= ZVTCR_IRGN0_VAL << ZVTCR_IRGN0_SHIFT;
		vtcr |= ZVTCR_ORGN0_VAL << ZVTCR_ORGN0_SHIFT;
		vtcr |= ZVTCR_SH0_VAL   << ZVTCR_SH0_SHIFT;
		vtcr |= ZVTCR_TG0_VAL   << ZVTCR_TG0_SHIFT;
		vtcr |= parange         << ZVTCR_PS_SHIFT;
		vtcr |= ZVTCR_RES1_BIT;
		write_vtcr_el2(vtcr);
		stgz_bc(STGZ_VTCR_IDX, (uint32_t)read_vtcr_el2());
	}

	/* VTTBR_EL2 -- VMID 0 (bits[63:48] left zero; see stage2_zephyr.h's
	 * header comment for why sharing VMID 0 with CPU0's stage2.c is safe:
	 * this register is banked per PE) + this table's own base address. */
	{
		uint64_t table_base = (uint64_t)(uintptr_t)&zstage2_l1[0][0];
		write_vttbr_el2(table_base);
		stgz_bc(STGZ_VTTBR_IDX, (uint32_t)read_vttbr_el2());
		stgz_bc(STGZ_TABLE_BASE_IDX, (uint32_t)table_base);
	}

	/* HCR_EL2.VM deliberately untouched here -- stage2_zephyr_enable() is
	 * a separate, explicit step. */
}

void
stage2_zephyr_enable(void)
{
	uint64_t hcr = read_hcr_el2();
	hcr |= HCR_EL2_VM;
	write_hcr_el2(hcr);
	zstage2_tlb_flush();
	stgz_bc(STGZ_HCR_IDX, (uint32_t)read_hcr_el2());
}

/* ------------------------------------------------------------------ *
 * Hardware isolation self-check -- see stage2_zephyr.h for the contract.
 * `AT S12E1W` walks THIS core's (CPU3's) current EL1 stage-1 regime + its
 * own stage-2 tables for a WRITE. At the point this runs (right after
 * stage2_zephyr_enable(), before Zephyr executes) EL1 stage-1 is OFF
 * (guest_config() left SCTLR_EL1.M=0), so the input VA is used as a flat
 * IPA and ONLY stage-2 is applied -- exactly what a guest write would hit.
 * ------------------------------------------------------------------ */
static uint64_t
zstage2_at_s12e1w(uint64_t va)
{
	uint64_t par;
	__asm__ volatile(
		"at s12e1w, %1\n\t"
		"isb\n\t"
		"mrs %0, par_el1"
		: "=r"(par)
		: "r"(va)
		: "memory");
	return par;
}

int
stage2_zephyr_isolation_selfcheck(void)
{
	/* Control point "outside the slice": start of the SAME 1 GiB L1 block
	 * that holds Zephyr's own slice (ZSTAGE2_L1_HIGH_IDX), but at an L2
	 * index (0) well below ZSTAGE2_L2_SLICE_IDX (496) -- proves the split
	 * genuinely limits validity to just the 16 slice entries, not the
	 * whole 1 GiB block the table descriptor covers. */
	uint32_t mmio_f    = (uint32_t)(zstage2_at_s12e1w(0x00000000UL) & 1u);
	uint32_t freebsd_f = (uint32_t)(zstage2_at_s12e1w(0x40000000UL) & 1u);
	uint32_t outside_f = (uint32_t)(zstage2_at_s12e1w(ZSTAGE2_HIGH_GIB_BASE) & 1u);
	uint32_t inside_f  = (uint32_t)(zstage2_at_s12e1w(ZSTAGE2_DRAM_BASE) & 1u);

	uint32_t pass = (mmio_f == 1u && freebsd_f == 1u && outside_f == 1u &&
	                 inside_f == 0u) ? 1u : 0u;

	stgz_bc(STGZ_ISOL_MAGIC_IDX, STGZ_ISOL_MAGIC);
	stgz_bc(STGZ_MMIO_F_IDX, mmio_f);
	stgz_bc(STGZ_FREEBSD_F_IDX, freebsd_f);
	stgz_bc(STGZ_OUTSIDE_F_IDX, outside_f);
	stgz_bc(STGZ_INSIDE_F_IDX, inside_f);
	stgz_bc(STGZ_PASS_IDX, pass);

	return (int)pass;
}
