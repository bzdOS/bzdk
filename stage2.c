/* SPDX-License-Identifier: BSD-2-Clause */

/* stage2.c — ARMv8-A EL2 stage-2 (IPA -> PA) identity translation for the
 * bzdOS microkernel-turned-hypervisor on the Allwinner A64 (Cortex-A53).
 * See stage2.h for the API contract and the table-topology summary; this
 * file is the implementation plus the exact register/descriptor bit
 * rationale.
 *
 * Freestanding, no libc: only <stdint.h>. Cache-coherent breadcrumb stores
 * (dc civac + dsb sy), same pattern as every other lane in this tree.
 */
#include <stdint.h>
#include "vgicd.h"   /* VGICD_BASE — the GICD trap L3 index is derived from it */
#include "stage2.h"
#include "vblk_emmc.h"   /* VBLK_MMIO_BASE — trapped virtio-mmio window */
#include "vgic.h"        /* VGIC_GICV_BASE — GICC->GICV redirect target */
#include "wdogtrap.h"    /* WDOGTRAP_PAGE_BASE — CCU/PIO/WDOG trap L3 index,
                           * see STAGE2_TRAP_WDOG_PAGE below */

/* ------------------------------------------------------------------ *
 * Breadcrumb window: 0x50000c00 ("STG2"). Distinct from every other window
 * in the tree (MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300, EL2
 * exceptions 0x50000400, jitter/TIMR 0x50000500, ring 0x50000600, alloc
 * 0x50000700, GIC timer 0x50000800, sched 0x50000a00, guest 0x50000b00).
 *
 *   [0] magic       0x53544732 ("STG2")
 *   [1] vtcr        VTCR_EL2 readback (low 32 bits) after stage2_init()
 *   [2] vttbr_lo    VTTBR_EL2 readback (low 32 bits, i.e. the table base
 *                   low word — VMID is 0 in this milestone so the whole
 *                   64-bit value's upper 16 bits are zero anyway)
 *   [3] hcr         HCR_EL2 readback (low 32 bits) after stage2_enable()
 *                   (or stage2_disable(), whichever ran last)
 *   [4] table_base  address of the top-level (concatenated) table, low 32
 *   [5] ndesc       number of valid descriptors written by stage2_init()
 *   [6] selfcheck   1 = self-check passed (our own table's DRAM entry for
 *                   STAGE2_SELFTEST_IPA decodes back to the same PA),
 *                   0 = failed (would indicate a descriptor-building bug)
 *   [7] par_lo      PAR_EL1 low 32 bits after an `AT S12E1R` (stage-1 +
 *                   stage-2) translation of STAGE2_SELFTEST_IPA — proves
 *                   the COMBINED walk the CPU's hardware table-walker
 *                   actually performs is consistent (see stage2_at_check).
 *                   PAR_EL1.F (bit0) == 0 => success; the PA is in
 *                   bits[47:12], the resulting memory attributes in
 *                   bits[63:56] (ATTR) / [11:7] (SH etc).
 *   [8] par_hi      PAR_EL1 high 32 bits (so ATTR[63:56] is readable).
 * ------------------------------------------------------------------ */
#define STG2_BC_BASE  0x50000c00UL
#define STG2_MAGIC    0x53544732u   /* "STG2" */

enum {
	STG2_MAGIC_IDX = 0,
	STG2_VTCR_IDX,
	STG2_VTTBR_IDX,
	STG2_HCR_IDX,
	STG2_TABLE_BASE_IDX,
	STG2_NDESC_IDX,
	STG2_SELFCHECK_IDX,
	STG2_PAR_LO_IDX,
	STG2_PAR_HI_IDX,
};

static inline void
stg2_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(STG2_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Stage-2 translation tables.
 *
 * VTCR_EL2.SL0 = 1 (start level 1) with T0SZ = 24 (40-bit IPA) requires,
 * per the architecture's stage-2 concatenation rule for 4 KiB granule /
 * start level 1, exactly 2^(25 - T0SZ) = 2^(25-24) = 2 concatenated
 * level-1 tables (a single level-1 table only spans 512 GiB = 2^39 bytes;
 * 40-bit IPA needs 2^40 bytes, i.e. double that). The concatenated set
 * must be one contiguous, naturally aligned block: 2 tables * 512 entries
 * * 8 bytes = 8192 bytes, aligned to 8192.
 *
 * Only table 0 (IPA 0..512 GiB) is ever populated — our whole identity
 * map (0..STAGE2_DRAM_BASE+STAGE2_DRAM_SIZE) sits far below 512 GiB.
 * Table 1 (IPA 512 GiB..1 TiB) stays all-zero, i.e. every entry has its
 * valid bit (bit0) clear, which is exactly what an unmapped-but-required
 * table should look like.
 * ------------------------------------------------------------------ */
#define STAGE2_L1_ENTRIES   512u
#define STAGE2_L1_TABLES    2u
#define STAGE2_BLOCK_SIZE   0x40000000UL   /* 1 GiB, one level-1 block */
#define STAGE2_BLOCK_SHIFT  30

static uint64_t stage2_l1[STAGE2_L1_TABLES][STAGE2_L1_ENTRIES]
	__attribute__((aligned(STAGE2_L1_TABLES * STAGE2_L1_ENTRIES * 8u)));

/* ------------------------------------------------------------------ *
 * Stage-2 leaf (block) descriptor bit layout (ARMv8-A, stage-2, 4 KiB
 * granule, level-1 block). This is DELIBERATELY not the stage-1 layout —
 * stage-2 has no AttrIndx/MAIR indirection, the memory attributes are
 * encoded directly in the descriptor:
 *
 *   bits[1:0]   = 0b01              valid + BLOCK (level < 3)
 *   bits[5:2]   = MemAttr[3:0]      stage-2 memory type/cacheability
 *   bits[7:6]   = S2AP[1:0]         stage-2 access permissions
 *   bits[9:8]   = SH[1:0]           shareability
 *   bit[10]     = AF                access flag (must be 1 — we do no
 *                                   software access-flag management)
 *   bits[47:30] = output address    PA[47:30], i.e. the 1 GiB block base
 *   bit[54]     = XN                execute-never (single bit at this
 *                                   ARMv8.0 stage-2 level; no separate
 *                                   UXN/PXN since stage-2 has no EL0/EL1
 *                                   privilege split of its own)
 *
 * MemAttr[3:0] values used here (ARM DDI0487, stage-2 MemAttr encoding):
 *   0b0001 = Device-nGnRE           (MMIO region)
 *   0b1111 = Normal, Inner & Outer Write-Back, Read/Write-Allocate
 *                                   (DRAM region)
 *
 * S2AP[1:0] = 0b11 (read+write) for both regions — this is a
 * pass-through identity map, not an isolation boundary yet.
 *
 * SH[1:0]: 0b11 = Inner Shareable for DRAM (matches VTCR_EL2.SH0 below —
 * cache-coherent with the other CPU-side agents); 0b10 = Outer Shareable
 * for MMIO (conventional choice for device memory: ensures other
 * observers see the same ordering without claiming Inner-Shareable
 * cacheable semantics that don't apply to a device access anyway).
 * ------------------------------------------------------------------ */
#define S2_DESC_VALID_BLOCK   0x1ull   /* bits[1:0] = 01 */

#define S2_MEMATTR_DEVICE_nGnRE  0x1u  /* 0b0001 */
#define S2_MEMATTR_NORMAL_WB     0xFu  /* 0b1111 */

#define S2AP_RW   0x3u   /* bits[7:6] */

#define S2_SH_OUTER   0x2u   /* bits[9:8] */
#define S2_SH_INNER   0x3u   /* bits[9:8] */

#define S2_AF_BIT     (1ull << 10)
#define S2_XN_BIT     (1ull << 54)

#define STAGE2_BLOCK_ADDR_MASK  (~(uint64_t)(STAGE2_BLOCK_SIZE - 1u))

static uint64_t
stage2_block_desc(uint64_t pa, unsigned memattr, unsigned sh, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_BLOCK;
	d |= (uint64_t)(memattr & 0xFu) << 2;   /* MemAttr[3:0]  -> bits[5:2] */
	d |= (uint64_t)(S2AP_RW)        << 6;   /* S2AP[1:0]     -> bits[7:6] */
	d |= (uint64_t)(sh & 0x3u)      << 8;   /* SH[1:0]       -> bits[9:8] */
	d |= S2_AF_BIT;                          /* AF            -> bit[10]  */
	d |= (pa & STAGE2_BLOCK_ADDR_MASK);      /* output address bits[47:30]*/
	if (xn)
		d |= S2_XN_BIT;                  /* XN            -> bit[54]  */
	return d;
}

/* ------------------------------------------------------------------ *
 * UART trap-and-emulate: level-2/level-3 tables that replace the single
 * 1 GiB MMIO block (level-1 index 0) so exactly one 4 KiB page
 * (UART0_BASE) can be left unmapped while everything else in the 0..1 GiB
 * MMIO window stays identity-mapped, same as before.
 *
 *   level-1[0][0]                -> TABLE -> stage2_l2_mmio[512] (2 MiB/entry)
 *   stage2_l2_mmio[UART_L2_IDX]  -> TABLE -> stage2_l3_uart[512] (4 KiB/entry)
 *   every other stage2_l2_mmio[i] is a plain 2 MiB BLOCK (Device-nGnRE),
 *   identical attributes to the 1 GiB block it replaces.
 *   stage2_l3_uart[UART_L3_IDX] is left all-zero (invalid, bits[1:0]=00);
 *   every other stage2_l3_uart[j] is a plain 4 KiB PAGE (Device-nGnRE)
 *   identity entry.
 *
 * UART0_BASE = 0x01C28000 -> (0x01C28000 >> 21) = 14 (2 MiB block base
 * 0x01C00000), offset 0x28000 within it -> (0x28000 >> 12) = 40 (4 KiB
 * page index within that block). Both indices are computed here from
 * UART0_BASE rather than hard-coded, so a UART0_BASE edit keeps them
 * consistent automatically.
 * ------------------------------------------------------------------ */
#define STAGE2_L2_ENTRIES     512u
/* How many 1 GiB DRAM L1 blocks the window covers — each gets its OWN row
 * of stage2_l2_dram[] (its own L2 table) so the W^X flip machinery works
 * uniformly across the whole window, not just the first GiB. Derived from
 * STAGE2_DRAM_SIZE: 1 by default, 2 with GUEST_DRAM_2G. Generalized
 * 2026-08-27 after the 2G guest died in a 45 kHz instruction-abort storm:
 * FreeBSD allocates userland from the TOP of RAM, its first userland exec
 * fault landed in the second GiB — then mapped only as a flat XN 1 GiB
 * block, whose IPAs stage2_wx_flip() rejected (l2_idx >= 512), leaving the
 * fault unowned and the guest spinning on the same instruction forever. */
#define STAGE2_DRAM_L1_BLOCKS  (unsigned)(STAGE2_DRAM_SIZE / STAGE2_BLOCK_SIZE)
_Static_assert(STAGE2_DRAM_SIZE % STAGE2_BLOCK_SIZE == 0,
               "STAGE2_DRAM_SIZE must be a whole number of 1 GiB blocks");
#define STAGE2_L2_BLOCK_SIZE  0x200000UL   /* 2 MiB */
#define STAGE2_L2_BLOCK_SHIFT 21

#define STAGE2_L2_ADDR_MASK   (~(uint64_t)(STAGE2_L2_BLOCK_SIZE - 1u))

#define STAGE2_L3_ENTRIES     512u
#define STAGE2_L3_PAGE_SIZE   0x1000UL      /* 4 KiB */
#define STAGE2_L3_PAGE_SHIFT  12
#define STAGE2_L3_ADDR_MASK   (~(uint64_t)(STAGE2_L3_PAGE_SIZE - 1u))

#define STAGE2_TABLE_ADDR_MASK  0x0000FFFFFFFFF000ULL /* bits[47:12] */

#define UART_L2_IDX  ((unsigned)((UART0_BASE >> STAGE2_L2_BLOCK_SHIFT) % STAGE2_L2_ENTRIES))
#define UART_L3_IDX  ((unsigned)((UART0_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))

/* GICD trap (vgicd.c). The distributor sits in the SAME 2 MiB block as UART0,
 * so this is one more invalid entry in the SAME stage2_l3_uart[] table -- no
 * new table, no new level, exactly as the GICC-redirect comment below predicted
 * this file would be extended. Verified live before the change: index 129 held
 * a valid identity page descriptor to 0x01c81000.
 *
 * WHY trap it at all: under IMO=0 the guest programs the GIC directly, which is
 * correct and cheap for ONE guest. With two guests the distributor is shared,
 * and nothing stopped one from writing GICD_ITARGETSR to aim its own SPI at the
 * other's core. See vgicd.h. */
#define VGICD_L3_IDX ((unsigned)((VGICD_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))
_Static_assert(VGICD_L3_IDX == 129u,
               "GICD is not at L3 index 129 -- the live-verified assumption changed");
_Static_assert((VGICD_BASE >> STAGE2_L2_BLOCK_SHIFT) == (UART0_BASE >> STAGE2_L2_BLOCK_SHIFT),
               "GICD is no longer in the same 2 MiB block as UART0 -- needs its own L3 table");

/* CCU/PIO/WDOG trap (wdogtrap.c) -- STAGE2_TRAP_WDOG_PAGE, default OFF. Same
 * "one more invalid entry in the SAME stage2_l3_uart[] table" shape as the
 * GICD trap just above: WDOGTRAP_PAGE_BASE (0x01C20000) sits in the SAME 2
 * MiB block as UART0 (verified live via the same >>21 arithmetic used
 * throughout this file), so no new table, no new level -- see
 * docs/wdog-ccu-pio-stage2.md for the full analysis and why this stays
 * behind a flag rather than being unconditional like the GICD trap.
 *
 * WHY GATED, UNLIKE GICD: the GICD trap was hardware-verified before landing
 * (this file's own history). This page sits on the critical path of eMMC
 * clocking (PC5, see soc_a64.h's SOC_A64_PIO_PC_CFG0) and CCU covers clock
 * gates the guest's real EHCI/OHCI/MMC drivers genuinely need at boot
 * (docs/dma-bypass-stage2.md's own inventory says as much) -- turning every
 * access to this page into a stage-2 fault has NOT been hardware-validated,
 * and DEBUG_RULES.md is explicit that this class of change (could stop the
 * guest from ever reaching a mounted root) must not be guessed at. Flip this
 * flag and reload once boot-verified; until then it stays off and the page
 * below is byte-for-byte identity-mapped exactly as before wdogtrap.c
 * existed. */
#ifdef STAGE2_TRAP_WDOG_PAGE
#define WDOGTRAP_L3_IDX ((unsigned)((WDOGTRAP_PAGE_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))
_Static_assert(WDOGTRAP_L3_IDX == 32u,
               "CCU/PIO/WDOG page is not at L3 index 32 -- the live-verified assumption changed");
_Static_assert((WDOGTRAP_PAGE_BASE >> STAGE2_L2_BLOCK_SHIFT) == (UART0_BASE >> STAGE2_L2_BLOCK_SHIFT),
               "CCU/PIO/WDOG page is no longer in the same 2 MiB block as UART0 -- needs its own L3 table");
#endif

/* vGIC GICC->GICV redirect (internal task): VGIC_GICC_BASE (0x01c82000) sits in
 * the SAME 2 MiB MMIO block as UART0 (both are within 0x01c00000..0x01e00000,
 * block index UART_L2_IDX==14 — verified by the same >>21 arithmetic used
 * above), which stage2_build_mmio_tables() already splits to 4 KiB
 * page-granularity for the UART trap. So the GICC redirect just needs two
 * more entries in the SAME stage2_l3_uart[] table, no new table/level.
 * VGIC_GICC_BASE is exactly 0x2000 (2 pages, per the sun50i-a64.dtsi GIC-400
 * node cited in vgic.h) -> L3 indices 130 and 131. Redirecting these two
 * pages to VGIC_GICV_BASE (the physical virtual-CPU-interface page pair)
 * instead of leaving them identity-mapped to the real GICC is the standard
 * KVM/GICv2-virtualization trick: the guest's IAR/EOIR/PMR/CTLR accesses at
 * its "GICC" IPA transparently hit GICV, so vgic.c's GICH list registers
 * (not the shared physical CPU interface gic_timer.c owns) decide what the
 * guest sees. See vgic.h/vgic.c for the GICH/GICV side of this. */
static uint64_t stage2_l2_mmio[STAGE2_L2_ENTRIES]
	__attribute__((aligned(STAGE2_L2_ENTRIES * 8u)));
static uint64_t stage2_l3_uart[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));

