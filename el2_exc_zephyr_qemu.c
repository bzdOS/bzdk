/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_zephyr_qemu.c — EL2 trap dispatch for the Zephyr-guest-on-QEMU
 * target (main_zephyr_qemu.c).
 *
 * Like el2_exc_qemu.c, this is a NEW minimal el2_trap() rather than a reuse
 * of the board's el2_exc.c (which pulls in dbgmon, hwbp/onebp/single-step,
 * vblk_emmc, musb/usbacm, smp, the PSCI intercept and the A64 watchdog —
 * none of which exist or apply here). It differs from el2_exc_qemu.c in
 * exactly one respect, which is the entire point of this target: the guest's
 * console is NOT a real device it can write to directly. It is the
 * trap-and-emulated Allwinner UART0 page, so the dominant trap on this
 * target is a stage-2 data abort from Zephyr's ns16550 driver, and this file
 * hands those to vconsole.c — the same object, unmodified, that serves the
 * FreeBSD guest on real hardware.
 *
 * Three jobs, in order of frequency:
 *
 *   1. UART0 stage-2 data abort -> vconsole_handle_fault(). Then drain
 *      vconsole's TX tee ring (the same ring usbacm.c drains on CPU1 on the
 *      real board, see vconsole.h's "INTERACTIVE CONSOLE" section) and echo
 *      each byte to QEMU's PL011, line-prefixed "[guest] ". Using the tee
 *      rather than peeking at the capture ring keeps this file out of
 *      vconsole's internals and means the bytes are consumed exactly once.
 *
 *   2. PASS detection. Because every guest console byte passes through here,
 *      the hypervisor can recognise its own success condition and shut QEMU
 *      down deterministically — no output-scraping race in the CI script,
 *      the same design as el2_exc_qemu.c's tick-count PASS. The marker is
 *      the guest's second heartbeat (see PASS_MARKER below for why the
 *      second and not the banner).
 *
 *   3. Anything else — a guest fault that is not UART0, or an EL2-own
 *      synchronous exception — is a real failure on this boot path: print
 *      ESR/ELR/FAR with a greppable FAIL marker and power off. Not
 *      auto-recovered: there is no watchdog or supervisor on this target and
 *      resuming into a possibly-corrupt guest would only produce a confusing
 *      log.
 */
#include <stdint.h>
#include "exceptions.h"
#include "vconsole.h"
#include "pl011_qemu.h"

/* ESR_EL2.EC values we care about. Spelled out locally (same as
 * el2_exc_qemu.c does) rather than pulled from a shared header — these two
 * are the whole vocabulary of this file. */
#define EC_HVC64      0x16u
#define EC_DABT_LOWER 0x24u

/* The string whose arrival means "this worked".
 *
 * "heartbeat 1" and not the "*** Booting Zephyr OS ***" banner, and not
 * "heartbeat 0": the banner only proves the ns16550 driver initialised and
 * printk() reached THR. "heartbeat 0" additionally proves the kernel
 * finished initialising and called the app's main(). Waiting one more beat
 * proves the guest SURVIVES and makes forward progress after main() — i.e.
 * that it came back out of k_busy_wait() and round the loop, rather than
 * printing once and wedging.
 *
 * Cost of that extra beat: two k_busy_wait(1000000) calls at the guest
 * board's CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC=400, which measures ~1.5 s of
 * wall clock under QEMU TCG for the whole run. If that constant is ever
 * re-calibrated upward against the real ~1.2 GHz Cortex-A53 (the board's
 * Kconfig.defconfig says 400 is a deliberately conservative guess and asks
 * for exactly that), this run gets proportionally slower — the 90 s timeout
 * in zephyr-qemu-ci.sh leaves a ~60x margin, so there is room, but that is
 * the coupling to know about.
 *
 * Matched against the raw byte stream (before the "[guest] " prefixing
 * below), so the prefix can change freely without breaking the gate. */
static const char PASS_MARKER[] = "heartbeat 1";

static void qemu_poweroff(void) __attribute__((noreturn));

/* PSCI SYSTEM_OFF via SMC — QEMU's virt machine answers PSCI itself for a
 * bare-metal EL2 payload booted with -kernel and no -bios, and exits 0. See
 * el2_exc_qemu.c's copy of this for the longer note. */
static void
qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

