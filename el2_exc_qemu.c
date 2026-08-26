/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_qemu.c — EL2 trap handler for the QEMU `virt` CI target.
 *
 * This is a NEW, minimal el2_trap(), not a reuse of el2_exc.c: the real
 * el2_exc.c is the full board debugger's dispatch table (dbgmon, vconsole
 * 16550 emulation, hwbp/onebp/single-step, vblk_emmc, wdt, smp, PSCI
 * intercept/forward, flight recorder, ...) and pulls in a dozen
 * Allwinner-specific modules that either don't apply here (no USB/eMMC/EMAC
 * under QEMU virt) or aren't needed for this milestone's proof (stage-2
 * identity mapping + guest EL1 entry + GICv2 timer preemption + a serial
 * console — see ROADMAP.md T3). Exactly the exceptions.S vector table /
 * frame-save contract is reused unchanged; only the C-level dispatch is
 * new and deliberately small.
 *
 * Behavior:
 *   - IRQ/FIQ (the GICv2 tick): ack/re-arm/EOI via gic_timer_qemu_irq(),
 *     bump guest.c's preemption counter when it interrupted the EL1 guest,
 *     print an occasional progress line, and — once enough ticks have
 *     landed to prove the guest was preempted repeatedly while running
 *     under stage-2 translation — print a single greppable PASS marker and
 *     cleanly terminate QEMU via PSCI SYSTEM_OFF. This is the one place a
 *     CI script needs to look for a deterministic "it worked" signal.
 *   - Guest (lower-EL) HVC: acknowledged and skipped (ELR+4), matching
 *     guest.c's own documented optional-hypercall demo, in case a future
 *     guest payload on this target issues one; this minimal target's own
 *     payload (guest_qemu_payload.c) does not.
 *   - Anything else (an unexpected guest fault, or an EL2-own synchronous
 *     exception): printed with ESR/ELR/FAR and then halted (wfi loop) —
 *     deliberately NOT auto-recovered/rebooted (there is no watchdog or
 *     supervisor on this target), so a CI run either prints PASS or hangs
 *     until the wrapping `timeout` kills it — both are unambiguous,
 *     greppable failure signals for a regression.
 */
#include <stdint.h>
#include "exceptions.h"
#include "guest.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* At the 100 ms period main_qemu.c arms, 20 ticks is ~2 seconds — long
 * enough to prove several preemptions happened, short enough that CI stays
 * fast. */
#define PASS_AFTER_TICKS 20u

/* qemu_poweroff()/report_fault() used to be defined here, verbatim-duplicated
 * across every QEMU CI handler; they now live in el2_exc_qemu_common.c as
 * qemu_psci_poweroff()/qemu_report_fault(). Behaviour is unchanged, including
 * this target's choice to HALT in wfi rather than power off after a fault (see
 * this file's header: a CI run either prints PASS or hangs until the wrapping
 * `timeout` kills it — both greppable). */
#define qemu_poweroff qemu_psci_poweroff

/* This target's own scenario config for the shared guest-sync chain.
 *
 * want_hvc_ack + hvc_advance_elr: preserves this file's original behaviour
 * exactly. NOTE, carried over from el2_exc_dual2_qemu.c's header rather than
 * quietly changed: the `+4` is architecturally WRONG for an HVC trap (ELR_EL2
 * already holds the post-call return address), and it is harmless here only
 * because this target's payload (guest_qemu_payload.c) never issues an HVC —
 * i.e. it is dead code. It is left as-is so this refactor changes no observable
 * behaviour; fixing it is a separate, deliberate change.
 *
 * No console, no SMC forward, no MMIO absorb: this minimal target's payload
 * uses none of them. Dynamic W^X promotion is unconditional in the shared
 * chain and needs no flag. */
static const struct qemu_guest_sync_ops qemu_ci_ops = {
	.want_hvc_ack    = 1u,
	.hvc_advance_elr = 1u,
};

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		if (gic_timer_qemu_irq(frame)) {
			uint64_t ticks = gic_timer_qemu_ticks();

			if ((kind >> 2) == 2u)
				guest_note_preempt();   /* interrupted the EL1 guest */

			if (ticks == 1u) {
				pl011_puts("HV: first tick — EL2 preempted the EL1 guest\n");
			} else if (ticks % 5u == 0u) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(" (preempted guest)\n");
			}

			if (ticks >= PASS_AFTER_TICKS) {
				pl011_puts("QEMU-CI: PASS (stage-2 + EL1 guest + GICv2 timer preemption confirmed)\n");
				qemu_poweroff();
			}
		}
		return;   /* never advance ELR/SPSR for an async exception */
	}

	/* Guest (lower-EL) synchronous exception: the shared chain handles the
	 * HVC acknowledgement this file's header documents AND the dynamic W^X
	 * promotion without which the guest cannot execute a single instruction
	 * under STAGE2_WX_DYNAMIC's default XN mapping. See
	 * el2_exc_qemu_common.h for what is shared and why. */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (qemu_guest_sync(frame, ec, &qemu_ci_ops))
			return;
	}

	/* Anything else — an unexpected guest fault, or our own EL2
	 * synchronous exception — is a genuine problem on this milestone's
	 * boot path: report it and halt rather than silently resuming into a
	 * possibly-corrupt state. */
	qemu_report_fault(frame, kind, "QEMU-CI", /*poweroff=*/0);
}
