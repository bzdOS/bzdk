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

/* At the 100 ms period main_qemu.c arms, 20 ticks is ~2 seconds — long
 * enough to prove several preemptions happened, short enough that CI stays
 * fast. */
#define PASS_AFTER_TICKS 20u

static void qemu_poweroff(void) __attribute__((noreturn));

/* PSCI SYSTEM_OFF (0x84000008) via SMC. QEMU's `virt` machine answers PSCI
 * calls itself (no real EL3/secure firmware is needed — this is exactly how
 * every bare-metal EL2 payload booted with `-kernel` and no `-bios` cleanly
 * shuts QEMU down) and terminates the process with exit status 0 on
 * SYSTEM_OFF. The wfi fallback below is only for the (not expected) case
 * that the SMC doesn't actually terminate the emulator. */
static void
qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	pl011_puts("QEMU-CI: FAULT kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	for (;;)
		__asm__ volatile("wfi");
}

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

	/* Guest (lower-EL) synchronous HVC — acknowledge and resume, matching
	 * guest.c's documented optional hypercall path (see guest_demo_el1()'s
	 * comment); this target's own payload doesn't issue one, but a future
	 * one might. */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (ec == 0x16u) {   /* HVC from AArch64 */
			frame->elr += 4u;
			return;
		}
	}

	/* Anything else — an unexpected guest fault, or our own EL2
	 * synchronous exception — is a genuine problem on this milestone's
	 * boot path: report it and halt rather than silently resuming into a
	 * possibly-corrupt state. */
	report_fault(frame, kind);
}