#ifdef HV_VETRAP
#include "vetrap.h"
#endif
#ifdef HV_RSBTRAP
/* The 2 MiB block 0x01E00000 (R_ peripherals: R_PRCM, R_PIO, R_RSB, R_PWM),
 * split to 4 KiB so the one page holding rsb@1f03400 can be invalid: every
 * guest access to the RSB controller faults to rsbtrap.c. */
#include "rsbtrap.h"
#define RSBTRAP_L2_IDX ((unsigned)(RSBTRAP_PAGE_BASE >> STAGE2_L2_BLOCK_SHIFT))
#define RSBTRAP_L3_IDX ((unsigned)((RSBTRAP_PAGE_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))
_Static_assert(RSBTRAP_L2_IDX == 15u, "RSB page left the 0x01E00000 block");
_Static_assert(RSBTRAP_L3_IDX == 259u, "RSB page index drifted");
_Static_assert(RSBTRAP_L2_IDX != UART_L2_IDX, "RSB block is the UART block: use stage2_l3_uart");
static uint64_t stage2_l3_rpriv[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));
#endif

#if defined(HV_HDMI) && defined(HV_FB_GUEST)
/* L3 table for the one DRAM block that holds the BUF0/BUF1 boundary. Separate
 * from stage2_l3_uart[] because that one lives under the MMIO L2 table and its
 * pages are Device-nGnRE; these are Normal-WB guest DRAM. */
static uint64_t stage2_l3_hvfb[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));
#endif

#define S2_DESC_VALID_TABLE   0x3ull   /* bits[1:0] = 11 (table, or page @L3) */

/* Non-leaf table descriptor: valid + table (bits[1:0]=11), output address
 * = the next-level table's PA, bits[47:12]. Stage-2 table descriptors
 * carry no hierarchical attribute bits (that's a stage-1-only concept —
 * stage-2 has no APTable/XNTable/NSTable/PXNTable overrides to worry
 * about), so this is just the address. */
static uint64_t
stage2_table_desc(uint64_t next_table_pa)
{
	return S2_DESC_VALID_TABLE | (next_table_pa & STAGE2_TABLE_ADDR_MASK);
}

/* Level-3 leaf (page) descriptor — same attribute encoding as
 * stage2_block_desc() above, just bits[1:0]=11 (page, the only valid leaf
 * encoding at the final level) instead of 01, and the output address
 * aligned to 4 KiB instead of 1 GiB. */
static uint64_t
stage2_page_desc(uint64_t pa, unsigned memattr, unsigned sh, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_TABLE; /* 0b11 = page descriptor at level 3 */
	d |= (uint64_t)(memattr & 0xFu) << 2;
	d |= (uint64_t)(S2AP_RW)        << 6;
	d |= (uint64_t)(sh & 0x3u)      << 8;
	d |= S2_AF_BIT;
	d |= (pa & STAGE2_L3_ADDR_MASK);
	if (xn)
		d |= S2_XN_BIT;
	return d;
}

/* Level-2 leaf (2 MiB block) descriptor — same encoding as
 * stage2_block_desc() (bits[1:0]=01, block, since level<3) but with the
 * output address masked/aligned to 2 MiB (STAGE2_L2_ADDR_MASK) instead of
 * stage2_block_desc()'s 1 GiB mask. Needed because the level-1 block
 * descriptor helper cannot be reused as-is at this finer granularity. */
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

/* Build the level-2 MMIO table (+ its nested level-3 UART table), leaving
 * exactly the UART0_BASE page invalid. Returns the level-2 table's PA, to
 * be installed as level-1[0][0] by the caller (stage2_init()).
 *
 * H2(b) CAVEAT: every entry this function marks INVALID gates only CPU
 * (stage-2) accesses. Several other MMIO blocks left identity-mapped below
 * are DMA-capable engines (EHCI/OHCI, the MMC/eMMC controllers, the crypto
 * engine, the system DMA controller) with no SMMU in front of them on this
 * SoC — a guest-programmed DMA descriptor reaches DRAM (including hv-image/
 * hv-scratch below) without ever going through this table. See
 * docs/dma-bypass-stage2.md for the full inventory of what's genuinely
 * needed by the guest, what's fixable here (one draft patch, not yet
 * applied), and what's an inherent hardware limitation. */
static uint64_t
stage2_build_mmio_tables(void)
{
	/* Level-3: every 4 KiB page in the UART's 2 MiB block is an identity
	 * Device-nGnRE page, EXCEPT the UART0_BASE page itself, which is left
	 * all-zero (invalid) so a guest access there takes a stage-2
	 * translation fault instead of reaching real hardware.
	 * AND EXCEPT the two GICC pages, which are redirected to GICV. */
	uint64_t l2_block_base = (uint64_t)UART_L2_IDX << STAGE2_L2_BLOCK_SHIFT;

	/* Breadcrumb (DRAM): confirm stage2_build_mmio_tables() is called */
	stg2_bc(9, 0x4d4d494f);  /* "MMIO" at index 9 */

	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == UART_L3_IDX) {
			stage2_l3_uart[j] = 0;   /* INVALID: the trapped UART page */
			continue;
		}

		if (j == VGICD_L3_IDX) {
			/* INVALID: the trapped GICD page. Every guest access to the
			 * distributor now faults to EL2 and is policed by
			 * vgicd_handle_fault(). GICD is touched during interrupt
			 * SETUP, not on the acknowledge/EOI hot path (that goes
			 * through the CPU interface), so the cost is per-config. */
			stage2_l3_uart[j] = 0;
			continue;
		}

#ifdef STAGE2_TRAP_WDOG_PAGE
		if (j == WDOGTRAP_L3_IDX) {
			/* INVALID: the trapped CCU/PIO/WDOG page. Every guest access
			 * now faults to EL2 and is policed by wdogtrap_handle_fault()
			 * -- see that file and docs/wdog-ccu-pio-stage2.md. Unlike
			 * GICD/UART, this page carries real per-boot AND potentially
			 * per-poll traffic (CCU clock gates, PIO pinmux) whose volume
			 * has not been measured on hardware -- see the design doc's
			 * hardware-validation section before ever flipping this flag
			 * on a board that matters. */
			stage2_l3_uart[j] = 0;
			continue;
		}
