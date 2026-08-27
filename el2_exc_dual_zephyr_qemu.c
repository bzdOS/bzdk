/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_dual_zephyr_qemu.c — EL2 trap handler for the QEMU `virt`
 * dual-guest Step 2 CI target (main_dual_zephyr_qemu.c / dual-zephyr-qemu-
 * ci.sh): CPU0 running the same guest_demo_el1() proof Step 1
 * (el2_exc_dual2_qemu.c) already used, CPU3 running a REAL Zephyr RTOS
 * image instead of Step 1's four-instruction trivial payload.
 *
 * WHY THIS FILE COULD NOT JUST BE el2_exc_dual2_qemu.c AGAIN
 * ------------------------------------------------------------------------
 * Step 1's trivial payload (zg3_trivial_payload.S) never traps past its own
 * four instructions -- no UART, no GIC, nothing. Step 1's el2_exc_dual2_qemu.c
 * therefore only ever needed to handle CPU0's own tick/HVC and treated ANY
 * trap from CPU3 as an unconditional failure (see that file's report_fault
 * gate: "smp_cpu_id() != 0 -> report_fault").
 *
 * Real Zephyr is not that quiet. Per stage2_zephyr.h's design, CPU3 gets
 * ZERO real MMIO passthrough -- every access below IPA 0x40000000 stage-2-
 * faults -- and the Zephyr board (zephyr-guest/boards/bzdos/bpi_m64_hv/
 * bpi_m64_hv.dts) still describes a UART0 and a GIC that its drivers probe
 * unconditionally at boot. So CPU3 WILL trap, repeatedly, the moment Zephyr
 * starts running: this file's whole reason to exist is routing those traps
 * exactly the way the real board's el2_exc.c already does for its own CPU3
 * dispatch (see that file's header comment, which this mirrors) --
 * vconsole.c's channel-1 UART emulation first, mmio_absorb.c's catch-all
 * for everything else -- reused here UNMODIFIED, just wired into a new,
 * minimal QEMU-target dispatcher instead of the full board debugger.
 *
 * ============================================================================
 * VERDICT DESIGN: THREE INDEPENDENT THINGS MUST ALL BE TRUE
 * ============================================================================
 * A naive "did some counter increase" check is not enough here (see
 * main_dual_zephyr_qemu.c's own header on why real Zephyr's absolute-
 * address-sensitive data structures might make a relocated boot LOOK like
 * it is running -- consuming CPU, taking traps -- while actually being
 * subtly corrupted). This file therefore checks THREE separate things
 * before ever printing an overall PASS, exactly matching the task's own
 * "verify empirically, don't assume" requirement:
 *
 *   1. CPU0 STILL MAKES PROGRESS (guest.c's GST1 loop counter, byte-for-
 *      byte the same check Step 1 used) -- proves this new target didn't
 *      regress the FreeBSD-shaped side of the proof.
 *   2. CPU3's console ring GENUINELY ADVANCES BETWEEN TWO SAMPLES a real
 *      wall-clock interval apart (vconsole channel 1's own total_bytes
 *      counter, HVMAP_VCONSOLE_CHAN1_HDR word[1]) -- the Step 1 concurrency
 *      proof, generalised: instead of a synthetic fixed-address counter
 *      (which the real Zephyr image does not have and would never write),
 *      this reads the SAME byte-count vconsole.c already maintains for
 *      every guest console write, on the SAME channel real Zephyr's ns16550
 *      driver actually uses. Two samples with a gap, both climbing, is
 *      still the same "concurrent, not sequential" argument Step 1 made.
 *   3. THE ACTUAL, REAL ZEPHYR BANNER+HEARTBEAT TEXT ARRIVED, byte-for-byte
 *      matched against the known-good string this project already recorded
 *      on real hardware and under zephyr-qemu-ci.sh (see PASS_MARKER below)
 *      -- this is the guard against "byte counter went up because garbage
 *      is being emitted" or "climbed briefly then wedged mid-boot". Matched
 *      independently of (2)'s sample timing, via the same streaming
 *      substring matcher el2_exc_zephyr_qemu.c already uses, so a marker
 *      that arrives before, between, or after the two sample points still
 *      counts.
 *
 * Only if ALL THREE hold does this print "DUAL-ZEPHYR-QEMU-CI: PASS". Any
 * one alone (e.g. rising byte counts from a corrupted image spewing
 * whatever the CPU happens to execute into THR) is explicitly NOT accepted
 * as evidence Zephyr actually booted.
 *
 * ============================================================================
 * CPU3 FAILURE REPORTING, KEPT DISTINCT FROM CPU0's
 * ============================================================================
 * If real Zephyr's relocation-sensitive absolute pointers (device/init-level
 * tables, sw_isr_table, etc. -- see main_dual_zephyr_qemu.c/zguest_cpu3.c's
 * header discussion) are in fact broken by zload2's plain positional shift
 * to 0xBE000000, the most likely observable failure is CPU3 branching to (or
 * loading through) a stale 0x51xxxxxx-range absolute address that its own
 * stage-2 table (stage2_zephyr.c) leaves entirely INVALID -- an INSTRUCTION
 * abort (EC 0x20/0x21) or a data abort whose FAR is nowhere near UART0/GIC,
 * neither of which vconsole_handle_fault()/mmio_absorb_fault() claims (both
 * only ever look at data aborts, and mmio_absorb_fault() never sees an
 * instruction abort at all -- see mmio_absorb.h's contract). report_cpu3_fault
 * below is exactly that "fell through everything known-safe" path: it prints
 * ESR/ELR/FAR distinctly (a "CPU3 FAULT" line, never conflated with CPU0's
 * own "FAIL" line) and powers off immediately rather than waiting out CPU0's
 * sampling window -- a wedged or wandering CPU3 is conclusive evidence on its
 * own, and printing it immediately, with the faulting address, is far more
 * useful than a bare eventual timeout.
 */
#include <stdint.h>
#include "exceptions.h"
#include "guest.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "vconsole.h"
#include "mmio_absorb.h"
#include "hv_addrmap.h"
#include "smp.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header.
                                   * mmio_absorb.h/vconsole.h stay included above: CPU3's
                                   * dispatch (handle_cpu3_trap() below) keeps calling both
                                   * directly rather than through the shared ops struct — see
                                   * that function's own comment for why. */

/* CPU0's guest.c breadcrumb window (GUEST_BC_BASE, "GST1") -- word[1] is
 * guest_demo_el1()'s own loop counter, bumped every iteration at EL1. Same
 * address el2_exc_dual2_qemu.c (Step 1) already reads. */
#define GUEST_BC_BASE          0x50000b00UL
#define GUEST_LOOP_COUNT_ADDR  (GUEST_BC_BASE + 4UL)

/* CPU3/Zephyr's REAL console progress: vconsole channel 1's own byte
 * counter (word[1] of its header ring -- see vconsole.h's layout comment,
 * identical field position to channel 0's). Not a synthetic fixed counter
 * (Step 1's trivial payload had one at ZSTAGE2_DRAM_BASE+0x100; real
 * Zephyr has no such thing and would never write it) -- this is the SAME
 * accounting vconsole_capture_byte() already performs for every byte the
 * guest's ns16550 driver writes to THR, on the channel real Zephyr's
 * console actually uses. */
#define ZCHAN1_TOTAL_BYTES_ADDR (HVMAP_VCONSOLE_CHAN1_HDR + 4UL)

/* zguest_cpu3.c's/zload2.c's own breadcrumb words -- see their respective
 * headers for the full layout. Read back here purely as diagnostics (which
 * boot stage CPU3 reached), never as part of the PASS/FAIL decision itself
 * (that is ZCHAN1_TOTAL_BYTES_ADDR + PASS_MARKER, both real, externally
 * observable console evidence). */
#define ZG3_BC_STATE_ADDR      (HVMAP_ZGUEST3_BC + 4UL)   /* word[1] */
#define ZLD2_BC_ELF_VALID_ADDR (HVMAP_ZLOAD2_BC  + 4UL)   /* word[1] */
#define ZLD2_BC_N_SEG_ADDR     (HVMAP_ZLOAD2_BC  + 8UL)   /* word[2] */
#define ZLD2_BC_ZBASE_ADDR     (HVMAP_ZLOAD2_BC  + 12UL)  /* word[3] */
#define ZLD2_BC_PABASE_ADDR    (HVMAP_ZLOAD2_BC  + 16UL)  /* word[4] */
#define ZLD2_BC_ENTRY_PA_ADDR  (HVMAP_ZLOAD2_BC  + 20UL)  /* word[5] */
#define ZLD2_BC_BYTES_ADDR     (HVMAP_ZLOAD2_BC  + 24UL)  /* word[6] */
#define ZLD2_BC_FAIL_ADDR      (HVMAP_ZLOAD2_BC  + 28UL)  /* word[7] */

#define SAMPLE_TICK_BASE  2u     /* ~200 ms in */
#define SAMPLE_TICK_FINAL 60u    /* ~6 s in -- real Zephyr's boot (banner +
                                  * device/driver init under QEMU TCG) is
                                  * much slower than Step 1's four-
                                  * instruction payload, but measured runs
                                  * (see dual-zephyr-qemu-ci.sh's commit
                                  * message) reach "heartbeat 1" well under
                                  * 1 s -- this leaves ample margin without
                                  * making every CI run wait needlessly. */

