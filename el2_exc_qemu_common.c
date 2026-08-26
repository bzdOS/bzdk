/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_qemu_common.c — implementation of the shared half of the QEMU-virt
 * CI targets' el2_trap(). See el2_exc_qemu_common.h for WHY this exists (nine
 * duplicated dispatch chains meant a stage2.h policy change reached none of
 * them and the whole board-free gate went red) and for the exact split between
 * what is shared here and what deliberately stays in each scenario.
 *
 * Freestanding: <stdint.h> plus this tree's own headers, no libc.
 *
 * WEAK REFERENCES. This one object is linked into targets with very different
 * object lists — some have no stage2.o, some no vconsole.o, some no
 * mmio_absorb.o. Rather than splitting it per target (which would recreate the
 * duplication it exists to remove) every optional collaborator is referenced
 * weakly and skipped when absent, the same pattern el2_exc.c already uses for
 * vnet_mmio_fault()/vblk_mmio_fault() and gic_timer.c for dbg_vcpu1. A target
 * that later starts linking one of them gets the behaviour automatically
 * instead of silently not getting it — which is precisely the failure mode
 * this file was created in response to.
 */
#include <stdint.h>
#include "el2_exc_qemu_common.h"
#include "pl011_qemu.h"
#include "stage2_wx_qemu.h"

/* vconsole.c's trap-emulated UART. Weak: el2_exc_dual_qemu.c's target links no
 * vconsole.o at all (it proves PSCI CPU_ON bring-up, it has no guest console). */
extern int vconsole_handle_fault(struct el2_frame *frame, int chan)
	__attribute__((weak));

/* mmio_absorb.c's read-as-zero/write-as-noop catch-all. Weak: only the targets
 * running a guest whose DTS describes absent devices link it. */
extern int mmio_absorb_fault(struct el2_frame *frame) __attribute__((weak));

/* The guest-DRAM window used for the mmio-absorb exclusion comes from
 * stage2.h's STAGE2_DRAM_BASE/STAGE2_DRAM_SIZE. Those are COMPILE-TIME macros,
 * not linker symbols, so they are available here regardless of whether the
 * target links stage2.o — no weak reference is possible or needed. (A target
 * without stage2.o also has no mmio_absorb.o in practice, so the exclusion is
 * simply never consulted there.) */
#include "stage2.h"

void
qemu_psci_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;   /* SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

void
qemu_report_fault(struct el2_frame *frame, unsigned long kind,
                  const char *marker, int poweroff)
{
	pl011_puts(marker);
	pl011_puts(": FAULT kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");

	if (poweroff)
		qemu_psci_poweroff();
	for (;;)
		__asm__ volatile("wfi");
}

/*
 * purpose:     absorb a non-DRAM lower-EL data abort as a harmless MMIO probe
 * input:       frame — the live trap frame
 * output:      1 if absorbed, 0 if this IPA must NOT be absorbed
 * sideEffects: on success mmio_absorb_fault() fakes the access and advances ELR
 *
 * The DRAM exclusion is the whole substance of this helper; see the
 * want_mmio_absorb comment in the header for why absorbing unconditionally
 * would turn a real guest bug into a green CI run.
 */
static int
absorb_non_dram(struct el2_frame *frame)
{
	uint64_t hpfar, ipa;

	if (mmio_absorb_fault == 0)
		return 0;   /* target links no mmio_absorb.o */

	/* Same IPA reconstruction vgicd.c and stage2_wx_fault() use: FAR_EL2 is
	 * only good for the low 12 bits once the guest's stage-1 MMU is on, so
	 * the page comes from HPFAR_EL2 bits[39:4] = IPA[47:12]. */
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;

	if (ipa >= STAGE2_DRAM_BASE &&
	    ipa < STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE)
		return 0;   /* real DRAM fault — must reach report_fault() */

	return mmio_absorb_fault(frame);
}

int
qemu_guest_sync(struct el2_frame *frame, uint32_t ec,
                const struct qemu_guest_sync_ops *ops)
{
	/* A scenario's own mechanism-under-test gets first refusal. */
	if (ops->claim_first && ops->claim_first(frame, ec))
		return 1;

	/* Trap-emulated console: by far the most frequent fault on the targets
	 * that have one, so it is checked before the rarer cases below. */
	if (ops->want_console && ec == QEMU_EC_DABT_LOWER &&
	    vconsole_handle_fault != 0 &&
	    vconsole_handle_fault(frame, (int)ops->console_chan)) {
		if (ops->after_console)
			ops->after_console();
		return 1;
	}

	/* Dynamic W^X promotion. Unconditional — not an ops flag — because it is
	 * correct for ANY guest running under stage2.c's tables, and making it
	 * opt-in is exactly the mistake that left nine handlers without it and
	 * the whole gate red. stage2_wx_qemu_try() is itself a no-op where
	 * stage2.o is not linked. */
	if (stage2_wx_qemu_try(frame, ec))
		return 1;

	if (ops->want_hvc_ack && ec == QEMU_EC_HVC64) {
		if (ops->hvc_advance_elr)
			frame->elr += 4u;
		return 1;
	}

	if (ops->on_smc && ec == QEMU_EC_SMC64) {
		ops->on_smc(frame);
		return 1;
	}

	if (ops->want_mmio_absorb && ec == QEMU_EC_DABT_LOWER &&
	    absorb_non_dram(frame))
		return 1;

	return 0;   /* not ours — caller reports it */
}