#endif

		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;

		/* GICC->GICV redirect (RE-ADDED for the interrupt-virtualization
		 * milestone; see the block comment above this function for the
		 * index derivation — j==130/131 are VGIC_GICC_BASE's two 4 KiB
		 * pages). Under HCR_EL2.IMO=1 (main_dbg.c) the REAL GICC
		 * (0x1c82000) is EL2's alone — gic_timer_irq() is the only reader
		 * of its GICC_IAR/EOIR/DIR. If the guest's own "GICC" IPA accesses
		 * hit that same real hardware, it would race/corrupt EL2's own
		 * IAR/EOIR sequencing. Redirecting the guest's GICC IPA to the
		 * VIRTUAL CPU interface (GICV, 0x1c86000) instead — the standard
		 * KVM/GICv2-virtualization trick — lets the guest ack/EOI through
		 * GICV, which vgic.c's GICH List Registers actually control, while
		 * EL2 keeps the real GICC to itself. CORRECT ONLY paired with
		 * IMO=1 + COMPLETE HW-mode LR forwarding (gic_timer.c) — see
		 * vgic.h's STATUS note for why an earlier attempt at exactly this
		 * redirect, alone, without complete forwarding, was reverted
		 * (FIX-1, 2026-07-16): under the old IMO=0 policy the guest still
		 * needed the REAL GICC to service interrupts directly, so
		 * redirecting it to GICV left it unable to ack anything. */
		if (j == 130u || j == 131u) {
			uint64_t gicv_pa = VGIC_GICV_BASE +
				(uint64_t)(j - 130u) * STAGE2_L3_PAGE_SIZE;

			stage2_l3_uart[j] = stage2_page_desc(gicv_pa,
				S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
			continue;
		}

#ifdef HV_VETRAP
		if (vetrap_pa_traced(pa)) {   /* INVALID: video engine (and neighbours), traced */
			stage2_l3_uart[j] = 0;
			continue;
		}
#endif
		stage2_l3_uart[j] = stage2_page_desc(pa,
			S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
	}

	/* Level-2: identity 2 MiB Device-nGnRE blocks everywhere, except the
	 * one block containing UART0_BASE, which is a TABLE descriptor down
	 * to stage2_l3_uart[] instead of a block; and the virtio-mmio block at
	 * VBLK_MMIO_BASE (0x0A000000, L2 index 80), left INVALID so every guest
	 * access there faults to EL2 for vblk_mmio_fault() (exactly like the UART0
	 * page, but at 2 MiB granularity — nothing real lives in 0x0A000000..
	 * 0x0A200000, so no L3 split is needed). See docs/virtio-blk-design.md §5
	 * and docs/virtio-blk-integration.md §3. */
#ifdef HV_RSBTRAP
	{
		uint64_t base = (uint64_t)RSBTRAP_L2_IDX << STAGE2_L2_BLOCK_SHIFT;
		for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++)
			stage2_l3_rpriv[j] = (j == RSBTRAP_L3_IDX) ? 0 :   /* INVALID: trapped */
			    stage2_page_desc(base + (uint64_t)j * STAGE2_L3_PAGE_SIZE,
				S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
	}
#endif
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (i == UART_L2_IDX) {
			uint64_t l3_pa = (uint64_t)(uintptr_t)&stage2_l3_uart[0];
			stage2_l2_mmio[i] = stage2_table_desc(l3_pa);
			continue;
		}
#ifdef HV_RSBTRAP
		if (i == RSBTRAP_L2_IDX) {
			stage2_l2_mmio[i] = stage2_table_desc(
			    (uint64_t)(uintptr_t)&stage2_l3_rpriv[0]);
			continue;
		}
#endif
		if (i == (unsigned)(VBLK_MMIO_BASE >> STAGE2_L2_BLOCK_SHIFT)) { /* ==80 */
			stage2_l2_mmio[i] = 0;   /* INVALID: trapped virtio-mmio window */
			continue;
		}
		uint64_t pa = (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_mmio[i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
	}

	return (uint64_t)(uintptr_t)&stage2_l2_mmio[0];
}

/* ------------------------------------------------------------------ *
 * First-fault probe: unmap the guest's EL1 vector page (GUEST_VECTOR_IPA)
 * so its first vector fetch takes a stage-2 instruction abort to EL2. See
 * the big comment in stage2.h for the rationale.
 *
 * GUEST_VECTOR_IPA (0x46927000) is inside DRAM, which stage2_init() maps as
 * plain 1 GiB level-1 BLOCKs. To carve out a single 4 KiB page we split the
 * 1 GiB block that holds it (level-1 index VEC_L1_IDX) into a level-2 table
 * of 2 MiB blocks, and the one 2 MiB block that holds it (VEC_L2_IDX) into a
 * level-3 table of 4 KiB pages — every entry an identity Normal-WB
 * executable descriptor (matching the DRAM block it replaces) EXCEPT the
 * vector page (VEC_L3_IDX), left all-zero/invalid. Same two-level split as
 * the UART0 trap above, only Normal-WB/Inner-Shareable/XN=0 (this is
 * executable guest DRAM) instead of Device-nGnRE, and rooted at a level-1
 * DRAM block instead of the level-1 MMIO block.
 *
 *   GUEST_VECTOR_IPA 0x46927000
 *     -> level-1 idx (>>30)                       = 1   (DRAM 1..2 GiB)
 *     -> level-2 idx ((>>21) % 512)               = 52  (2 MiB @ 0x46800000)
 *     -> level-3 idx ((&0x1fffff) >> 12)          = 295 (4 KiB @ 0x46927000)
 * Indices are derived from GUEST_VECTOR_IPA so a base edit stays consistent.
 * ------------------------------------------------------------------ */
#define VEC_L1_IDX  ((unsigned)(GUEST_VECTOR_IPA >> STAGE2_BLOCK_SHIFT))
#define VEC_L2_IDX  ((unsigned)((GUEST_VECTOR_IPA >> STAGE2_L2_BLOCK_SHIFT) % STAGE2_L2_ENTRIES))
#define VEC_L3_IDX  ((unsigned)((GUEST_VECTOR_IPA & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT))

static uint64_t stage2_l2_dram[STAGE2_DRAM_L1_BLOCKS][STAGE2_L2_ENTRIES]
	__attribute__((aligned(STAGE2_L2_ENTRIES * 8u)));
static uint64_t stage2_l3_vec[STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));

/* Defined further down (with VTCR/VTTBR programming); forward-declared here
 * because the probe helpers below flush the TLB after re-topologising the
 * DRAM block. */
static void stage2_tlb_flush(void);

/* ------------------------------------------------------------------ *
 * A1 — guest/HV DRAM partitioning (ROADMAP milestone A1: "static
 * partitioning à la Jailhouse" — no IMO=1/vGIC, just carve the HV's own
 * memory out of the guest's identity map so a guest write there takes a
 * stage-2 fault instead of silently corrupting the hypervisor).
 *
 * The two excluded windows below are NOT invented here — they mirror the
 * `reserved-memory` node this exact DTB (bananapi-min.dtb) already declares
 * (`hv-image@42000000` / `hv-scratch@50000000`, both `no-map`), which is
 * what already keeps a WELL-BEHAVED guest's own allocator off these ranges.
 * This code is what turns that convention into a hard boundary: a guest
 * that ignores/doesn't know about the DTB reservation (or is compromised)
 * gets a stage-2 translation fault instead of write access.
 *
 *   hv-image    0x42000000, 1 MiB reserved in the DTB, current actual image
 *               (.text+.data+.bss) is ~265 KiB (see `make dbg`'s own `size`
 *               output) — rounding UP to one whole 2 MiB L2 block (index 16
 *               within stage2_l2_dram[]) costs nothing and avoids an L3
 *               sub-split for a non-2MiB-aligned size; still >3x the DTB's
 *               own declared margin over the real image.
 *   hv-scratch  0x50000000, exactly 2 MiB in the DTB (index 128) — the
 *               breadcrumb/scratch DRAM windows used throughout the tree
 *               (STG2 0x50000c00, BMC 0x50000f00+, VBK 0x50020000, EBIO
 *               0x50020200, SD 0x50020500, flightrec "FLTR" 0x50012000, …)
 *               all sit inside this single 2 MiB window, which is why the
 *               DTB sized it exactly one L2 block.
 *
 * Both windows are read/written ONLY by EL2 code (the hypervisor's own
 * functions, plus CPU1's dedicated debug core, which per smp.c's design
 * never enters EL1/the guest) — stage-2 translation applies ONLY to
 * EL1/EL0 (the guest), never to EL2, so excluding these windows from the
 * guest's stage-2 map cannot break the hypervisor's own access to them.
 *
 * H2(a) (2026-07-24): CPU0's own EL2 runtime stack (smp_stacks[0], see
 * smp.h) rides inside the hv-image window above for free — it's a plain
 * .bss array linked into this same image, so HVIMG_L2_IDX already covers it.
 * Before this fix CPU0 ran on U-Boot's inherited SP instead, which lives
 * OUTSIDE both windows below (in plain identity-mapped guest DRAM) and thus
 * had no stage-2 protection at all — see start.S's header comment for the
 * full writeup. No change was needed here: the existing HVIMG carve-out
 * already had room (link.ld's 2 MiB ASSERT is the safety net that would
 * have caught it if not).
 *
 * SHARES stage2_l2_dram[] WITH stage2_unmap_guest_vector() below (both are
 * splits of the SAME 1 GiB DRAM block, IPA 0x40000000-0x7FFFFFFF). This
 * function (called from stage2_init(), i.e. BEFORE the vector-page probe
 * arms) builds the canonical, fully-populated table; stage2_unmap_guest_
 * vector() then only overwrites its OWN single entry (index VEC_L2_IDX=52,
 * disjoint from HVIMG_L2_IDX=16 and HVSCR_L2_IDX=128) rather than rebuilding
 * from scratch — see that function's own comment for why. */
#define HVIMG_BASE      0x42000000UL   /* DTB reserved-memory hv-image@... */
#define HVSCR_BASE      0x50000000UL   /* DTB reserved-memory hv-scratch@..*/

#define HVIMG_L2_IDX  ((unsigned)((HVIMG_BASE - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))
#define HVSCR_L2_IDX  ((unsigned)((HVSCR_BASE - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))

#ifdef HV_HDMI
/* HDMI framebuffer window (hdmi.h HDMI_FB_BASE = 0x4D000000, 1920x1080x4 ≈
 * 7.9 MiB → 8 MiB = 4 L2 blocks). Only carved when the HV drives the display:
 * EL2 and the DE2 scanout DMA own this DRAM, so it is excluded from the guest's
 * stage-2 map (a guest write there faults, exactly like hv-image/hv-scratch)
 * AND reserved no-map in the guest DTB (hv-fb@4d000000) so FreeBSD's allocator
 * never lands on it. The two MUST move together — without the DTB reservation
 * the guest could allocate here and take an A1 fault, which is why this carve
 * is gated on the same HV_HDMI that adds hdmi_init() to the boot path. */
#define HVFB_BASE       0x4D000000UL
#define HVFB_SIZE       0x800000UL     /* 8 MiB */
#define HVFB_L2_IDX     ((unsigned)((HVFB_BASE - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))
#define HVFB_L2_COUNT   ((unsigned)(HVFB_SIZE >> STAGE2_L2_BLOCK_SHIFT))

/* HV_FB_GUEST: share ONLY the front scanout buffer with the guest.
 *
 * The default above is right when the HV owns the screen: it draws the HUD there
 * and a guest write would corrupt it. For a paravirtual DISPLAY the guest has to
 * produce the pixels -- FreeBSD's simplefb(4) binds a `simple-framebuffer` DTB
 * node, maps the region pmap_mapdev_attr(VM_MEMATTR_WRITE_COMBINING) and draws
 * the vt console straight into it, which cannot work against an invalid stage-2
 * entry.
 *
 * Granularity matters here and is the reason this is not a one-line change.
 * One buffer is 1280*720*4 = 0x384000 = 3.52 MiB, so it is NOT a multiple of the
 * 2 MiB stage-2 L2 block: sharing "the front buffer" cannot be expressed in
 * blocks at all. Sharing the whole 8 MiB window instead would hand the guest
 * BUF1 as well -- the buffer the HV flips to -- for no reason. So:
 *
 *   block 104  0x4D000000..0x4D200000   entirely inside BUF0  -> shared, 2 MiB block
 *   block 105  0x4D200000..0x4D400000   holds the BUF0/BUF1 boundary at
 *                                       0x4D384000 -> split into 4 KiB pages,
 *                                       L3[0..387] shared, L3[388..511] invalid
 *   block 106  0x4D400000..0x4D600000   BUF1            -> invalid
 *   block 107  0x4D600000..0x4D800000   BUF1 tail/spare -> invalid
 *
 * Exactly one block needs an L3 table, and every index above is DERIVED from
 * hv_addrmap.h's constants below rather than written out, so a mode change that
 * moves the geometry cannot silently leave these stale. The _Static_asserts
 * pin the arithmetic to the numbers in this comment.
 *
 * The L3 split reuses the technique stage2_build_mmio_tables() already applies
 * to the UART and GICD pages -- same helpers, same shape, one more table.
 *
 * The DTB reservation stays either way. `no-map` keeps FreeBSD's page allocator
 * off this DRAM; the stage-2 mapping is what lets simplefb's own accesses
 * complete. Separate mechanisms, and a shared framebuffer needs both -- without
 * the reservation the guest could allocate its own pages on top of the buffer it
 * is scanning out.
 *
 * Isolation posture, stated rather than left implicit: the guest CAN reach BUF0
 * and CANNOT reach BUF1, hv-image or hv-scratch. stage2_isolation_selfcheck()
 * checks both directions instead of dropping the check.
 *
 * HV_FB_GUEST also suppresses hud_init()/hud_update() in main_dbg.c: a per-frame
 * HUD refresh on CPU1 would overwrite the guest's console every frame, which
 * would look exactly like "the guest's output never appears".
 */
