/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_dual2_qemu.c — EL2 trap handler for the QEMU `virt` dual-guest
 * Step 1 CI target (main_dual2_qemu.c / dual-qemu-ci.sh) and the zunhalt
 * re-arm repro (main_dual_rearm_qemu.c / dual-rearm-qemu-ci.sh).
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
 *   - CPU3's progress: vconsole.c's channel-1 "total_bytes" counter
 *     (HVMAP_VCONSOLE_CHAN1_HDR word[1]), bumped by THIS FILE's own
 *     el2_trap() every time it routes one of CPU3's UART0-THR stage-2
 *     faults to vconsole_handle_fault(frame, 1) -- see the CPU3 dispatch
 *     branch below and zg3_trivial_payload.S's header. This is a
 *     FAULT-OBSERVED signal: every increment happened because EL2 itself,
 *     on CPU0/this core, serviced a trap CPU3 took. There is no other
 *     core's cache in the loop, unlike the OLD probe this replaces (a
 *     DRAM word zg3_trivial_payload.S also still bumps, ZG3_TEST_COUNTER_PA
 *     below, kept for information only -- see docs/dual-guest.md,
 *     "Retrying a failed zboot ... and a probe that lied", for why that
 *     older probe is unsound on real hardware despite being reliable
 *     under QEMU, and open item 5 under "What's still open" for why this
 *     file now uses the fault-observed counter instead).
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
#include "vconsole.h"
#include "hv_addrmap.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* CPU0's guest.c breadcrumb window (GUEST_BC_BASE, "GST1") -- word[1] is
 * guest_demo_el1()'s own loop counter, bumped every iteration at EL1. See
 * guest.c's header comment for the full layout. */
#define GUEST_BC_BASE          0x50000b00UL
#define GUEST_LOOP_COUNT_ADDR  (GUEST_BC_BASE + 4UL)

/* CPU3's trivial-payload DRAM counter -- see zg3_trivial_payload.S's
 * header ("signal #1"). Kept in sync BY VALUE with that file; if it ever
 * changes, update both. INFORMATIONAL ONLY as of the fault-observed-probe
 * change: printed, but no longer part of the PASS/FAIL arithmetic below. */
#define ZG3_TEST_COUNTER_PA (ZSTAGE2_DRAM_BASE + 0x100UL)

/* CPU3's FAULT-OBSERVED heartbeat ("signal #2"): vconsole.c's channel-1
 * ring header word[1] (total_bytes), bumped only inside
 * vconsole_handle_fault(frame, 1) -- i.e. only when EL2 itself serviced a
 * real stage-2 data-abort trap from CPU3's UART0-THR write. THIS is what
 * PASS/FAIL is keyed on now. See hv_addrmap.h for the header layout
 * (word[0] magic, word[1] total_bytes, ...). */
#define ZG3_CHAN1_TOTAL_BYTES_PA (HVMAP_VCONSOLE_CHAN1_HDR + 4UL)

#define SAMPLE_TICK_BASE  2u
#define SAMPLE_TICK_FINAL 12u

static uint32_t g_cpu0_t0, g_cpu3_t0;
static uint32_t g_cpu3_dram_t0;   /* informational only, see above */
static int g_have_baseline;

static inline uint32_t
read_u32(uint64_t pa)
{
	return *(volatile uint32_t *)pa;
}

/* This target's own scenario config for the shared guest-sync chain (see
 * el2_exc_qemu_common.h for the chain's fixed order: claim_first, console,
 * then the unconditional dynamic W^X promotion, then HVC ack, SMC, MMIO
 * absorb). No console, no SMC forward, no MMIO absorb: CPU0's guest_demo_el1
 * payload runs with stage-2 disabled / flat physical and uses none of them
 * (CPU3's UART0-THR console traffic is a wholly separate path, routed by the
 * per-core branch near the top of el2_trap() below, which this refactor
 * leaves untouched). Dynamic W^X promotion is unconditional in the shared
 * chain and needs no flag here either.
 *
 * hvc_advance_elr = 0 IS THE POINT OF THIS FILE'S HEADER COMMENT IN
 * el2_exc_qemu_common.h: for an HVC trap, ARM's architecture already sets
 * ELR_EL2 to the instruction AFTER the HVC (it is a "return from call"
 * address, not a "faulting PC"), so no further adjustment is needed OR
 * correct. An earlier version of this file copied el2_exc_qemu.c's own
 * `frame->elr += 4u;` for this branch verbatim; that line is a latent bug in
 * this tree — dead code in el2_exc_qemu.c, since that target's own guest
 * payload never issues hvc — but LIVE and guest-derailing here, where
 * guest_demo_el1() genuinely does issue an hvc every GUEST_HVC_PERIOD
 * iterations: it double-advanced ELR past the following unconditional branch
 * and landed the guest's PC inside guest_config()'s body, which promptly
 * wrote an EL2-only sysreg from EL1 and wedged. Confirmed live via a
 * temporary debug print of ESR/ELR around this branch before removing the
 * extra `+= 4`. DO NOT set this to 1 to "harmonise" with el2_exc_qemu.c —
 * that would reintroduce the exact bug this comment documents. */
