/* SPDX-License-Identifier: BSD-2-Clause */

/* snapshot.h — guest checkpoint / restore (VM snapshot) for the bzdOS EL2
 * microkernel (AArch64, Allwinner A64 / Banana Pi M64, 4x Cortex-A53).
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * The FreeBSD/arm64 EL1 guest takes ~300 s to reach the mountroot prompt /
 * userland, and then reboot-loops (cngrab NULL, PSCI SYSTEM_RESET we already
 * block in el2_exc.c). Every debug iteration therefore pays that ~300 s again.
 *
 * A *snapshot* captures the COMPLETE EL1/EL0 architectural state plus the
 * guest's DRAM at a known-good instant (e.g. sitting at `mountroot>` or in a
 * shell), so a later *restore* can drop the guest back into that exact state
 * in <1 s — turning "boot for 5 minutes to reproduce" into "restore and go".
 *
 * This is a DESIGN SKELETON (see docs/snapshot-restore-design.md for the full
 * rationale and docs/snapshot-integration.md for the exact edits to wire it
 * in). Real MRS/MSR sequences are provided where the encoding is unambiguous;
 * anything that needs on-board verification is marked TODO(board).
 *
 * ============================================================================
 * WHAT IS (AND IS NOT) CAPTURED
 * ============================================================================
 * CAPTURED (this module):
 *   - GP regs x0..x30, plus the guest PC (ELR_EL2) and PSTATE (SPSR_EL2) — all
 *     already present in `struct el2_frame` handed to us at a trap boundary.
 *   - SP_EL0, SP_EL1 (banked stack pointers).
 *   - The full EL1 system-register set that defines the guest's MMU /
 *     exception / EL0-access / timer configuration (see struct snapshot_sysregs).
 *   - EL2-side per-guest translation control: VTTBR_EL2 / VTCR_EL2 (the
 *     stage-2 tables the guest runs under) and HCR_EL2.
 *   - The guest's DRAM: PA [SNAP_DRAM_BASE, SNAP_DRAM_BASE+SNAP_DRAM_SIZE)
 *     (== stage-2 identity range 0x40000000..0x80000000, 1 GiB).
 *
 * NOT CAPTURED (the correctness risk — see design doc "Risks"):
 *   - MMIO device state: EMAC, MMC/eMMC, USB-OTG(MUSB), UART, HDMI, PMIC,
 *     GPIO/pinmux. Registers live in real silicon, not guest DRAM, and are
 *     NOT rolled back on restore. A restored guest resumes with the physical
 *     devices in whatever state they are in NOW, not at snapshot time.
 *   - GIC distributor / redistributor / CPU-interface state and any in-flight
 *     or pending IRQs, and (if vgic is in the build) the vGIC list registers.
 *   - In-flight DMA (eMMC FIFO, EMAC descriptor rings): a descriptor the guest
 *     programmed may complete into DRAM that the snapshot already copied.
 *   - Generic-timer counter: CNTPCT keeps advancing across the snapshot;
 *     restoring stale CNTV_CVAL/CNTP_CVAL produces a timer skew (see design).
 *
 * The snapshot is therefore only sound at a QUIESCED trap boundary with no
 * outstanding device transactions — see snapshot_save() preconditions.
 *
 * ============================================================================
 * FREESTANDING
 * ============================================================================
 * <stdint.h> only, no libc, -mgeneral-regs-only, matches the rest of the tree.
 * The whole module runs at EL2 with the flat U-Boot map on, so any PA is a
 * plain load/store (same assumption cmd_read_words / coredump.c rely on).
 */
#ifndef BZDOS_SNAPSHOT_H
#define BZDOS_SNAPSHOT_H

#include <stdint.h>
#include "exceptions.h"   /* struct el2_frame */

/* ------------------------------------------------------------------ *
 * Guest DRAM range to snapshot — MUST match stage2.h's
 * STAGE2_DRAM_BASE / STAGE2_DRAM_SIZE (the stage-2 identity-mapped guest RAM).
 * Kept as local #defines (not an include of stage2.h) so this module has no
 * build dependency on the stage-2 lane; a static assert in snapshot.c pins
 * them together if stage2.h is ever included alongside.
 * ------------------------------------------------------------------ */
#define SNAP_DRAM_BASE   0x40000000UL   /* == STAGE2_DRAM_BASE  */
#define SNAP_DRAM_SIZE   0x40000000UL   /* == STAGE2_DRAM_SIZE (1 GiB) */

