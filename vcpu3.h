/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu3.h — a THIRD vCPU for the FreeBSD guest, on CPU3.
 *
 * Byte-for-byte the same design as vcpu2.h (CPU2), including the fix that
 * file needed before it was safe to arm: vgic_init() on this core, without
 * which every physical interrupt CPU3 takes -- in particular the
 * smp_rendezvous IPI FreeBSD's SMP bring-up sends the instant a 3rd/4th vCPU
 * needs cross-call coordination -- is accepted at the GIC and never reaches
 * the guest (see vcpu2.c's fix comment and RELEASE-0.0.2.md's account of the
 * identical CPU1 bug for the full mechanism). vcpu3.c is written WITH that
 * fix from the start, not retrofitted, precisely to avoid re-committing the
 * mistake this project has now made twice (vcpu1.c originally, vcpu2.c after
 * it) on a third file.
 *
 * WHAT THIS CHANGES
 *
 *      CPU0   guest vCPU0            (unchanged)
 *      CPU1   bzdk debug plane       (unchanged, and deliberately so)
 *      CPU2   guest vCPU (vcpu2.c), if armed -- otherwise async eMMC I/O
 *      CPU3   guest vCPU             <- this file, if armed
 *
 * WHY IT IS SAFE TO LET THE GUEST NAME AN ENTRY POINT HERE
 *
 * Identical argument to vcpu2.h's: el2_exc.c's PSCI filter (finding H3)
 * still never forwards CPU_ON to EL3, and the target core is already up and
 * parked in EL2 code; the guest's entry point is entered via kload_enter()
 * (an `eret` to EL1 under this core's own, ALREADY-SHARED stage-2 regime),
 * never a warm-boot at EL2. No new escalation surface.
 *
 * WHAT THE THIRD/FOURTH vCPU NEEDS, AND WHY NONE OF IT IS NEW
 *
 *   - Stage-2: the SAME tables as CPU0/CPU2. stage2_arm_secondary() arms
 *     this core's banked VTCR_EL2/VTTBR_EL2 against the tables stage2_init()
 *     already built -- one guest, one address space, however many PEs are
 *     armed. No second table set: that is stage2_zephyr.c's job, for a
 *     DIFFERENT guest, and is exactly what this file must never share a
 *     binary with (see the mutual-exclusion note below).
 *   - Interrupts: vgic_init() on THIS core (see the header note above).
 *   - EL1 configuration: guest_config(), core-agnostic, unchanged.
 *
 * MUTUALLY EXCLUSIVE WITH zguest_cpu3.c (the `dual` target's Zephyr guest),
 * BY CONSTRUCTION, AND ENFORCED AT LINK TIME, NOT JUST BY MAKEFILE
 * DISCIPLINE. Both files strongly define smp.c's cpu==3 dispatch target
 * (this file: vcpu3_run(); zguest_cpu3.c: zephyr_cpu3_run()), and BOTH now
 * additionally define the single non-weak marker symbol bzdos_cpu3_owner
 * (see vcpu3.c / zguest_cpu3.c) -- linking both objects into one image is a
 * genuine `multiple definition of bzdos_cpu3_owner` LINK ERROR, not a
 * runtime ambiguity to be discovered on hardware. The runtime hazard this
 * guards against, if the two were ever linked together with vcpu3 also
 * armed: vcpu3_run() parks on `wfe` waiting for FreeBSD's own PSCI CPU_ON
 * and, once that request lands, NEVER RETURNS (kload_enter() is noreturn)
 * -- so if the guest's DTB never advertises cpu@3 (feature armed but no
 * request ever arrives), CPU3 sits parked in vcpu3_run()'s wfe loop FOREVER
 * and zephyr_cpu3_run() -- and therefore the `dual` target's entire `zboot`
 * feature -- becomes silently unreachable. A comment saying "don't link
 * both" would rely on every future editor of the Makefile reading it; the
 * marker-symbol collision cannot be silently missed.
 *
 * DEFAULT OFF. dbg_vcpu3 gates it, so a build with this linked behaves
 * exactly as before until the flag is set. The guest also has to ASK: its
 * device tree must advertise cpu@3, or FreeBSD never issues the CPU_ON.
 */
#ifndef BZDOS_VCPU3_H
#define BZDOS_VCPU3_H

#include <stdint.h>

/* Set by el2_exc.c's PSCI filter when the guest issues CPU_ON for affinity 3.
 * Same contract as vcpu2_request()/vcpu1_request(): 1 if accepted (caller
 * answers PSCI SUCCESS), 0 if not (caller answers ALREADY_ON as before). */
int vcpu3_request(uint64_t entry_pa, uint64_t context_id);

/* CPU3's loop, called from smp_secondary_main() instead of
 * zephyr_cpu3_run() when dbg_vcpu3 is set. Parks on `wfe` until a request
 * arrives, then enters the guest at EL1 and never returns. Returns
 * immediately (falling through to zephyr_cpu3_run(), i.e. the `dual`
 * target's Zephyr path or the plain WFI park) if dbg_vcpu3 is 0 -- same
 * weak/strong dispatch shape vcpu2_run()/vblk_async_cpu2_run() already use
 * for CPU2. */
void vcpu3_run(void);

/* Runtime gate, default 0. Settable from dbgmon (`vcpu3 on`) so the feature
 * can be armed on a running board before the guest is rebooted into a DTB
 * that advertises the fourth core. */
extern volatile uint32_t dbg_vcpu3;

/* Breadcrumb layout at HVMAP_VCPU3_BC (hv_addrmap.h), all u32 -- identical
 * shape to VCPU1_BC/VCPU2_BC:
 *   [0] magic      'VCP3' 0x56435033
 *   [1] state      0=not reached, 1=parked, 2=request accepted, 3=entering EL1
 *   [2] requests   CPU_ON requests seen for affinity 3 (including refused)
 *   [3] entry_lo   low 32 bits of the guest's requested entry point
 *   [4] entry_hi   high 32 bits
 *   [5] ctxid_lo   low 32 bits of the guest's context ID (x0 at entry)
 *   [6] refused    requests refused, and why is in [7]
 *   [7] last_why   0=accepted, 1=gate off, 2=already handed over, 3=not parked
 *   [8] vtcr       VTCR_EL2 as read ON CPU3, after arming
 *   [9] vttbr_lo   VTTBR_EL2 low 32, ditto
 *  [10] vttbr_hi   VTTBR_EL2 high 32
 *  [11] hcr        HCR_EL2 low 32
 *  [12] sctlr_el1  SCTLR_EL1 low 32, just before entering the guest
 */
#define VCPU3_MAGIC        0x56435033u
#define VCPU3_BC_NWORDS    16

#endif /* BZDOS_VCPU3_H */
