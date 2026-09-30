/* SPDX-License-Identifier: BSD-2-Clause */

/* gic_timer.h — GICv2 (GIC-400) + ARM Generic Timer (EL2 physical timer)
 * periodic-tick driver for the bzdOS microkernel/hypervisor.
 *
 * This is the interrupt-driven counterpart to timer.h's polling timebase:
 * it programs the non-secure physical timer comparator (CNTP), routes its PPI
 * through the GIC-400 distributor + CPU interface, and exposes a single
 * entry point — gic_timer_irq() — meant to be called from the EL2 IRQ arm
 * of el2_trap() (exceptions.S / el2_exc.c, owned by the integration lane).
 *
 * Freestanding, no libc: only <stdint.h>. Depends on exceptions.h (for
 * struct el2_frame — the IRQ handler signature matches el2_trap's call
 * site) and timer.h (timer_now(), timer_freq(), struct jitter).
 *
 * WHAT THIS FILE DOES NOT DO — read before wiring it in:
 *   - It does not unmask PSTATE.I/PSTATE.F (the DAIF I/F bits). A CPU
 *     sitting at EL2 with those masked will never take the interrupt no
 *     matter how the GIC/timer are programmed. gic_timer_init() arms the
 *     timer and the GIC but deliberately leaves DAIF alone — enabling
 *     delivery is a scheduling decision (when is the rest of the system
 *     ready for preemption?) that belongs to the integration lane.
 *
 *     ROUTING: this driver uses INTID 30 (CNTP, the non-secure physical
 *     timer), which firmware already owns as a non-secure GIC Group 1
 *     interrupt — delivered to the non-secure world (where EL2 lives on
 *     this part) as an IRQ. So unmasking IRQ — `msr daifclr, #2` — is the
 *     correct and sufficient action for the tick to arrive. RECOMMENDED
 *     belt-and-suspenders: use `msr daifclr, #3` (clears BOTH I and F).
 *     el2_trap already routes FIQ→gic_timer_irq, so clearing F as well
 *     guarantees the tick lands even if some board delivered it as FIQ.
 *     (History: v1/v2 targeted CNTHP/INTID 26 — the EL2 physical timer —
 *     but two hardware tests proved that PPI is secure-owned on this
 *     two-security-state GIC-400 and never reaches non-secure EL2; v3
 *     switched to CNTP/INTID 30. See gic_timer.c for the full evidence.)
 *   - It DOES set HCR_EL2.IMO = 1 (read-modify-write, one bit) so physical
 *     IRQs are routed to and taken at EL2. This is mandatory on this part:
 *     with IMO=0 a physical IRQ is routed to EL1, and an async exception
 *     targeting a lower EL than the current one is never taken while we run
 *     at EL2 — that was the v1-v3 "pending but never taken" bug. HCR_EL2 is
 *     really EL2 setup that could equally live in el2_install(); setting it
 *     here is idempotent (OR of one bit), so the integration lane may move
 *     it with no behavioural change. FMO/AMO are left untouched (the tick
 *     is a Group 1 IRQ; IMO suffices).
 */
#ifndef BZDOS_GIC_TIMER_H
#define BZDOS_GIC_TIMER_H

#include <stdint.h>
#include "exceptions.h"
#include "timer.h"

/* Initialize the GIC-400 distributor + CPU interface for exactly the one
 * PPI this module uses (non-secure physical timer CNTP, INTID 30), and arm the first
 * timer interval. `period_us` is the desired tick period in microseconds
 * (e.g. 1000 for a 1 ms tick); it is converted to ticks via timer_freq().
 *
 * Safe to call once, after el2_install() and before IRQs are unmasked.
 * Does NOT unmask PSTATE.I — see the header-comment note above.
 */
void gic_timer_init(uint32_t period_us);

/* gic_timer_init() but leaving CNTVOFF_EL2 exactly as it was.
 *
 * For the IMO=1 / vGIC policy (main_dbg.c), where the ONLY reason to want
 * this module's CNTP tick is that vtimer_mask_watchdog() -- the recovery for a
 * self-latching CNTV mask -- is called from the tick handler and is otherwise
 * unreachable. Found the hard way 2026-07-30: one lost virtual CNTV injection
 * left CNTV_CTL.IMASK set, which stops further CNTV PPIs, which removes every
 * chance to re-inject, which killed the guest's timebase for the rest of the
 * boot. An independent EL2 tick is the only thing that can notice and undo it.
 *
 * gic_timer_init() zeroes CNTVOFF_EL2 (see its own comment: a workaround for a
 * FreeBSD DELAY() hang under the OLD pre-vGIC policy). Under the current
 * policy the guest boots and ticks fine on the CNTVOFF ATF left in place --
 * 8047 forwarded CNTV ticks were measured before the mask latched -- so
 * changing it here would alter a working virtual timebase for no reason, and
 * would confound any single-boot verification. Save and restore it instead.
 *
 * Claiming INTID 30 is safe on this board: irq_counter[30] was measured 0
 * across a full boot, i.e. the FreeBSD guest never programs CNTP (it uses
 * CNTV). gic_timer_irq() also has to route INTID 30 to EL2's own tick rather
 * than forwarding it like any other physical IRQ -- see its vgic_active()
 * condition. */
