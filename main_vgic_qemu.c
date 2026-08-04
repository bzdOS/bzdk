/* SPDX-License-Identifier: BSD-2-Clause */

/* main_vgic_qemu.c — bzdOS microkernel, QEMU `virt` CI target for INTERRUPT
 * VIRTUALIZATION (the vGIC).
 *
 * ============================================================================
 * THE HOLE THIS TARGET EXISTS TO FILL
 * ============================================================================
 * Before this target, NOTHING in the board-free gate touched GIC
 * virtualization:
 *
 *   - qemu-ci.sh proves the HOST side of the GIC only: EL2 programs GICD/GICC
 *     (gic_timer_qemu.c), takes a physical CNTP IRQ at EL2 with HCR_EL2.IMO=1,
 *     and preempts an EL1 guest. The guest in that target never receives an
 *     interrupt of its own; vgic.c is not even linked in.
 *   - zephyr-qemu-ci.sh boots a real Zephyr guest, but that guest's GIC is
 *     identity-mapped to the A64's addresses (0x01c81000...), where QEMU virt
 *     has NOTHING. Those accesses are silently dropped, so Zephyr's
 *     arm_gic_init() "succeeds" against a black hole and — the build being
 *     tickless — no interrupt is ever enabled by anyone. That gate cannot say
 *     anything at all about GIC emulation or interrupt delivery, by
 *     construction. (Its own file banner says so.)
 *   - vgic.c's own bare-metal self-test guest (vgic_selftest_guest_el1(), in
 *     vgic.c since the interrupt-virtualization milestone) was written to
 *     prove exactly this chain, and had never been run by anything: no target
 *     called vgic_selftest_start(), on the board or off it.
 *
 * This target runs THAT self-test, against QEMU virt's real GICv2
 * virtualization extensions, with no board attached.
 *
 * ============================================================================
 * WHAT A PASS HERE PROVES
 * ============================================================================
 * It executes the REAL vgic.c — not a hand-transcribed mirror, not a stub: the
 * same source file the board's `dbg`/`gdb`/`fbsd`/`repl`/`zephyr` targets link,
 * recompiled (as vgic_qemu.o) with only the four GIC base addresses overridden
 * on the command line (see vgic.h's #ifndef guards and the QEMU devicetree
 * citation there). A PASS therefore proves, end to end:
 *
 *   1. vgic_init() programs a REAL GICv2 hypervisor control interface: it
 *      reads GICH_VTR and derives a plausible List-Register count (asserted
 *      below: QEMU virt reports 4, same as the A64's GIC-400), clears every
 *      LR, presets GICH_VMCR, sets GICH_HCR.En, writes CNTVOFF_EL2, and
 *      enables the maintenance INTID at the distributor — each readback
 *      checked, not assumed. Under zephyr-qemu-ci.sh every one of these
 *      writes would have gone to a black hole.
 *   2. vgic_inject_cntv() finds a free LR via GICH_ELRSR0 and builds an LR
 *      word the HARDWARE accepts (correct VirtualID/Group/State/priority
 *      fields) — proven by delivery, not by inspection.
 *   3. The virtual CPU interface actually signals EL1: the guest takes a
 *      VIRTUAL IRQ through its own VBAR_EL1 while EL2 keeps taking the
 *      PHYSICAL timer IRQ. Two independent interrupt streams, correctly
 *      separated by HCR_EL2.IMO=1.
 *   4. The guest's own reads/writes of the virtual CPU interface work: it
 *      reads GICV_IAR and gets back exactly the vINTID we injected
 *      (VGIC_VTIMER_INTID, 27 — asserted, not just "nonzero"), and its
 *      virtual GICV_EOIR write retires the interrupt and frees the LR, which
 *      is what lets the NEXT tick be injected at all. A stuck LR would show up
 *      as vgic.c's own duplicate-vINTID gate silently swallowing every
 *      subsequent tick, i.e. as virq_count freezing at 1 — which this target
 *      fails on.
 *
 * ============================================================================
 * WHAT A PASS HERE DOES *NOT* PROVE — read this before trusting it
 * ============================================================================
 *   - Nothing about the A64's GIC-400 at its own addresses, nor about the
 *     stage-2 GICC->GICV redirect: this target has no such redirect, and the
 *     self-test payload is pointed straight at GICV via VGIC_GICC_BASE (see
 *     vgic.h). The redirect, and its documented coupling to IMO=1, stay
 *     hardware-only.
 *   - Nothing about HW=1 (hardware-backed) List Registers, hence nothing about
 *     the physical-deactivation-on-guest-EOI behaviour that the EHCI INTID 106
 *     storm was about. This target uses the VGIC_CNTV_HW=0 SOFTWARE vtimer
 *     path (EL2 fully EOIs the physical INTID 30 itself, then injects a
 *     PURE-VIRTUAL vINTID 27), which is vgic.h's own default.
 *   - Nothing about the LR-exhaustion pending queue or vgic_maintenance():
 *     with one vINTID in flight at a time, 4 LRs are never exhausted and
 *     GICH_HCR.UIE is never set, so INTID 25 never fires here. That logic is
 *     covered separately and host-side by test_vgic_pendq.
 *   - Nothing about the FreeBSD guest's real GIC driver, or about device SPIs.
 *
 * Boot order matches vgic.h's documented preconditions for
 * vgic_selftest_start() exactly (el2_install -> gic_timer -> vgic_init ->
 * stage-2 -> IRQ unmasked -> enter guest).
 */
