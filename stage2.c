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
#include "stage2.h"
#include "vblk_emmc.h"   /* VBLK_MMIO_BASE — trapped virtio-mmio window */

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
 * be installed as level-1[0][0] by the caller (stage2_init()). */
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

		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;

		/* FIX-1 (2026-07-16): GICC->GICV redirect REMOVED. Under IMO=0 the guest
		 * must ack/EOI via the REAL GICC (0x1c82000); redirecting to the inert
		 * GICV made it unable to service any interrupt -> hang at the MMC/root
		 * interrupt-driven phase. j==130 now falls through to the identity
		 * default (0x1c00000 + 130*0x1000 = 0x1c82000 = real GICC). The earlier
		 * "regression" from removing it was a MISDIAGNOSIS — those NO_EMAC were
		 * caused by a chimpd autostart=no bug (bootelf didn't jump), now fixed.
		 * See memory gicv-redirect-working. */

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
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (i == UART_L2_IDX) {
			uint64_t l3_pa = (uint64_t)(uintptr_t)&stage2_l3_uart[0];
			stage2_l2_mmio[i] = stage2_table_desc(l3_pa);
			continue;
		}
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

static uint64_t stage2_l2_dram[STAGE2_L2_ENTRIES]
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

/* Build the level-2 DRAM table for the 1 GiB block starting at `block_base`
 * into stage2_l2_dram[]: identity Normal-WB executable 2 MiB blocks
 * everywhere, EXCEPT the hv-image/hv-scratch entries, left INVALID
 * (all-zero). Only called for the DRAM block that actually contains those
 * windows (see stage2_dram_block_needs_split()) — a block with neither
 * stays the simple flat 1 GiB descriptor, unchanged from before this
 * milestone. Returns stage2_l2_dram[]'s PA for the caller to install. */
static uint64_t
stage2_build_dram_table(uint64_t block_base)
{
	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
		if (block_base == STAGE2_DRAM_BASE &&
		    (i == HVIMG_L2_IDX || i == HVSCR_L2_IDX)) {
			stage2_l2_dram[i] = 0;   /* INVALID: HV image or HV scratch */
			continue;
		}
		uint64_t pa = block_base + (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
		stage2_l2_dram[i] = stage2_l2_block_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	}
	return (uint64_t)(uintptr_t)&stage2_l2_dram[0];
}

/* Does the 1 GiB block at `block_base` contain either excluded window?
 * Both windows are known to fit inside a single 1 GiB block each (their L2
 * indices above are computed relative to STAGE2_DRAM_BASE specifically),
 * so this is only ever true for the block starting at STAGE2_DRAM_BASE
 * itself today; written as a real range check (not a hardcoded block
 * index) so it stays correct if STAGE2_DRAM_SIZE grows to cover more
 * blocks later. */
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

	/* Level-3: identity Normal-WB executable 4 KiB pages across the 2 MiB
	 * block, EXCEPT the vector page itself (invalid -> stage-2 abort). */
	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == VEC_L3_IDX) {
			stage2_l3_vec[j] = 0;   /* INVALID: the trapped vector page */
			continue;
		}
		uint64_t pa = l2_block_base + (uint64_t)j * STAGE2_L3_PAGE_SIZE;
		stage2_l3_vec[j] = stage2_page_desc(pa,
			S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	}

	/* A1 NOTE: stage2_l2_dram[] was already fully built by stage2_init()
	 * (via stage2_build_dram_table(), called because this 1 GiB block
	 * contains hv-image/hv-scratch) BEFORE this function ever runs — see
	 * its own comment above. Replacing all 512 entries here, as an
	 * earlier version of this function did, would silently re-identity-map
	 * (and thus un-protect) the HVIMG_L2_IDX/HVSCR_L2_IDX exclusions. Only
	 * this ONE entry (VEC_L2_IDX=52, disjoint from both) is touched. */
	stage2_l2_dram[VEC_L2_IDX] =
		stage2_table_desc((uint64_t)(uintptr_t)&stage2_l3_vec[0]);

	/* stage2_l1[0][VEC_L1_IDX] already points at stage2_l2_dram[] — that
	 * install happened in stage2_init() (either as a table descriptor, if
	 * this block needed the A1 split, or — see stage2_dram_block_needs_
	 * split() — this function is only ever armed on the block that DOES
	 * need it, since GUEST_VECTOR_IPA and HVIMG_BASE/HVSCR_BASE all sit in
	 * the same 1 GiB span today). Just flush the combined stage-1+2 TLB so
	 * the walker picks up the level-3 split before the guest runs. */
	stage2_tlb_flush();
}

void
stage2_map_guest_vector(void)
{
	/* Restore the single vector page to a plain identity Normal-WB
	 * executable page (the rest of the split tables already are), then
	 * flush. Called by firstfault_handle() once it has latched the
	 * original fault, so the guest's re-entered vector fetch now succeeds
	 * and no further stage-2 abort loops in EL2. Assumes
	 * stage2_unmap_guest_vector() already built stage2_l3_vec[]. */
	uint64_t pa = GUEST_VECTOR_IPA & STAGE2_L3_ADDR_MASK;
	stage2_l3_vec[VEC_L3_IDX] = stage2_page_desc(pa,
		S2_MEMATTR_NORMAL_WB, S2_SH_INNER, /*xn=*/0);
	stage2_tlb_flush();
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
	/* vmalls12e1: invalidate stage-1 AND stage-2 combined TLB entries
	 * for all VMIDs at EL1 — the correct, broad flush any time VTCR/
	 * VTTBR or HCR.VM just changed. dsb ish + isb: make sure the
	 * invalidation is globally observed and no stale translation is
	 * used by an instruction fetched after this point. */
	__asm__ volatile("tlbi vmalls12e1\n\tdsb ish\n\tisb" ::: "memory");
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

	/* Index 1..N: DRAM as contiguous 1 GiB Normal WB blocks, XN=0
	 * (guest code lives here and must be executable). N is derived
	 * from STAGE2_DRAM_SIZE so bumping that one #define is enough to
	 * map more RAM later.
	 *
	 * A1: a block that overlaps the hv-image or hv-scratch DTB-reserved
	 * windows gets a TABLE descriptor down to a level-2 table (built by
	 * stage2_build_dram_table(), with those two windows left INVALID)
	 * instead of a single flat BLOCK — every other block is unaffected,
	 * identical to before this milestone. */
	{
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

	/* VTCR_EL2: every field derived/explained in the big comment above.
	 * PS comes from this core's actual PARange, read at runtime. */
	uint64_t parange = read_id_aa64mmfr0_el1() & 0xFull; /* bits[3:0] */
	uint64_t vtcr = 0;
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

	/* VTTBR_EL2: VMID = 0 (bits[63:48] left zero) + table base address
	 * of the concatenated level-1 tables (BADDR field). Table 0's
	 * address is the base of the whole 8 KiB concatenated set. */
	uint64_t table_base = (uint64_t)(uintptr_t)&stage2_l1[0][0];
	write_vttbr_el2(table_base);
	stg2_bc(STG2_VTTBR_IDX, (uint32_t)read_vttbr_el2());
	stg2_bc(STG2_TABLE_BASE_IDX, (uint32_t)table_base);
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