static uint32_t g_cpu0_t0, g_cpu3_t0;
static int g_have_baseline;

/* Set by handle_cpu3_trap() below the moment the real banner+heartbeat
 * marker is matched (see PASS_MARKER); read only by CPU0's tick handler.
 * Plain volatile word, no dc civac -- live, never-persisted state, SMPEN
 * cache coherency is enough, same convention as zguest_cpu3.c's
 * g_zguest3_start_req. */
static volatile int g_zephyr_marker_seen;

static inline uint32_t
read_u32(uint64_t pa)
{
	return *(volatile uint32_t *)pa;
}

/* dual_zephyr_qemu_poweroff() used to be defined here, verbatim-duplicated
 * across every QEMU CI handler (see el2_exc_qemu_common.h's header for the
 * full "nine copies" rationale); it now lives in el2_exc_qemu_common.c as
 * qemu_psci_poweroff(), unchanged in behaviour (identical SMC #0x84000008
 * SYSTEM_OFF, wfi fallback loop). Kept as a #define under the old name so
 * every call site below reads exactly as it did before this refactor.
 * (main_dual_zephyr_qemu.c has its OWN separate `static` definition of a
 * same-named function — checked via grep before this change — so this is a
 * same-file rename only, not a cross-TU forwarder.) */
#define dual_zephyr_qemu_poweroff qemu_psci_poweroff

