/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu1.h — a THIRD vCPU for the FreeBSD guest, on CPU1 — EXPERIMENTAL.
 *
 * READ THIS BEFORE ARMING dbg_vcpu1. It trades away this project's only
 * independent crash-recovery witness for a third vCPU's worth of guest
 * compute. That is a real, documented tradeoff, not an oversight — see
 * vcpu2.h's own "CPU1 IS NOT UP FOR DISCUSSION" banner, which this file
 * exists specifically to revisit with a mitigation, not to quietly ignore.
 *
 * WHY CPU1 WAS UNTOUCHABLE UNTIL NOW
 *
 * CPU1 runs a tight, IRQ-masked poll loop (smp.c's SMP_DEBUG_CPU branch):
 * EMAC/dbgmon service, the GDB stub, the HDMI HUD/vblank/relock, the
 * USB-ACM console bridge, and — the part that matters here — an
 * UNCONDITIONAL hardware-watchdog kick every single pass. Because that core
 * NEVER executes guest/EL1 code, nothing the guest does (a spin loop, a
 * deadlock, a driver bug) can ever touch it: it is hardware-independent of
 * whatever CPU0 is doing. That independence is the entire reason a total
 * EL2/guest wedge on CPU0 still self-recovers — CPU1 stops feeding the
 * watchdog only if CPU1 itself dies, which nothing on CPU0 can cause.
 *
 * THE MITIGATION THIS FILE IMPLEMENTS
 *
 * HCR_EL2.IMO=1 (already this project's standing policy — main_dbg.c sets
 * it, gic_timer.c's own header explains why: physical IRQs route to EL2
 * UNCONDITIONALLY once IMO=1, regardless of the running EL1 context's own
 * PSTATE.I mask — that is precisely what lets gic_timer.c's existing tick
 * preempt CPU0's guest today even if the guest tried to mask interrupts).
 * The same guarantee applies to CPU1 the moment it starts running guest
 * code under IMO=1: a periodic physical timer IRQ on THIS core traps to EL2
 * no matter what the vCPU running there is doing, SOFTWARE-WISE. So this
 * file arms a periodic CNTP tick on CPU1 (gic_timer_arm_preserving_cntvoff,
 * the same mechanism CPU0 already uses) and gic_timer_irq() — see the
 * `smp_cpu_id() == SMP_DEBUG_CPU && dbg_vcpu1` block added there — kicks the
 * hardware watchdog and services one bounded dbgmon_service() call on EVERY
 * tick, GIC-priority-preempting whatever guest code was running on CPU1,
 * before that guest code gets to run again.
 *
 * WHAT THIS DOES NOT FIX, HONESTLY
 *
 * A *software* guest hang on CPU1 (spin loop, masked EL1 interrupts, a
 * deadlocked driver) can no longer starve the watchdog kick — IMO=1 routes
 * around it exactly as it already does for CPU0. What is NOT recovered is a
 * *hardware*-level wedge of this specific physical core (a cache-coherency
 * deadlock, an exception storm that also corrupts EL2's own state on this
 * core, a bus hang) — if the core itself stops fetching instructions, no
 * amount of GIC priority helps, because EL2's tick handler can't run either.
 * That is the residual risk this project accepted CPU1's separateness to
 * avoid, and it is real: this file does not make it go away, it narrows it
 * from "any guest bug on CPU1" down to "a hardware-level wedge of CPU1
 * specifically" — which is rarer, but not impossible, and this project's own
 * history has hit strange hardware-level wedge states before (see the
 * hold-gate-plus-breakpoint-crashes-board incident). Whoever arms this
 * feature is accepting that narrower, but nonzero, residual risk.
 *
 * WHAT ELSE STOPS RUNNING ON CPU1 WHEN THIS IS ARMED
 *
 * UPDATED 2026-08-26: the HDMI HUD repaint / real-vblank duty described
 * below as a flat scope cut now has a real-interrupt equivalent, built the
 * same way the USB-ACM path already was — read on for what changed and
 * what is still NOT hardware-verified.
 *
 * The eMMC PC5 pinmux enforcement and (under HV_HDMI) the PHY relock get a
 * tick-path equivalent, on the 10 ms VCPU1_TICK_PERIOD_US cadence (see
 * gic_timer_irq()'s dbg_vcpu1 block).
 *
 * Neither the USB-ACM console bridge (usbacm.c) nor the HDMI vblank/HUD
 * duty moves to that same 10 ms tick — a fixed period is the wrong fit for
 * either: USB full-speed frame timing (~1 ms/frame; a 10 ms tick would miss
 * ~10 frames' worth of MUSB endpoint state-machine servicing between calls)
 * and a ~16.7 ms 60 Hz vblank period (a 10 ms tick can straddle or miss an
 * edge of TCON_INT0's LATCHING status bit — hdmi.c's own header on that
 * register documents a past bug from assuming otherwise). Both are instead
 * wired to their REAL hardware SPI, targeted at CPU1 alone, and serviced
 * the instant the device asserts an event — genuinely event-driven, not a
 * period either duty has to wait out:
 *
 *   - USB-ACM: musb.h's MUSB_IRQ_INTID (GIC SPI 71, "mc" — cited from the
 *     live DTB, not guessed), armed by musb_irq_arm_cpu1() (gic_timer.c)
 *     and serviced by gic_timer_irq()'s MUSB_IRQ_INTID arm, which calls
 *     usbacm_poll(). See musb.h and gic_timer.c's musb_irq_arm_cpu1()
 *     comment for exactly what is and is not hardware-verified about this
 *     (GICD_ITARGETSR's bit-to-core mapping, GICD_ICFGR's level config) —
 *     this one IS hardware-proven (see MUSB_IRQ_BUDGET_PER_TICK's comment
 *     in gic_timer.c for the live storm it caught).
 *
 *   - HDMI vblank/HUD: hdmi.h's HDMI_TCON1_IRQ_INTID (GIC SPI 87, the
 *     TV-facing TCON that feeds HDMI, "lcd-controller@1c0d000" — cited
 *     from the same live DTB, matching TCON1_BASE in hdmi.c exactly),
 *     armed by hdmi_irq_arm_cpu1() (gic_timer.c, built under #ifdef
 *     HV_HDMI) and serviced by gic_timer_irq()'s HDMI_TCON1_IRQ_INTID arm,
 *     which calls the SAME hdmi_vblank_poll() the old smp.c tight loop
 *     used to call directly — so the vblank count/timestamp the guest's
 *     scanout register file publishes keep meaning exactly what they meant
 *     before. The HUD's own periodic REPAINT (as opposed to vblank
 *     counting) is not separately re-armed here: hdmi_vblank_poll() was
 *     always the pacing signal, not the paint call, so restoring it
 *     restores the pacing a repaint loop would consume, but this file does
 *     not itself add a repaint call to the tick or IRQ path — a real,
 *     narrower gap than the old blanket "HUD stops" cut, named rather than
 *     silently assumed closed. Carries the same MUSB_IRQ_BUDGET_PER_TICK-
 *     shaped storm budget (HDMI_IRQ_BUDGET_PER_TICK, gic_timer.c) as a
 *     precaution, but UNLIKE MUSB this path is NOT YET HARDWARE-VERIFIED
 *     AT ALL — built and passing `make dbg` + `ci.sh` only. Before trusting
 *     it live: confirm GICD_ITARGETSR_BYTE(HDMI_TCON1_IRQ_INTID) reads back
 *     0x02, confirm TCON_INT0 bit30 (the enable bit hdmi_vblank_irq_enable()
 *     sets) actually reads back 1, and confirm irq_counter[119] increments
 *     in CPU1's g_gt[] slot once a real vblank occurs — see
 *     hdmi_irq_arm_cpu1()'s own TODO(board) list (gic_timer.c) for the full
 *     set of unverified assumptions, including one this project has not
 *     needed before: that TCON_INT0's enable-bit position (BIT(30), a
 *     cross-SoC-family Linux convention, not a per-board DTB fact) is
 *     right on THIS silicon revision.
 *
 * WIRING IN THE CALL: hdmi_irq_arm_cpu1() is declared in gic_timer.h and
 * implemented in gic_timer.c (both files this pass was allowed to touch);
 * it is NOT yet called from anywhere, because the call site is vcpu1.c's
 * entry sequence, which is owned by another lane. Wire it in with a single
 * line, immediately next to (either just before or just after) the
 * existing `musb_irq_arm_cpu1();` call vcpu1_run() already makes — same
 * ordering constraint as that call: after gic_timer_cpuif_init() and before
 * IRQs are unmasked on CPU1. Guard it exactly the way the rest of this file
 * guards HDMI-specific code, i.e. `#ifdef HV_HDMI hdmi_irq_arm_cpu1();
 * #endif`, since gic_timer.h only declares the function under that same
 * guard.
 *
 * `dbgmon_service()` still runs every tick (see gic_timer.c), so the EMAC
 * debug channel and the hardware-watchdog kick — the two duties this file's
 * whole tradeoff is about — are exactly as live as before.
 *
 * THAT WAS TRUE ONLY AFTER ONE MORE FIX (2026-08-26). As first written, arming
 * this feature killed the EMAC debug channel a few minutes into every guest
 * boot, permanently — and the cause was NOT the reduced cadence, which was the
 * first (wrong) suspicion. dbgmon_service() calls console_poll() ->
 * emac_poll(), which drains up to N_RX_DESC (64) frames per call, so 100 Hz is
 * ~6400 frames/s of headroom: cadence was never the problem.
 *
 * The real cause was that smp.c calls vcpu1_run() BEFORE its own
 * `dbg_core_active = 1`, and vcpu1_run() never returns — so that flag stayed 0
 * forever whenever this feature was armed. It is not a status bit but the
 * EMAC/dbgmon SINGLE-OWNER MUTEX (emac.c's header: "emac_poll() is only ever
 * invoked from ONE core at a time ... so g_rx_slot/rx_ring are single-owner",
 * citing this flag as the gate). Stuck at 0, el2_exc.c's six
 * `if (!dbg_core_active) dbgmon_service(frame);` guards on the guest's MMIO
 * trap paths kept CPU0 servicing dbgmon while CPU1's tick did the same —
 * concurrent emac_poll() on two cores, permanently corrupting g_rx_slot and
 * the console ring's head/tail. Signature: fine during guest boot (few traps),
 * dead once the guest is active, never recovers however quiet the link gets,
 * while stateless paths serviced inside emac_poll() itself (dbgtools' raw peek,
 * hvdbg's address-independent wdt_reset) keep answering. The same stuck flag
 * also disabled el2_exc.c's guest-frame snapshot, so gr/sr/gdbstub would have
 * silently reported stale registers. vcpu1_run() now claims the flag on the
 * accepted path — see the long comment at that store for why it must be there
 * and not while still parked.
 *
 * KEEP IN MIND when touching either file: this feature moves an EMAC/dbgmon
 * duty from a foreground loop to an interrupt path, and any OTHER invariant
 * phrased as "only the debug core does X" is gated the same way. Grep
 * dbg_core_active before assuming a duty transferred just because the tick
 * calls the same function.
 *
 * gr/sr STILL MEAN CPU0's GUEST. dbgmon_service() is called from CPU1's own
 * tick with a SNAPSHOT of CPU0's guest frame (el2_snapshot_guest_frame()),
 * not CPU1's own vCPU2 frame — deliberately, to leave dbgmon's existing
 * gr/sr/gva semantics completely unchanged. CPU1's own vCPU2 register state
 * is not yet exposed through any command; that is a real limitation, named
 * rather than silently assumed away, and a natural follow-up (e.g. a core-
 * selecting `gr1`/`sr1`) once this has some hardware track record.
 *
 * WHY THIS FILE LOOKS LIKE vcpu2.c
 *
 * Deliberately: same park-and-enter structure, same stage2/HCR/guest_config
 * reuse argument (see vcpu2.h's own comment for why none of that is new).
 * The differences are exactly the two things CPU1 needs that CPU2 didn't:
 * arming a periodic CNTP tick (CPU2 has none), and the watchdog/dbgmon
 * service call wired into that tick rather than into a foreground loop.
 *
 * CURRENTLY ARMED BY DEFAULT — this said "DEFAULT OFF" until 2026-08-26 and
 * that is no longer true, so read it here rather than being surprised by it.
 * board-config.xml has `<feature name="vcpu1" ... enabled="true">`, so
 * gen_config.py writes VCPU1=1 into config.mk, the Makefile adds
 * -DVCPU1_DEFAULT_ON=1, AND the same run writes the matching cpu@1 node into
 * the DTB — the two halves stay in lockstep by construction, which is exactly
 * why gen_config.py exists (this feature was twice built armed with no cpu@1
 * node to ask for the core). Flipping that one attribute to "false" and re-
 * running gen_config.py disarms both halves together; `vcpu1 off` at the
 * dbgmon prompt still works at runtime on an already-running board.
 *
 * The tradeoff in this file's header is therefore LIVE, not hypothetical, in
 * the default `dbg` build: a SOFTWARE hang of the guest on CPU1 is covered by
 * the unmaskable tick, a HARDWARE-level wedge of that specific core is not.
 * Hardware track record so far: 4h09m continuous with hw.ncpu=2,
 * cpu1:preempt at 171472, and the watchdog kick plus the EMAC debug channel
 * responsive throughout.
 */
#ifndef BZDOS_VCPU1_H
#define BZDOS_VCPU1_H

#include <stdint.h>

/* Set by el2_exc.c's PSCI filter when the guest issues CPU_ON for affinity 1.
 * Same contract as vcpu2_request(): 1 if accepted (caller answers PSCI
 * SUCCESS), 0 if not (caller answers ALREADY_ON as before). */
int vcpu1_request(uint64_t entry_pa, uint64_t context_id);

/* CPU1's loop, called from smp_secondary_main() BEFORE the existing
 * SMP_DEBUG_CPU tight-loop branch, only when dbg_vcpu1 is set. Parks on
 * `wfe` until a request arrives, arms the periodic CNTP tick, then enters
 * the guest at EL1 and never returns. Returns immediately (falling through
 * to the existing debug-core loop) if dbg_vcpu1 is 0. */
void vcpu1_run(void);

/* Runtime gate, default 0. Settable from dbgmon (`vcpu1 on`) before the
 * guest is rebooted into a DTB that advertises cpu@1. ALSO read by
 * gic_timer_irq() (gic_timer.c) to decide whether CPU1's tick should kick
 * the watchdog and service dbgmon — declared weak there so builds without
 * vcpu1.o linked see a permanent 0. */
extern volatile uint32_t dbg_vcpu1;

/* Breadcrumb layout at HVMAP_VCPU1_BC (hv_addrmap.h), all u32 — same shape
 * as vcpu2.h's VCPU2_BC, plus one field this core's tick-path service needs:
 *   [0] magic      'VCP1' 0x56435031
 *   [1] state      0=not reached, 1=parked, 2=request accepted, 3=entering EL1
 *   [2] requests   CPU_ON requests seen for affinity 1 (including refused)
 *   [3] entry_lo   low 32 bits of the guest's requested entry point
 *   [4] entry_hi   high 32 bits
 *   [5] ctxid_lo   low 32 bits of the guest's context ID (x0 at entry)
 *   [6] refused    requests refused, and why is in [7]
 *   [7] last_why   0=accepted, 1=gate off, 2=already handed over, 3=not parked
 *   [8] vtcr       VTCR_EL2 as read ON CPU1, after arming
 *   [9] vttbr_lo   VTTBR_EL2 low 32, ditto
 *  [10] vttbr_hi   VTTBR_EL2 high 32
 *  [11] hcr        HCR_EL2 low 32
 *  [12] sctlr_el1  SCTLR_EL1 low 32, just before entering the guest
 *  [13] ticks      this core's own gic_timer tick count, as last observed by
 *                  the tick-path service block (gic_timer.c) — NOT written
 *                  by this file directly; gic_timer_irq() owns it, listed
 *                  here so the whole feature's state lives in one lane
 */
#define VCPU1_MAGIC        0x56435031u
#define VCPU1_BC_NWORDS    16

#endif /* BZDOS_VCPU1_H */
