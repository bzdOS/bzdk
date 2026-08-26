/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_dual_qemu.c — EL2 trap handler for the QEMU `virt` SMP/PSCI
 * CPU_ON proof target (main_dual_qemu.c / smp-qemu-ci.sh).
 *
 * Same rationale as every other target's own el2_exc_*_qemu.c (see
 * el2_exc_qemu.c's header): this is a NEW, minimal el2_trap(), not a reuse of
 * the real board's el2_exc.c (which pulls in the full board debugger and a
 * dozen Allwinner-only modules this target has no use for and deliberately
 * does not link — see smp_qemu_stub.c's header for exactly which of el2_exc.c's
 * symbols this target needs instead, and why as inert stand-ins).
 *
 * This target's proof (PSCI CPU_ON brings up 3 secondaries that reach
 * smp_secondary_main()) never arms a timer and never unmasks IRQ/FIQ on any
 * core (CPU0's main() unmasks nothing; the secondaries' smp_secondary_main()
 * dispatch — cpu==1 with dbg_core_enable=0, cpu==2/3 — never calls
 * smp_timer_init_secondary() either, since nothing in the current smp.c
 * dispatch does; see main_dual_qemu.c's header for the full argument). So
 * el2_trap() is NOT expected to run at all during a healthy pass: it exists
 * purely because exceptions.S's el2_common unconditionally calls it (`bl
 * el2_trap`), so the symbol must be resolved for exceptions.o to link, and as
 * a safety net — an unexpected fault on CPU0 OR a secondary (secondaries
 * share the SAME VBAR_EL2, reloaded from smp_boot_config[] by
 * _start_secondary — see start_secondary_qemu.S) lands here instead of
 * running off into undefined behavior.
 */
#include <stdint.h>
#include "exceptions.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* dual_qemu_poweroff() used to be defined here (PSCI SYSTEM_OFF 0x84000008 via
 * SMC, verbatim-duplicated across every QEMU CI handler); it now lives in
 * el2_exc_qemu_common.c as qemu_psci_poweroff() — same mechanism, same "QEMU's
 * `virt` machine answers PSCI itself, no real EL3 firmware needed" rationale,
 * see that file's own comment. Behaviour is unchanged: this target still
 * powers off (rather than halting in wfi) after the fault report below. */

/* This target's own scenario config for the shared guest-sync chain: a fully
 * zeroed struct, because this proof needs nothing beyond what qemu_guest_sync()
 * already does unconditionally — dynamic W^X promotion. No console, no HVC
 * ack, no SMC forward, no MMIO absorb: this target never enters a guest at
 * all (see this file's header), so none of those can ever fire; the struct
 * exists only so the "guest's first instruction fetch traps here and that is
 * normal" case below shares the same chain every other QEMU target uses. */
static const struct qemu_guest_sync_ops dual_qemu_sync_ops = { 0 };

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	/* Dynamic W^X promotion, checked BEFORE the "any trap here is a failure"
	 * report below — because since stage2.h made STAGE2_WX_DYNAMIC's XN
	 * mapping the default, a guest's FIRST INSTRUCTION FETCH legitimately
	 * traps here and is normal progress, not a bring-up failure. Without
	 * this, this target's own payloads cannot execute at all. Everything
	 * genuinely unexpected still falls through unchanged — qemu_guest_sync()
	 * (via stage2_wx_qemu_try()) only claims guest-DRAM permission faults.
	 * See el2_exc_qemu_common.h. */
	if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (qemu_guest_sync(frame, ec, &dual_qemu_sync_ops))
			return;
	}

	/* Whichever core (primary or secondary) took this, it means something
	 * unexpected happened during the SMP bring-up proof. Report and stop —
	 * the wrapping CI script's `timeout` (or, if we get here after
	 * main_dual_qemu.c already printed a verdict, the missing PASS grep)
	 * turns this into an unambiguous FAIL, same convention every other QEMU
	 * target's fault path uses.
	 *
	 * The marker passed to qemu_report_fault() is this file's ENTIRE original
	 * "DUAL-QEMU-CI: UNEXPECTED TRAP" prefix, not just "DUAL-QEMU-CI" — smp-
	 * qemu-ci.sh greps for the literal substring "UNEXPECTED TRAP" as one of
	 * its immediate-FAIL triggers (see that script), so that exact wording has
	 * to survive this refactor even though qemu_report_fault()'s own wording
	 * for everything after the marker is "FAULT kind=..." rather than this
	 * file's original "UNEXPECTED TRAP kind=...". poweroff=1 preserves this
	 * target's original terminal behaviour (dual_qemu_poweroff(), not a wfi
	 * halt). */
	qemu_report_fault(frame, kind, "DUAL-QEMU-CI: UNEXPECTED TRAP", /*poweroff=*/1);
}