#ifdef HV_FB_GUEST
#define HVFB_GUEST_SHARED 1

#define HVFB_BUF0_END   (HVMAP_FB_BUF0_BASE + HVMAP_FB_BUF_SIZE)
/* Last L2 block wholly contained in BUF0, and the block holding the boundary. */
#define HVFB_SPLIT_L2_IDX \
	((unsigned)((HVFB_BUF0_END - STAGE2_DRAM_BASE) >> STAGE2_L2_BLOCK_SHIFT))
#define HVFB_SPLIT_BLOCK_BASE \
	(STAGE2_DRAM_BASE + ((uint64_t)HVFB_SPLIT_L2_IDX << STAGE2_L2_BLOCK_SHIFT))
/* First L3 entry in that block that is NO LONGER BUF0. */
#define HVFB_SPLIT_L3_IDX \
	((unsigned)((HVFB_BUF0_END - HVFB_SPLIT_BLOCK_BASE) >> STAGE2_L3_PAGE_SHIFT))

_Static_assert(HVFB_L2_IDX == 104u,
               "hv-fb no longer starts at L2 block 104 -- the block map in the "
               "comment above is stale");
_Static_assert(HVFB_SPLIT_L2_IDX == 105u,
               "the BUF0/BUF1 boundary moved out of L2 block 105");
_Static_assert(HVFB_SPLIT_L3_IDX == 388u,
               "the BUF0/BUF1 boundary moved off L3 page 388");
_Static_assert(HVFB_SPLIT_L3_IDX <= STAGE2_L3_ENTRIES,
               "BUF0 end lies outside the split block entirely");
_Static_assert(HVMAP_FB_BUF1_BASE == HVFB_BUF0_END,
               "BUF1 no longer begins exactly where BUF0 ends -- sharing BUF0 by "
               "page would leak part of BUF1 or hide part of BUF0");
#else
#define HVFB_GUEST_SHARED 0
#endif
#endif  /* HV_HDMI */

/* Default XN for a freshly-built guest-DRAM leaf: 0 (executable — today's
 * behavior, and what every target still gets since STAGE2_WX_DYNAMIC
 * defaults to 0 in stage2.h) or 1 (writable-but-execute-never, the starting
 * state the opt-in dynamic W^X mechanism flips away from on demand — see
 * stage2.h's STAGE2_WX_DYNAMIC block and docs/wx-enforcement.md). Used at
 * every call site that used to hard-code a literal xn argument of 0 for a
 * DRAM leaf, so there is exactly one place this policy is decided. When the
 * flag is 0 this expands to the literal constant 0 those call sites already
 * had — byte-for-byte the same descriptors as before this change. */
#define STAGE2_DRAM_XN_DEFAULT ((unsigned)STAGE2_WX_DYNAMIC)

/* Build the level-2 DRAM table for the 1 GiB block starting at `block_base`
 * into that block's row of stage2_l2_dram[]: identity Normal-WB 2 MiB
 * blocks everywhere, EXCEPT the hv-image/hv-scratch entries, left INVALID
 * (all-zero) — those windows only exist in the FIRST DRAM block, which the
 * block_base == STAGE2_DRAM_BASE tests below key on. Called for EVERY DRAM
 * 1 GiB block (stage2_init()), not just the one holding HV windows: the W^X
 * flip machinery needs a per-block L2 table it can split, wherever the
 * guest ends up executing from. Returns the built row's PA for the caller
 * to install. */
static uint64_t
stage2_build_dram_table(uint64_t block_base)
{
	unsigned b = (unsigned)((block_base - STAGE2_DRAM_BASE)
	                        >> STAGE2_BLOCK_SHIFT);

	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (block_base == STAGE2_DRAM_BASE &&
		    (i == HVIMG_L2_IDX || i == HVSCR_L2_IDX
#if defined(HV_HDMI) && !HVFB_GUEST_SHARED
		     || (i >= HVFB_L2_IDX && i < HVFB_L2_IDX + HVFB_L2_COUNT)
#endif
		    )) {
			stage2_l2_dram[b][i] = 0;   /* INVALID: HV image / scratch / framebuffer */
			continue;
		}

#if defined(HV_HDMI) && HVFB_GUEST_SHARED
		/* Front buffer shared with the guest, back buffer NOT. Blocks past
		 * the boundary block are BUF1 and stay invalid; the boundary block
		 * itself becomes a table so the split lands on a 4 KiB page. Blocks
		 * wholly inside BUF0 need no special case -- they fall through to
		 * the ordinary Normal-WB mapping below, which is exactly what a
		 * shared framebuffer wants. */
		if (block_base == STAGE2_DRAM_BASE &&
		    i > HVFB_SPLIT_L2_IDX && i < HVFB_L2_IDX + HVFB_L2_COUNT) {
			stage2_l2_dram[b][i] = 0;   /* INVALID: BUF1 / spare */
			continue;
		}
		if (block_base == STAGE2_DRAM_BASE && i == HVFB_SPLIT_L2_IDX) {
			for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
				if (j >= HVFB_SPLIT_L3_IDX) {
					stage2_l3_hvfb[j] = 0;   /* INVALID: BUF1 */
					continue;
				}
				uint64_t ppa = HVFB_SPLIT_BLOCK_BASE +
				               (uint64_t)j * STAGE2_L3_PAGE_SIZE;
				stage2_l3_hvfb[j] = stage2_page_desc(ppa,
					S2_MEMATTR_NORMAL_WB, S2_SH_INNER,
					/*xn=*/1);   /* framebuffer: never executable */
			}
			stage2_l2_dram[b][i] = stage2_table_desc(
				(uint64_t)(uintptr_t)&stage2_l3_hvfb[0]);
			continue;
		}
#endif
		uint64_t pa = block_base + (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_dram[b][i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/STAGE2_DRAM_XN_DEFAULT);
	}
	return (uint64_t)(uintptr_t)&stage2_l2_dram[b][0];
}

/* (Historical: stage2_dram_block_needs_split() used to gate WHICH DRAM
 * blocks got an L2 table — only the one holding the HV windows, the rest
 * stayed flat 1 GiB descriptors. Removed 2026-08-27 when GUEST_DRAM_2G
 * made the flat case live and it turned out un-flippable under
 * STAGE2_WX_DYNAMIC; every block now gets a table, see stage2_init().) */

/* Forward declarations: the vector-page mutators and stage2_init() live
 * above the W^X section where these are defined (see that section's own
 * comment for the lock's rationale). */
static volatile uint32_t stage2_wx_lock;
static int  stage2_wx_acquire_bounded(void);
static void stage2_wx_unlock(void);

void
stage2_unmap_guest_vector(void)
{
	/* Base of the 2 MiB block that holds the vector page (its level-3
	 * table's coverage), and base of the whole 1 GiB block (only used to
	 * compute l2_block_base below — the level-2 table itself is NOT
	 * rebuilt here, see the comment above stage2_build_dram_table()). */
	uint64_t l1_block_base = (uint64_t)VEC_L1_IDX << STAGE2_BLOCK_SHIFT;
	uint64_t l2_block_base = l1_block_base +
		((uint64_t)VEC_L2_IDX << STAGE2_L2_BLOCK_SHIFT);

	/* Runs on CPU0 during guest bring-up, but takes the same lock as the
	 * W^X flips: a vCPU that has already launched could fault into this
	 * same 2 MiB block's neighborhood concurrently. Unconditional hold is
	 * safe here — firstfault_handle() runs before the guest is entered
	 * and after stage2_init() zeroed the lock. */
	stage2_wx_acquire_bounded();

	/* Level-3: identity Normal-WB 4 KiB pages across the 2 MiB block
	 * (executable unless STAGE2_WX_DYNAMIC opts into the writable-but-XN
	 * default — see STAGE2_DRAM_XN_DEFAULT's comment), EXCEPT the vector
	 * page itself (invalid -> stage-2 abort). */
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == VEC_L3_IDX) {
			stage2_l3_vec[j] = 0;   /* INVALID: the trapped vector page */
			continue;
		}
		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
		stage2_l3_vec[j] = stage2_page_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/STAGE2_DRAM_XN_DEFAULT);
	}

	/* A1 NOTE: the first DRAM block's stage2_l2_dram[0][] row was already
	 * fully built by stage2_init() (via stage2_build_dram_table(), which
	 * every DRAM block gets) BEFORE this function ever runs — see its own
	 * comment above. Replacing all 512 entries here, as an earlier version
	 * of this function did, would silently re-identity-map (and thus
	 * un-protect) the HVIMG_L2_IDX/HVSCR_L2_IDX exclusions. Only this ONE
	 * entry (VEC_L2_IDX=52, disjoint from both) is touched. */
	stage2_l2_dram[0][VEC_L2_IDX] =
		stage2_table_desc((uint64_t)(uintptr_t)&stage2_l3_vec[0]);

	/* stage2_l1[0][VEC_L1_IDX] already points at that row — the install
	 * happened in stage2_init(), which gives EVERY DRAM block a table
	 * descriptor (GUEST_VECTOR_IPA and HVIMG_BASE/HVSCR_BASE all sit in
	 * the same first 1 GiB span). Just flush the combined stage-1+2 TLB so
	 * the walker picks up the level-3 split before the guest runs. */
	stage2_tlb_flush();
	stage2_wx_unlock();
}

void
stage2_map_guest_vector(void)
{
	/* Restore the single vector page to a plain identity Normal-WB
	 * executable page (the rest of the split tables already are), then
	 * flush. Called by firstfault_handle() once it has latched the
	 * original fault, so the guest's re-entered vector fetch now succeeds
	 * and no further stage-2 abort loops in EL2. Assumes
	 * stage2_unmap_guest_vector() already built stage2_l3_vec[]. Same
	 * lock discipline as the unmap side above. */
	stage2_wx_acquire_bounded();
	uint64_t pa = GUEST_VECTOR_IPA & STAGE2_L3_ADDR_MASK;
	stage2_l3_vec[VEC_L3_IDX] = stage2_page_desc(pa,
		S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/STAGE2_DRAM_XN_DEFAULT);
	stage2_tlb_flush();
	stage2_wx_unlock();
}