static const struct qemu_guest_sync_ops dual2_qemu_ops = {
	.want_hvc_ack    = 1u,
	.hvc_advance_elr = 0u,
};

/* dual2_qemu_poweroff()/report_fault() used to be defined here,
 * verbatim-duplicated across every QEMU CI handler (see
 * el2_exc_qemu_common.h's header for the full "nine copies" rationale); the
 * PSCI SYSTEM_OFF half now lives in el2_exc_qemu_common.c as
 * qemu_psci_poweroff(), unchanged in behaviour (identical SMC #0x84000008
 * SYSTEM_OFF, wfi fallback loop). Kept as a #define under the old name so the
 * call site below reads the same as before this refactor. */
#define dual2_qemu_poweroff qemu_psci_poweroff

/* report_fault() is kept LOCAL rather than folded entirely into the shared
 * qemu_report_fault(): this target's original FAULT line carries one extra
 * field qemu_report_fault()'s fixed "<marker>: FAULT kind=.. ESR=.. ELR=..
 * FAR=.." format has no hook for — cpu=<id>, which matters here specifically
 * because this handler is reached from multiple cores (see the CPU3 routing
 * branch in el2_trap() below) and a FAULT line without it would leave a
 * reader guessing which core actually died. Per el2_exc_qemu_common.h's
 * contract for this situation, the extra is printed as its own line
 * immediately before calling the shared helper — same pattern
 * el2_exc_linux_qemu.c/el2_exc_zephyr_qemu.c already use for their own kept
 * extras. */
