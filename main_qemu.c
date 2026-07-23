/* main_qemu.c — bzdOS microkernel, QEMU `virt`-machine CI target
 * (ROADMAP.md T3). See docs/qemu-ci.md for the full invocation and what
 * this proves; the file banners of start_qemu.S / stage2.h / guest.h /
 * gic_timer_qemu.h / el2_exc_qemu.c / guest_qemu_payload.c cover the
 * per-module rationale.
 *
 * Brings up, in order: the PL011 console, our own EL2 vector table
 * (exceptions.S, unchanged), stage-2 identity translation (stage2.c,
 * UNCHANGED — see the DO-NOT-MODIFY note in that file; it happens to need
 * no QEMU-specific changes at all, since QEMU virt's RAM base (0x40000000)
 * and low MMIO region both fall inside the exact ranges stage2.c already
 * identity-maps), the GICv2 timer tick (gic_timer_qemu.c, QEMU-address
 * variant of gic_timer.c), and finally drops to EL1 to run the minimal
 * guest payload (guest_qemu_payload.c) — which el2_exc_qemu.c's tick
 * handler periodically preempts, proving the whole chain end to end.
 */
#include <stdint.h>
#include "exceptions.h"
#include "stage2.h"
#include "guest_qemu_payload.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"

/* stage2_at_check()'s documented breadcrumb addresses (stage2.h/.c) for the
 * combined stage-1+stage-2 AT S12E1R self-check: PAR_EL1 low/high 32 bits,
 * written to fixed DRAM words at 0x50000c1c / 0x50000c20. Reading these
 * back from EL2 host code is safe on this target: stage-2 translation only
 * governs EL1/EL0 accesses, never EL2's own (which run flat-physical, MMU
 * off — see start_qemu.S), so this plain load is unaffected by anything
 * stage2.c excludes from the GUEST's view of this address range. */
#define S2_AT_PAR_LO (*(volatile uint32_t *)0x50000c1cUL)

long
main(void)
{
	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt CI target ===\n");
	pl011_puts("HV: entered at EL2, MMU off, physical addressing (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* stage2.c/.h: reused completely unmodified — see the file banner
	 * above for why QEMU virt's memory map happens to fit it as-is. */
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

	/* guest.c's guest_config()/guest_enter(): also reused unmodified. */
	qemu_guest_start();   /* noreturn: eret to EL1. EL2 regains control
	                        * only on the periodic tick (el2_exc_qemu.c),
	                        * which decides when the CI run is done. */

	for (;;)
		__asm__ volatile("wfi");
}
