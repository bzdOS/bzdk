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
#include "pl011_qemu.h"

static void
dual_qemu_poweroff(void) __attribute__((noreturn));

/* PSCI SYSTEM_OFF (0x84000008) via SMC — same mechanism every other QEMU
 * target in this tree uses to exit cleanly (see el2_exc_qemu.c's
 * qemu_poweroff()); QEMU's `virt` machine answers PSCI itself with no real
 * EL3 firmware needed. */
static void
dual_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	/* Whichever core (primary or secondary) took this, it means something
	 * unexpected happened during the SMP bring-up proof. Report and stop —
	 * the wrapping CI script's `timeout` (or, if we get here after
	 * main_dual_qemu.c already printed a verdict, the missing PASS grep)
	 * turns this into an unambiguous FAIL, same convention every other QEMU
	 * target's fault path uses. */
	pl011_puts("DUAL-QEMU-CI: UNEXPECTED TRAP kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	dual_qemu_poweroff();
}
