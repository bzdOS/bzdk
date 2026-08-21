/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu2.h — a SECOND vCPU for the FreeBSD guest, on CPU2.
 *
 * WHAT THIS CHANGES
 *
 * Until now the guest has been single-vCPU by construction: guest_enter() is
 * called exactly once, from CPU0, and smp_secondary_main() never calls it for
 * CPU1..3 — they run EL2's own loops forever (CPU1 the debug plane, CPU2 async
 * eMMC I/O, CPU3 idle or Zephyr in the `dual` build). A guest PSCI CPU_ON for
 * any of them was answered ALREADY_ON, truthfully: they really were up, running
 * hypervisor code.
 *
 * This gives CPU2 to the guest, so the target allocation becomes:
 *
 *      CPU0   guest vCPU0            (unchanged)
 *      CPU1   bzdk debug plane       (unchanged, and deliberately so)
 *      CPU2   guest vCPU1            <- this file
 *      CPU3   idle, or Zephyr in the `dual` build
 *
 * CPU1 IS NOT UP FOR DISCUSSION. It owns the EMAC/dbgmon channel, the GDB stub,
 * and — the part that matters — the hardware watchdog kick. It is the only
 * automatic recovery path this board has: a total EL2 wedge lets the WDOG fire
 * and reboot to U-Boot, where chimpd catches it and reloads, with no human and
 * no physical power cycle. That has saved this board repeatedly. Handing it to
 * the guest would mean designing a replacement crash-recovery mechanism first,
 * which is a separate body of work, not a bigger version of this one.
 *
 * WHY IT IS SAFE TO LET THE GUEST NAME AN ENTRY POINT HERE
 *
 * el2_exc.c's PSCI filter (finding H3) refuses to forward CPU_ON to EL3, and
 * the reason is exact: PSCI CPU_ON warm-boots the target core at whatever EL
 * the platform's own warm-boot path uses, which on this SoC is EL2 for every
 * core this hypervisor CPU_ON's itself. Forwarding a guest-supplied
 * entry_point_address would therefore execute guest memory AT EL2 — full
 * hypervisor privilege from EL1.
 *
 * This file does NOT relax that. EL3 is still never asked. The target core is
 * already up and parked in EL2 code below; the guest's entry point is entered
 * via kload_enter(), i.e. an `eret` to **EL1**, under this core's own stage-2
 * regime. The guest picks where its own vCPU starts executing, at its own
 * exception level, which is exactly what PSCI is for and carries no escalation.
 *
 * WHAT THE SECOND vCPU NEEDS, AND WHY NONE OF IT IS NEW
 *
 *   - Stage-2: the SAME tables as CPU0. VTTBR_EL2/VTCR_EL2 are banked per-PE,
 *     so stage2_enable() on this core arms this core's regime against the
 *     tables stage2_init() already built. One guest, one address space, two
 *     PEs — no second table set, unlike stage2_zephyr.c which is a different
 *     guest and therefore needs one.
 *   - Interrupts: vgic.c is already per-core (struct vg_percpu g_vg[] indexed
 *     by smp_cpu_id()), and gic_timer_cpuif_init() was factored out precisely
 *     so a caller can open one core's GIC CPU interface. Both predate this file.
 *   - EL1 configuration: guest_config() is core-agnostic and already reused
 *     verbatim by main_zephyr.c and zguest_cpu3.c.
 *
 * So this file is a park-and-enter sequence, structurally identical to
 * zguest_cpu3.c, and deliberately written to look like it.
 *
 * DEFAULT OFF. dbg_vcpu2 gates it, so a build with this linked behaves exactly
 * as before until the flag is set — CPU2 keeps doing async eMMC I/O. The guest
 * also has to ASK: its device tree must advertise cpu@2, or FreeBSD never
 * issues the CPU_ON in the first place (the shipped bananapi-min.dtb lists only
 * cpu@0, which is why hw.ncpu has always read 1).
 */
#ifndef BZDOS_VCPU2_H
#define BZDOS_VCPU2_H

#include <stdint.h>

/* Set by el2_exc.c's PSCI filter when the guest issues CPU_ON for affinity 2.
 * Records the guest's requested entry point and context ID and wakes the parked
 * core. Returns 1 if the request was accepted (caller should answer
 * PSCI SUCCESS), 0 if it was not (caller answers ALREADY_ON as before) — which
 * is the case when dbg_vcpu2 is off, or the core has already been handed over,
 * or it has not reached its park loop yet. */
int vcpu2_request(uint64_t entry_pa, uint64_t context_id);

/* CPU2's loop, called from smp_secondary_main() instead of
 * vblk_async_cpu2_run() when dbg_vcpu2 is set. Parks on `wfe` until a request
 * arrives, then enters the guest at EL1 and never returns. */
void vcpu2_run(void);

/* Runtime gate, default 0. Settable from dbgmon (`vcpu2 on`) so the feature can
 * be armed on a running board before the guest is rebooted into a DTB that
 * advertises the second core. */
extern volatile uint32_t dbg_vcpu2;

/* Breadcrumb layout at HVMAP_VCPU2_BC (hv_addrmap.h), all u32:
 *   [0] magic      'VCP2' 0x56435032
 *   [1] state      0=not reached, 1=parked, 2=request accepted, 3=entering EL1
 *   [2] requests   CPU_ON requests seen for affinity 2 (including refused)
 *   [3] entry_lo   low 32 bits of the guest's requested entry point
 *   [4] entry_hi   high 32 bits
 *   [5] ctxid_lo   low 32 bits of the guest's context ID (x0 at entry)
 *   [6] refused    requests refused, and why is in [7]
 *   [7] last_why   0=accepted, 1=gate off, 2=already handed over, 3=not parked
 *   [8] vtcr       VTCR_EL2 as read ON CPU2, after arming
 *   [9] vttbr_lo   VTTBR_EL2 low 32, ditto
 *  [10] vttbr_hi   VTTBR_EL2 high 32
 *  [11] hcr        HCR_EL2 low 32
 *  [12] sctlr_el1  SCTLR_EL1 low 32, just before entering the guest
 *
 * 8..12 exist because these registers are BANKED PER-PE: reading them over the
 * debug channel returns CPU1's copy, not this core's. Publishing them from the
 * core itself is the only honest way to see them.
 */
#define VCPU2_MAGIC        0x56435032u
#define VCPU2_BC_NWORDS    16

#endif /* BZDOS_VCPU2_H */
