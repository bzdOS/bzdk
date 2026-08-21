/* SPDX-License-Identifier: BSD-2-Clause */

/* main_holdtest_qemu.c — cheapest-possible QEMU repro of the "pause guest
 * before its first instruction + software breakpoint" board hang (see
 * memories dbgtools-infra-added / hold-gate-plus-breakpoint-crashes-board).
 *
 * WHAT THIS TESTS, PRECISELY: the real board's `main_gdb.c` arms
 * MDCR_EL2.TDE=1 and patches a guest software BRK (gdbstub.c's bp_insert(),
 * BRK_INSTR = 0xD420FA00) BEFORE the guest's very first-ever instruction
 * executes, then erets into EL1. Live-tested twice on real hardware: this
 * wedges/crashes the WHOLE BOARD within ~15s. In EVERY prior use of this
 * hypervisor's gdb facilities, TDE was only ever armed AFTER the guest had
 * already been running for a while (a human attaching gdb later) -- this
 * session was the first time the ordering was reversed. Hypothesis under
 * test: something about THIS EXACT ORDERING (TDE set / BRK patched BEFORE
 * the first EL1 instruction, vs. after the guest has been happily running)
 * breaks the exception-divert path.
 *
 * This is a NEW, throwaway, single-core diagnostic target -- NOT a reuse of
 * the real board's main_gdb.c/gdbstub.c/el2_exc.c/smp.c (see the Makefile's
 * HOLDTEST_OBJS comment for exactly why those don't drop in). It deliberately
 * reuses, UNCHANGED: start_qemu.o (EL2 entry, MMU off, flat physical --
 * same regime as the real board's EL2), exceptions.o (the real vector table
 * + full-context save/restore contract), pl011_qemu.o (console). It does
 * NOT reuse stage2.o/gic_timer_qemu.o: this hypothesis is purely about
 * EL1<->EL2 debug-exception routing (MDCR_EL2.TDE + a software BRK), which
 * does not depend on stage-2 translation being enabled or a timer tick
 * existing at all -- per the task's own guidance, start with the simplest
 * thing that could show the bug.
 *
 * Design: ONE QEMU boot runs BOTH scenarios back to back for direct
 * contrast (see el2_exc_holdtest_qemu.c for the state machine):
 *   TEST A (the board's actual new ordering): BRK patched into
 *     holdtest_entryA's first instruction and MDCR_EL2.TDE=1 armed BEFORE
 *     the FIRST eret ever happens -- nothing has run at EL1 yet.
 *   TEST B (the ordering every previous session actually used): guest
 *     erets first with TDE=0, spins for a few million iterations (a stand-in
 *     for "already running for a while"), then HVCs to ask EL2 to arm the
 *     exact same BRK+TDE mechanism on an upcoming (not-yet-executed)
 *     instruction, then keeps running forward into it.
 * Both use the IDENTICAL patch/arm code (el2_exc_holdtest_qemu.c's
 * patch_brk()/set_tde(), hand-transcribed bit-for-bit from gdbstub.c) and
 * the IDENTICAL guest entry SPSR (EL1h|D|A, i.e. KLOAD_GUEST_SPSR_EL2) --
 * the ONLY variable between the two tests is the before/after ordering
 * itself, isolating exactly what the task asked to isolate.
 *
 * A run either prints two clear "HOLDTEST: TEST A/B -> ..." verdict lines
 * and cleanly PSCI-poweroffs (deterministic, greppable, see run_holdtest.sh),
 * or it hangs/faults somewhere -- both are unambiguous signals for this
 * throwaway target, same philosophy as el2_exc_qemu.c's report_fault().
 */
#include <stdint.h>
#include "exceptions.h"
#include "holdtest_guest.h"
#include "pl011_qemu.h"

#define SPSR_EL1h (0x5ull)
#define SPSR_D    (1ull << 9)
#define SPSR_A    (1ull << 8)
#define HOLDTEST_GUEST_SPSR (SPSR_EL1h | SPSR_D | SPSR_A)   /* == KLOAD_GUEST_SPSR_EL2 */

#define GUEST_STACK_WORDS (4096 / 8)
static uint64_t g_stackA[GUEST_STACK_WORDS] __attribute__((aligned(16)));

long
main(void)
{
	pl011_init();
	pl011_puts("\n=== bzdOS holdtest -- QEMU repro of pre-entry TDE+BRK ordering ===\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* EL1 setup: MMU/caches off (flat physical, matching this target's EL2
	 * regime -- see start_qemu.S), RW so EL1 is AArch64, our OWN vector
	 * table installed so a TDE-routing FAILURE is distinguishable (see
	 * guest_holdtest_asm.S) instead of silently derailing into address 0. */
	{
		uint64_t hcr;
		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr |= (1ull << 31);   /* HCR_EL2.RW: EL1 is AArch64 */
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");

		/* SCTLR_EL1: MMU+caches off, ARMv8.0 RES1 bits only -- identical
		 * bit set to guest.c's write_sctlr_el1(SCTLR_EL1_RES1). */
		__asm__ volatile(
			"msr sctlr_el1, %0\n\tisb"
			:: "r"((1ull<<11)|(1ull<<20)|(1ull<<22)|(1ull<<23)|(1ull<<28)|(1ull<<29))
			: "memory");

		__asm__ volatile("msr vbar_el1, %0\n\tisb"
		                  :: "r"((uint64_t)holdtest_el1_vectors) : "memory");
	}
	pl011_puts("HV: EL1 configured (MMU off, VBAR_EL1 = holdtest_el1_vectors)\n");

	/* ---- TEST A: arm BRK + TDE BEFORE this guest has ever executed a
	 * single instruction -- the ordering the real board's main_gdb.c uses
	 * with the new pause-before-entry gate. ---- */
	pl011_puts("HV: --- starting TEST A (pre-entry: TDE+BRK armed BEFORE "
	           "guest's first-ever instruction) ---\n");
	holdtest_arm_phase_a();   /* patch_brk() + set_tde(1), see that file */
	pl011_puts("HV: BRK patched into holdtest_entryA + MDCR_EL2.TDE=1 armed. "
	           "eret'ing now -- guest has NEVER run before this instant.\n");

	__asm__ volatile("msr sp_el1, %0" :: "r"((uint64_t)&g_stackA[GUEST_STACK_WORDS]));
	__asm__ volatile("msr spsr_el2, %0" :: "r"(HOLDTEST_GUEST_SPSR) : "memory");
	__asm__ volatile("msr elr_el2, %0" :: "r"((uint64_t)holdtest_entryA) : "memory");
	__asm__ volatile("eret" ::: "memory");

	__builtin_unreachable();
}