/* ------------------------------------------------------------------ *
 * CPU3/Zephyr real console echo + PASS-marker matching -- same technique
 * el2_exc_zephyr_qemu.c already uses for the standalone Zephyr-on-QEMU
 * target (see that file's header for why "heartbeat 1" and not the banner
 * or "heartbeat 0" is the chosen marker: it proves the guest survived and
 * made forward progress after main(), not just that printk() reached THR
 * once). Matched against the raw byte stream tee'd by vconsole.c's channel
 * 1, independent of this file's own "[zephyr] " prefixing below.
 * ------------------------------------------------------------------ */
static const char PASS_MARKER[] = "heartbeat 1";

static int zc_at_line_start = 1;
static unsigned zc_marker_pos;
static uint32_t g_zephyr_bytes;   /* diagnostic: total bytes echoed */

static void
echo_zephyr_byte(uint8_t c)
{
	g_zephyr_bytes++;

	if (c == '\r')
		return;
	if (c == '\n') {
		pl011_putc('\n');
		zc_at_line_start = 1;
		return;
	}
	if (zc_at_line_start) {
		pl011_puts("[zephyr] ");
		zc_at_line_start = 0;
	}
	pl011_putc((char)c);
}

static int
zephyr_marker_step(uint8_t c)
{
	if (c == (uint8_t)PASS_MARKER[zc_marker_pos]) {
		zc_marker_pos++;
		if (PASS_MARKER[zc_marker_pos] == '\0') {
			zc_marker_pos = 0;
			return 1;
		}
		return 0;
	}
	zc_marker_pos = (c == (uint8_t)PASS_MARKER[0]) ? 1u : 0u;
	return 0;
}

/* NOTE: channel 1 has no USB-ACM tee ring at all (vconsole.h: TX-only by
 * design -- vc_chan1.interactive == 0, so vconsole_capture_byte() never
 * calls vc_txtee_push() on that path). Unlike el2_exc_zephyr_qemu.c's
 * channel-0 equivalent, this file cannot drain a tee ring; instead
 * handle_cpu3_trap() below echoes each byte straight from channel 1's
 * capture-ring accounting the moment vconsole_handle_fault() reports it
 * (total_bytes strictly increases by exactly one per THR write/fault). */

