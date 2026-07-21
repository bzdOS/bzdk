/* el2_ncmap.h — EL2 stage-1 remap: make the hypervisor itself a NON-CACHEABLE
 * observer of guest-owned DRAM, so no HV-side cache line can ever go stale or
 * dirty against the guest's non-coherent IDMAC/EMAC DMA. See
 * docs/aw-mmc-dma-coherency.md for the full root-cause analysis and
 * docs/el2-nc-guest-dram.md for this fix's design/inventory/residual risks.
 *
 * SCOPE (read before calling): this touches ONLY our own EL2 stage-1
 * (TTBR0_EL2 / TCR_EL2 / MAIR_EL2) — the translation regime that applies
 * when the CPU is executing AT EL2 (main(), trap handlers, dbgmon). It does
 * NOT touch stage-2 (VTCR_EL2/VTTBR_EL2, stage2.c) or the guest's own EL1
 * stage-1 — what the GUEST sees of its own memory is completely unchanged.
 * Before this file existed, EL2 ran on whatever flat, cacheable (Normal-WB)
 * identity map U-Boot's `go` handed it (confirmed by stage2.h's own header:
 * "we do NOT touch stage-1 anywhere ... our own EL2 stage-1 ... stays
 * exactly as U-Boot's flat mapping left it"). This file replaces that
 * inherited mapping with a purpose-built one, in HV .bss, and switches to it.
 *
 * ENTRY POINT: call el2_ncmap_apply() exactly once from main(), AFTER
 * el2_install() + wdt_arm() and BEFORE smp_init() (see main_dbg.c's call
 * site comment for the full ordering rationale). It is FAIL-SAFE: if any of
 * its runtime assumptions don't hold (see el2_ncmap.c), it leaves a
 * diagnostic breadcrumb and returns WITHOUT switching TTBR0_EL2 — the HV
 * keeps running on U-Boot's original tables exactly as it does today. It
 * never leaves the system in a half-switched state.
 */
#ifndef BZDOS_EL2_NCMAP_H
#define BZDOS_EL2_NCMAP_H

/* Guest-owned DRAM, per PROJECT.md / docs/aw-mmc-dma-coherency.md. This is
 * the OUTER bound of the region this file may re-attribute; two islands
 * inside it (below) are deliberately left Normal-WB, untouched. */
#define EL2_NCMAP_DRAM_BASE   0x40000000UL
#define EL2_NCMAP_DRAM_END    0x80000000UL

/* Island 1: the HV image itself (link.ld base 0x42000000). Kept Normal-WB +
 * executable, exactly as today, because EL2 executes code from here. */
#define EL2_NCMAP_HVIMG_BASE  0x42000000UL
#define EL2_NCMAP_HVIMG_SIZE  0x00100000UL   /* 1 MiB; actual image ~224 KiB
                                               * as of 2026-07-20 (see nm),
                                               * checked at runtime against
                                               * __bss_end before switching */

/* Island 2: the breadcrumb/debug DRAM window (vconsole ring, virtio-blk
 * breadcrumbs, eMMC lock word, etc. — see smp.h's window map). Kept
 * Normal-WB, NOT executable (data only). */
#define EL2_NCMAP_BCW_BASE    0x50000000UL
#define EL2_NCMAP_BCW_SIZE    0x00100000UL

/* Low-memory MMIO window this HV actually touches (GIC, CCU, PIO, MUSB,
 * EMAC, eMMC controller — see docs/el2-nc-guest-dram.md inventory). Mapped
 * Device-nGnRE, unchanged in spirit from today. */
#define EL2_NCMAP_MMIO_BASE   0x01000000UL
#define EL2_NCMAP_MMIO_END    0x02000000UL

/* Apply the remap. See el2_ncmap.c for the full build/self-check/switch/
 * transition-maintenance sequence. Safe to call exactly once; idempotent
 * (a second call is a harmless no-op) but main_dbg.c only ever calls it once. */
void el2_ncmap_apply(void);

#endif /* BZDOS_EL2_NCMAP_H */