void gic_timer_arm_preserving_cntvoff(uint32_t period_us);

/* Just the CPU-interface-wide + distributor-group-enable half of
 * gic_timer_init() above (GICD_CTLR=0x3, GICC_PMR=0xff, GICC_CTLR=0x3|
 * EOImode) — NOT per-INTID (no CNTP arm, no INTID-30-specific GICD writes,
 * no HCR_EL2.IMO write, no diagnostic probe). For a build that wants EL2 to
 * see IRQs at the CPU-interface level (e.g. to run vgic_init() and forward
 * physical IRQs to a guest via List Registers) WITHOUT also running this
 * module's own periodic CNTP debug tick — main_dbg.c's interrupt-
 * virtualization policy is exactly that case: dbgmon already gets served via
 * vconsole traps, and wdt_pet() now fires on every EL2 exception (which
 * happens on every guest device IRQ too), so the CNTP tick is redundant
 * there. gic_timer_init() itself calls this internally, so calling BOTH
 * (this then gic_timer_init()) would simply re-do these three writes
 * harmlessly — but a caller that wants the tick-free path should call only
 * this one. Idempotent, safe to call once before IRQs are unmasked. */
void gic_timer_cpuif_init(void);

/* Target the real MUSB "mc" SPI (MUSB_IRQ_INTID, musb.h) at CPU1's GIC CPU
 * interface alone and enable it at the distributor, so gic_timer_irq() can
 * service usbacm_poll() straight from that IRQ instead of a fixed-period
 * tick. Part of the CPU1-as-vCPU1 design (vcpu1.c, EXPERIMENTAL) — see
 * gic_timer.c's own header comment above this function's definition for
 * the full rationale and the UNVERIFIED hardware assumptions (ITARGETSR
 * bit-to-core mapping, ICFGR level config) a board session must confirm.
 * Call once, from vcpu1_run(), after gic_timer_cpuif_init() and before
 * unmasking IRQs on CPU1. */
void musb_irq_arm_cpu1(void);

#ifdef HV_HDMI
/* Target the real TCON1 vblank SPI (HDMI_TCON1_IRQ_INTID, hdmi.h) at CPU1's
 * GIC CPU interface alone, enable it at the distributor, AND turn on the
 * device-level interrupt enable bit (hdmi_vblank_irq_enable(), hdmi.c) so it
 * actually asserts -- so gic_timer_irq() can service hdmi_vblank_poll()
 * (the HUD repaint / real-vblank duty vcpu1.h documents as otherwise lost
 * when CPU1 becomes a third vCPU) straight from that IRQ instead of a
 * fixed-period tick, mirroring musb_irq_arm_cpu1() exactly. See
 * gic_timer.c's own header comment above this function's definition for the
 * full rationale and the same UNVERIFIED hardware assumptions
 * musb_irq_arm_cpu1() carries (ITARGETSR bit-to-core mapping, ICFGR level
 * config) -- this function has NOT been exercised on real hardware yet.
 * Call once, from vcpu1_run(), after gic_timer_cpuif_init() and before
 * unmasking IRQs on CPU1 -- same call site as musb_irq_arm_cpu1(), see
 * vcpu1.h for exactly where. Guarded by HV_HDMI (target-scoped CFLAGS, see
 * gic_timer.c) because HDMI_TCON1_IRQ_INTID is declared in hdmi.h, which
 * this header does not otherwise depend on. */
void hdmi_irq_arm_cpu1(void);
#endif

/* Call this from el2_trap()'s IRQ case (kind & 3 == EL2_KIND_IRQ). Reads
 * GICC_IAR (acknowledges), confirms the INTID is ours, samples the jitter
 * meter, re-arms the next interval, writes GICC_EOIR, and increments the
 * tick counter. Bounded, non-blocking, safe from interrupt context: no
 * loops other than the fixed-count breadcrumb writes, no allocation.
 *
 * If the acknowledged INTID is not ours (spurious 1023, or some other
 * source we didn't expect since only one PPI is enabled), it still EOIs
 * any non-spurious ID to avoid leaving the GIC's active bit stuck, then
 * returns without touching the timer or jitter state.
 */
void gic_timer_irq(struct el2_frame *frame);

/* Total ticks handled so far (monotonic, wraps only after 2^64 ticks). */
/* GICD_ISENABLER for one SPI (the IDMAC queues re-arm their line). */
void gic_timer_spi_enable(uint32_t intid);
uint64_t gic_timer_ticks(void);

/* Read-only access to the internal jitter tracker so a REPL command can
 * report min/max/last delta and deviation without this module having to
 * grow its own formatting code. Returns a pointer to module-static storage
 * — treat as read-only; do not call jitter_init/jitter_sample on it from
 * outside gic_timer.c. */
const struct jitter *gic_timer_jitter(void);

#endif /* BZDOS_GIC_TIMER_H */
