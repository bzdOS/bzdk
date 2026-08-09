/* SPDX-License-Identifier: BSD-2-Clause */

/* main_dual_zephyr_qemu.c — bzdOS microkernel, QEMU `virt` CI target:
 * dual-guest Step 2, swapping Step 1's (fc20b63) hand-built trivial ELF on
 * CPU3 for the REAL Zephyr RTOS image, still running genuinely concurrently
 * with FreeBSD-shaped guest_demo_el1() on CPU0.
 *
 * THIS FILE'S C CODE IS DELIBERATELY IDENTICAL, LOGIC-FOR-LOGIC, TO
 * main_dual2_qemu.c. That is not an oversight: zguest_cpu3.c's one-way boot
 * sequence (zload2_parse_and_place() -> stage2_zephyr_init/enable() ->
 * isolation selfcheck -> vconsole_init_chan1() -> guest_config() ->
 * kload_enter()) never inspects which ELF was staged at ZG3_ELF_STAGE_PA —
 * that address is a plain physical-memory drop zone, filled by THIS
 * script's own `-device loader,file=...,force-raw=on` argument (see
 * dual-zephyr-qemu-ci.sh), not by anything main() does. Swapping Step 1's
 * zg3_trivial_payload.elf for the real zephyr.elf therefore needed ZERO
 * changes here — the entire Step 2 delta lives in the EL2 trap dispatch
 * (el2_exc_dual_zephyr_qemu.c), which is where it must live: real Zephyr
 * probes a UART and a GIC that the trivial payload never touched (see
 * stage2_zephyr.h: CPU3 gets ZERO real MMIO passthrough, so every such
 * access is a stage-2 fault into EL2), and Step 1's el2_exc_dual2_qemu.c
 * had no code for that at all (its trivial payload never faults past its
 * own four instructions).
 *
 * WHY A SEPARATE FILE INSTEAD OF JUST REUSING main_dual2_qemu.c AS-IS: this
 * project's own convention (see e.g. main_zephyr.c vs main_zephyr_qemu.c,
 * or main_dual_qemu.c vs main_dual2_qemu.c) is one clearly-named main per
 * milestone, with its own banner documenting exactly what THAT target
 * proves, even when the body is a near-duplicate of a predecessor — a
 * reader should never have to cross-reference commit history to know which
 * proof a given ELF represents. The alternative (parameterizing
 * main_dual2_qemu.c with e.g. an #ifdef) would blur that history for a
 * ~10-line diff's worth of savings.
 *
 * See el2_exc_dual_zephyr_qemu.c's header for the actual Step 2 mechanism
 * and verdict logic, and dual-zephyr-qemu-ci.sh for the SKIP-if-no-Zephyr-
 * tree convention and the exact assertions checked.
 */
#include <stdint.h>
#include "exceptions.h"
#include "smp.h"
#include "pl011_qemu.h"
#include "gic_timer_qemu.h"
#include "guest.h"
#include "zguest_cpu3.h"

/* smp.c's own "ISOLATION TEST flag" -- see main_dual_qemu.c (Step 0) for the
 * full rationale; reused verbatim here for the same reason: CPU1's
 * board-only EMAC/dbgmon/WDOG/USB MMIO does not exist under QEMU. */
extern volatile uint32_t dbg_core_enable;

#define SMP_BC(i) (*(volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)(i) * 4u))

static void
dual_zephyr_qemu_poweroff(void) __attribute__((noreturn));

static void
dual_zephyr_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull; /* PSCI SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

long
main(void)
{
	uint32_t online, bc_online;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt dual-guest Step 2 proof (real Zephyr) ===\n");
	pl011_puts("HV: entered at EL2, MMU off (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	dbg_core_enable = 0;
	pl011_puts("HV: dbg_core_enable=0 (CPU1 debug-core MMIO skipped under QEMU -- smp.c unmodified)\n");

	pl011_puts("HV: calling the REAL smp_init() (smp.c, unmodified) -- PSCI CPU_ON for cores 1..3\n");
	smp_init();

	online    = smp_num_online();
	bc_online = SMP_BC(1);
	pl011_puts("HV: smp_num_online()=");
	pl011_put_udec(online);
	pl011_puts(" online-bitmap=0x");
	pl011_put_hex32(bc_online);
	pl011_puts("\n");

	if (online != SMP_MAX_CPUS || bc_online != 0xFu) {
		pl011_puts("DUAL-ZEPHYR-QEMU-CI: FAIL (PSCI CPU_ON bring-up did not complete -- "
		           "see smp-qemu-ci.sh for isolated diagnosis)\n");
		dual_zephyr_qemu_poweroff();
	}

	pl011_puts("HV: CPU3 is parked in zguest_cpu3.c's wfe-poll loop (real, strong zephyr_cpu3_run())\n");
	pl011_puts("HV: kicking CPU3's REAL one-way boot sequence against the REAL Zephyr ELF staged at "
	           "ZG3_ELF_STAGE_PA: zload2_parse_and_place() -> stage2_zephyr_init/enable() -> "
	           "isolation selfcheck -> vconsole_init_chan1() -> guest_config() -> kload_enter()\n");
	zguest_cpu3_start_set();

	gic_timer_qemu_init(100000u);   /* 100 ms tick, same period as qemu-ci.sh */
	pl011_puts("HV: GICv2 timer armed (INTID 30 / CNTP, 100 ms period) on CPU0 only\n");

	__asm__ volatile("msr daifclr, #3" ::: "memory");   /* unmask IRQ + FIQ on CPU0 */
	pl011_puts("HV: IRQ/FIQ unmasked on CPU0 -- dropping to EL1's guest_demo_el1() "
	           "(guest.c, already proven by qemu-ci.sh/the real board) now\n");

	guest_start_demo();

	for (;;)
		__asm__ volatile("wfi");
}