#include <stdint.h>
#include "exceptions.h"
#include "stage2.h"
#include "vgic.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "vgic_qemu_ci.h"

/* Same AT S12E1R self-check breadcrumb main_qemu.c reads (stage2.h/.c): PAR_EL1
 * low word at 0x50000c1c. EL2's own loads are never subject to stage-2, so
 * reading it back here is safe. */
#define S2_AT_PAR_LO (*(volatile uint32_t *)0x50000c1cUL)

/* The physical tick period. 10 ms: fast enough that the ~10 virtual IRQs this
 * target waits for land in ~100 ms (CI stays quick), slow enough that the
 * guest's EL1 handler + virtual EOI comfortably complete between ticks even
 * under TCG, so a PASS is not a race. */
#define VGIC_CI_TICK_US 10000u

long
main(void)
{
	uint32_t vtr, nr_lr, hcr, vmcr, cntvoff_set;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt vGIC CI target ===\n");
	pl011_puts("HV: entered at EL2, MMU off (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* Order note: gic_timer_qemu_init() BEFORE vgic_init(), because the
	 * former is what sets HCR_EL2.IMO=1, and IMO=1 is the architectural
	 * precondition for a virtual IRQ to be signalled to EL1 at all (with
	 * IMO=0 the vGIC's virtual interrupt output is simply not enabled). This
	 * is the same ordering main_dbg.c uses on the board. */
	gic_timer_qemu_init(VGIC_CI_TICK_US);
	pl011_puts("HV: GICv2 physical timer armed (INTID 30 / CNTP) + HCR_EL2.IMO=1\n");

	/* THE code under test: the real vgic.c, compiled with QEMU's GIC bases. */
	vgic_init();

	/* Read vgic.c's own breadcrumb window back and CHECK it, rather than
	 * trusting that the writes went anywhere. This is precisely the
	 * distinction zephyr-qemu-ci.sh cannot make: against a black hole,
	 * GICH_VTR reads as 0, so nr_lr would come out as 1 and GICH_HCR would
	 * read back as 0 instead of En=1. Both are caught here. */
	vtr         = VGIC_BC_RD(VGIC_BC_VTR);
	nr_lr       = VGIC_BC_RD(VGIC_BC_NR_LR);
	hcr         = VGIC_BC_RD(VGIC_BC_HCR);
	cntvoff_set = VGIC_BC_RD(VGIC_BC_CNTVOFF);
	vmcr        = VGIC_BC_RD(VGIC_BC_VMCR);

	pl011_puts("HV: vgic_init(): GICH_VTR=0x");
	pl011_put_hex32(vtr);
	pl011_puts(" nr_lr=");
	pl011_put_udec(nr_lr);
	pl011_puts(" GICH_HCR=0x");
	pl011_put_hex32(hcr);
	pl011_puts(" GICH_VMCR=0x");
	pl011_put_hex32(vmcr);
	pl011_puts("\n");

	if (VGIC_BC_RD(VGIC_BC_MAGIC_IDX) != VGIC_BC_MAGIC_VAL) {
		pl011_puts(VGIC_CI_FAIL "vgic breadcrumb magic missing -- vgic_init() did not run\n");
		vgic_ci_poweroff();
	}
	/* A real GICv2 virtual interface must report at least 4 List Registers
	 * (GICH_VTR[5:0] = LRs-1). QEMU virt and the A64's GIC-400 both give 4.
	 * nr_lr == 1 is the black-hole signature (VTR read as 0). */
	if (nr_lr < 4u) {
		pl011_puts(VGIC_CI_FAIL "GICH_VTR reports only ");
		pl011_put_udec(nr_lr);
		pl011_puts(" List Register(s) -- no real GICH here\n");
		vgic_ci_poweroff();
	}
	if ((hcr & 1u) == 0u) {
		pl011_puts(VGIC_CI_FAIL "GICH_HCR.En did not stick (read back 0x");
		pl011_put_hex32(hcr);
		pl011_puts(")\n");
		vgic_ci_poweroff();
	}
	if (vmcr == 0u) {
		pl011_puts(VGIC_CI_FAIL "GICH_VMCR read back 0 after preset\n");
		vgic_ci_poweroff();
	}
	if (cntvoff_set != 1u) {
		pl011_puts(VGIC_CI_FAIL "CNTVOFF_EL2 was never written\n");
		vgic_ci_poweroff();
	}
	pl011_puts("HV: vgic_init() self-check PASS (real GICH: >=4 LRs, En=1, VMCR preset, CNTVOFF=0)\n");

	stage2_init();
	stage2_enable();
	stage2_at_check(STAGE2_SELFTEST_IPA);
	if ((S2_AT_PAR_LO & 1u) == 0u) {
		pl011_puts("HV: stage-2 programmed + AT S12E1R self-check PASS\n");
	} else {
		pl011_puts(VGIC_CI_FAIL "stage-2 AT S12E1R self-check FAIL (PAR_EL1.F=1)\n");
		vgic_ci_poweroff();
	}

	__asm__ volatile("msr daifclr, #3" ::: "memory");   /* unmask IRQ + FIQ */
	pl011_puts("HV: IRQ/FIQ unmasked -- entering the vgic.c self-test EL1 guest\n");

	/* noreturn: erets to EL1. EL2 regains control on each physical tick
	 * (el2_exc_vgic_qemu.c), which injects the virtual tick and decides when
	 * the run is done. */
	vgic_selftest_start();

	for (;;)
		__asm__ volatile("wfi");
}
