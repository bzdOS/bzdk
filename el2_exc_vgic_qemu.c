/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_vgic_qemu.c — EL2 trap handler for the QEMU-virt vGIC CI target.
 *
 * Sibling of el2_exc_qemu.c, and a NEW minimal el2_trap() for the same reason
 * that one is (the real el2_exc.c is the full board debugger and drags in a
 * dozen Allwinner-specific modules). What differs from el2_exc_qemu.c is the
 * one thing this target exists for: on every PHYSICAL timer tick taken at EL2,
 * it asks the real vgic.c to inject a VIRTUAL interrupt into the guest, and
 * then decides the CI verdict from whether the guest actually received it.
 *
 * The two interrupt streams, and why they are not the same stream:
 *   - PHYSICAL: INTID 30 (CNTP), programmed and fully EOIed by
 *     gic_timer_qemu.c. HCR_EL2.IMO=1 means EL2 takes it even while the guest
 *     is running. The guest never sees it and cannot EOI it.
 *   - VIRTUAL: vINTID 27 (CNTV), injected here via vgic_inject_cntv() into a
 *     GICH List Register. The guest takes it through its own VBAR_EL1, reads
 *     GICV_IAR, and writes GICV_EOIR. EL2 never EOIs this one.
 * This is vgic.h's VGIC_CNTV_HW=0 "software vtimer" mode (its default): EL2
 * owns the physical side end to end, the guest owns the virtual side end to
 * end, and the ONLY coupling is the List Register. That coupling is exactly
 * what is under test.
 *
 * Why the verdict is computed here rather than in the guest: the guest payload
 * is vgic.c's own vgic_selftest_guest_el1(), which is reused UNMODIFIED (that
 * is the point — it is the production self-test). It has no console and only
 * publishes breadcrumbs, so EL2 reads those breadcrumbs and prints the
 * greppable verdict. See vgic_qemu_ci.h for the window layout.
 *
 * Both verdicts end in PSCI SYSTEM_OFF, so the wrapping script never has to
 * rely on its timeout to tell PASS from FAIL.
 */
#include <stdint.h>
#include "exceptions.h"
#include "vgic.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "vgic_qemu_ci.h"

/* How many VIRTUAL interrupts the guest must have taken for a PASS. >1 is the
 * load-bearing part: a single delivery could happen with a permanently stuck
 * List Register, whereas the Nth delivery requires that the guest's virtual
 * EOI actually retired each of the previous N-1 and freed the LR — otherwise
 * vgic_inject_cntv()'s duplicate-vINTID gate swallows every later tick and
 * this counter freezes. 10 keeps the run ~100 ms of virtual time. */
#define PASS_AFTER_VIRQS 10u

/* Give up after this many PHYSICAL ticks (10 ms each => ~3 s). Chosen ~30x the
 * ticks a healthy run needs, so TCG jitter cannot make a good build fail, yet
 * a broken build still reports FAIL in seconds instead of hanging until the
 * script's timeout (a hang is a much worse CI signal than a verdict). */
#define FAIL_AFTER_TICKS 300u

void
vgic_ci_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;   /* PSCI SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

/* Dump every breadcrumb a failure could possibly be diagnosed from — the same
 * words one would read off a live board over the debug channel. */
static void
dump_state(void)
{
	pl011_puts("HV: VGIC[inject_count=");
	pl011_put_udec(VGIC_BC_RD(VGIC_BC_INJECT_CNT));
	pl011_puts(" inject_ok=");
	pl011_put_udec(VGIC_BC_RD(VGIC_BC_INJECT_OK));
	pl011_puts(" cntv_inject=");
	pl011_put_udec(VGIC_BC_RD(VGIC_BC_CNTV_INJ));
	pl011_puts(" cntv_gate=");
	pl011_put_udec(VGIC_BC_RD(VGIC_BC_CNTV_GATE));
	pl011_puts(" cntv_drop=");
	pl011_put_udec(VGIC_BC_RD(VGIC_BC_CNTV_DROP));
	pl011_puts(" last_lr=0x");
	pl011_put_hex32(VGIC_BC_RD(VGIC_BC_LAST_LR));
	pl011_puts(" last_elrsr=0x");
	pl011_put_hex32(VGIC_BC_RD(VGIC_BC_LAST_ELRSR));
	pl011_puts("]\n");

	pl011_puts("HV: VGST[magic=0x");
	pl011_put_hex32(VGST_BC_RD(VGST_MAGIC_IDX));
	pl011_puts(" guest_el=");
	pl011_put_udec(VGST_BC_RD(VGST_GUEST_EL));
	pl011_puts(" alive=");
	pl011_put_udec(VGST_BC_RD(VGST_ALIVE));
	pl011_puts(" virq_count=");
	pl011_put_udec(VGST_BC_RD(VGST_VIRQ_COUNT));
	pl011_puts(" last_iar=0x");
	pl011_put_hex32(VGST_BC_RD(VGST_LAST_IAR));
	pl011_puts(" spurious=");
	pl011_put_udec(VGST_BC_RD(VGST_SPURIOUS));
	pl011_puts(" other_exc=");
	pl011_put_udec(VGST_BC_RD(VGST_OTHER_EXC));
	pl011_puts("]\n");
}

