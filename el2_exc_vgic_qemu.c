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
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

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

/* vgic_ci_poweroff() used to build the PSCI SYSTEM_OFF SMC + wfi backstop
 * inline here, verbatim-duplicated with every other QEMU CI handler's
 * poweroff; that sequence now lives once in el2_exc_qemu_common.c as
 * qemu_psci_poweroff(). This stays a real function rather than becoming a
 * `#define vgic_ci_poweroff qemu_psci_poweroff` alias (el2_exc_qemu.c's
 * pattern for its own qemu_poweroff()) because vgic_qemu_ci.h declares
 * vgic_ci_poweroff() as this target's public symbol and main_vgic_qemu.c
 * calls it directly from its OWN translation unit -- a macro visible only in
 * this file would not reach that other caller, which still needs a real,
 * linkable vgic_ci_poweroff(). Behaviour is unchanged either way. */
void
vgic_ci_poweroff(void)
{
	qemu_psci_poweroff();
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

/* report_fault() used to be defined here (dump_state()'s breadcrumb dump
 * followed by the same ESR/ELR/FAR-dump-then-poweroff every other QEMU CI
 * handler duplicated); it is now qemu_report_fault() in
 * el2_exc_qemu_common.c, called at this file's one call site below.
 * dump_state() -- the EXTRA info beyond kind/ESR/ELR/FAR that this variant
 * printed and the shared helper does not know about -- is still called
 * immediately before it, so no diagnostic is lost. The marker passed is
 * "VGIC-QEMU-CI" (this target's own CI id, matching the "QEMU-CI" /
 * "QEMU-SNAPSHOT-CI" markers the other converted variants pass) rather than
 * the VGIC_CI_FAIL macro used by ci_fail()/ci_pass() below: VGIC_CI_FAIL bakes
 * in the verdict word ("VGIC-QEMU-CI: FAIL ") for THIS scenario's own
 * pass/fail logic, which is untouched by this refactor, while this catch-all
 * path is the generic "something else happened" report the shared function
 * already standardises as "<marker>: FAULT kind=...". poweroff=1 preserves
 * this target's original choice of calling vgic_ci_poweroff() rather than
 * halting in wfi (unlike el2_exc_qemu.c's target, this one always terminates
 * QEMU itself so the CI script never has to fall back on its wrapping
 * `timeout`). See el2_exc_qemu_common.h for why the marker stays per-target. */

/* This target's own scenario config for the shared guest-sync chain.
 *
 * want_hvc_ack + hvc_advance_elr: preserves this file's original `if (ec ==
 * 0x16u) { frame->elr += 4u; return; }` exactly. Carried over from
 * el2_exc_qemu.c's own ops struct rather than quietly changed: the `+4` is
 * architecturally WRONG for an HVC trap (ELR_EL2 already holds the post-call
 * return address, since it is a return address and not a faulting PC), and it
 * is harmless here only because this target's guest payload
 * (vgic_selftest_guest_el1() in vgic.c, reused unmodified) never issues an
 * HVC -- i.e. it is dead code, same as in el2_exc_qemu.c. Preserved verbatim
 * rather than fixed so this refactor changes no observable behaviour.
 *
 * No claim_first: the mechanism under test (vgic_inject_cntv(), the vCPU
 * interface, the guest's virtual IAR/EOIR) is entirely on the IRQ/FIQ side,
 * which stays in el2_trap() below untouched -- there is nothing for the sync
 * chain to claim first here.
 *
 * No console, no SMC forward, no MMIO absorb: this target's payload uses
 * none of them (it has no console and issues no SMC; its DTS needs are
 * whatever vgic.c's own self-test already assumes). Dynamic W^X promotion is
 * unconditional in the shared chain and needs no flag. */
static const struct qemu_guest_sync_ops vgic_ci_ops = {
	.want_hvc_ack    = 1u,
	.hvc_advance_elr = 1u,
};

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
			 * VGIC_CNTV_HW=0 path.
			 *
			 * GATED ON THE GUEST HAVING INSTALLED ITS VECTORS — fix for
			 * a ~1-in-3 flake in this gate, root-caused 2026-08-26.
			 * Symptom: `VGIC-QEMU-CI: FAULT ... ESR=0x8200000e
			 * ELR=FAR=0x280` with the whole VGST window still zero, i.e.
			 * the guest never got going at all.
			 *
			 * 0x280 is not a random address: it is the Current-EL-SPx IRQ
			 * slot of an AArch64 vector table (see vgic_st_vectors' own
			 * `b vgic_st_irq` at that offset in vgic.c). guest_config()
			 * leaves VBAR_EL1 = 0, and vgic_selftest_guest_el1() installs
			 * its real table a few instructions into its own prologue —
			 * but vgic_init() has ALREADY preset GICH_VMCR with vGrp1
			 * enabled and the virtual PMR wide open, and the guest is
			 * entered with EL1 IRQs takeable. So a vIRQ injected in that
			 * first-few-instructions window is delivered against
			 * VBAR_EL1 == 0, the guest branches to IPA 0x280, and that
			 * address is not executable guest DRAM — dead guest, and a
			 * red gate for a completely healthy build.
			 *
			 * VGST_VBAR_SET is exactly the right interlock: the payload
			 * publishes it in the instruction immediately AFTER its
			 * write_vbar_el1(), so nonzero proves the vectors are live.
			 * Not VGST_MAGIC_IDX — the payload writes the magic BEFORE
			 * setting VBAR, so gating on it would leave the same window
			 * open, just narrower.
			 *
			 * This does NOT weaken what the gate proves: PASS_AFTER_VIRQS
			 * deliveries are still required, and a guest that never
			 * publishes VGST_VBAR_SET at all now trips the
			 * FAIL_AFTER_TICKS deadline below with `injected == 0`, which
			 * ci_fail() already reports precisely. It only stops EL2 from
			 * firing an interrupt at a guest that provably cannot yet
			 * take one. */
			if (VGST_BC_RD(VGST_VBAR_SET) != 0u)
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

	/* Guest (lower-EL) synchronous exception: the shared chain handles the
	 * HVC acknowledgement vgic_ci_ops below documents AND the dynamic W^X
	 * promotion without which the guest cannot execute a single instruction
	 * under STAGE2_WX_DYNAMIC's default XN mapping (this target's guest
	 * payload is vgic.c's own vgic_selftest_guest_el1(), reused unmodified —
	 * see this file's header — and it is code just like any other guest
	 * payload, so it needs the same promotion). See el2_exc_qemu_common.h for
	 * what is shared and why. */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (qemu_guest_sync(frame, ec, &vgic_ci_ops))
			return;
	}

	/* Anything else -- an unexpected guest fault, or our own EL2 synchronous
	 * exception -- is a genuine problem on this scenario's proof path.
	 * dump_state() preserves the extra VGIC/VGST breadcrumb dump this file's
	 * report_fault() used to print (see the comment above); qemu_report_fault()
	 * does the standardised kind/ESR/ELR/FAR line and terminates QEMU. */
	dump_state();
	qemu_report_fault(frame, kind, "VGIC-QEMU-CI", /*poweroff=*/1);
}