/* ------------------------------------------------------------------ *
 * VTCR_EL2 — stage-2 translation control.
 *
 *   T0SZ    = 24            bits[5:0]    IPA size = 64 - T0SZ = 40 bits.
 *                                        40-bit IPA matches the A64's
 *                                        typical 40-bit PARange (checked
 *                                        at runtime below, not assumed)
 *                                        and comfortably covers the
 *                                        0..0x80000000 identity range
 *                                        with headroom for a real guest
 *                                        physical map later.
 *   SL0     = 1              bits[7:6]   Start level 1 (see table-topology
 *                                        comment in stage2.h): gives us
 *                                        1 GiB block descriptors directly
 *                                        at the first table level, with
 *                                        exactly 2 concatenated level-1
 *                                        tables (required by T0SZ=24 —
 *                                        see stage2_l1's comment above).
 *   IRGN0   = 0b01           bits[9:8]   Normal, Inner Write-Back,
 *                                        Read/Write-Allocate — cacheable
 *                                        stage-2 table walks.
 *   ORGN0   = 0b01           bits[11:10] Same, Outer Write-Back RW-Alloc.
 *   SH0     = 0b11           bits[13:12] Inner Shareable table walks —
 *                                        matches the DRAM leaf attributes
 *                                        and this being a single-core-at-
 *                                        this-milestone coherent system.
 *   TG0     = 0b00           bits[15:14] 4 KiB granule.
 *   PS      = ID_AA64MMFR0_EL1.PARange bits[18:16] — read at runtime
 *                                        rather than hard-coded, so VTCR
 *                                        always matches what this specific
 *                                        core actually implements (A64 /
 *                                        Cortex-A53 is expected to report
 *                                        0b010 = 40-bit PA, which is also
 *                                        why 40-bit IPA above was chosen —
 *                                        IPA and PA line up 1:1 for this
 *                                        identity map).
 *   bit[31] = 1 (RES1)                   Architecturally reserved-as-one
 *                                        in VTCR_EL2; written for
 *                                        correctness/forward-compat, not
 *                                        because it changes behavior here.
 *
 * Everything else (VS/VMID size, HA/HD hardware AF/dirty management,
 * NSA/NSW) is left 0 — 8-bit VMID (we only ever use VMID 0), no hardware
 * access-flag/dirty-bit management (we set AF=1 in every descriptor by
 * hand instead), no NS bit games (single security state at this
 * milestone).
 * ------------------------------------------------------------------ */
#define VTCR_T0SZ_SHIFT   0
#define VTCR_T0SZ_VAL     24ull   /* 64 - 40 = 40-bit IPA */

#define VTCR_SL0_SHIFT    6
#define VTCR_SL0_VAL      1ull    /* start level 1, concatenated x2 */

#define VTCR_IRGN0_SHIFT  8
#define VTCR_IRGN0_VAL    1ull    /* Normal WB, RW-Allocate (inner) */

#define VTCR_ORGN0_SHIFT  10
#define VTCR_ORGN0_VAL    1ull    /* Normal WB, RW-Allocate (outer) */

#define VTCR_SH0_SHIFT    12
#define VTCR_SH0_VAL      3ull    /* Inner Shareable */

#define VTCR_TG0_SHIFT    14
#define VTCR_TG0_VAL      0ull    /* 4 KiB granule */

#define VTCR_PS_SHIFT     16

#define VTCR_RES1_BIT     (1ull << 31)

static inline uint64_t
read_id_aa64mmfr0_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
	return v;
}

static inline uint64_t
read_vtcr_el2(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, vtcr_el2" : "=r"(v));
	return v;
}