/* CPU0's own scenario config for the shared guest-sync chain (see
 * el2_exc_qemu_common.h for the chain's fixed order: claim_first, console,
 * then the unconditional dynamic W^X promotion, then HVC ack, SMC, MMIO
 * absorb). No console, no SMC forward, no MMIO absorb: CPU0's guest_demo_el1
 * payload runs with stage-2 disabled / flat physical and uses none of them
 * (CPU3's real-Zephyr console/MMIO traffic is a wholly separate path, routed
 * by handle_cpu3_trap() above, which this refactor deliberately leaves
 * untouched -- see that function's own comment). Dynamic W^X promotion is
 * unconditional in the shared chain and needs no flag here either (and is a
 * no-op on this target regardless: no stage2.o is linked into the
 * dual-zephyr-qemu build, only stage2_zephyr.o, so stage2_wx_fault resolves
 * weak-absent -- see stage2_wx_qemu.h).
 *
 * hvc_advance_elr = 0, NOT 1: this is the exact case el2_exc_qemu_common.h's
 * own header comment and this file's task brief call out by name. ELR_EL2
 * already holds the post-HVC return address (it is a "return from call"
 * address, not a faulting PC), so no adjustment is correct or needed --
 * matching what this file's original code did (see the comment that used to
 * sit on the `if (ec == 0x16u) return;` line below, preserved here) and
 * el2_exc_dual2_qemu.c's own finding of the same thing, arrived at the hard
 * way (a live guest wedge from a double-advanced ELR) before this file was
 * ever written. */
static const struct qemu_guest_sync_ops dual_zephyr_cpu0_ops = {
	.want_hvc_ack    = 1u,
	.hvc_advance_elr = 0u,
};

/* Kept LOCAL rather than delegated to the shared qemu_report_fault(): that
 * helper's format is fixed as "<marker>: FAULT kind=.. ESR=.. ELR=.. FAR=..",
 * but dual-zephyr-qemu-ci.sh greps literally for "DUAL-ZEPHYR-QEMU-CI: FAIL"
 * (see its concurrent-pass check) -- calling qemu_report_fault() here would
 * print "...: FAULT kind=.." instead and silently break that check. The
 * "FAIL cpu=0" text is therefore load-bearing for CI, not just cosmetic, so
 * this function is not a shared-format candidate at all (contrast
 * el2_exc_dual2_qemu.c's report_fault(), which prints an EXTRA line and then
 * DOES delegate -- that works there because dual-qemu-ci.sh/dual-rearm-
 * qemu-ci.sh grep for the shared "FAULT" text, not "FAIL"). Only the
 * terminal call is shared now, via the dual_zephyr_qemu_poweroff macro
 * above. */
