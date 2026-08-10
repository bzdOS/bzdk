/* SPDX-License-Identifier: BSD-2-Clause */

/* main_dual_rearm_qemu.c — bzdOS microkernel, QEMU `virt` CI target: reproduce,
 * board-free, the ONE defect real hardware found in the dual-guest feature that
 * QEMU had never exercised — a `zboot` issued AFTER a `zunhalt` re-arm.
 *
 * ============================================================================
 * WHAT WENT WRONG ON THE BOARD (2026-08-10) AND WHY THIS TARGET EXISTS
 * ============================================================================
 * On real hardware, the sequence "attempt fails -> zunhalt -> stage a good image
 * -> zboot" reached kload_enter() and then produced nothing at all:
 *
 *   - zguest_cpu3 breadcrumb state 3 ("about to enter the guest"), attempts 2,
 *     entry_pa 0xBE000000 -- so CPU3 ran the whole boot sequence;
 *   - zload2 breadcrumb clean: elf_valid 1, bytes_copied 24, fail_reason 0;
 *   - stage2_zephyr_isolation_selfcheck() passed;
 *   - the placed bytes at 0xBE000000 verified byte-for-byte against objdump;
 *   - and the payload's own counter never moved, with no faults recorded.
 *
 * The SAME firmware boots a real Zephyr image correctly on a FIRST attempt, so
 * the defect is specific to the second trip through zephyr_cpu3_run()'s loop.
 * Every existing dual-guest QEMU target only ever exercises a first attempt, so
 * none of them could have caught this.
 *
 * ============================================================================
 * HOW THIS REPRODUCES IT WITHOUT A BOARD
 * ============================================================================
 * The payload is staged by QEMU's generic loader at ZSTAGE_LOW_PA (the low-DRAM
 * TFTP landing window), and this target deliberately does NOT call
 * zguest_stage_copyin() before smp_init(). So the destination is empty, the
 * first `zboot` equivalent fails exactly as it did on the board (0xBAD1), and
 * only THEN does this target re-arm, run the copy-in, and try again. That is the
 * board's sequence, step for step, with the failure caused the same way.
 *
 * CPU0's side and the PASS criteria are unchanged and shared: this target links
 * el2_exc_dual2_qemu.o and guest.c's already-proven guest_demo_el1(), and the
 * verdict is still "both counters strictly increased across a real interval".
 * A re-arm that silently produces a dead guest therefore shows up as CPU3's
 * counter sitting at zero, which is precisely the board symptom.
 *
 * Progress between steps is detected by POLLING zguest_cpu3.c's own breadcrumb
 * rather than by sleeping: CPU3 publishes each state transition there, so a
 * bounded spin on that word is both faster and more honest than a delay that
 * might be too short on a loaded host. Each wait has a generous iteration cap
 * and reports which transition it was waiting for if it times out, so a hang
 * here can never be mistaken for a hang somewhere else.
 *
 * -m 2048 -smp 4 required, same as every other dual-guest QEMU target (Zephyr's
 * slice lives in the high GiB, and CPU3 must exist).
 */
#include <stdint.h>
#include "exceptions.h"
#include "smp.h"
#include "pl011_qemu.h"
#include "gic_timer_qemu.h"
#include "guest.h"
#include "zguest_cpu3.h"
#include "zstage.h"
#include "hv_addrmap.h"

extern volatile uint32_t dbg_core_enable;

#define SMP_BC(i) (*(volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)(i) * 4u))

/* zguest_cpu3.c's breadcrumb words -- see that file for the layout. */
#define ZG3_STATE     (*(volatile uint32_t *)(uintptr_t)(HVMAP_ZGUEST3_BC + 4u))
#define ZG3_ATTEMPTS  (*(volatile uint32_t *)(uintptr_t)(HVMAP_ZGUEST3_BC + 12u))
#define ZG3_LAST_FAIL (*(volatile uint32_t *)(uintptr_t)(HVMAP_ZGUEST3_BC + 16u))
#define ZG3_REARMS    (*(volatile uint32_t *)(uintptr_t)(HVMAP_ZGUEST3_BC + 20u))

/* Generous: CPU3's work between transitions is a few thousand instructions, but
 * QEMU on a loaded host is unpredictable, and a spurious timeout here would be
 * read as the very bug this target hunts. */
#define WAIT_SPINS 200000000ul

static void
dual_rearm_qemu_poweroff(void) __attribute__((noreturn));

static void
dual_rearm_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull; /* PSCI SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

/* Spin until CPU3's published state equals `want`. Returns 1 on success, 0 on
 * timeout (and says what it was waiting for, and what it saw instead). */
static int
wait_state(uint32_t want, const char *what)
{
	unsigned long i;

	for (i = 0; i < WAIT_SPINS; i++) {
		if (ZG3_STATE == want)
			return 1;
		__asm__ volatile("" ::: "memory");
	}

	pl011_puts("DUAL-REARM-CI: FAIL (timed out waiting for ");
	pl011_puts(what);
	pl011_puts("; state=0x");
	pl011_put_hex32(ZG3_STATE);
	pl011_puts(" attempts=");
	pl011_put_udec(ZG3_ATTEMPTS);
	pl011_puts(" rearms=");
	pl011_put_udec(ZG3_REARMS);
	pl011_puts(")\n");
	return 0;
}