/* ------------------------------------------------------------------ *
 * Snapshot STORE region.
 *
 * The BPI-M64 2 GiB variant has PA 0x40000000..0xC0000000 of DRAM, but the
 * guest's stage-2 map only exposes the LOW 1 GiB (0x40000000..0x80000000 —
 * see coredump.c's DRAM_HI=0x80000000). The HIGH 1 GiB (0x80000000..
 * 0xC0000000) is invisible to the guest and unused by the hypervisor, so it is
 * the natural in-DRAM store: a full-DRAM checkpoint fits exactly once, with the
 * guest unable to observe or clobber it.
 *
 * Layout inside the store:
 *   SNAP_STORE_BASE + 0x0000            : struct snapshot_hdr (metadata,
 *                                         magic "SNP1", cache-coherent stores)
 *   SNAP_STORE_BASE + SNAP_META_SIZE    : verbatim copy of guest DRAM
 *                                         (SNAP_DRAM_SIZE bytes)
 *
 * On a 1 GiB board (no high DRAM) this store does not exist — fall back to the
 * eMMC store (emmc_bio_write, see design doc) or stream-over-EMAC. Selected at
 * build time by SNAP_STORE_KIND.
 * ------------------------------------------------------------------ */
#define SNAP_STORE_BASE   0x80000000UL          /* high (guest-invisible) GiB */
#define SNAP_META_SIZE    0x00010000UL          /* 64 KiB reserved for header */
#define SNAP_DRAM_STORE   (SNAP_STORE_BASE + SNAP_META_SIZE)

/* Metadata magic ("SNP1"), placed at word[0] of the store header. */
#define SNAP_MAGIC        0x534E5031u

/* Store back-ends (compile-time selectable). Only DRAM is fleshed out in the
 * skeleton; the others document their intended hook. */
enum snapshot_store_kind {
	SNAP_STORE_DRAM = 0,   /* high GiB reserved window (default, fastest) */
	SNAP_STORE_EMMC = 1,   /* emmc_bio_write() blocks (survives power loss) */
	SNAP_STORE_EMAC = 2,   /* stream over the dbgmon EMAC channel to host */
};

/* ------------------------------------------------------------------ *
 * The EL1 system-register set that, together with struct el2_frame and the
 * DRAM copy, fully defines the guest's architectural state. Ordering here is
 * the on-disk/store order; keep it stable (versioned via snapshot_hdr.version).
 *
 * Grouped by function for the reader; every field is a 64-bit sysreg. The
 * "generic timer" group is the one that needs the most care on restore (skew).
 * ------------------------------------------------------------------ */
struct snapshot_sysregs {
	/* --- MMU / translation --- */
	uint64_t sctlr_el1;    /* system control: M/C/I (MMU+caches), WXN, etc. */
	uint64_t ttbr0_el1;    /* translation table base 0 (+ ASID)            */
	uint64_t ttbr1_el1;    /* translation table base 1 (+ ASID)            */
	uint64_t tcr_el1;      /* translation control (granule, T0SZ/T1SZ, ...) */
	uint64_t mair_el1;     /* memory attribute indirection                 */
	uint64_t amair_el1;    /* aux memory attribute indirection             */
	uint64_t contextidr_el1;/* context ID (ASID mirror / debug)            */

	/* --- Exception / vector state --- */
	uint64_t vbar_el1;     /* EL1 vector base                              */
	uint64_t esr_el1;      /* last EL1 syndrome                            */
	uint64_t far_el1;      /* last EL1 fault address                       */
	uint64_t elr_el1;      /* EL1 exception link (guest's own EL1 ELR)     */
	uint64_t spsr_el1;     /* EL1 saved PSTATE (guest's own EL1 SPSR)      */
	uint64_t sp_el0;       /* EL0 stack pointer                            */
	uint64_t sp_el1;       /* EL1 stack pointer (banked; live SP at EL1h)  */

	/* --- Thread / TLS pointers --- */
	uint64_t tpidr_el0;    /* EL0 thread pointer (RW)                      */
	uint64_t tpidrro_el0;  /* EL0 thread pointer (RO)                      */
	uint64_t tpidr_el1;    /* EL1 thread pointer                           */

