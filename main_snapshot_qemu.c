/* SPDX-License-Identifier: BSD-2-Clause */

/* main_snapshot_qemu.c — bzdOS microkernel, QEMU `virt`-machine snapshot
 * exercise (ROADMAP D1: "freeze the guest, dump RAM, restore it").
 *
 * Identical boot sequence to main_qemu.c (PL011 console, our own EL2
 * vectors, stage-2 identity translation — all reused completely unmodified,
 * see that file's header for why QEMU virt's memory map happens to fit
 * stage2.c as-is), but drops into guest_snapshot_payload.c's observable
 * counter-loop guest instead of guest_qemu_payload.c's printing one, and
 * uses el2_exc_snapshot_qemu.c as the trap handler, which is where the
 * actual snapshot_save()/snapshot_restore() calls happen (see that file's
 * header for the full tick schedule and what the verdict means).
 */
#include <stdint.h>
#include "exceptions.h"
#include "stage2.h"
#include "guest_snapshot_payload.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"

/* Same fixed breadcrumb main_qemu.c reads for its own AT self-check
 * (stage2_at_check()'s documented address); reused verbatim, not
 * reinterpreted. */
#define S2_AT_PAR_LO (*(volatile uint32_t *)0x50000c1cUL)

long
main(void)
{
	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt SNAPSHOT exercise (ROADMAP D1) ===\n");
	pl011_puts("HV: entered at EL2, MMU off, physical addressing (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	stage2_init();
	stage2_enable();
	pl011_puts("HV: stage-2 identity map programmed + enabled (HCR_EL2.VM=1)\n");

	stage2_at_check(STAGE2_SELFTEST_IPA);
	if ((S2_AT_PAR_LO & 1u) == 0u)
		pl011_puts("HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)\n");
	else
		pl011_puts("HV: stage-2 AT S12E1R self-check FAIL (PAR_EL1.F=1)\n");

	gic_timer_qemu_init(100000u);   /* 100 ms tick */
	pl011_puts("HV: GICv2 timer armed (INTID 30 / CNTP, 100 ms period)\n");

	__asm__ volatile("msr daifclr, #3" ::: "memory");   /* unmask IRQ + FIQ */
	pl011_puts("HV: IRQ/FIQ unmasked -- dropping to EL1 guest now\n");

	snapshot_guest_start();   /* noreturn: eret to EL1. EL2 regains control
	                            * only on the periodic tick
	                            * (el2_exc_snapshot_qemu.c), which drives the
	                            * freeze/dump/restore sequence and decides
	                            * when the run is done. */

	for (;;)
		__asm__ volatile("wfi");
}
