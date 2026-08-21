/* SPDX-License-Identifier: BSD-2-Clause */

/* guest_holdtest_payload.c — EL1-side C logic for the hold-gate/BRK-ordering
 * QEMU diagnostic (see holdtest_guest.h / main_holdtest_qemu.c for the big
 * picture). Reached via `b` from guest_holdtest_asm.S's two entry stubs.
 *
 * Every function here runs AT EL1, MMU off, flat physical addressing
 * (same regime as guest_qemu_payload.c) — plain C, pl011_puts() reaches the
 * real UART directly, same as that file's rationale explains.
 */
#include <stdint.h>
#include "holdtest_guest.h"
#include "pl011_qemu.h"

static inline uint64_t
read_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v >> 2;
}

/* ---- Test A: BRK planted + TDE armed BEFORE this code ever ran ------ *
 * Reached only if EL2 successfully caught the BRK at holdtest_entryA's
 * patch site, restored the original NOP, and resumed past it — i.e. this
 * function running at all IS the "pre-entry divert worked" signal. */
void
holdtest_entryA_c(void)
{
	pl011_puts("GUEST A: reached past the patch site (CurrentEL=");
	pl011_put_udec((uint32_t)read_currentel());
	pl011_puts(") -- the pre-entry BRK was caught and resumed cleanly\n");
	__asm__ volatile("hvc #1" ::: "memory");   /* HOLDTEST_HVC_A_DONE */
	/* Should never return here (EL2 re-erets elsewhere for test B). If it
	 * somehow does, don't run off into whatever follows in memory. */
	for (;;)
		__asm__ volatile("wfi");
}

/* ---- Test B: guest spins with TDE=0 first (the "already running" -------
 * control case), THEN asks EL2 to arm the SAME mechanism on an
 * upcoming (not-yet-executed) instruction. ------------------------------ */
#define HOLDTEST_B_SPIN_ITERS 3000000u

void
holdtest_entryB_c(void)
{
	uint32_t n;

	pl011_puts("GUEST B: running with TDE=0 (no debug armed) -- simulating "
	           "\"already running for a while\"\n");
	for (n = 0; n < HOLDTEST_B_SPIN_ITERS; n++)
		__asm__ volatile("" ::: "memory");   /* opaque no-op, defeats -O2 dead-loop elision */

	pl011_puts("GUEST B: spin done -- asking EL2 to arm BRK+TDE now, on an "
	           "instruction ahead of where I am\n");
	__asm__ volatile("hvc #2" ::: "memory");   /* HOLDTEST_HVC_B_ARM_NOW */

	/* EL2 has now patched holdtest_entryB_patchsite (a few instructions
	 * ahead, still unexecuted) and armed TDE, then resumed us right here
	 * (ELR advanced past the hvc, same context/stack). This function was
	 * reached via a plain `b` (not `bl`) from guest_holdtest_asm.S, so x30
	 * is NOT a valid return address -- falling off the end into a normal
	 * C `ret` would jump to garbage. Branch to the patch site explicitly
	 * instead of ever returning. */
	__asm__ volatile("b holdtest_entryB_patchsite");
	__builtin_unreachable();
}

/* Reached only if EL2 successfully caught the BRK at holdtest_entryB's
 * patch site (armed AFTER the spin above) and resumed past it. */
void
holdtest_entryB_resume(void)
{
	pl011_puts("GUEST B: DIAG patch site instr now reads 0x");
	pl011_put_hex32(*(volatile uint32_t *)holdtest_entryB_patchsite);
	pl011_puts("\n");
	pl011_puts("GUEST B: reached past the patch site -- the post-entry "
	           "(already-running) BRK was caught and resumed cleanly\n");
	__asm__ volatile("hvc #3" ::: "memory");   /* HOLDTEST_HVC_B_DONE */
	for (;;)
		__asm__ volatile("wfi");
}

/* Reached ONLY if a debug/BRK exception was NOT routed to EL2 (MDCR_EL2.TDE
 * failed to divert it) -- lands at EL1's own "current EL, SPx" sync vector
 * instead. This is the concrete, distinguishable "routing failed" signal;
 * without this handler the same event would silently derail into address 0
 * (VBAR_EL1 unset), which is what a REAL not-yet-initialized guest would do
 * — see guest_holdtest_asm.S's vector-table comment. */
void
holdtest_el1_selftrap(void)
{
	uint64_t esr, elr;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	pl011_puts("GUEST: *** SELF-TRAP AT EL1 *** (TDE did NOT route this "
	           "debug exception to EL2) ESR_EL1=0x");
	pl011_put_hex64(esr);
	pl011_puts(" ELR_EL1=0x");
	pl011_put_hex64(elr);
	pl011_puts("\n");
	__asm__ volatile("hvc #9" ::: "memory");   /* HOLDTEST_HVC_SELFTRAP */
	for (;;)
		__asm__ volatile("wfi");
}