/* ------------------------------------------------------------------ *
 * Guest console echo + marker match.
 * ------------------------------------------------------------------ */

static int at_line_start = 1;
static unsigned marker_pos;      /* how much of PASS_MARKER has matched */
static uint32_t guest_bytes;     /* diagnostic: total bytes echoed */

static void
echo_guest_byte(uint8_t c)
{
	guest_bytes++;

	/* Line-prefix so guest output is unmistakably distinguishable from
	 * the hypervisor's own lines in one shared serial stream. '\r' is
	 * swallowed: the guest app prints CRLF (it is talking to what it
	 * believes is a real serial terminal) and QEMU's -nographic stdout
	 * does not want the CR. */
	if (c == '\r')
		return;
	if (c == '\n') {
		pl011_putc('\n');
		at_line_start = 1;
		return;
	}
	if (at_line_start) {
		pl011_puts("[guest] ");
		at_line_start = 0;
	}
	pl011_putc((char)c);
}

/* Streaming substring match. PASS_MARKER has no repeated prefix, so a plain
 * "mismatch resets to 0, unless this byte restarts the match" step is
 * sufficient — no KMP failure table needed. Returns 1 on the byte that
 * completes the marker. */
static int
marker_step(uint8_t c)
{
	if (c == (uint8_t)PASS_MARKER[marker_pos]) {
		marker_pos++;
		if (PASS_MARKER[marker_pos] == '\0') {
			marker_pos = 0;
			return 1;
		}
		return 0;
	}
	marker_pos = (c == (uint8_t)PASS_MARKER[0]) ? 1u : 0u;
	return 0;
}

static void
drain_guest_console(void)
{
	uint8_t c;

	while (vconsole_tx_tee_getc(&c)) {
		echo_guest_byte(c);
		if (marker_step(c)) {
			pl011_puts("\nZEPHYR-QEMU-CI: PASS (real Zephyr image loaded by kload.c,"
			           " running at EL1 under stage-2, console via vconsole 16550"
			           " trap-emulation)\n");
			pl011_puts("ZEPHYR-QEMU-CI: guest console bytes emulated: ");
			pl011_put_udec(guest_bytes);
			pl011_puts("\n");
			qemu_poweroff();
		}
	}
}

static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	/* Flush whatever the guest had already printed before it died — on a
	 * guest crash that tail is the most informative thing in the log. */
	uint8_t c;

	while (vconsole_tx_tee_getc(&c))
		echo_guest_byte(c);
	if (!at_line_start)
		pl011_putc('\n');

	pl011_puts("ZEPHYR-QEMU-CI: FAIL — unhandled exception kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" EC=0x");
	pl011_put_hex32(((uint32_t)(frame->esr >> 26)) & 0x3fu);
	pl011_puts(" ESR=0x");
	pl011_put_hex32((uint32_t)frame->esr);
	pl011_puts(" ELR=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts(" FAR=0x");
	pl011_put_hex64(frame->far);
	pl011_puts("\n");
	pl011_puts("ZEPHYR-QEMU-CI: guest console bytes emulated before the fault: ");
	pl011_put_udec(guest_bytes);
	pl011_puts("\n");
	qemu_poweroff();
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);
	unsigned group = (unsigned)(kind >> 2);
	uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

	/* Asynchronous exceptions. This build arms no timer and the guest
	 * enables no interrupt source, so an IRQ/FIQ here is not expected —
	 * but it is also not evidence of a bug in what this target is testing,
	 * and treating a spurious one as fatal would make the gate flaky.
	 * Return without touching ELR/SPSR (never advance for an async
	 * exception). */
	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ)
		return;

	if (group == 2u) {   /* from a lower EL: the guest */
		if (ec == EC_DABT_LOWER) {
			if (vconsole_handle_fault(frame, 0)) {
				drain_guest_console();
				return;
			}
			/* A guest data abort somewhere other than UART0 — a
			 * real bug (a stage-2 hole, or the guest touching an
			 * address this map does not cover). Fall through. */
		}

		if (ec == EC_HVC64) {
			/* Zephyr does not issue HVC on this board, but the
			 * generic guest.c contract documents one, so honour it
			 * rather than treating it as fatal. */
			frame->elr += 4u;
			return;
		}
	}

	report_fault(frame, kind);
}