	/* --- EL0 access control / feature enables --- */
	uint64_t cpacr_el1;    /* FP/SIMD + trace trap control                 */
	uint64_t mdscr_el1;    /* debug: MDE/SS/KDE (must be restored coherent
	                        * with any EL2 single-step state)              */
	uint64_t pmcr_el0;     /* perf-monitor control (optional, FreeBSD uses) */

	/* --- Generic timer (SKEW-SENSITIVE, see design "timer skew") --- */
	uint64_t cntkctl_el1;  /* EL0 timer access control                     */
	uint64_t cntp_ctl_el0; /* physical timer control (enable/imask/istatus) */
	uint64_t cntp_cval_el0;/* physical timer compare value (ABSOLUTE count) */
	uint64_t cntv_ctl_el0; /* virtual timer control                        */
	uint64_t cntv_cval_el0;/* virtual timer compare value (ABSOLUTE count)  */
	/* CNTVOFF_EL2 is captured in snapshot_hdr (EL2-owned) not here. */

	/* --- EL2-side per-guest translation (stage-2 + trap routing) --- */
	uint64_t vttbr_el2;    /* stage-2 table base + VMID                    */
	uint64_t vtcr_el2;     /* stage-2 translation control                  */
	uint64_t hcr_el2;      /* host control (VM/IMO/RW/TSC bits)            */
	uint64_t cntvoff_el2;  /* virtual counter offset (timer skew knob)     */
};

/* ------------------------------------------------------------------ *
 * Store header. Written to SNAP_STORE_BASE by snapshot_save(), read back by
 * snapshot_restore(). All multi-word fields are little-endian; the whole
 * header is written with the dc-civac + dsb-sy coherent-store convention so a
 * warm WDT reset (DRAM survives) does not lose it and a host `bc`/`d` dump of
 * 0x80000000 shows a valid record.
 * ------------------------------------------------------------------ */
struct snapshot_hdr {
	uint32_t magic;        /* SNAP_MAGIC ("SNP1")                          */
	uint32_t version;      /* struct layout version (bump on any change)   */
	uint32_t valid;        /* 0 while writing, 1 once fully committed       */
	uint32_t store_kind;   /* enum snapshot_store_kind used                */

	uint64_t dram_base;    /* SNAP_DRAM_BASE (sanity vs restore target)    */
	uint64_t dram_size;    /* SNAP_DRAM_SIZE                               */
	uint64_t dram_store;   /* PA where the DRAM copy lives (DRAM back-end) */
	uint64_t taken_cntpct; /* CNTPCT_EL0 at save time (for timer re-base)  */

	struct el2_frame        frame;    /* x0..x30, ELR_EL2, SPSR_EL2, ...   */
	struct snapshot_sysregs sysregs;  /* full EL1 + per-guest EL2 set      */

	uint32_t crc32;        /* optional integrity check over DRAM copy      */
	uint32_t reserved;
};

#define SNAPSHOT_VERSION  1u

/* ------------------------------------------------------------------ *
 * Public API. Both are meant to be invoked at a GUEST TRAP BOUNDARY, from the
 * dbgmon tick path (dbgmon_service / a new `snap`/`rest` command) with the
 * live guest `frame` — i.e. the exact frame el2_common will eret back into.
 * At that instant the guest is quiesced at EL2 and `frame` holds a consistent
 * x0..x30 / PC / PSTATE; the EL1 sysregs are readable via MRS from EL2.
 * ------------------------------------------------------------------ */

/* Capture the full guest state (frame + EL1 sysregs + DRAM) into the store.
 * `frame` MUST be the current guest trap frame. Returns 0 on success, negative
 * on a store failure (e.g. eMMC back-end timeout). Non-destructive: the guest
 * can be resumed normally afterwards (this is a checkpoint, not a teardown). */
int snapshot_save(const struct el2_frame *frame);

/* Restore a previously saved snapshot into the live guest. Overwrites guest
 * DRAM, reloads all EL1 sysregs + stage-2 control, and rewrites `frame` in
 * place so that when el2_trap returns and el2_common erets, the CPU lands in
 * the restored guest at the saved PC/PSTATE with the saved registers.
 * Returns 0 on success, negative if no valid snapshot is present.
 *
 * Caches/TLBs are invalidated as required (see snapshot.c). SMP: secondary
 * cores MUST be parked before calling (see design "SMP considerations"). */
int snapshot_restore(struct el2_frame *frame);

/* True (1) if a committed snapshot is present in the store. */
int snapshot_present(void);

#endif /* BZDOS_SNAPSHOT_H */