static void
report(const char *tag)
{
	pl011_puts("HV: ");
	pl011_puts(tag);
	pl011_puts(": state=0x");
	pl011_put_hex32(ZG3_STATE);
	pl011_puts(" attempts=");
	pl011_put_udec(ZG3_ATTEMPTS);
	pl011_puts(" last_fail=0x");
	pl011_put_hex32(ZG3_LAST_FAIL);
	pl011_puts(" rearms=");
	pl011_put_udec(ZG3_REARMS);
	pl011_puts("\n");
}

long
main(void)
{
	uint32_t online, bc_online;
	uint64_t copied;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt dual-guest zunhalt RE-ARM repro ===\n");

	el2_install();
	dbg_core_enable = 0;

	/* DELIBERATELY NOT calling zguest_stage_copyin() here, unlike
	 * main_dual2_qemu.c. The destination must be empty for the first attempt
	 * so that it fails the same way it failed on the board. */
	pl011_puts("HV: copy-in deliberately SKIPPED so attempt #1 fails (0xBAD1), "
	           "reproducing the board's sequence\n");

	pl011_puts("HV: smp_init() -- PSCI CPU_ON for cores 1..3\n");
	smp_init();

	online    = smp_num_online();
	bc_online = SMP_BC(1);
	pl011_puts("HV: smp_num_online()=");
	pl011_put_udec(online);
	pl011_puts(" online-bitmap=0x");
	pl011_put_hex32(bc_online);
	pl011_puts("\n");
	if (online != SMP_MAX_CPUS || bc_online != 0xFu) {
		pl011_puts("DUAL-REARM-CI: FAIL (PSCI CPU_ON bring-up did not complete)\n");
		dual_rearm_qemu_poweroff();
	}

	if (!wait_state(1u, "CPU3 to park initially"))
		dual_rearm_qemu_poweroff();
	report("parked");

	/* ---- attempt #1: must fail, exactly as on the board ---------------- */
	pl011_puts("HV: step 1 -- zboot with nothing staged (expect 0xBAD1)\n");
	zguest_cpu3_start_set();
	if (!wait_state(0xBAD1u, "attempt #1 to fail with 0xBAD1"))
		dual_rearm_qemu_poweroff();
	report("after zboot #1");
	if (ZG3_ATTEMPTS != 1u) {
		pl011_puts("DUAL-REARM-CI: FAIL (attempts != 1 after the first zboot)\n");
		dual_rearm_qemu_poweroff();
	}

	/* ---- re-arm: must return CPU3 to an OBSERVABLE parked state -------- */
	pl011_puts("HV: step 2 -- zunhalt (expect parked, attempts unchanged, "
	           "last_fail sticky)\n");
	zguest_cpu3_rearm_set();
	if (!wait_state(1u, "zunhalt to return CPU3 to parked"))
		dual_rearm_qemu_poweroff();
	report("after zunhalt");
	if (ZG3_REARMS != 1u || ZG3_ATTEMPTS != 1u || ZG3_LAST_FAIL != 0xBAD1u) {
		pl011_puts("DUAL-REARM-CI: FAIL (re-arm bookkeeping wrong: rearms must be "
		           "1, attempts must still be 1, last_fail must stay sticky)\n");
		dual_rearm_qemu_poweroff();
	}

	/* ---- now stage the image and retry -------------------------------- */
	pl011_puts("HV: step 3 -- zguest_stage_copyin() (the image is at ZSTAGE_LOW_PA)\n");
	copied = zstage_copy_to(0xBF000000ull, 0x01000000ull);
	pl011_puts("HV: copy-in copied ");
	pl011_put_udec((uint32_t)copied);
	pl011_puts(" bytes\n");
	if (copied == 0u) {
		pl011_puts("DUAL-REARM-CI: FAIL (nothing staged at ZSTAGE_LOW_PA -- the "
		           "test harness itself is misconfigured, not the firmware)\n");
		dual_rearm_qemu_poweroff();
	}

	pl011_puts("HV: step 4 -- zboot AFTER the re-arm (this is the case the board "
	           "showed reaching kload_enter and then doing nothing)\n");
	zguest_cpu3_start_set();
	if (!wait_state(3u, "attempt #2 to reach kload_enter (state 3)"))
		dual_rearm_qemu_poweroff();
	report("after zboot #2");
	if (ZG3_ATTEMPTS != 2u) {
		pl011_puts("DUAL-REARM-CI: FAIL (attempts != 2 after the retry)\n");
		dual_rearm_qemu_poweroff();
	}

	/* From here the shared harness decides: state 3 only means "about to
	 * enter", so the counter sampling in el2_exc_dual2_qemu.c is what
	 * actually establishes whether the re-armed guest RUNS. */
	pl011_puts("HV: CPU3 reached kload_enter -- whether it actually RUNS is now up "
	           "to the two-sample counter check (state 3 means 'about to enter')\n");

	gic_timer_qemu_init(100000u);
	__asm__ volatile("msr daifclr, #3" ::: "memory");
	guest_start_demo();

	for (;;)
		__asm__ volatile("wfi");
}
