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

#define EC_HVC64      0x16u
#define EC_SMC64      0x17u
#define EC_DABT_LOWER 0x24u

/* "Linux version" — the substring of init/main.c's start_kernel() banner
 * line (`pr_notice("%s", linux_banner)`, format string "Linux version %s
 * (%s) ..." — the same line `uname -a`/every boot log on earth opens with).
 * Printed via the early/boot-console replay mechanism the moment earlycon
 * registers, so it is expected to be the first (or one of the first) lines
 * this target's console ever shows — see main_linux_qemu.c's banner. */
static const char PASS_MARKER[] = "Linux version";

static void qemu_poweroff(void) __attribute__((noreturn));

static void
qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

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

static void report_fault(struct el2_frame *frame, unsigned long kind) __attribute__((noreturn));

static void
report_fault(struct el2_frame *frame, unsigned long kind)
{
	uint8_t c;

	while (vconsole_tx_tee_getc(&c))
		echo_guest_byte(c);
	if (!at_line_start)
		pl011_putc('\n');

	pl011_puts("LINUX-QEMU-CI: FAIL — unhandled exception kind=0x");
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
	pl011_puts("LINUX-QEMU-CI: guest console bytes emulated before the fault: ");
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

	/* This build arms no timer/vgic; an async exception here is not
	 * expected but is also not evidence of a bug in what this target is
	 * testing — return without touching ELR/SPSR (never advance for an
	 * async exception), same as el2_exc_zephyr_qemu.c. */
	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ)
		return;

	if (group == 2u) {   /* from a lower EL: the guest */
		if (ec == EC_DABT_LOWER) {
			if (vconsole_handle_fault(frame)) {
				drain_guest_console();
				return;
			}
			/* Not UART0 — a real bug (stage-2 hole, or the guest
			 * touching an address this map does not cover). Fall
			 * through to report_fault(). */
		}

		if (ec == EC_SMC64) {
			forward_psci_smc(frame);
			return;
		}

		if (ec == EC_HVC64) {
			/* This DTB advertises method="smc", so Linux's PSCI
			 * client should never issue HVC here — kept for
			 * parity with el2_exc_zephyr_qemu.c/guest.c's documented
			 * optional-hypercall contract, in case anything else
			 * in the guest ever does. */
			frame->elr += 4u;
			return;
		}
	}

	report_fault(frame, kind);
}
