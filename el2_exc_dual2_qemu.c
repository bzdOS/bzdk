/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_dual2_qemu.c — EL2 trap handler for the QEMU `virt` dual-guest
 * Step 1 CI target (main_dual2_qemu.c / dual-qemu-ci.sh).
 *
 * Same rationale as every sibling *_qemu.c trap handler (see
 * el2_exc_qemu.c's header for the canonical statement of it): a NEW,
 * minimal el2_trap(), not a reuse of the real board's el2_exc.c, which
 * pulls in the full board debugger and a dozen Allwinner-only modules this
 * target has no use for.
 *
 * THE ACTUAL PROOF LIVES HERE, NOT IN main_dual2_qemu.c:
 * Only CPU0 ever arms a timer and unmasks IRQ (see main_dual2_qemu.c), so
 * only CPU0 ever takes this trap on the healthy path -- and it does so
 * periodically, PREEMPTING its own guest_demo_el1() exactly like
 * qemu-ci.sh's target already does. Each tick is CPU0's one opportunity to
 * look at BOTH payloads' progress from the outside, without either payload
 * having to cooperate:
 *
 *   - CPU0's own progress: guest.c's GST1 breadcrumb word[1] (loop_count),
 *     bumped by guest_demo_el1() every iteration. Read directly (no new
 *     accessor added to guest.h) at its documented fixed address, matching
 *     Step 0's own main_dual_qemu.c convention of reading fixed breadcrumb
 *     addresses straight from a QEMU CI target.
 *   - CPU3's progress: the fixed counter address zg3_trivial_payload.S
 *     writes to (ZSTAGE2_DRAM_BASE + 0x100 -- see that file's header and
 *     stage2_zephyr.h). EL2's own accesses are never subject to CPU3's
 *     stage-2 regime (stage-2 only ever governs EL1/EL0; VTCR_EL2/
 *     VTTBR_EL2 are per-PE banked -- see stage2_zephyr.h's header), so a
 *     plain volatile read of this physical address from EL2, on CPU0,
 *     works regardless of what CPU3's own stage-2 table says.
 *
 * Tick 2 (~200 ms in) takes the baseline sample; tick 12 (~1.2 s in) takes
 * the final sample and requires BOTH counters to have strictly increased
 * in between before printing PASS. This is deliberately NOT "did CPU3 ever
 * print anything" or "did CPU3 reach state X once" -- either of those would
 * also be true of "boots, then the other boots" sequentially. Two samples
 * with a real wall-clock gap between them, both increasing, is what
 * distinguishes genuine concurrency from sequencing.
 */
#include <stdint.h>
#include "exceptions.h"
#include "guest.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "stage2_zephyr.h"
#include "smp.h"

/* CPU0's guest.c breadcrumb window (GUEST_BC_BASE, "GST1") -- word[1] is
 * guest_demo_el1()'s own loop counter, bumped every iteration at EL1. See
 * guest.c's header comment for the full layout. */
#define GUEST_BC_BASE          0x50000b00UL
#define GUEST_LOOP_COUNT_ADDR  (GUEST_BC_BASE + 4UL)

/* CPU3's trivial-payload counter -- see zg3_trivial_payload.S's header.
 * Kept in sync BY VALUE with that file; if it ever changes, update both. */
#define ZG3_TEST_COUNTER_PA (ZSTAGE2_DRAM_BASE + 0x100UL)

#define SAMPLE_TICK_BASE  2u
#define SAMPLE_TICK_FINAL 12u

static uint32_t g_cpu0_t0, g_cpu3_t0;
static int g_have_baseline;

static inline uint32_t
read_u32(uint64_t pa)
{
	return *(volatile uint32_t *)pa;
}

static void
dual2_qemu_poweroff(void) __attribute__((noreturn));

static void
dual2_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	pl011_puts("DUAL-QEMU-CI2: FAULT cpu=");
	pl011_put_udec(smp_cpu_id());
	pl011_puts(" kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	dual2_qemu_poweroff();
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	/* Only CPU0 ever arms/unmasks this tick (main_dual2_qemu.c) -- a trap
	 * reaching here on any other core is unexpected: CPU1/CPU2 park in
	 * WFI (dbg_core_enable=0 / the weak vblk_async_cpu2_run() default),
	 * and CPU3's trivial payload never touches MMIO/HVC and never arms
	 * its own CNTP (see zg3_trivial_payload.S's header). Treat it as a
	 * genuine, unambiguous FAIL rather than silently mis-attributing a
	 * sample to the wrong core. */
	if (smp_cpu_id() != 0)
		report_fault(frame, kind);

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		if (gic_timer_qemu_irq(frame)) {
			uint64_t ticks = gic_timer_qemu_ticks();

			if ((kind >> 2) == 2u)
				guest_note_preempt();   /* interrupted the EL1 guest */

			if (ticks == SAMPLE_TICK_BASE && !g_have_baseline) {
				g_cpu0_t0 = read_u32(GUEST_LOOP_COUNT_ADDR);
				g_cpu3_t0 = read_u32(ZG3_TEST_COUNTER_PA);
				g_have_baseline = 1;
				pl011_puts("HV: baseline sample (tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(") cpu0_loop=");
				pl011_put_udec(g_cpu0_t0);
				pl011_puts(" cpu3_loop=");
				pl011_put_udec(g_cpu3_t0);
				pl011_puts("\n");
			} else if (ticks % 4u == 0u) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts("\n");
			}

			if (ticks >= SAMPLE_TICK_FINAL) {
				uint32_t cpu0_t1 = read_u32(GUEST_LOOP_COUNT_ADDR);
				uint32_t cpu3_t1 = read_u32(ZG3_TEST_COUNTER_PA);
				int cpu0_advanced = g_have_baseline && (cpu0_t1 > g_cpu0_t0);
				int cpu3_advanced = g_have_baseline && (cpu3_t1 > g_cpu3_t0);

				pl011_puts("HV: final sample (tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(") cpu0_loop=");
				pl011_put_udec(cpu0_t1);
				pl011_puts(" cpu3_loop=");
				pl011_put_udec(cpu3_t1);
				pl011_puts("\n");

				pl011_puts("DUAL-QEMU-CI2: cpu0 before=");
				pl011_put_udec(g_cpu0_t0);
				pl011_puts(" after=");
				pl011_put_udec(cpu0_t1);
				pl011_puts(" cpu3 before=");
				pl011_put_udec(g_cpu3_t0);
				pl011_puts(" after=");
				pl011_put_udec(cpu3_t1);
				pl011_puts("\n");

				if (cpu0_advanced && cpu3_advanced) {
					pl011_puts("DUAL-QEMU-CI2: PASS (CPU0's guest_demo_el1 AND CPU3's "
					           "zguest_cpu3/zload2 trivial payload both advanced "
					           "concurrently between sample 1 and sample 2)\n");
				} else {
					pl011_puts("DUAL-QEMU-CI2: FAIL (");
					if (!cpu0_advanced)
						pl011_puts("cpu0 did not advance ");
					if (!cpu3_advanced)
						pl011_puts("cpu3 did not advance ");
					pl011_puts(")\n");
				}
				dual2_qemu_poweroff();
			}
		}
		return;   /* never advance ELR/SPSR for an async exception */
	}

	/* Guest (lower-EL) synchronous HVC -- acknowledge and resume, matching
	 * guest.c's documented optional hypercall path (guest_demo_el1() issues
	 * one periodically). */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		/* HVC from AArch64: unlike a data/instruction abort (where ELR_EL2
		 * is the address of the FAULTING instruction and must be advanced
		 * past it manually), the ARM architecture already sets ELR_EL2 to
		 * the instruction AFTER the HVC for an HVC trap -- it is the
		 * "return from call" address, not a "faulting PC" -- so no
		 * adjustment is needed or correct here. (An earlier version of
		 * this file copied el2_exc_qemu.c's own `frame->elr += 4u;` for
		 * this branch verbatim; that line is a latent bug in this tree --
		 * dead code there, since that target's own guest payload never
		 * issues hvc -- but live and guest-derailing here, where
		 * guest_demo_el1() genuinely does every GUEST_HVC_PERIOD
		 * iterations: it double-advanced ELR past the following
		 * unconditional branch and landed the guest's PC inside
		 * guest_config()'s body, which promptly wrote an EL2-only sysreg
		 * from EL1 and wedged. Confirmed live via a temporary debug print
		 * of ESR/ELR around this branch before removing the extra `+= 4`.) */
		if (ec == 0x16u)
			return;
	}

	/* Anything else -- an unexpected guest fault, or our own EL2
	 * synchronous exception -- is a genuine problem on this milestone's
	 * boot path: report it and halt rather than silently resuming into a
	 * possibly-corrupt state. */
	report_fault(frame, kind);
}