static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	pl011_puts("DUAL-QEMU-CI2: cpu=");
	pl011_put_udec(smp_cpu_id());
	pl011_puts("\n");

	qemu_report_fault(frame, kind, "DUAL-QEMU-CI2", /*poweroff=*/1);
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	/* Only CPU0 ever arms/unmasks this tick (main_dual2_qemu.c) -- an IRQ/
	 * FIQ reaching here on any other core is unexpected: CPU1/CPU2 park in
	 * WFI (dbg_core_enable=0 / the weak vblk_async_cpu2_run() default),
	 * and CPU3's trivial payload never arms its own CNTP (see
	 * zg3_trivial_payload.S's header). CPU3's payload DOES now deliberately
	 * take ONE well-formed, expected kind of trap on every loop iteration:
	 * a lower-EL synchronous data abort on UART0's THR register, because
	 * stage2_zephyr.c maps zero real MMIO for CPU3 (see that file's
	 * header). Route exactly that one case to vconsole_handle_fault(...,
	 * 1) -- the same channel-1 emulated 16550 real Zephyr's own console
	 * output already goes through -- and treat anything else from a
	 * non-CPU0 core (any trap on CPU1/CPU2, or any OTHER kind of trap from
	 * CPU3) as the genuine, unambiguous FAIL it always was. */
	if (smp_cpu_id() != 0) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (smp_cpu_id() == 3u && (kind >> 2) == 2u && t == EL2_KIND_SYNC &&
		    ec == 0x24u && vconsole_handle_fault(frame, 1))
			return;

		report_fault(frame, kind);
	}

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		if (gic_timer_qemu_irq(frame)) {
			uint64_t ticks = gic_timer_qemu_ticks();

			if ((kind >> 2) == 2u)
				guest_note_preempt();   /* interrupted the EL1 guest */

			if (ticks == SAMPLE_TICK_BASE && !g_have_baseline) {
				g_cpu0_t0 = read_u32(GUEST_LOOP_COUNT_ADDR);
				g_cpu3_t0 = read_u32(ZG3_CHAN1_TOTAL_BYTES_PA);
				g_cpu3_dram_t0 = read_u32(ZG3_TEST_COUNTER_PA);
				g_have_baseline = 1;
				pl011_puts("HV: baseline sample (tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(") cpu0_loop=");
				pl011_put_udec(g_cpu0_t0);
				pl011_puts(" cpu3_chan1_bytes=");
				pl011_put_udec(g_cpu3_t0);
				pl011_puts(" cpu3_dram=");
				pl011_put_udec(g_cpu3_dram_t0);
				pl011_puts("\n");
			} else if (ticks % 4u == 0u) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts("\n");
			}

			if (ticks >= SAMPLE_TICK_FINAL) {
				uint32_t cpu0_t1 = read_u32(GUEST_LOOP_COUNT_ADDR);
				uint32_t cpu3_t1 = read_u32(ZG3_CHAN1_TOTAL_BYTES_PA);
				uint32_t cpu3_dram_t1 = read_u32(ZG3_TEST_COUNTER_PA);
				int cpu0_advanced = g_have_baseline && (cpu0_t1 > g_cpu0_t0);
				/* PASS/FAIL is keyed on the FAULT-OBSERVED channel-1 byte
				 * counter, not the DRAM word -- see this file's header and
				 * zg3_trivial_payload.S's for why the DRAM word alone is
				 * not trustworthy on real hardware. */
				int cpu3_advanced = g_have_baseline && (cpu3_t1 > g_cpu3_t0);

				pl011_puts("HV: final sample (tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(") cpu0_loop=");
				pl011_put_udec(cpu0_t1);
				pl011_puts(" cpu3_chan1_bytes=");
				pl011_put_udec(cpu3_t1);
				pl011_puts(" cpu3_dram=");
				pl011_put_udec(cpu3_dram_t1);
				pl011_puts("\n");

				/* "cpu3" here (and below) is the FAULT-OBSERVED vconsole
				 * channel-1 byte counter -- see this file's header comment.
				 * The line's shape (label text, before=/after= pairs) is
				 * otherwise unchanged so dual-qemu-ci.sh's/
				 * dual-rearm-qemu-ci.sh's existing sed/grep parsing keeps
				 * working unmodified. */
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
					           "concurrently between sample 1 and sample 2 -- CPU3's "
					           "progress is the fault-observed vconsole channel-1 byte "
					           "counter, not a DRAM word read from another core)\n");
				} else {
					/* NOT "FAIL": this firmware has no idea which of the
					 * two callers is running it, or what THEY expect. Both
					 * dual-qemu-ci.sh's pass A and dual-rearm-qemu-ci.sh
					 * need cpu3_advanced==1 here (a real failure), but
					 * dual-qemu-ci.sh's pass B *requires*
					 * cpu3_advanced==0 (the stale-image wipe refusing a
					 * destination-only image is the whole point of that
					 * pass) -- see dual-qemu-ci.sh's run_pass()/"expect".
					 * Printing "FAIL" unconditionally here previously read
					 * as a gate verdict even on pass B's correct refusal,
					 * right next to the script's own "PASS" a few lines
					 * later, and cost real debugging time. So: report the
					 * bare facts only, with a word that cannot be mistaken
					 * for a verdict; the caller who knows what was expected
					 * decides PASS/FAIL from the before=/after= line above. */
					pl011_puts("DUAL-QEMU-CI2: OUTCOME cpu0_advanced=");
					pl011_puts(cpu0_advanced ? "1" : "0");
					pl011_puts(" cpu3_advanced=");
					pl011_puts(cpu3_advanced ? "1" : "0");
					pl011_puts(" (not a verdict -- see dual-qemu-ci.sh's "
					           "'expect' parameter for whether cpu3 NOT "
					           "advancing is the correct outcome here)\n");
				}
				dual2_qemu_poweroff();
			}
		}
		return;   /* never advance ELR/SPSR for an async exception */
	}

	/* Guest (lower-EL) synchronous exception: the shared chain handles the
	 * dynamic W^X promotion (without which the guest cannot execute its
	 * first instruction under STAGE2_WX_DYNAMIC's default XN mapping) and
	 * the HVC acknowledgement guest_demo_el1() periodically exercises --
	 * with hvc_advance_elr=0, see the big comment on dual2_qemu_ops above
	 * (and el2_exc_qemu_common.h) for why NOT advancing ELR here is the
	 * correct, hard-won behaviour for this file specifically. */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (qemu_guest_sync(frame, ec, &dual2_qemu_ops))
			return;
	}

	/* Anything else -- an unexpected guest fault, or our own EL2
	 * synchronous exception -- is a genuine problem on this milestone's
	 * boot path: report it and halt rather than silently resuming into a
	 * possibly-corrupt state. */
	report_fault(frame, kind);
}