static void report_fault_cpu0(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault_cpu0(struct el2_frame *frame, unsigned long kind)
{
	pl011_puts("DUAL-ZEPHYR-QEMU-CI: FAIL cpu=0 kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	dual_zephyr_qemu_poweroff();
}

static void report_cpu3_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

/* See file header "CPU3 FAILURE REPORTING" section: this is the "fell
 * through vconsole AND mmio_absorb, or wasn't even a data abort" path --
 * the concrete, hardware-observed evidence a naive relocated boot is
 * actually broken, not just slow. Distinct marker string from CPU0's own
 * FAIL, on purpose (see this file's header).
 *
 * Kept LOCAL, not delegated to qemu_report_fault(): dual-zephyr-qemu-ci.sh
 * greps literally for the substring "CPU3 FAULT" (space, not the shared
 * helper's "<marker>: FAULT" colon-then-space shape) to detect a genuine
 * CPU3 wedge, and this function also carries an EC= field plus two extra
 * diagnostic lines (console-bytes-so-far, marker-seen) that the shared
 * fixed format has no room for. Per el2_exc_qemu_common.h's contract for
 * this situation, none of that is reproducible through the shared helper,
 * so the whole function stays as it was; only the terminal call is shared,
 * via the dual_zephyr_qemu_poweroff macro. */
static void
report_cpu3_fault(struct el2_frame *frame, unsigned long kind)
{
	uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

	if (!zc_at_line_start)
		pl011_putc('\n');
	pl011_puts("DUAL-ZEPHYR-QEMU-CI: CPU3 FAULT (real Zephyr) -- kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" EC=0x");
	pl011_put_hex32(ec);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	pl011_puts("DUAL-ZEPHYR-QEMU-CI: CPU3 zephyr console bytes emulated before the fault: ");
	pl011_put_udec(g_zephyr_bytes);
	pl011_puts(" (marker 'heartbeat 1' seen: ");
	pl011_puts(g_zephyr_marker_seen ? "yes" : "no");
	pl011_puts(")\n");
	dual_zephyr_qemu_poweroff();
}

/* ------------------------------------------------------------------ *
 * CPU3 dispatch -- mirrors el2_exc.c's own CPU3 routing (see that file's
 * header for the original): vconsole channel 1 first, mmio_absorb's
 * catch-all second, anything else is a genuine, reportable failure.
 *
 * DELIBERATELY NOT ROUTED THROUGH qemu_guest_sync()/qemu_guest_sync_ops,
 * unlike CPU0's chain below. Two genuine entanglements with this file's
 * per-core routing, not just an oversight:
 *
 *   1. want_mmio_absorb's DRAM exclusion (qemu_guest_sync_ops.h's own
 *      comment) checks the reconstructed IPA against stage2.h's
 *      STAGE2_DRAM_BASE/STAGE2_DRAM_SIZE -- FreeBSD's DRAM gigabyte,
 *      0x40000000-0x80000000, the range this file's own header explicitly
 *      names as where a broken relocation could send CPU3 wandering (the
 *      "stale 0x51xxxxxx-range absolute address" scenario under "CPU3
 *      FAILURE REPORTING"). The unconditional mmio_absorb_fault(frame) call
 *      kept below absorbs a fault anywhere non-DRAM OR in that range alike
 *      (today's real, if surprising, behaviour); switching to
 *      want_mmio_absorb would make exactly that scenario stop being
 *      absorbed and start reaching report_cpu3_fault() instead -- a genuine
 *      behaviour change this refactor must not make, however much more
 *      correct it looks.
 *   2. qemu_guest_sync() unconditionally calls stage2_wx_qemu_try(), which
 *      issues an AT S12E1W against the CALLING core's own banked VTTBR_EL2
 *      -- CPU3's, pointed at stage2_zephyr.c's wholly disjoint table by
 *      design (see stage2_zephyr.h's isolation rationale). This target
 *      links no stage2.o, so stage2_wx_fault resolves weak-absent and the
 *      call is a true no-op today, but wiring it into CPU3's path anyway
 *      would be a needless, unproven interaction with the very isolation
 *      boundary this scenario exists to prove, for a call that is a no-op
 *      here regardless.
 *
 * Both are "entangled with per-core routing" in the sense the conversion
 * brief calls out: left alone rather than guessed at. Only the terminal
 * poweroff (report_cpu3_fault()'s last line) is shared, via the
 * dual_zephyr_qemu_poweroff macro above.
 * ------------------------------------------------------------------ */
static void
handle_cpu3_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);
	unsigned group = (unsigned)(kind >> 2);
	uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

	/* Zephyr's build is tickless and enables no interrupt source (see
	 * zguest_cpu3.h/stage2_zephyr.h), and nothing on this target ever
	 * arms a timer or routes an SPI to affinity 3 -- an IRQ/FIQ here would
	 * be unexpected but is not itself evidence this milestone's own boot
	 * path is broken (matches el2_exc_zephyr_qemu.c's stance for the
	 * standalone target). Do not advance ELR/SPSR for an async exception. */
	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ)
		return;

	if (group == 2u && t == EL2_KIND_SYNC && ec == 0x24u) {
		/* Data abort from CPU3/Zephyr. Try the channel-1 virtual UART0
		 * first -- the one real, meaningfully-emulated device Zephyr's
		 * console needs (vconsole.c, unmodified, same object the real
		 * board's el2_exc.c CPU3 routing calls). */
		if (vconsole_handle_fault(frame, 1)) {
			/* vconsole_handle_fault() has ALREADY advanced frame->elr
			 * and, on a THR write, captured the byte into channel 1's
			 * ring (feeding ZCHAN1_TOTAL_BYTES_ADDR, which CPU0's tick
			 * handler samples) -- but channel 1 has no TX-tee ring to
			 * drain (see vconsole.h: TX-only, no RX, no USB-ACM tee for
			 * channel 1). Echo/marker-match straight off the SAME
			 * emulation this call just performed: re-derive whether
			 * this fault was a THR write by re-reading channel 1's
			 * total_bytes counter and comparing to what this function
			 * last observed -- simplest correct approach given
			 * vconsole.c exposes no dedicated "last byte written"
			 * accessor for a non-interactive channel. */
			static uint32_t last_total;
			uint32_t total = read_u32(ZCHAN1_TOTAL_BYTES_ADDR);

			while (last_total != total) {
				/* Byte-for-byte replay isn't available (the capture
				 * ring is write-only by contract -- see vconsole.h),
				 * but every byte THIS fault captured is the single
				 * one at (total-1) mod buf_size, freshly written and
				 * therefore still safe to read back for echo/marker
				 * purposes even though the ring is not meant to be
				 * drained in general. Only ever one byte behind here
				 * (one THR write == one fault == one byte), so this
				 * loop runs at most once in practice. */
				uint32_t buf_size = read_u32(HVMAP_VCONSOLE_CHAN1_HDR + 20UL); /* word[5] */
				uint32_t buf_base = read_u32(HVMAP_VCONSOLE_CHAN1_HDR + 16UL); /* word[4] */
				uint32_t off = last_total % (buf_size ? buf_size : 1u);
				uint8_t c = *(volatile uint8_t *)((uint64_t)buf_base + off);

				echo_zephyr_byte(c);
				if (zephyr_marker_step(c))
					g_zephyr_marker_seen = 1;
				last_total++;
			}
			return;
		}
		if (mmio_absorb_fault(frame))
			return;
		/* Fell outside every known-safe range -- do NOT silently
		 * swallow (see file header: this is the concrete evidence a
		 * naive relocated boot is broken, not just slow). */
	}

	report_cpu3_fault(frame, kind);
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	uint32_t cpu = smp_cpu_id();

	if (cpu == 3u) {
		handle_cpu3_trap(frame, kind);
		return;
	}

	if (cpu != 0u) {
		/* CPU1/CPU2 park in wfi (dbg_core_enable=0 / the weak
		 * vblk_async_cpu2_run() default) -- a trap reaching here on
		 * either is unexpected on this target. */
		report_fault_cpu0(frame, kind);
	}

	{
		unsigned t = (unsigned)(kind & 3u);

		if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
			if (gic_timer_qemu_irq(frame)) {
				uint64_t ticks = gic_timer_qemu_ticks();

				if ((kind >> 2) == 2u)
					guest_note_preempt();

				if (ticks == SAMPLE_TICK_BASE && !g_have_baseline) {
					g_cpu0_t0 = read_u32(GUEST_LOOP_COUNT_ADDR);
					g_cpu3_t0 = read_u32(ZCHAN1_TOTAL_BYTES_ADDR);
					g_have_baseline = 1;
					pl011_puts("HV: baseline sample (tick ");
					pl011_put_udec((uint32_t)ticks);
					pl011_puts(") cpu0_loop=");
					pl011_put_udec(g_cpu0_t0);
					pl011_puts(" cpu3_console_bytes=");
					pl011_put_udec(g_cpu3_t0);
					pl011_puts("\n");
				} else if (ticks % 20u == 0u) {
					pl011_puts("HV: tick ");
					pl011_put_udec((uint32_t)ticks);
					pl011_puts(" cpu3_bytes=");
					pl011_put_udec(read_u32(ZCHAN1_TOTAL_BYTES_ADDR));
					pl011_puts(" ZG3state=0x");
					pl011_put_hex32(read_u32(ZG3_BC_STATE_ADDR));
					pl011_puts("\n");
				}

				if (ticks >= SAMPLE_TICK_FINAL) {
					uint32_t cpu0_t1 = read_u32(GUEST_LOOP_COUNT_ADDR);
					uint32_t cpu3_t1 = read_u32(ZCHAN1_TOTAL_BYTES_ADDR);
					int cpu0_advanced = g_have_baseline && (cpu0_t1 > g_cpu0_t0);
					int cpu3_advanced = g_have_baseline && (cpu3_t1 > g_cpu3_t0);

					if (!zc_at_line_start)
						pl011_putc('\n');
					pl011_puts("HV: final sample (tick ");
					pl011_put_udec((uint32_t)ticks);
					pl011_puts(") cpu0_loop=");
					pl011_put_udec(cpu0_t1);
					pl011_puts(" cpu3_console_bytes=");
					pl011_put_udec(cpu3_t1);
					pl011_puts("\n");

					/* Diagnostic: dump zguest_cpu3.c's/zload2.c's own
					 * breadcrumbs so a stall (cpu3 not advancing) can
					 * be attributed to a specific boot stage instead
					 * of just "never advanced" -- see their own
					 * headers for the full state-value legend. */
					pl011_puts("HV: diag ZG3(state)=0x");
					pl011_put_hex32(read_u32(ZG3_BC_STATE_ADDR));
					pl011_puts(" ZLD2(elf_valid)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_ELF_VALID_ADDR));
					pl011_puts(" ZLD2(n_seg)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_N_SEG_ADDR));
					pl011_puts(" ZLD2(zbase_lo)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_ZBASE_ADDR));
					pl011_puts(" ZLD2(pa_base_lo)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_PABASE_ADDR));
					pl011_puts(" ZLD2(entry_pa_lo)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_ENTRY_PA_ADDR));
					pl011_puts(" ZLD2(bytes_copied)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_BYTES_ADDR));
					pl011_puts(" ZLD2(fail_reason)=0x");
					pl011_put_hex32(read_u32(ZLD2_BC_FAIL_ADDR));
					pl011_puts("\n");

					pl011_puts("DUAL-ZEPHYR-QEMU-CI: cpu0 before=");
					pl011_put_udec(g_cpu0_t0);
					pl011_puts(" after=");
					pl011_put_udec(cpu0_t1);
					pl011_puts(" cpu3_bytes before=");
					pl011_put_udec(g_cpu3_t0);
					pl011_puts(" after=");
					pl011_put_udec(cpu3_t1);
					pl011_puts(" zephyr_marker_seen=");
					pl011_puts(g_zephyr_marker_seen ? "1" : "0");
					pl011_puts("\n");

					if (cpu0_advanced && cpu3_advanced && g_zephyr_marker_seen) {
						pl011_puts("DUAL-ZEPHYR-QEMU-CI: PASS (CPU0's guest_demo_el1 AND "
						           "CPU3's REAL Zephyr image both advanced concurrently, "
						           "AND the real 'heartbeat 1' banner text was confirmed)\n");
					} else {
						/* NOT "FAIL": same reasoning as
						 * el2_exc_dual2_qemu.c's identical branch. This
						 * firmware cannot tell dual-zephyr-qemu-ci.sh's
						 * pass A (where cpu3 not advancing IS a failure)
						 * from its pass B (where cpu3 NOT advancing, and
						 * the marker NEVER being seen, is the correct,
						 * expected outcome of the stale-image wipe). Report
						 * the bare facts with a word that cannot be
						 * mistaken for a verdict; the caller decides. */
						pl011_puts("DUAL-ZEPHYR-QEMU-CI: OUTCOME cpu0_advanced=");
						pl011_puts(cpu0_advanced ? "1" : "0");
						pl011_puts(" cpu3_advanced=");
						pl011_puts(cpu3_advanced ? "1" : "0");
						pl011_puts(" zephyr_marker_seen=");
						pl011_puts(g_zephyr_marker_seen ? "1" : "0");
						pl011_puts(" (not a verdict -- see "
						           "dual-zephyr-qemu-ci.sh's 'expect' "
						           "parameter)\n");
					}
					dual_zephyr_qemu_poweroff();
				}
			}
			return;
		}

		/* Guest (lower-EL) synchronous exception -- the shared chain handles
		 * both the dynamic W^X promotion (without which the guest cannot
		 * execute its first instruction under STAGE2_WX_DYNAMIC's default XN
		 * mapping -- a no-op on this particular target, see
		 * dual_zephyr_cpu0_ops's comment) and the HVC acknowledgement
		 * guest_demo_el1() periodically needs (see guest.c). ELR_EL2 already
		 * holds the correct post-call return address for an HVC trap (unlike
		 * a data/instr abort) -- see dual_zephyr_cpu0_ops's comment and
		 * el2_exc_dual2_qemu.c's header for the +=4 bug this mirrors the fix
		 * of; no adjustment here either. */
		if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
			uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

			if (qemu_guest_sync(frame, ec, &dual_zephyr_cpu0_ops))
				return;
		}

		report_fault_cpu0(frame, kind);
	}
}
