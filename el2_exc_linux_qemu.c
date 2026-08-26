/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_linux_qemu.c — EL2 trap dispatch for the Linux-guest-on-QEMU
 * target (main_linux_qemu.c).
 *
 * Structurally the same as el2_exc_zephyr_qemu.c (guest console byte ->
 * vconsole.c -> tee ring -> PL011, PASS-marker matched against the raw
 * stream, anything else is a real failure), with one addition that target
 * never needed:
 *
 *   PSCI SMC passthrough. This DTB (linux-guest/bpi_m64_hv-linux.dts)
 *   advertises `psci { method = "smc"; }` — a normal, expected thing for a
 *   guest OS devicetree to say, and Linux's own PSCI client code
 *   (drivers/firmware/psci/psci.c) WILL probe it (PSCI_VERSION) during
 *   early boot regardless of whether this particular milestone cares about
 *   CPU hotplug/suspend/reboot. Architecturally, absent an implemented EL3,
 *   an SMC instruction executed at EL1 traps unconditionally to EL2 — there
 *   is nowhere else for it to go. QEMU's `virt` machine answers PSCI calls
 *   made AT EL2 by intercepting the `smc` instruction itself (no real EL3
 *   firmware is modelled; every other QEMU CI target in this tree already
 *   relies on exactly this for its own `qemu_poweroff()`). So the correct
 *   and general thing to do with a guest's PSCI SMC is not to answer it
 *   ourselves, but to re-issue the SAME smc instruction while WE are at
 *   EL2 (i.e., let QEMU's firmware emulation answer it as it would for any
 *   top-EL caller) and copy the result back into the guest's saved x0-x3
 *   before returning past the instruction. This is the standard "PSCI
 *   passthrough" shape any real Type-1 hypervisor without a trusted
 *   firmware layer of its own needs — see forward_psci_smc() below.
 *
 * Everything else — UART0 trap -> vconsole, PASS/FAIL marker detection,
 * fault reporting — is line-for-line the same shape as
 * el2_exc_zephyr_qemu.c; see that file for the longer rationale, not
 * repeated here.
 */
#include <stdint.h>
#include "exceptions.h"
#include "vconsole.h"
#include "pl011_qemu.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* "Linux version" — the substring of init/main.c's start_kernel() banner
 * line (`pr_notice("%s", linux_banner)`, format string "Linux version %s
 * (%s) ..." — the same line `uname -a`/every boot log on earth opens with).
 * Printed via the early/boot-console replay mechanism the moment earlycon
 * registers, so it is expected to be the first (or one of the first) lines
 * this target's console ever shows — see main_linux_qemu.c's banner. */
static const char PASS_MARKER[] = "Linux version";

/* qemu_poweroff()/report_fault() used to be defined here, verbatim-duplicated
 * across every QEMU CI handler; qemu_poweroff() now lives in
 * el2_exc_qemu_common.c as qemu_psci_poweroff() (same SYSTEM_OFF SMC, same
 * fallback wfi loop for the case QEMU doesn't terminate). This target's own
 * report_fault() is kept below — see its definition for why (it prints extra
 * guest-console-byte-count lines the shared qemu_report_fault() does not know
 * about) — but now delegates the actual FAULT line + terminal behaviour to
 * qemu_report_fault(). */
#define qemu_poweroff qemu_psci_poweroff

/* ------------------------------------------------------------------ *
 * PSCI SMC passthrough — see file header.
 * ------------------------------------------------------------------ */
static void
forward_psci_smc(struct el2_frame *frame)
{
	register uint64_t x0 __asm__("x0") = frame->x[0];
	register uint64_t x1 __asm__("x1") = frame->x[1];
	register uint64_t x2 __asm__("x2") = frame->x[2];
	register uint64_t x3 __asm__("x3") = frame->x[3];

	__asm__ volatile("smc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	                 :
	                 : "memory");

	frame->x[0] = x0;
	frame->x[1] = x1;
	frame->x[2] = x2;
	frame->x[3] = x3;
	frame->elr += 4;
}

/* ------------------------------------------------------------------ *
 * Guest console echo + marker match — identical shape to
 * el2_exc_zephyr_qemu.c's copy.
 * ------------------------------------------------------------------ */

static int at_line_start = 1;
static unsigned marker_pos;
static uint32_t guest_bytes;

static void
echo_guest_byte(uint8_t c)
{
	guest_bytes++;

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
			pl011_puts("\nLINUX-QEMU-CI: PASS (mainline Linux/arm64 Image loaded by "
			           "main_linux_qemu.c's own Image-header handoff, running at EL1 "
			           "under stage-2, console via vconsole 16550 trap-emulation)\n");
			pl011_puts("LINUX-QEMU-CI: guest console bytes emulated: ");
			pl011_put_udec(guest_bytes);
			pl011_puts("\n");
			qemu_poweroff();
		}
	}
}