static inline void
write_vtcr_el2(uint64_t v)
{
	__asm__ volatile("msr vtcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

static inline uint64_t
read_vttbr_el2(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, vttbr_el2" : "=r"(v));
	return v;
}

static inline void
write_vttbr_el2(uint64_t v)
{
	__asm__ volatile("msr vttbr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

static inline uint64_t
read_hcr_el2(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(v));
	return v;
}

static inline void
write_hcr_el2(uint64_t v)
{
	__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

/* HCR_EL2.VM — bit 0, stage-2 translation enable. We only ever OR/AND this
 * single bit; RW (bit31, set by guest.c), IMO (bit4, set by gic_timer.c)
 * and anything else already programmed are read-modify-write preserved. */
#define HCR_EL2_VM  (1ull << 0)

static void
stage2_tlb_flush(void)
{
	/* Leading dsb ish: the stage-2 tables are built with PLAIN cacheable
	 * stores into .bss (stage2_l*[...] = desc), NOT through a clean+barrier
	 * helper, so this barrier is what guarantees every descriptor store is
	 * complete in the inner-shareable domain BEFORE the invalidate — else the
	 * tlbi (and the walker after it) may race a not-yet-visible table store.
	 * vmalls12e1: invalidate stage-1 AND stage-2 combined TLB entries for all
	 * VMIDs at EL1 — the correct broad flush any time VTCR/VTTBR or HCR.VM just
	 * changed. Trailing dsb ish + isb: make the invalidation globally observed
	 * and stop any instruction fetched after this from using a stale walk. */
	__asm__ volatile("dsb ish\n\ttlbi vmalls12e1\n\tdsb ish\n\tisb" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * Public API (see stage2.h for the full contract).
 * ------------------------------------------------------------------ */

void
stage2_init(void)
{
	stg2_bc(STG2_MAGIC_IDX, STG2_MAGIC);

	/* Rebuild both concatenated tables from scratch (idempotent). */
	for (unsigned t = 0; t < STAGE2_L1_TABLES; t++)
		for (unsigned i = 0; i < STAGE2_L1_ENTRIES; i++)
			stage2_l1[t][i] = 0;

	/* Same WDT-warm-reset discipline as EMAC's TX lock: DRAM survives the
	 * reset, so a stale held-lock from the previous boot must never
	 * deadlock the fresh one. Zero it BEFORE anything can flip. */
	stage2_wx_lock = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t ndesc = 0;

	/* Index 0: the low 1 GiB SoC MMIO cluster (GIC 0x01c81000, EMAC
	 * 0x01c30000, CCU 0x01c20000, UART, etc., all inside
	 * 0x00000000-0x3FFFFFFF). No longer a single 1 GiB block — now a
	 * TABLE descriptor down to a level-2/level-3 hierarchy
	 * (stage2_build_mmio_tables()) so the single UART0_BASE page can be
	 * left unmapped (faults to EL2 for vconsole.c to emulate) while
	 * every other MMIO byte stays identity-mapped exactly as before. */
	{
		/* Breadcrumb (SRAM): confirm stage2_init reached this point */
		*(volatile uint32_t *)0x0001900c = 0x53544732;  /* "STG2" in init */

		unsigned idx = (unsigned)(STAGE2_MMIO_BASE >> STAGE2_BLOCK_SHIFT);
		__asm__ volatile("nop" ::: "memory");  /* prevent optimization from skipping */
		stg2_bc(12, 0x42444752);  /* "BDGR" right before stage2_build_mmio_tables() */
		uint64_t l2_pa = stage2_build_mmio_tables();
		stg2_bc(13, 0x4146544F);  /* "AFTO" right after stage2_build_mmio_tables() */
		stage2_l1[0][idx] = stage2_table_desc(l2_pa);
		ndesc++;
	}

	/* Index 1..N: EVERY DRAM 1 GiB block gets a TABLE descriptor down to
	 * its own level-2 table (that block's row of stage2_l2_dram[], built
	 * by stage2_build_dram_table()). There is deliberately no flat 1 GiB
	 * block any more: with STAGE2_WX_DYNAMIC the leaves start RW+XN, and a
	 * flat 1 GiB descriptor cannot be flipped per-region — measured live
	 * 2026-08-27, the first 2G guest's userland exec fault (FreeBSD
	 * allocates from the top of RAM, i.e. the second GiB) had no owner and
	 * stormed at 45 kHz. Identity Normal-WB everywhere, XN default from
	 * STAGE2_DRAM_XN_DEFAULT, HV windows left INVALID inside the first
	 * block's row by stage2_build_dram_table() itself. */
	{
		unsigned nblocks = STAGE2_DRAM_L1_BLOCKS;
		unsigned base_idx = (unsigned)(STAGE2_DRAM_BASE >> STAGE2_BLOCK_SHIFT);
		for (unsigned b = 0; b < nblocks; b++) {
			uint64_t pa = STAGE2_DRAM_BASE + (uint64_t)b * STAGE2_BLOCK_SIZE;
			uint64_t l2_pa = stage2_build_dram_table(pa);
			stage2_l1[0][base_idx + b] = stage2_table_desc(l2_pa);
			ndesc++;
		}
	}

	/* VTCR_EL2 + VTTBR_EL2 are BANKED PER-PE, so programming them is a
	 * per-core act even though the tables they point at are global. Factored
	 * into stage2_program_this_pe() so a second guest vCPU can arm the same
	 * tables on its own copies -- see stage2_arm_secondary() and vcpu2.c.
	 * Leaving this inline was a real bug: CPU2 entered EL1 with VM=1 but a
	 * ZERO VTTBR/VTCR, so its very first instruction fetch took a stage-2
	 * translation fault at level 0 (ESR 0x82000004), 5.7 million times. */
	stage2_program_this_pe();
	stg2_bc(STG2_NDESC_IDX, ndesc);

	/* Self-check: walk our own top-level table in software for
	 * STAGE2_SELFTEST_IPA and confirm it decodes back to the same PA
	 * (i.e. the identity property actually holds for the descriptor we
	 * built, independent of whether the MMU hardware agrees).
	 *
	 * A1 UPDATE: the level-1 DRAM entry can now be EITHER a plain 1 GiB
	 * BLOCK (bits[1:0]=0b01) or, for a block containing an excluded window
	 * (see stage2_dram_block_needs_split()), a TABLE (bits[1:0]=0b11) down
	 * to stage2_l2_dram[]. Both descriptor-type bits must be checked
	 * (desc & 0x3, not the old single-bit S2_DESC_VALID_BLOCK mask, which
	 * only tested bit0 — true for both a block AND a table descriptor, so
	 * it would have silently mis-resolved a table's address as if it were
	 * a 1 GiB-aligned block base). The table case walks one more level
	 * into stage2_l2_dram[] to find the actual leaf. STAGE2_SELFTEST_IPA
	 * is chosen (see stage2.h) to land on a plain L2 block entry in that
	 * table, never one of the excluded/further-split indices. */
	{
		unsigned idx = (unsigned)(STAGE2_SELFTEST_IPA >> STAGE2_BLOCK_SHIFT);
		uint64_t desc = stage2_l1[0][idx];
		uint32_t pass = 0;
		uint64_t desc_type = desc & 0x3ull;
		if (desc_type == S2_DESC_VALID_BLOCK) {
			uint64_t block_pa = desc & STAGE2_BLOCK_ADDR_MASK;
			uint64_t offset = STAGE2_SELFTEST_IPA & (STAGE2_BLOCK_SIZE - 1u);
			if ((block_pa | offset) == STAGE2_SELFTEST_IPA)
				pass = 1;
		} else if (desc_type == S2_DESC_VALID_TABLE) {
			uint64_t l2_table_pa = desc & STAGE2_TABLE_ADDR_MASK;
			const uint64_t *l2 = (const uint64_t *)(uintptr_t)l2_table_pa;
			unsigned l2_idx = (unsigned)((STAGE2_SELFTEST_IPA >> STAGE2_L2_BLOCK_SHIFT)
				% STAGE2_L2_ENTRIES);
			uint64_t l2_desc = l2[l2_idx];
			if ((l2_desc & 0x3ull) == S2_DESC_VALID_BLOCK) {
				uint64_t block_pa = l2_desc & STAGE2_L2_ADDR_MASK;
				uint64_t offset = STAGE2_SELFTEST_IPA & (STAGE2_L2_BLOCK_SIZE - 1u);
				if ((block_pa | offset) == STAGE2_SELFTEST_IPA)
					pass = 1;
			}
		}
		stg2_bc(STG2_SELFCHECK_IDX, pass);
	}

	/* HCR_EL2.VM is deliberately untouched here — stage2_enable() is a
	 * separate, explicit step. */
}

/*
 * purpose:     Program THIS PE's banked stage-2 control registers (VTCR_EL2,
 *              VTTBR_EL2) to point at the already-built global tables.
 * input:       none (reads stage2_l1[][] and this core's ID_AA64MMFR0_EL1)
 * output:      none
 * sideEffects: writes VTCR_EL2/VTTBR_EL2 on the calling core; publishes both
 *              readbacks to the stage-2 breadcrumb window
 *
 * Called by stage2_init() for the boot core and by stage2_arm_secondary() for
 * any further guest vCPU. Builds no tables -- the descriptors are global and
 * shared; only the pointers to them are per-PE.
 */
void
stage2_program_this_pe(void)
{
	/* PS comes from THIS core's actual PARange, read at runtime. */
	uint64_t parange = read_id_aa64mmfr0_el1() & 0xFull; /* bits[3:0] */
	uint64_t vtcr = 0;
	uint64_t table_base;

	vtcr |= VTCR_T0SZ_VAL  << VTCR_T0SZ_SHIFT;
	vtcr |= VTCR_SL0_VAL   << VTCR_SL0_SHIFT;
	vtcr |= VTCR_IRGN0_VAL << VTCR_IRGN0_SHIFT;
	vtcr |= VTCR_ORGN0_VAL << VTCR_ORGN0_SHIFT;
	vtcr |= VTCR_SH0_VAL   << VTCR_SH0_SHIFT;
	vtcr |= VTCR_TG0_VAL   << VTCR_TG0_SHIFT;
	vtcr |= parange        << VTCR_PS_SHIFT;
	vtcr |= VTCR_RES1_BIT;
	write_vtcr_el2(vtcr);
	stg2_bc(STG2_VTCR_IDX, (uint32_t)read_vtcr_el2());

	/* VMID = 0 (bits[63:48] left zero) + the base of the concatenated
	 * level-1 tables. One VMID: both vCPUs are the same guest. */
	table_base = (uint64_t)(uintptr_t)&stage2_l1[0][0];
	write_vttbr_el2(table_base);
	stg2_bc(STG2_VTTBR_IDX, (uint32_t)read_vttbr_el2());
	stg2_bc(STG2_TABLE_BASE_IDX, (uint32_t)table_base);
}

/*
 * purpose:     Bring a SECONDARY guest vCPU's stage-2 regime up on the calling
 *              core, against the tables stage2_init() already built.
 * input:       none
 * output:      none
 * sideEffects: programs this core's VTCR_EL2/VTTBR_EL2, sets HCR_EL2.VM,
 *              flushes this core's stage-2 TLBs
 *
 * stage2_init() must already have run (on the boot core) -- this deliberately
 * builds nothing, so two vCPUs of one guest cannot end up with two different
 * views of memory. Not idempotent-checked: calling it twice on one core simply
 * reprograms the same values.
 */
void
stage2_arm_secondary(void)
{
	stage2_program_this_pe();
	stage2_enable();
}

void
stage2_enable(void)
{
	uint64_t hcr = read_hcr_el2();
	hcr |= HCR_EL2_VM;
	write_hcr_el2(hcr);
	stage2_tlb_flush();
	stg2_bc(STG2_HCR_IDX, (uint32_t)read_hcr_el2());
}

void
stage2_disable(void)
{
	uint64_t hcr = read_hcr_el2();
	hcr &= ~HCR_EL2_VM;
	write_hcr_el2(hcr);
	stage2_tlb_flush();
	stg2_bc(STG2_HCR_IDX, (uint32_t)read_hcr_el2());
}

/* ------------------------------------------------------------------ *
 * Combined-translation coherency proof.
 *
 * `AT S12E1R` performs the exact translation the CPU's hardware table-
 * walker does for an EL1 read: it walks the current EL1 stage-1 regime
 * AND our stage-2 tables, and deposits the result (PA + combined memory
 * attributes, or a fault indication) in PAR_EL1. This is the definitive
 * check that stage-2's cacheability/shareability for DRAM is not silently
 * downgrading (or faulting) the accesses the guest's own page-table walker
 * makes through it.
 *
 * IMPORTANT timing note: this uses the CURRENT EL1 stage-1 state
 * (SCTLR_EL1/TCR_EL1/TTBR*_EL1). At the point main_fbsd.c enables stage-2
 * the guest kernel has NOT yet run, so EL1 stage-1 is OFF (SCTLR_EL1.M=0,
 * as guest_config() left it). With stage-1 disabled the input VA is used
 * as a flat IPA and only stage-2 is applied — which is exactly what we
 * want to prove here: that our stage-2 DRAM mapping resolves
 * STAGE2_SELFTEST_IPA to the same PA as Normal/WB/Inner-Shareable memory.
 * PAR_EL1.F==0 with PA==input and ATTR indicating Normal-WB is the
 * pass signal; PAR_EL1.F==1 would mean the combined walk faulted.
 *
 * PAR_EL1 (successful, F==0) layout used to read the result:
 *   bit[0]    F      0 = success
 *   bits[10:9] SH    shareability of the translation
 *   bits[47:12] PA   output physical address (of the input page)
 *   bits[63:56] ATTR memory attributes (MAIR-style byte): 0xFF = Normal
 *                    Inner+Outer WB RW-alloc (the coherent case we want).
 */
static uint64_t
stage2_at_s12e1r(uint64_t va)
{
	uint64_t par;
	/* AT S12E1R, <Xt>: translate `va` as an EL1 read through stage-1+2.
	 * isb ensures the AT (a context-changing op wrt PAR_EL1) has
	 * completed before we read PAR_EL1 back. */
	__asm__ volatile(
		"at s12e1r, %1\n\t"
		"isb\n\t"
		"mrs %0, par_el1"
		: "=r"(par)
		: "r"(va)
		: "memory");
	return par;
}

void
stage2_at_check(uint64_t va)
{
	uint64_t par = stage2_at_s12e1r(va);
	stg2_bc(STG2_PAR_LO_IDX, (uint32_t)par);
	stg2_bc(STG2_PAR_HI_IDX, (uint32_t)(par >> 32));
}

/* ------------------------------------------------------------------ *
 * A1 isolation self-check (ROADMAP milestone A1 DoD: "гость пишет в
 * HV-адрес → перехват"). Proves — in HARDWARE, via the CPU's own
 * table-walker, not by re-reading our descriptors in software — that the
 * guest's EL1 translation regime physically CANNOT reach the two HV DRAM
 * windows (hv-image / hv-scratch). `AT S12E1W` walks the CURRENT EL1
 * stage-1 regime AND our stage-2 tables for a WRITE and deposits the
 * result in PAR_EL1. At the point this runs (right after stage2_enable() +
 * stage2_unmap_guest_vector(), before the guest kernel executes) EL1
 * stage-1 is OFF (SCTLR_EL1.M=0), so the input VA is used as a flat IPA and
 * ONLY stage-2 is applied — exactly the mapping a guest write would hit.
 *
 * PAR_EL1.F (bit0): 1 => the combined walk FAULTED (address unreachable) —
 * this is the WANTED result for an HV window; 0 => it translated (reachable)
 * — wanted for the control address. So a passing boundary is:
 *   HVIMG.F == 1  &&  HVSCR.F == 1  &&  control.F == 0
 *
 * Breadcrumbs (STG2 window 0x50000c00 — itself inside the hv-scratch window,
 * written by EL2 so stage-2 never gates it):
 *   [14] magic 0x49534f4c ("ISOL")   [15] HVIMG PAR.F (want 1)
 *   [16] HVSCR PAR.F (want 1)         [17] control PAR.F (want 0)
 *   [18] overall pass (1 = boundary proven, 0 = a window is reachable!)
 * ------------------------------------------------------------------ */
static uint64_t
stage2_at_s12e1w(uint64_t va)
{
	uint64_t par;
	/* AT S12E1W, <Xt>: translate `va` as an EL1 WRITE through stage-1+2. */
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
stage2_isolation_selfcheck(void)
{
	uint32_t hvimg_f   = (uint32_t)(stage2_at_s12e1w(HVIMG_BASE) & 1u);
	uint32_t hvscr_f   = (uint32_t)(stage2_at_s12e1w(HVSCR_BASE) & 1u);
	uint32_t control_f = (uint32_t)(stage2_at_s12e1w(STAGE2_SELFTEST_IPA) & 1u);
#if defined(HV_HDMI) && HVFB_GUEST_SHARED
	/* Paravirtual display (HV_FB_GUEST): the posture is DELIBERATELY split, so
	 * check both halves rather than dropping the check. BUF0 is shared with the
	 * guest and must translate; BUF1 is the HV's own flip target and must still
	 * fault. Testing only one of those would let the sharing edit silently open
	 * the back buffer too -- which is the mistake this check exists to catch. */
	uint32_t hvfb_f     = (uint32_t)(stage2_at_s12e1w(HVMAP_FB_BUF0_BASE) & 1u);
	uint32_t hvfb1_f    = (uint32_t)(stage2_at_s12e1w(HVMAP_FB_BUF1_BASE) & 1u);
	uint32_t hvfb_want  = 0u;   /* BUF0 REACHABLE on purpose */
#elif defined(HV_HDMI)
	/* HV owns the display: the framebuffer window is a third excluded region and
	 * must be equally unreachable from the guest. */
	uint32_t hvfb_f     = (uint32_t)(stage2_at_s12e1w(HVFB_BASE) & 1u);
	uint32_t hvfb1_f    = 1u;   /* whole window carved; nothing separate to test */
	uint32_t hvfb_want  = 1u;
#else
	uint32_t hvfb_f     = 1u;   /* not carved in this build; treat as "held" */
	uint32_t hvfb1_f    = 1u;
	uint32_t hvfb_want  = 1u;
#endif

	/* Boundary holds iff every HV window behaves as this build intends and the
	 * control DRAM address still translates (F=0 — proves the check itself isn't
	 * just faulting on everything). hvfb_want is 1 in every build except the
	 * shared-display one, where BUF0 reachability IS the intended state. */
	uint32_t pass = (hvimg_f == 1u && hvscr_f == 1u &&
	                 hvfb_f == hvfb_want && hvfb1_f == 1u &&
	                 control_f == 0u) ? 1u : 0u;

	stg2_bc(14, 0x49534f4c);   /* "ISOL" */
	stg2_bc(15, hvimg_f);
	stg2_bc(16, hvscr_f);
	stg2_bc(17, control_f);
	stg2_bc(18, pass);
	stg2_bc(10, hvfb_f);       /* BUF0 PAR.F: 1 = carved, 0 = shared on purpose */
	stg2_bc(11, hvfb1_f);      /* BUF1 PAR.F: must be 1 in every build */
	return (int)pass;
}

/* ------------------------------------------------------------------ *
 * W^X self-check (ROADMAP v1 gate: "W^X на гостевых маппингах" — the last
 * unchecked half of the isolation bullet; the other half, "HV вырезан из
 * stage-2" / "гость пишет в HV-регион -> перехват", is stage2_isolation_
 * selfcheck() above, already hardware-proven).
 *
 * WHAT THIS PROVES, EXACTLY: a general, exhaustive walk of every valid leaf
 * this file's tables actually contain (not a sample of known addresses,
 * unlike stage2_isolation_selfcheck() above) — for each one, is it
 * simultaneously writable (S2AP write bit set) and executable (XN==0)?
 *
 * CURRENT MEASURED STATE (verified by this function and pinned board-free in
 * test_stage2_tables.c's W^X section): every MMIO/device leaf this file ever
 * builds is XN=1 (stage2_build_mmio_tables() passes xn=1 to every call, with
 * no exception) — that half of W^X is real, permanent, and enforced by
 * construction. Every guest-DRAM leaf, at every level this file can produce
 * (the flat level-1 1 GiB block in stage2_init(), the level-2 blocks in
 * stage2_build_dram_table(), and the level-3 pages in stage2_unmap_guest_
 * vector()/stage2_map_guest_vector()), is S2AP=RW *and* XN=0 — writable and
 * executable together. So this self-check WILL currently report violations
 * > 0 on every real boot. That is an honest measurement, not a bug in this
 * function or a regression to chase.
 *
 * WHY GUEST DRAM CANNOT SIMPLY BE FIXED HERE: stage2.c has no notion of the
 * guest's own code/data split. That split lives in the guest ELF's program
 * headers, which kload.c parses (kload_parse_elf()/kload_place_segments(),
 * both of which run BEFORE stage2_init() in main_dbg.c, so the segment
 * geometry — base/size/executable per PT_LOAD — is technically available in
 * time). Inspecting the actual deployed kernel (readelf -lW on the TFTP'd
 * ELF, done while investigating this gate item) shows a genuinely clean
 * split — .text+.plt is R+E only, .rodata is R only, .data/.bss are RW only,
 * no segment is both W and X — so a per-segment stage-2 refinement of the
 * INITIAL kernel image is architecturally sound, not blocked by a messy ELF.
 *
 * But that refinement would only cover the kernel's initial boot-time image.
 * FreeBSD's kldload path allocates NEW physical pages for every loaded
 * kernel module from general free RAM *outside* that image and marks them
 * executable on demand, at run time, with no hypercall or other channel that
 * tells this hypervisor which page just became guest code. A static,
 * boot-time-only split can only do one of two things to the REST of guest
 * DRAM: leave it exactly as writable+executable as today (in which case the
 * "fix" only narrows the initial kernel image and every kldload'd module is
 * still sitting in a W+X page, unchanged), or mark it execute-never (in
 * which case kldload — a real, previously hard-won, hardware-verified
 * feature per project memory, "kldload hang SOLVED" — breaks outright, and
 * nothing in this tree's QEMU CI boots a real FreeBSD guest that exercises
 * kldload to catch that regression board-free). Neither is a genuine W^X
 * close; the second is actively worse than today. That decision needs either
 * a live guest-cooperative protocol (the guest tells the HV when a page
 * becomes code) or a deliberate, hardware-verified acceptance of the kldload
 * regression — this function does not make that call, and per this task's
 * own instruction not to weaken a check (or, symmetrically, break the guest)
 * just to force a pass, neither does the rest of this change. See
 * docs/dma-bypass-stage2.md for this project's existing precedent of writing
 * up exactly this kind of accepted, currently-open gap instead of forcing a
 * close.
 *
 * Breadcrumbs (STG2 window 0x50000c00 — see the layout comment at the top of
 * this file; this self-check's own block, added alongside ISOL's [14..18]):
 *   [19] magic 0x5758434b ("WXCK")
 *   [20] violations    total W+X leaf count found (leaves, not bytes)
 *   [21] first_ipa_lo  low 32 bits of the first (lowest-IPA) violating
 *        leaf's IPA — 0 if violations == 0
 *   [22] pass          1 iff violations == 0 (informational; currently
 *        always 0 on this build — see above, that is the honest state)
 * ------------------------------------------------------------------ */

/* A single leaf found to violate W^X: its IPA and the table level (1 = 1 GiB
 * block, 2 = 2 MiB block, 3 = 4 KiB page) it was found at. Mirrors the shape
 * test_stage2_tables.c's own s2_walk() already decodes per-address; this is
 * the production counterpart that walks the raw tables directly instead of
 * probing one IPA at a time. */
struct stage2_wx_violation {
	uint64_t ipa;
	unsigned level;
};

/* Is a decoded (s2ap, xn) pair simultaneously writable and executable?
 * S2AP[1:0] (descriptor bits[7:6]) encodes {00: none, 01: read-only,
 * 10: write-only, 11: read/write} — bit1 (value 0x2) is the write-enable
 * sub-bit (matching Linux/KVM's own stage-2 PTE bit naming: PTE_S2_RDONLY is
 * 0b01, PTE_S2_RDWR is 0b11, i.e. bit0=read/bit1=write). XN==0 means
 * executable (stage-2's sense is inverted: XN=1 forbids execution — see
 * S2_XN_BIT's own comment above). Every descriptor this file ever builds
 * uses S2AP_RW (0b11) — no S2AP_RO exists yet — so checking the write
 * sub-bit vs. checking `s2ap == S2AP_RW` are equivalent for every leaf this
 * tree currently produces; the sub-bit form is used so this stays correct if
 * a read-only encoding is ever added. */
static inline int
stage2_leaf_is_wx(unsigned s2ap, unsigned xn)
{
	return ((s2ap & 0x2u) != 0u) && (xn == 0u);
}

/* Walk every valid leaf in the concatenated level-1 table set — both
 * STAGE2_L1_TABLES tables, following each valid TABLE descriptor down
 * through level-2 and, where present, level-3 — and count how many are
 * W^X violations (stage2_leaf_is_wx()). A GENERAL walk: it does not assume
 * which regions exist or at which level a given IPA resolves, so it stays
 * correct if the topology changes (STAGE2_DRAM_SIZE grows, a new device
 * window is added, a future fix narrows some leaves to read-only, etc.)
 * without needing a matching edit here.
 *
 * first_violation, if non-NULL, is filled with the first (lowest-IPA, in
 * scan order) offending leaf's IPA + level; left untouched if the return
 * value is 0. Returns the total violation count in leaves, not bytes (three
 * violating 2 MiB blocks count as 3, not 3*0x200000). */
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
				continue;   /* invalid: not guest-accessible at all */

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

					/* Level-3 has no BLOCK encoding, only PAGE (0b11) or
					 * invalid (anything else) — same as every stage2_page_
					 * desc() consumer already relies on. */
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

int
stage2_wx_selfcheck(void)
{
	struct stage2_wx_violation first = { 0, 0 };
	uint32_t violations = stage2_wx_scan(&first);
	uint32_t pass = (violations == 0u) ? 1u : 0u;

	stg2_bc(19, 0x5758434bu);            /* "WXCK" */
	stg2_bc(20, violations);
	stg2_bc(21, (uint32_t)first.ipa);
	stg2_bc(22, pass);
	return (int)pass;
}

/* ------------------------------------------------------------------ *
 * DYNAMIC W^X enforcement for guest DRAM (ROADMAP v1 gate, opt-in — see
 * stage2.h's STAGE2_WX_DYNAMIC block and docs/wx-enforcement.md for the
 * full design, the evidence behind every number in it, and why this is
 * shipped disabled rather than either claiming the gate closed or refusing
 * to write it at all).
 *
 * THE MECHANISM: when STAGE2_WX_DYNAMIC is nonzero, every guest-DRAM leaf
 * this file builds starts life RW+XN (writable, not executable —
 * STAGE2_DRAM_XN_DEFAULT above) instead of today's RW+X. The FIRST
 * instruction fetch from a 4 KiB page therefore takes a stage-2 PERMISSION
 * fault (not a translation fault — the page IS mapped, just not
 * executable); el2_exc.c routes it here (stage2_wx_fault()), which splits
 * that page's 2 MiB block into an L3 table on demand (from the bounded
 * pool below) if it is not split already, then flips that ONE 4 KiB leaf
 * to READ+EXECUTE — and, critically, REVOKES WRITE on it (S2AP_RO, not
 * just XN=0), because a leaf that is executable AND still writable is
 * exactly the W^X violation stage2_wx_scan() above already knows how to
 * detect. A LATER write to that same page (e.g. the module it belongs to
 * is unloaded and the physical page recycled for data) takes the symmetric
 * PERMISSION fault on the data side and flips it back to RW+XN. At every
 * instant, every dynamically-managed leaf is in EXACTLY ONE of those two
 * states — never both W and X — which is what makes this a real W^X
 * mechanism and not merely "sometimes XN".
 *
 * WHY A LEAF NEVER NEEDS EVICTING (bounding Q4's ping-pong risk): the pool
 * below hands out an L3 TABLE once per 2 MiB block, permanently, the first
 * time ANY page in that block executes. After that, EVERY flip (either
 * direction) rewrites one already-allocated L3 entry — it costs one fault
 * and one TLB invalidate, never a new table. So the pool's occupancy is
 * bounded by "how many distinct 2 MiB regions have ever contained guest
 * code this boot", not by how many times any one of them ping-pongs.
 *
 * WHY POOL EXHAUSTION FAILS OPEN, NOT CLOSED: if a brand-new 2 MiB block
 * takes its first execute fault after the pool is already full,
 * stage2_wx_flip() does NOT deny the fetch — denying would hang the guest
 * (a guest stage-2 fault never advances ELR, so a denied fetch just
 * re-faults forever until the watchdog resets the board: exactly the
 * "static split breaks kldload" failure this whole feature exists to
 * avoid). Instead it flips the WHOLE 2 MiB block back to plain RW+X —
 * today's unconditional, hardware-verified behavior for that region — and
 * counts the fallback. stage2_wx_scan() will then honestly report that
 * region as a violation: that is the TRUTH (it is not enforced there) and
 * is the documented, bounded cost of a fixed-size pool, not a bug. See
 * docs/wx-enforcement.md Q2/Q4 for the numbers behind the pool size and why
 * "never freed" is the deliberately simple, bounded-risk choice.
 *
 * SCOPE NOTE: touches one row of stage2_l2_dram[] per 1 GiB DRAM block —
 * every DRAM block HAS such a row since 2026-08-27 (before that the array
 * was a single 1 GiB table and the second GiB of a GUEST_DRAM_2G window
 * was a flat un-flippable XN block, which is exactly what stormed). The
 * row is selected by the block index, the entry within it by the 2 MiB
 * index — see the raw/b/l2_idx split in stage2_wx_flip().
 *
 * CONCURRENCY NOTE: the W^X runtime mutators (stage2_wx_flip(),
 * stage2_unmap_guest_vector(), stage2_map_guest_vector()) all hold the
 * stage2_wx_lock test-and-set across their edit+publish+flush — required
 * since vcpu1/2/3 gave the guest four concurrently-faulting vCPUs
 * (2026-08-27; the note below predates that and described the single-core
 * era). WITHIN one core the reasoning still holds: a core cannot take a
 * second synchronous exception while still inside this handler for the
 * first, so there is no same-core re-entry into the lock. What the lock
 * GUARDS against is cross-core: two vCPUs flipping the same 2 MiB block,
 * and the pool_used counter racing. The tick path and dbgmon never touch
 * these tables, so no lock-ordering hazard exists against them.
 * ------------------------------------------------------------------ */
#if STAGE2_WX_DYNAMIC
#include "hv_addrmap.h"   /* HVMAP_WXDYN_BC / HVMAP_WXDYN_MAGIC */

/* S2AP encoding this file otherwise never needs: every OTHER descriptor
 * helper in stage2.c hardcodes S2AP_RW (see stage2_leaf_is_wx()'s own
 * comment above, which already anticipated this exact addition: "the
 * sub-bit form is used so this stays correct if a read-only encoding is
 * ever added"). Bit layout per the ARMv8-A stage-2 S2AP field: bit1 (0x2)
 * = write-enable, bit0 (0x1) = read-enable — 0b01 is therefore genuinely
 * read-only, not a made-up value. */
#define S2AP_RO   0x1u

/* Bit-for-bit the same layout as stage2_page_desc() above, with an
 * explicit S2AP argument instead of that function's hardcoded S2AP_RW.
 * Deliberately a SEPARATE function rather than adding a parameter to
 * stage2_page_desc() itself: every one of that function's existing call
 * sites (UART/MMIO pages, the vector-page probe, the plain DRAM L3
 * builder below) stays completely untouched by this feature, on every
 * build, flag on or off — the smallest possible blast radius for a change
 * to the most safety-critical file in the tree. */
static uint64_t
stage2_wx_page_desc(uint64_t pa, unsigned s2ap, unsigned xn)
{
	uint64_t d = S2_DESC_VALID_TABLE;   /* page descriptor at level 3 */
	d |= (uint64_t)(S2_MEMATTR_NORMAL_WB & 0xFu) << 2;
	d |= (uint64_t)(s2ap & 0x3u) << 6;
	d |= (uint64_t)(S2_SH_INNER & 0x3u) << 8;
	d |= S2_AF_BIT;
	d |= (pa & STAGE2_L3_ADDR_MASK);
	if (xn)
		d |= S2_XN_BIT;
	return d;
}

/* The bounded pool: STAGE2_WX_POOL_TABLES (stage2.h) on-demand L3 tables.
 * A plain monotonic counter is the whole allocator — matching alloc.h's
 * arena_bump() philosophy (no free, no search: "is this block already
 * split" is answered by the L2 descriptor's OWN type, not a side table, so
 * there is nothing here to look up by IPA). */
static uint64_t stage2_wx_pool[STAGE2_WX_POOL_TABLES][STAGE2_L3_ENTRIES]
	__attribute__((aligned(STAGE2_L3_ENTRIES * 8u)));
static uint32_t stage2_wx_pool_used;
static uint32_t stage2_wx_pool_exhausted;   /* distinct blocks that fell back */
static uint32_t stage2_wx_flip_count;       /* total flips, either direction */

/* The stage-2 table W^X lock. Every runtime mutator of stage2_l2_dram[][] /
 * the W^X L3 pool (stage2_wx_flip(), stage2_unmap_guest_vector(),
 * stage2_map_guest_vector()) holds it across its whole edit+publish+flush.
 * With four guest vCPUs (vcpu1/2/3 armed) the fault handlers run
 * concurrently on four cores, and pre-2026-08-27 this was a real race:
 * two cores flipping the same 2 MiB block could both see a plain BLOCK
 * descriptor, both consume a pool table for it, and the loser's L3 fill
 * would be silently discarded by the winner's table_desc install -- plus
 * the unsynchronized pool_used counter could hand the SAME pool table to
 * two cores at once. ldaxr/stlxr test-and-set, same idiom as
 * emac_tx_trylock() / vblk_emmc_trylock() (A53 has no LSE). Zeroed in
 * stage2_init() with the same WDT-warm-reset discipline as EMAC's TX
 * lock: DRAM survives the reset, so a stale 1 must never deadlock a
 * fresh boot. */
static volatile uint32_t stage2_wx_lock;

static int stage2_wx_trylock(void)
{
    volatile uint32_t *p = &stage2_wx_lock;
    uint32_t prev, status, one = 1u;
    __asm__ volatile(
        "	ldaxr	%w0, [%3]\n"
        "	cbnz	%w0, 1f\n"
        "	stlxr	%w1, %w2, [%3]\n"
        "	b	2f\n"
        "1:	mov	%w1, #1\n"
        "2:\n"
        : "=&r"(prev), "=&r"(status)
        : "r"(one), "r"(p)
        : "memory");
    if (prev == 0u && status == 0u) {
        __asm__ volatile("dsb sy" ::: "memory");
        return 1;
    }
    return 0;
}

static void stage2_wx_unlock(void)
{
    volatile uint32_t *p = &stage2_wx_lock;
    __asm__ volatile("dsb sy" ::: "memory");
    *p = 0u;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* Bounded acquire. A critical section here is one L2 read + at most one
 * 512-entry L3 fill + a full TLB flush -- all bounded, none of it blocks
 * on an external event, so a few thousand spins is generous. A failed
 * acquire (astronomically unlikely: flips are ~2/s aggregate) must NOT
 * hang a guest trap: return 0 and let the caller decide -- stage2_wx_fault()
 * declines the flip (the fault stays unowned exactly as if the block were
 * not DRAM, which today means the generic recorder logs it; a lost flip
 * self-heals on the guest's next identical fault). */
#define STAGE2_WX_LOCK_SPINS  20000u
static int stage2_wx_acquire_bounded(void)
{
    for (uint32_t i = 0; i < STAGE2_WX_LOCK_SPINS; i++) {
        if (stage2_wx_trylock())
            return 1;
        __asm__ volatile("yield" ::: "memory");
    }
    return 0;
}

static inline void
wxd_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_WXDYN_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static void
wxd_publish(uint64_t last_ipa)
{
	wxd_bc(0, HVMAP_WXDYN_MAGIC);
	wxd_bc(1, stage2_wx_pool_used);
	wxd_bc(2, stage2_wx_pool_exhausted);
	wxd_bc(3, stage2_wx_flip_count);
	wxd_bc(4, (uint32_t)last_ipa);
}

/* Split-or-reuse-then-flip ONE 4 KiB leaf inside the covering 1 GiB DRAM
 * block's row of stage2_l2_dram[] (see the SCOPE NOTE above). `ipa` must
 * already be known to lie inside guest DRAM and outside both HV windows —
 * stage2_wx_fault() below is the only caller and checks that first.
 * `want_exec` selects the direction: 1 = an execute (instruction-fetch)
 * fault, flip to RO+X; 0 = a write fault on a currently-RO+X leaf, flip
 * back to RW+XN. Always returns 1 once called with an in-range DRAM IPA,
 * except the defensive (should-be-unreachable) case noted inline. */
static int
stage2_wx_flip(uint64_t ipa, unsigned want_exec)
{
	uint64_t block_ipa = ipa & STAGE2_L2_ADDR_MASK;
	unsigned raw = (unsigned)((block_ipa - STAGE2_DRAM_BASE)
	                          >> STAGE2_L2_BLOCK_SHIFT);
	unsigned b = raw / STAGE2_L2_ENTRIES;      /* which 1 GiB block's row   */
	unsigned l2_idx = raw % STAGE2_L2_ENTRIES; /* entry within that row     */
	uint64_t l2d;
	uint64_t *l3;

	if (raw >= STAGE2_DRAM_L1_BLOCKS * STAGE2_L2_ENTRIES)
		return 0;   /* defensive: unreachable given stage2_wx_fault()'s range check */

	if (!stage2_wx_acquire_bounded())
		return 0;   /* another core mid-flip past the spin budget: decline,
		             * see stage2_wx_acquire_bounded()'s own comment */

	l2d = stage2_l2_dram[b][l2_idx];

	if ((l2d & 0x3ull) == S2_DESC_VALID_TABLE) {
		/* Already split — a previous flip of this same block, or (if
		 * l2_idx == VEC_L2_IDX) the first-fault vector probe's own L3
		 * table. Either way, reuse it: the walker cannot tell the
		 * difference and neither do we need to. */
		l3 = (uint64_t *)(uintptr_t)(l2d & STAGE2_TABLE_ADDR_MASK);
	} else if ((l2d & 0x3ull) == S2_DESC_VALID_BLOCK) {
		/* First-ever touch of this 2 MiB block. This can only be an
		 * EXECUTE fault: the block is still a plain RW+XN descriptor, so a
		 * WRITE to it is already permitted and could not have faulted. */
		if (!want_exec) {
			stage2_wx_unlock();
			return 0;   /* defensive: unreachable, see above */
		}

		if (stage2_wx_pool_used >= STAGE2_WX_POOL_TABLES) {
			/* FAIL OPEN — see this block's own header comment for why.
			 * Degrade this one 2 MiB region to plain RW+X (today's
			 * unconditional behavior) and let the guest proceed; never
			 * deny the fetch. */
			stage2_l2_dram[b][l2_idx] = stage2_l2_block_desc(block_ipa,
				S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
			stage2_wx_pool_exhausted++;
			wxd_publish(block_ipa);
			stage2_tlb_flush();
			stage2_wx_unlock();
			return 1;
		}

		l3 = &stage2_wx_pool[stage2_wx_pool_used][0];
		stage2_wx_pool_used++;

		/* Populate every page in the new table identically to the block
		 * descriptor it replaces (RW+XN) — splitting alone must change
		 * nothing observable except granularity. The one faulting page is
		 * flipped below, same as the already-split case. */
		for (unsigned k = 0; k < STAGE2_L3_ENTRIES; k++) {
			uint64_t pa = block_ipa + (uint64_t)k * STAGE2_L3_PAGE_SIZE;
			l3[k] = stage2_page_desc(pa, S2_MEMATTR_NORMAL_WB,
				S2_SH_INNER, /*xn=*/1);
		}
		stage2_l2_dram[b][l2_idx] = stage2_table_desc((uint64_t)(uintptr_t)l3);
	} else {
		stage2_wx_unlock();
		return 0;   /* invalid — unreachable given stage2_wx_fault()'s
		             * HV-window check, but never guess here */
	}

	{
		unsigned l3_idx = (unsigned)((ipa & (STAGE2_L2_BLOCK_SIZE - 1u)) >> STAGE2_L3_PAGE_SHIFT);
		uint64_t pa = ipa & STAGE2_L3_ADDR_MASK;

		if (want_exec)
			l3[l3_idx] = stage2_wx_page_desc(pa, S2AP_RO, /*xn=*/0);
		else
			l3[l3_idx] = stage2_page_desc(pa, S2_MEMATTR_NORMAL_WB,
				S2_SH_INNER, /*xn=*/1);
	}

	/* Broad, hardware-proven flush — the SAME primitive stage2_unmap_guest_
	 * vector()/stage2_map_guest_vector() already use for a runtime table
	 * edit — rather than a narrower by-IPA invalidate this tree has never
	 * exercised. See docs/wx-enforcement.md Q5 for why a wrong-scoped or
	 * wrong-timed narrower invalidate is a WORSE risk here (a stale TLB
	 * entry could keep permitting a write the new descriptor just denied)
	 * than the extra cost of flushing everything on every flip. */
	stage2_tlb_flush();
	stage2_wx_flip_count++;
	wxd_publish(ipa);
	stage2_wx_unlock();
	return 1;
}

int
stage2_wx_fault(struct el2_frame *frame)
{
	uint32_t ec  = (uint32_t)((frame->esr >> 26) & 0x3Fu);
	uint32_t fsc = (uint32_t)(frame->esr & 0x3Fu);   /* IFSC or DFSC alike */
	unsigned want_exec;
	uint64_t hpfar, ipa;

	if (ec == 0x20u || ec == 0x21u)
		want_exec = 1u;   /* instruction abort: the guest tried to FETCH */
	else if (ec == 0x24u || ec == 0x25u)
		want_exec = 0u;   /* data abort: the guest tried to WRITE (or read) */
	else
		return 0;         /* not one of ours at all */

	/* PERMISSION fault only (status code class 0b0011 at bits[5:2] of the
	 * ISS, any level — the same field position/encoding for both IFSC and
	 * DFSC). Anything else — a translation fault into the still-genuinely-
	 * unmapped hv-image/hv-scratch carve, the vblk/vnet trapped MMIO
	 * windows, a real out-of-range access — is NOT ours: it must fall
	 * through to every existing handler and the generic fault recorder
	 * exactly as it does today. Getting this test wrong in either direction
	 * would be a real regression: too loose swallows a genuine A1 boundary
	 * violation; too tight and this function never fires. */
	if ((fsc & 0x3Cu) != 0x0Cu)
		return 0;

	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;

	if (ipa < STAGE2_DRAM_BASE || ipa >= STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE)
		return 0;   /* not guest DRAM at all */

	/* The two HV windows are supposed to be UNMAPPED (stage2_dram_block_
	 * needs_split()'s carve), not permission-denied — if either one ever
	 * produces a PERMISSION fault instead of a translation fault, that is a
	 * bug somewhere else in this file and must NOT be silently absorbed
	 * here as if it were an ordinary W^X flip. */
	if (ipa >= HVIMG_BASE && ipa < HVIMG_BASE + STAGE2_L2_BLOCK_SIZE)
		return 0;
	if (ipa >= HVSCR_BASE && ipa < HVSCR_BASE + STAGE2_L2_BLOCK_SIZE)
		return 0;

	return stage2_wx_flip(ipa, want_exec);
}
#endif /* STAGE2_WX_DYNAMIC */

void
stage2_selftest(void)
{
	stage2_init();
	stage2_enable();

	/* Re-read back VTCR_EL2/HCR_EL2 to confirm the programmed bits
	 * actually stuck in hardware (not just that we issued the writes) —
	 * both are re-stored, making this idempotent with stage2_init()/
	 * stage2_enable()'s own breadcrumb writes above. */
	stg2_bc(STG2_VTCR_IDX, (uint32_t)read_vtcr_el2());
	stg2_bc(STG2_HCR_IDX, (uint32_t)read_hcr_el2());

	/* Combined stage-1+2 translation coherency proof of a known DRAM
	 * address -> PAR_EL1 into breadcrumb words 7/8. */
	stage2_at_check(STAGE2_SELFTEST_IPA);
}