static void ci_fail(const char *why) __attribute__((noreturn));

static void
ci_fail(const char *why)
{
	dump_state();
	pl011_puts(VGIC_CI_FAIL);
	pl011_puts(why);
	pl011_puts("\n");
	vgic_ci_poweroff();
}

static void ci_pass(void) __attribute__((noreturn));

static void
ci_pass(void)
{
	dump_state();
	pl011_puts(VGIC_CI_PASS "(GICH LR injection -> GICV -> EL1 vIRQ -> virtual EOI -> LR reclaim, ");
	pl011_put_udec(VGST_BC_RD(VGST_VIRQ_COUNT));
	pl011_puts(" virtual IRQs delivered, vINTID 0x");
	pl011_put_hex32(VGST_BC_RD(VGST_LAST_IAR));
	pl011_puts(")\n");
	vgic_ci_poweroff();
}

static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	dump_state();
	pl011_puts(VGIC_CI_FAIL "unexpected exception kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	vgic_ci_poweroff();
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		if (gic_timer_qemu_irq(frame)) {
			uint64_t ticks = gic_timer_qemu_ticks();
			uint32_t virqs, injected;

			/* THE call under test. Physical side is already fully
			 * EOIed by gic_timer_qemu_irq() above (EOImode=0), which
			 * is precisely the precondition vgic.h documents for the
			 * VGIC_CNTV_HW=0 path. */
			vgic_inject_cntv();

			injected = VGIC_BC_RD(VGIC_BC_CNTV_INJ);
			virqs    = VGST_BC_RD(VGST_VIRQ_COUNT);

			if (ticks == 1u) {
				pl011_puts("HV: first physical tick -- injected vINTID 27 into a List Register\n");
			} else if (ticks % 25u == 0u) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(": cntv_inject=");
				pl011_put_udec(injected);
				pl011_puts(" guest virq_count=");
				pl011_put_udec(virqs);
				pl011_puts("\n");
			}

			/* A guest fault must fail the run rather than be masked
			 * by the deadline below: vgic_st_other_c() counts any
			 * non-IRQ EL1 exception, and the self-test payload should
			 * never take one. */
			if (VGST_BC_RD(VGST_OTHER_EXC) != 0u)
				ci_fail("guest took a non-IRQ EL1 exception (VGST other_exc != 0)");

			if (virqs >= PASS_AFTER_VIRQS) {
				if (VGST_BC_RD(VGST_MAGIC_IDX) != VGST_BC_MAGIC_VAL)
					ci_fail("guest breadcrumb magic missing");
				if (VGST_BC_RD(VGST_GUEST_EL) != 1u)
					ci_fail("guest did not report CurrentEL==1");
				/* The delivered vINTID must be the one we injected,
				 * not merely "something": IAR is the guest's only
				 * evidence of WHICH interrupt arrived. */
				if ((VGST_BC_RD(VGST_LAST_IAR) & 0x3ffu) != VGIC_VTIMER_INTID)
					ci_fail("guest read a virtual IAR that is not vINTID 27");
				if (VGST_BC_RD(VGST_ALIVE) == 0u)
					ci_fail("guest main loop never ran (VGST alive == 0)");
				ci_pass();
			}

			if (ticks >= FAIL_AFTER_TICKS) {
				if (injected == 0u)
					ci_fail("vgic_inject_cntv() never placed vINTID 27 in any List Register");
				ci_fail("virtual IRQs were injected but the guest never took enough of them");
			}
		}
		return;   /* never advance ELR/SPSR for an async exception */
	}

	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (ec == 0x16u) {   /* HVC from AArch64 — ack and resume */
			frame->elr += 4u;
			return;
		}
	}

	report_fault(frame, kind);
}