/* report_fault() is kept LOCAL rather than folded entirely into
 * qemu_report_fault(): this target's original fault report printed two extra
 * things qemu_report_fault() knows nothing about — the guest console bytes
 * drained/echoed one last time, and a "guest console bytes emulated before
 * the fault" counter line — so those are still produced here, then the
 * standard "<marker>: FAULT kind=.. ESR=.. ELR=.. FAR=.." line and the
 * terminal behaviour (this target powers off, like the original did) come
 * from the shared qemu_report_fault(). Per el2_exc_qemu_common.h's contract,
 * the extras are printed BEFORE calling it, so on a real FAIL run the console
 * drain/byte-count line now appears just above the FAULT line rather than
 * just below it as before — a cosmetic reordering of a diagnostic-only path
 * that no CI script parses (linux-qemu-ci.sh greps only for the PASS marker;
 * a run that never prints it is caught by the wrapping timeout instead).
 *
 * The EC field the original FAIL line carried is KEPT, printed here among the
 * extras rather than dropped. Yes, ESR bits[31:26] already encode it — but the
 * whole value of this line is being readable at a glance by whoever is staring
 * at a broken CI log, and "EC=0x24" is the single most diagnostic field in it.
 * Making a human decode it out of a hex ESR every time is a real loss for a
 * refactor that is supposed to change nothing. */
static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	uint8_t c;

	while (vconsole_tx_tee_getc(&c))
		echo_guest_byte(c);
	if (!at_line_start)
		pl011_putc('\n');

	pl011_puts("LINUX-QEMU-CI: guest console bytes emulated before the fault: ");
	pl011_put_udec(guest_bytes);
	pl011_puts("\n");

	pl011_puts("LINUX-QEMU-CI: EC=0x");
	pl011_put_hex32(((uint32_t)(frame->esr >> 26)) & 0x3fu);
	pl011_puts("\n");

	qemu_report_fault(frame, kind, "LINUX-QEMU-CI", /*poweroff=*/1);
}

/* This target's own scenario config for the shared guest-sync chain — see
 * el2_exc_qemu_common.h for the chain's fixed order (console, then dynamic
 * W^X, then HVC, then SMC, then MMIO-absorb). That order differs textually
 * from this file's old hand-written chain (which checked SMC before W^X and
 * HVC after), but is NOT a behaviour change: EC_SMC64/EC_HVC64 are disjoint
 * from the EC_DABT_LOWER/EC_IABT_LOWER foursome stage2_wx_qemu_try() acts on
 * (see stage2_wx_qemu.h), so no two of these branches ever compete for the
 * same fault — only their relative ORDER in the source changed, not which
 * one claims a given ec. The one EC that two branches both look at,
 * EC_DABT_LOWER (console vs. W^X), is tried console-first in the shared
 * chain exactly as it was here, so that fallthrough (console misses ->
 * W^X gets a shot, e.g. a real stage-2 hole or the guest touching an address
 * this map doesn't cover) is unchanged too.
 *
 * want_console + console_chan(0) + after_console: this target's UART0 trap ->
 * vconsole.c path. after_console runs drain_guest_console() only when
 * vconsole actually claimed the fault, same as the original's inline
 * `if (vconsole_handle_fault(...)) { drain_guest_console(); return; }`.
 *
 * want_hvc_ack + hvc_advance_elr: this DTB advertises method="smc", so
 * Linux's PSCI client should never issue HVC here — kept for parity with
 * el2_exc_zephyr_qemu.c/guest.c's documented optional-hypercall contract, in
 * case anything else in the guest ever does. hvc_advance_elr=1 reproduces
 * this file's original `frame->elr += 4u;` verbatim.
 *
 * on_smc = forward_psci_smc: see this file's header for the PSCI passthrough
 * rationale. forward_psci_smc() already advances frame->elr itself, so no
 * extra ELR handling is needed here (unlike the HVC ack above).
 *
 * No claim_first, no want_mmio_absorb: this target has no mechanism under
 * test ahead of the shared chain, and its DTB describes no absent devices to
 * absorb. Dynamic W^X promotion is unconditional in the shared chain (see
 * el2_exc_qemu_common.h) and needs no flag here.
 *
 * CORRECTED 2026-08-26: an earlier draft of this comment said the promotion
 * "used to be this file's own stage2_wx_qemu_try() call". It never was. This
 * file — like all nine QEMU handlers — had NO dynamic W^X hook at all before
 * the commit that introduced stage2_wx_qemu.h, and that missing hook IS the
 * bug which left the whole board-free gate red. Saying otherwise would send
 * the next reader hunting for code that never existed. */
static const struct qemu_guest_sync_ops linux_qemu_ops = {
	.want_console    = 1u,
	.console_chan    = 0u,
	.after_console   = drain_guest_console,
	.want_hvc_ack    = 1u,
	.hvc_advance_elr = 1u,
	.on_smc          = forward_psci_smc,
};

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);
	unsigned group = (unsigned)(kind >> 2);

	/* This build arms no timer/vgic; an async exception here is not
	 * expected but is also not evidence of a bug in what this target is
	 * testing — return without touching ELR/SPSR (never advance for an
	 * async exception), same as el2_exc_zephyr_qemu.c. */
	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ)
		return;

	if (group == 2u) {   /* from a lower EL: the guest */
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		/* The shared chain handles the UART0 console trap, dynamic W^X
		 * promotion, the HVC ack, and the PSCI SMC passthrough this
		 * file's header documents. See el2_exc_qemu_common.h for what
		 * is shared and why. */
		if (qemu_guest_sync(frame, ec, &linux_qemu_ops))
			return;
	}

	report_fault(frame, kind);
}
