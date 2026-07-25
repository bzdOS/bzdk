/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_ncmap.c — build a fresh EL2 stage-1 identity map that treats
 * guest-owned DRAM as Normal Non-cacheable, and switch TTBR0_EL2 to it. See
 * el2_ncmap.h for scope/entry-point contract and docs/el2-nc-guest-dram.md
 * for the full design writeup (findings, ordering rationale, exclusive-
 * atomics audit, residual risks, live-test plan).
 *
 * ---------------------------------------------------------------------
 * WHY A FRESH TABLE INSTEAD OF EDITING THE LIVE ONE
 * ---------------------------------------------------------------------
 * EL2 currently runs on U-Boot's inherited stage-1 tables (stage2.h says so
 * explicitly: "our own EL2 stage-1 ... stays exactly as U-Boot's flat
 * mapping left it"). We do not know U-Boot's MAIR_EL2 index assignment, its
 * table granule/levels, or whether it has holes anywhere in the 4 GiB space
 * (PROJECT.md claims 2 GiB DRAM but the guest's own window is only the low
 * 1 GiB — U-Boot's map of the *rest* of the address space is unverified).
 * Editing an unknown table in place, live, is exactly the kind of thing that
 * produced the FAILED prior attempt (main_dbg.c's dead `if (0)` block):
 * that code ran a blanket `dc civac` over all of guest DRAM (no island
 * exclusion) BEFORE el2_install() had even run, so any stray fault from an
 * unmapped hole in U-Boot's table would have been delivered to U-BOOT's own
 * vector handler, not ours — a very plausible wedge mechanism, alongside
 * the simpler fact that it invalidated the HV's own running image and
 * (probably) U-Boot's own stack/tables mid-execution. See the big comment
 * in el2_ncmap_apply() for how this file is built to be immune to both.
 *
 * Building our OWN table means we know EXACTLY what is mapped, at exactly
 * the granularity we choose, with zero reliance on U-Boot's layout — and we
 * get to pick MAIR_EL2 from scratch (no need to match/guess U-Boot's index
 * assignment).
 *
 * ---------------------------------------------------------------------
 * TABLE TOPOLOGY
 * ---------------------------------------------------------------------
 * 4 KiB granule, T0SZ = 25 (39-bit input address space = 512 GiB, 3 levels:
 * L1/L2/L3, 9 bits per level over a 12-bit page offset — exactly enough to
 * reach every address this HV ever touches, all of which sit below 2 GiB).
 * PS comes from ID_AA64MMFR0_EL1.PARange read at runtime, mirroring
 * stage2.c's own VTCR_EL2.PS convention (never hard-coded).
 *
 *   el2_l1[512]      1 GiB/entry.  idx0 -> el2_l2_low (VA 0..1G),
 *                                  idx1 -> el2_l2_dram (VA 1G..2G, i.e.
 *                                  0x40000000..0x80000000 = guest DRAM),
 *                                  idx2 -> el2_l2_high (VA 2G..3G, the
 *                                  HV-private HIGH GiB of this 2 GiB board:
 *                                  U-Boot's post-relocation stack/tables
 *                                  live there, and dbgmon peek/poke must
 *                                  keep reaching it — Normal-WB, invisible
 *                                  to the guest so its cacheability can
 *                                  never corrupt guest DMA).
 *                                  Everything else: 0 = fault.
 *
 *   el2_l2_low[512]  2 MiB/entry, VA 0..1G. idx 8..15 (0x01000000..
 *                                  0x02000000, the MMIO window this HV
 *                                  actually uses) as Device-nGnRE blocks,
 *                                  PLUS idx0 (0x0..0x200000) also Device-
 *                                  nGnRE: it covers the SRAM breadcrumb
 *                                  windows (GICT 0x18200, IRQ-counter
 *                                  0x18300, VGIC 0x18000). Those writers
 *                                  (gic_timer.c/vgic.c) are dead code in
 *                                  the dbg build (only repl.c calls their
 *                                  inits), but mapping the block is cheap
 *                                  insurance against a stray write turning
 *                                  into an EL2 translation fault inside an
 *                                  exception path. Everything else: fault.
 *
 *   el2_l2_dram[512] 2 MiB/entry, VA 0x40000000..0x80000000 = guest DRAM.
 *                                  Default: Normal Non-cacheable block.
 *                                  idx16  (0x42000000..0x42200000) and
 *                                  idx128 (0x50000000..0x50200000) instead
 *                                  point down to el2_l3_img[]/el2_l3_bcw[]
 *                                  (see below — the two WB islands are each
 *                                  exactly half of one of these 2 MiB
 *                                  blocks, so 2 MiB block granularity alone
 *                                  can't express the split).
 *                                  Whichever 2 MiB blocks bracket the
 *                                  CURRENTLY EXECUTING call stack (captured
 *                                  at entry — see the stack-safety comment
 *                                  below) are ALSO left Normal-WB instead of
 *                                  NC, generously padded.
 *
 *   el2_l3_img[512]  4 KiB/entry, VA 0x42000000..0x42200000.
 *                                  idx0..255   (0x42000000..0x42100000):
 *                                    Normal-WB, executable (XN=0) — the HV
 *                                    image itself; EL2 fetches code from
 *                                    here, so this MUST stay cacheable+exec.
 *                                  idx256..511 (0x42100000..0x42200000):
 *                                    Normal-NC, XN=1 — ordinary guest DRAM
 *                                    immediately above the image.
 *
 *   el2_l3_bcw[512]  4 KiB/entry, VA 0x50000000..0x50200000.
 *                                  idx0..255   (0x50000000..0x50100000):
 *                                    Normal-WB, XN=1 (data only — vconsole
 *                                    ring, virtio-blk breadcrumbs, the eMMC
 *                                    lock word at 0x50020100 which needs
 *                                    LDAXR/STLXR to keep working — see
 *                                    docs/el2-nc-guest-dram.md's exclusive-
 *                                    atomics audit).
 *                                  idx256..511 (0x50100000..0x50200000):
 *                                    Normal-NC, XN=1. NOTE: emac.c's own
 *                                    DMA scratch (descriptors + packet
 *                                    bufs, SCRATCH_BASE=0x50100000..
 *                                    0x50109000, emac.c:140-144) lives HERE,
 *                                    just past the stated 1 MiB breadcrumb
 *                                    window, not inside it. Falling back to
 *                                    NC is SAFE for it (see the doc) and
 *                                    arguably a latent-bug fix, since EMAC
 *                                    already hand-rolls the exact same
 *                                    dc-civac/dc-ivac dance aw_mmc does for
 *                                    its own non-coherent descriptor DMA.
 *
 * Total new .bss: 5 tables * 4 KiB = 20 KiB — negligible next to the 1 MiB
 * HV-image budget (current image ~224 KiB per `nm`, checked live below).
 *
 * ---------------------------------------------------------------------
 * FAIL-SAFE DESIGN
 * ---------------------------------------------------------------------
 * Because this cannot be tested on the real board by this change (hard
 * rule: no board access), every assumption this file depends on is checked
 * at RUNTIME before anything irreversible happens, and a failed check
 * aborts the remap (leaves a breadcrumb, returns) rather than switching to
 * a table that might immediately fault:
 *   1. Our own image (__bss_end) must still fit inside the 1 MiB HV-image
 *      island — else part of OUR OWN new tables/state would fall outside
 *      the WB island we're about to carve out, into the NC region.
 *   2. The captured stack pointer must fall inside guest DRAM (0x40000000..
 *      0x80000000) — this is where U-Boot's post-relocation SP is
 *      overwhelmingly likely to be (see the doc), but it is READ, never
 *      assumed; if it is not, we do not know how to build a safe map and
 *      we abort rather than guess.
 *   3. A software self-walk of the freshly-built tables must resolve a
 *      handful of representative addresses (HV image, both halves of both
 *      islands, DRAM base/top, MMIO base) to the PA/attributes we intended,
 *      mirroring stage2.c's own self-test pattern.
 * Only if all three pass do we ever write MAIR_EL2/TCR_EL2/TTBR0_EL2.
 */
#include <stdint.h>
#include "wdt.h"

#define NCM_BC_BASE   0x50011000UL   /* clear of every window in smp.h's map
                                       * and past vconsole's 64 KiB ring
                                       * (0x50000f10..0x50010f10) — see
                                       * docs/el2-nc-guest-dram.md */
#define NCM_BC_MAGIC  0x4E434D31u    /* "NCM1" */

/* Status codes, word [1] of the breadcrumb — see docs/el2-nc-guest-dram.md
 * for the decode table. */
#define NCM_ST_STARTED         0x00000001u
#define NCM_ST_ABORT_BSSBOUND  0xA0000002u
#define NCM_ST_ABORT_SPRANGE   0xA0000003u
#define NCM_ST_ABORT_SELFCHECK 0xA0000004u
#define NCM_ST_TABLES_BUILT    0x00000005u
#define NCM_ST_SWITCHED        0x00000006u
#define NCM_ST_SWEPT_DONE      0x00000007u

static inline void ncm_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(NCM_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	/* Same cache-coherent-store pattern as every other breadcrumb in this
	 * tree (dc civac + dsb): this word lives in the WB breadcrumb island,
	 * so it is always valid to civac regardless of whether we've switched
	 * tables yet. */
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Address-space layout constants.
 * ------------------------------------------------------------------ */
#define DRAM_BASE     0x40000000ULL
#define DRAM_END      0x80000000ULL
#define BLOCK_2M      0x00200000ULL
#define PAGE_4K       0x00001000ULL

#define HVIMG_BASE    0x42000000ULL
#define HVIMG_SIZE    0x00100000ULL      /* checked live vs __bss_end below */
#define HVIMG_L2_IDX  ((unsigned)((HVIMG_BASE - DRAM_BASE) / BLOCK_2M))   /* 16 */

#define BCW_BASE      0x50000000ULL
#define BCW_SIZE      0x00100000ULL
#define BCW_L2_IDX    ((unsigned)((BCW_BASE - DRAM_BASE) / BLOCK_2M))     /* 128 */

#define MMIO_BASE     0x01000000ULL
#define MMIO_END      0x02000000ULL
#define MMIO_LOW_IDX  ((unsigned)(MMIO_BASE / BLOCK_2M))  /* 8  in el2_l2_low */
#define MMIO_LOW_CNT  ((unsigned)((MMIO_END - MMIO_BASE) / BLOCK_2M))  /* 8 */

/* HV-private high GiB (2 GiB board: DRAM is 0x40000000..0xC0000000, the guest
 * only sees the low half). U-Boot's post-relocation stack/tables live here,
 * dbgmon peek/poke and the snapshot store need to reach it. Normal-WB is safe
 * here BECAUSE the guest cannot see or DMA into this range. */
#define HIGH_BASE     0x80000000ULL
#define HIGH_END      0xC0000000ULL

/* Stack-safety margin (see the big comment above + docs/el2-nc-guest-dram.md
 * "residual risks": we can only see the SP at the moment we run, not every
 * future call depth for the rest of this HV's live session, so we pad
 * generously). Rounded out to 2 MiB block boundaries. */
#define STACK_MARGIN_BELOW  (16ULL * 1024 * 1024)
#define STACK_MARGIN_ABOVE  ( 2ULL * 1024 * 1024)

/* ------------------------------------------------------------------ *
 * MAIR_EL2 — chosen from scratch (we do not need to match U-Boot's).
 * ------------------------------------------------------------------ */
#define MAIR_IDX_DEVICE_nGnRE  0u
#define MAIR_IDX_NORMAL_NC     1u
#define MAIR_IDX_NORMAL_WB     2u

#define MAIR_ATTR_DEVICE_nGnRE 0x04ULL   /* encoding for Device-nGnRE       */
#define MAIR_ATTR_NORMAL_NC    0x44ULL   /* Normal, Inner+Outer Non-cacheable */
#define MAIR_ATTR_NORMAL_WB    0xFFULL   /* Normal, Inner+Outer WB, RW-alloc */

static inline uint64_t build_mair_el2(void)
{
	uint64_t m = 0;
	m |= MAIR_ATTR_DEVICE_nGnRE << (8u * MAIR_IDX_DEVICE_nGnRE);
	m |= MAIR_ATTR_NORMAL_NC    << (8u * MAIR_IDX_NORMAL_NC);
	m |= MAIR_ATTR_NORMAL_WB    << (8u * MAIR_IDX_NORMAL_WB);
	return m;
}

/* ------------------------------------------------------------------ *
 * TCR_EL2 (HCR_EL2.E2H == 0 format: T0SZ[5:0], RES1[6], IRGN0[9:8],
 * ORGN0[11:10], SH0[13:12], TG0[15:14], PS[18:16], TBI[20], HPD[24], RES1[23]
 * and RES1[31] — verified against the ARM system-register reference for
 * this exact register/format before use). We set: T0SZ=25 (39-bit VA,
 * 512 GiB, 3-level 4K-granule walk starting at level 1 — more than enough
 * for the <2 GiB of PA this tree ever touches), IRGN0/ORGN0 = Normal
 * Write-Back RW-Allocate, SH0 = Inner Shareable (matches stage-2's own
 * VTCR_EL2 choice and this being an SMP-coherent system), TG0 = 4 KiB,
 * PS = this core's actual PARange (read at runtime, never hard-coded —
 * mirrors stage2.c's read_id_aa64mmfr0_el1() convention exactly). Bits
 * 23 and 31 are RES1 and must be written as 1 (bits [7:6], by contrast, ARE
 * RES0 — verified against the register reference before use, since this is
 * an easy pair to confuse with TCR_EL1's similarly-shaped layout); everything
 * else (HPD, HA/HD hardware AF/DBM, TBI, MTX/DS/TCMA/TBID — none implemented
 * or wanted here) is left 0, matching stage2.c's "we manage AF by hand"
 * convention.
 * ------------------------------------------------------------------ */
#define TCR_T0SZ_VAL    25ULL
#define TCR_IRGN0_VAL   1ULL   /* Normal WB, RW-Allocate (inner) */
#define TCR_ORGN0_VAL   1ULL   /* Normal WB, RW-Allocate (outer) */
#define TCR_SH0_VAL     3ULL   /* Inner Shareable */
#define TCR_TG0_VAL     0ULL   /* 4 KiB granule */

#define TCR_RES1_BITS   ((1ULL << 23) | (1ULL << 31))

static inline uint64_t read_id_aa64mmfr0_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
	return v;
}

static inline uint64_t build_tcr_el2(void)
{
	uint64_t parange = read_id_aa64mmfr0_el1() & 0xFULL;   /* bits[3:0] */
	uint64_t t = 0;
	t |= TCR_T0SZ_VAL  << 0;
	t |= TCR_IRGN0_VAL << 8;
	t |= TCR_ORGN0_VAL << 10;
	t |= TCR_SH0_VAL   << 12;
	t |= TCR_TG0_VAL   << 14;
	t |= parange        << 16;
	t |= TCR_RES1_BITS;
	return t;
}

/* ------------------------------------------------------------------ *
 * Descriptor helpers (stage-1 format — distinct from stage2.c's stage-2
 * MemAttr encoding: stage-1 uses AttrIndx into MAIR, not a direct MemAttr
 * nibble).
 * ------------------------------------------------------------------ */
#define D_VALID       (1ULL << 0)
#define D_TYPE_TBL    (1ULL << 1)   /* table (L1/L2) or page (L3, same bit) */
#define D_AP_RW       (0ULL << 6)   /* AP[2:1]=00: R/W (no lower-EL concept
                                     * in the EL2-only translation regime) */
#define D_SH_INNER    (3ULL << 8)
#define D_AF          (1ULL << 10) /* access flag, set by hand (no HA) */
#define D_XN          (1ULL << 54)

static inline uint64_t desc_table(uint64_t next_level_pa)
{
	return (next_level_pa & ~0xFFFULL) | D_TYPE_TBL | D_VALID;
}

static inline uint64_t desc_block2m(uint64_t pa, unsigned attridx, unsigned xn)
{
	uint64_t d = (pa & ~(BLOCK_2M - 1)) | D_VALID | D_AP_RW | D_SH_INNER | D_AF;
	d |= ((uint64_t)attridx & 0x7ULL) << 2;
	if (xn)
		d |= D_XN;
	return d;   /* bit1 left 0 => block descriptor at L1/L2 */
}

static inline uint64_t desc_page4k(uint64_t pa, unsigned attridx, unsigned xn)
{
	uint64_t d = (pa & ~(PAGE_4K - 1)) | D_TYPE_TBL | D_VALID | D_AP_RW | D_SH_INNER | D_AF;
	d |= ((uint64_t)attridx & 0x7ULL) << 2;
	if (xn)
		d |= D_XN;
	return d;   /* bit1=1 => page descriptor at L3 */
}

/* ------------------------------------------------------------------ *
 * Table storage — lives in HV .bss, i.e. inside the HV-image WB island.
 * Alignment MUST be exactly the table size (4 KiB) for a valid BADDR/
 * next-level-table-address field.
 * ------------------------------------------------------------------ */
static uint64_t el2_l1[512]      __attribute__((aligned(4096)));
static uint64_t el2_l2_low[512]  __attribute__((aligned(4096)));
static uint64_t el2_l2_dram[512] __attribute__((aligned(4096)));
static uint64_t el2_l2_high[512] __attribute__((aligned(4096)));
static uint64_t el2_l3_img[512]  __attribute__((aligned(4096)));
static uint64_t el2_l3_bcw[512]  __attribute__((aligned(4096)));

/* Linker symbol marking the end of the HV's own .bss (start.S / link.ld). */
extern uint8_t __bss_end[];

static inline uint64_t read_sp(void)
{
	uint64_t v;
	__asm__ volatile("mov %0, sp" : "=r"(v));
	return v;
}

/* A 2 MiB-block index [lo, hi) test used both when building el2_l2_dram and
 * when deciding what the transition sweep may touch. */
static inline int block_in_stack_margin(uint64_t block_pa, uint64_t sp_lo, uint64_t sp_hi)
{
	return block_pa + BLOCK_2M > sp_lo && block_pa < sp_hi;
}

/* ------------------------------------------------------------------ *
 * Table builders.
 * ------------------------------------------------------------------ */
static void build_l2_low(void)
{
	unsigned i;
	for (i = 0; i < 512; i++)
		el2_l2_low[i] = 0;   /* fault by default — see header */
	/* idx0: BROM/SRAM block (SRAM breadcrumb windows 0x18000..0x18400 —
	 * dead code in this build, mapped as cheap insurance; see header). */
	el2_l2_low[0] = desc_block2m(0, MAIR_IDX_DEVICE_nGnRE, /*xn=*/1);
	for (i = 0; i < MMIO_LOW_CNT; i++) {
		uint64_t pa = MMIO_BASE + (uint64_t)i * BLOCK_2M;
		el2_l2_low[MMIO_LOW_IDX + i] = desc_block2m(pa, MAIR_IDX_DEVICE_nGnRE, /*xn=*/1);
	}
}

/* HV-private high GiB: plain Normal-WB identity blocks (see HIGH_BASE note). */
static void build_l2_high(void)
{
	unsigned i;
	for (i = 0; i < 512; i++) {
		uint64_t pa = HIGH_BASE + (uint64_t)i * BLOCK_2M;
		el2_l2_high[i] = desc_block2m(pa, MAIR_IDX_NORMAL_WB, /*xn=*/1);
	}
}

static void build_l3_img(void)
{
	unsigned i;
	for (i = 0; i < 256; i++) {
		uint64_t pa = HVIMG_BASE + (uint64_t)i * PAGE_4K;
		el2_l3_img[i] = desc_page4k(pa, MAIR_IDX_NORMAL_WB, /*xn=*/0);  /* executable */
	}
	for (i = 256; i < 512; i++) {
		uint64_t pa = HVIMG_BASE + (uint64_t)i * PAGE_4K;
		el2_l3_img[i] = desc_page4k(pa, MAIR_IDX_NORMAL_NC, /*xn=*/1);
	}
}

static void build_l3_bcw(void)
{
	unsigned i;
	for (i = 0; i < 256; i++) {
		uint64_t pa = BCW_BASE + (uint64_t)i * PAGE_4K;
		el2_l3_bcw[i] = desc_page4k(pa, MAIR_IDX_NORMAL_WB, /*xn=*/1);  /* data only */
	}
	for (i = 256; i < 512; i++) {
		uint64_t pa = BCW_BASE + (uint64_t)i * PAGE_4K;
		el2_l3_bcw[i] = desc_page4k(pa, MAIR_IDX_NORMAL_NC, /*xn=*/1);
	}
}

static void build_l2_dram(uint64_t sp_lo, uint64_t sp_hi)
{
	unsigned i;
	for (i = 0; i < 512; i++) {
		uint64_t pa = DRAM_BASE + (uint64_t)i * BLOCK_2M;

		if (i == HVIMG_L2_IDX) {
			el2_l2_dram[i] = desc_table((uint64_t)(uintptr_t)&el2_l3_img[0]);
		} else if (i == BCW_L2_IDX) {
			el2_l2_dram[i] = desc_table((uint64_t)(uintptr_t)&el2_l3_bcw[0]);
		} else if (block_in_stack_margin(pa, sp_lo, sp_hi)) {
			/* Live U-Boot-inherited call stack: keep it Normal-WB so every
			 * ordinary push/pop stays fast and coherent, exactly as it is
			 * today. See docs/el2-nc-guest-dram.md's residual-risk note:
			 * this island is NOT reflected in the guest's DTB reserved-
			 * memory carve-out (only the HV image + breadcrumb window are),
			 * so it is a best-effort, not a hard guarantee. */
			el2_l2_dram[i] = desc_block2m(pa, MAIR_IDX_NORMAL_WB, /*xn=*/1);
		} else {
			el2_l2_dram[i] = desc_block2m(pa, MAIR_IDX_NORMAL_NC, /*xn=*/1);
		}
	}
}

static void build_l1(void)
{
	unsigned i;
	for (i = 0; i < 512; i++)
		el2_l1[i] = 0;   /* fault by default */
	el2_l1[0] = desc_table((uint64_t)(uintptr_t)&el2_l2_low[0]);   /* VA 0..1G   */
	el2_l1[1] = desc_table((uint64_t)(uintptr_t)&el2_l2_dram[0]);  /* VA 1G..2G  */
	el2_l1[2] = desc_table((uint64_t)(uintptr_t)&el2_l2_high[0]);  /* VA 2G..3G  */
}

/* ------------------------------------------------------------------ *
 * Software self-check — walk our OWN just-built tables for a handful of
 * representative addresses and confirm they resolve to the PA/attribute we
 * intended, BEFORE touching any hardware sysreg. Mirrors stage2.c's own
 * self-test pattern (stage2_init()'s STAGE2_SELFTEST_IPA check).
 * ------------------------------------------------------------------ */
static int walk_check(uint64_t va, uint64_t want_pa, unsigned want_nc /* 1=NC,0=WB/dev */)
{
	unsigned l1i = (unsigned)(va >> 30) & 0x1FF;
	uint64_t l1d = el2_l1[l1i];
	uint64_t l2_pa, l2d;
	unsigned l2i;

	if (!(l1d & D_VALID) || !(l1d & D_TYPE_TBL))
		return 0;
	l2_pa = l1d & ~0xFFFULL;
	l2i = (unsigned)(va >> 21) & 0x1FF;
	l2d = *(uint64_t *)(uintptr_t)(l2_pa + (uint64_t)l2i * 8u);

	if (!(l2d & D_VALID))
		return 0;
	if (!(l2d & D_TYPE_TBL)) {
		/* L2 block descriptor: resolved PA is this block + offset. */
		uint64_t block_pa = l2d & ~(BLOCK_2M - 1);
		uint64_t resolved = block_pa | (va & (BLOCK_2M - 1));
		unsigned attridx = (unsigned)((l2d >> 2) & 0x7ULL);
		unsigned is_nc = (attridx == MAIR_IDX_NORMAL_NC);
		return resolved == want_pa && is_nc == want_nc;
	} else {
		/* L2 table descriptor -> walk L3. */
		uint64_t l3_pa = l2d & ~0xFFFULL;
		unsigned l3i = (unsigned)(va >> 12) & 0x1FF;
		uint64_t l3d = *(uint64_t *)(uintptr_t)(l3_pa + (uint64_t)l3i * 8u);
		uint64_t resolved;
		unsigned attridx;
		unsigned is_nc;

		if (!(l3d & D_VALID) || !(l3d & D_TYPE_TBL))
			return 0;
		resolved = (l3d & ~(PAGE_4K - 1)) | (va & (PAGE_4K - 1));
		attridx = (unsigned)((l3d >> 2) & 0x7ULL);
		is_nc = (attridx == MAIR_IDX_NORMAL_NC);
		return resolved == want_pa && is_nc == want_nc;
	}
}

static int self_check(void)
{
	if (!walk_check(HVIMG_BASE, HVIMG_BASE, /*want_nc=*/0))
		return 0;
	if (!walk_check(HVIMG_BASE + HVIMG_SIZE, HVIMG_BASE + HVIMG_SIZE, /*want_nc=*/1))
		return 0;
	if (!walk_check(BCW_BASE, BCW_BASE, /*want_nc=*/0))
		return 0;
	if (!walk_check(BCW_BASE + BCW_SIZE, BCW_BASE + BCW_SIZE, /*want_nc=*/1))
		return 0;
	if (!walk_check(DRAM_BASE, DRAM_BASE, /*want_nc=*/1))
		return 0;
	if (!walk_check(DRAM_END - PAGE_4K, DRAM_END - PAGE_4K, /*want_nc=*/1))
		return 0;
	if (!walk_check(HIGH_BASE, HIGH_BASE, /*want_nc=*/0))         /* high GiB WB */
		return 0;
	if (!walk_check(HIGH_END - PAGE_4K, HIGH_END - PAGE_4K, /*want_nc=*/0))
		return 0;
	if (!walk_check(0x00018100ULL, 0x00018100ULL, /*want_nc=*/0)) /* SRAM device */
		return 0;
	if (!walk_check(0x01C81000ULL, 0x01C81000ULL, /*want_nc=*/0)) /* GICD device */
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Transition maintenance: clean+invalidate to PoC every 2 MiB block we just
 * marked Non-cacheable, and ONLY those — skipping the HV image, the
 * breadcrumb-window WB half, and the live stack-margin island by
 * construction. Runs AFTER the TTBR0_EL2 switch (see el2_ncmap_apply()'s
 * ordering comment for why that order, not before, is the textbook-correct
 * one and is what makes this provably immune to the old sweep's failure).
 * ------------------------------------------------------------------ */
static void civac_range(uint64_t start, uint64_t end)
{
	uint64_t p;
	uint32_t n = 0;

	for (p = start; p < end; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
		if ((++n & 0xFFFFu) == 0)
			wdt_pet();   /* belt-and-suspenders: keep the (already-armed,
			              * per main_dbg.c's call ordering) HW watchdog fed
			              * during this long loop, in case sheer duration
			              * ever turns out to matter — see the doc. */
	}
}

static void sweep_nc_region(uint64_t sp_lo, uint64_t sp_hi)
{
	unsigned i;

	for (i = 0; i < 512; i++) {
		uint64_t pa = DRAM_BASE + (uint64_t)i * BLOCK_2M;

		if (i == HVIMG_L2_IDX) {
			civac_range(HVIMG_BASE + HVIMG_SIZE, HVIMG_BASE + 2 * HVIMG_SIZE);
		} else if (i == BCW_L2_IDX) {
			civac_range(BCW_BASE + BCW_SIZE, BCW_BASE + 2 * BCW_SIZE);
		} else if (block_in_stack_margin(pa, sp_lo, sp_hi)) {
			continue;   /* WB island: never swept, never invalidated */
		} else {
			civac_range(pa, pa + BLOCK_2M);
		}
	}
	__asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

/* Clean (not invalidate) our own new table memory to PoC before pointing
 * hardware at it — belt-and-suspenders. Not strictly required for CPU0
 * (same-core table walks observe program-order stores without explicit
 * maintenance) or for a later CPU1 (it joins cache coherency via SMPEN
 * before its MMU/walker turns on — see smp.h/start.S), but the five tables
 * are only 20 KiB total, so the cost is negligible and it removes any doubt. */
static void clean_table_mem(void *base, uint32_t bytes)
{
	uintptr_t p = (uintptr_t)base & ~63ULL;
	uintptr_t end = (uintptr_t)base + bytes;

	for (; p < end; p += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
}

static int g_applied;   /* idempotency guard */

void el2_ncmap_apply(void)
{
	uint64_t sp, sp_lo, sp_hi;
	uint64_t mair, tcr, ttbr0;

	if (g_applied)
		return;

	ncm_bc(1, NCM_ST_STARTED);

	/* ---- Gate 1: our own image must still fit inside the 1 MiB HV-image
	 * WB island (see el2_ncmap.h's comment on EL2_NCMAP_HVIMG_SIZE). If a
	 * future change grows .bss past this, building el2_l3_img with a hard-
	 * coded 1 MiB split would be WRONG (part of our own live state would
	 * land in the NC half) — abort rather than build a table that could
	 * corrupt ourselves. */
	if ((uint64_t)(uintptr_t)__bss_end >= HVIMG_BASE + HVIMG_SIZE) {
		ncm_bc(1, NCM_ST_ABORT_BSSBOUND);
		ncm_bc(2, (uint32_t)(uint64_t)(uintptr_t)__bss_end);
		return;
	}

	/* ---- Gate 2: capture the live (U-Boot-inherited) stack pointer and
	 * make sure it is somewhere we know how to protect. Two valid cases:
	 *   (a) SP in the HV-private HIGH GiB (0x80000000..0xC0000000) — the
	 *       expected case on this 2 GiB board (U-Boot relocates its stack
	 *       to the top of real DRAM). The stack is then entirely OUTSIDE
	 *       the guest window and needs NO WB island inside guest DRAM at
	 *       all — the cleanest outcome (no cacheable-observer residue).
	 *   (b) SP inside guest DRAM (1 GiB-only configs) — carve the padded
	 *       WB stack island as designed.
	 * Anything else (SRAM? somewhere unmapped by our table?) — abort. */
	sp = read_sp();
	if (sp < DRAM_BASE || sp >= HIGH_END) {
		ncm_bc(1, NCM_ST_ABORT_SPRANGE);
		ncm_bc(2, (uint32_t)sp);
		return;
	}
	if (sp >= DRAM_END) {
		sp_lo = sp_hi = 0;   /* case (a): no island in guest DRAM needed */
	} else {
		sp_lo = (sp > STACK_MARGIN_BELOW) ? (sp - STACK_MARGIN_BELOW) : DRAM_BASE;
		sp_hi = sp + STACK_MARGIN_ABOVE;
		sp_lo &= ~(BLOCK_2M - 1);
		sp_hi = (sp_hi + BLOCK_2M - 1) & ~(BLOCK_2M - 1);
	}
	ncm_bc(3, (uint32_t)sp_lo);
	ncm_bc(4, (uint32_t)sp_hi);
	ncm_bc(6, (uint32_t)sp);

	/* ---- Build every table (pure software, no hardware state touched yet). */
	build_l2_low();
	build_l2_high();
	build_l3_img();
	build_l3_bcw();
	build_l2_dram(sp_lo, sp_hi);
	build_l1();

	/* ---- Gate 3: self-check the tables we just built. */
	if (!self_check()) {
		ncm_bc(1, NCM_ST_ABORT_SELFCHECK);
		return;
	}
	ncm_bc(1, NCM_ST_TABLES_BUILT);

	clean_table_mem(el2_l1,      sizeof(el2_l1));
	clean_table_mem(el2_l2_low,  sizeof(el2_l2_low));
	clean_table_mem(el2_l2_dram, sizeof(el2_l2_dram));
	clean_table_mem(el2_l2_high, sizeof(el2_l2_high));
	clean_table_mem(el2_l3_img,  sizeof(el2_l3_img));
	clean_table_mem(el2_l3_bcw,  sizeof(el2_l3_bcw));

	mair  = build_mair_el2();
	tcr   = build_tcr_el2();
	ttbr0 = (uint64_t)(uintptr_t)&el2_l1[0];

	/* ---- The switch. Exact sequence: dsb (publish table writes) ; write
	 * MAIR_EL2 + TCR_EL2 + TTBR0_EL2 together (no memory access with
	 * translation happens between these three MSRs, so there is no window
	 * where a mixed old/new combination is actually used) ; isb (make the
	 * new context architecturally visible) ; tlbi alle2 (drop every EL2 TLB
	 * entry cached under the OLD table/ASID-less-EL2 context) ; dsb nsh
	 * (only CPU0 is active at this point — smp_init() has not run yet, see
	 * the ordering comment at the main_dbg.c call site — so a non-shareable
	 * barrier is architecturally sufficient here) ; isb again. This is
	 * exactly the "changing memory region attributes" sequence recommended
	 * for stage-1: retype first, THEN sweep stale cache state (below) —
	 * because DC *VAC-family instructions act on the cache by PHYSICAL
	 * address regardless of the CURRENTLY active mapping's cacheability, so
	 * running the sweep with the NEW (fully, provably mapped) tables
	 * already active also catches any last-instant speculative cache fill
	 * that happened in the tiny window between the old and new mappings —
	 * the textbook reason ARM recommends invalidating AFTER the retype, not
	 * only before it. */
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("msr mair_el2, %0" :: "r"(mair) : "memory");
	__asm__ volatile("msr tcr_el2, %0"  :: "r"(tcr)  : "memory");
	__asm__ volatile("msr ttbr0_el2, %0" :: "r"(ttbr0) : "memory");
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("tlbi alle2" ::: "memory");
	__asm__ volatile("dsb nsh" ::: "memory");
	__asm__ volatile("isb" ::: "memory");

	ncm_bc(1, NCM_ST_SWITCHED);
	ncm_bc(5, (uint32_t)ttbr0);

	/* ---- Transition maintenance: sweep every NC-marked block to PoC,
	 * skipping the HV image, the breadcrumb-window WB half, and the live
	 * stack margin by construction (see sweep_nc_region()). This is the
	 * ONE-TIME flush the old, now-dead `if (0)` block in main_dbg.c was
	 * trying to do — U-Boot itself leaves dirty/stale lines all over DRAM,
	 * and this removes them before emac_init()/kload/smp_init/the guest can
	 * ever race a DMA engine against one. Unlike the old attempt, this
	 * sweep provably cannot touch: the running HV image/code (excluded),
	 * the live call stack (excluded), or anything CPU1 might be using
	 * (CPU1 does not exist yet — smp_init() has not run). */
	sweep_nc_region(sp_lo, sp_hi);

	ncm_bc(1, NCM_ST_SWEPT_DONE);
	g_applied = 1;
}
