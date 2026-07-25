/* SPDX-License-Identifier: BSD-2-Clause */

/* main.c — bzdOS microkernel Stage-1 firmware main.
 *
 * Brings up the MUSB CDC-ACM gadget console (musb.h/musb.c) and runs a
 * pure polling console loop: continuous musb_poll() so USB enumeration
 * never starves (see PROJECT.md — this is the fix for the FreeBSD gadget
 * console's "only polls inside printf" bug), a throttled heartbeat once
 * the host has enumerated us, and byte echo with a 'q'/'Q' escape back to
 * U-Boot.
 *
 * Freestanding: no libc, no delays/timers/interrupts, only <stdint.h> and
 * "musb.h". Returning 0 from main() unwinds through start.S back into
 * U-Boot's prompt.
 */
#include <stdint.h>
#include "musb.h"
#include "wdt.h"

/* Loop-counter throttle for the heartbeat — NOT wall-clock time (no timer
 * available). Tune by trial against observed CPU/poll speed; this only
 * needs to be "occasionally", not precise. */
#define HEARTBEAT_PERIOD 3000000u

/* ------------------------------------------------------------------ *
 * DRAM breadcrumb (offline diagnostics — see PROJECT.md / diag-bc2.py)
 *
 * The MUSB console TX is the very thing we are debugging, so we cannot
 * rely on it to report progress. Instead we scribble a progress record
 * into a fixed, otherwise-unused DRAM word window at 0x50000000 (well
 * clear of our own 0x42000000..0x42800000 image and of U-Boot's
 * relocated-to-top-of-RAM copy — this is the same scratch address the
 * FreeBSD diag breadcrumb used). After a run, catch U-Boot and
 *     md.l 0x50000000 8
 * to see EXACTLY how far the microkernel got and the USB state it left
 * behind. Layout (matches read_breadcrumb() in boot-diag-breadcrumb.py:
 * word0 magic, word1 stage, word2 counter):
 *   [0] 0xB2D0C0DE  magic (proves the microkernel wrote this, vs. random DRAM)
 *   [1] stage       max checkpoint reached (see BC_STAGE_* below)
 *   [2] poll_count  musb_poll() call count (proves the loop is spinning)
 *   [3] usb_state   0=RESET 1=WAIT_SETUP 2=CONNECTED
 *   [4] usb_ready   1 once SET_CONFIGURATION seen (ttyACM up)
 *   [5] intr_seen   OR of every INTUSB value observed (0x01 reset / 0x10
 *                   connect / 0x20 disconnect) — did the host ever reset us?
 *   [6] setup_count EP0 SETUP packets handled
 *   [7] last_csr0   last EP0 CSR0 value seen in the poll loop
 * ------------------------------------------------------------------ */
/* Every BC write is cleaned to the Point of Coherency (dc civac) + dsb:
 * we run with U-Boot's MMU/D-cache ON, and a plain store sits in cache and
 * DIES with the WDT reset — the post-reset `md.l` then shows stale DRAM.
 * (Verified: zeroed words resurrected to years-old values after reset.) */
static inline void bc_write(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(0x50000000UL + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}
#define BC(i, v) bc_write((i), (uint32_t)(v))

/* NEW magic (old 0xB2D0C0DE belonged to the FreeBSD diag kernel and still
 * haunts DRAM) — lets the harness tell a FRESH microkernel record from the
 * stale one left behind when the microkernel never executed at all. */
#define BC_MAGIC          0xB2D0CAFEu
#define BC_STAGE_ENTER    1u   /* entered main(), before wdt_arm */
#define BC_STAGE_WDT      2u   /* watchdog armed */
#define BC_STAGE_INITED   4u   /* musb_init() returned (musb.c writes 3 inside) */
#define BC_STAGE_BANNER   7u   /* first musb_puts()/flush() done */
#define BC_STAGE_LOOP     8u   /* poll loop entered */
#define BC_STAGE_READY    9u   /* host configured (usb_ready true) */
#define BC_STAGE_ALIVE   10u   /* first BZDOS-MK-ALIVE emitted */

/* If the host never configures us within this many poll-loop iterations we
 * give up, STOP petting the watchdog, and spin — letting the ~16 s WDT fire a
 * clean system reset back to a FRESH U-Boot (whose own USB gadget re-inits and
 * enumerates normally, so the port reappears and the breadcrumb can be read).
 * We deliberately do NOT return to U-Boot here and do NOT disarm the WDT: a
 * bare return would drop into a U-Boot whose USB console we may have disturbed,
 * which we cannot `reset` out of (guardrail). Only cold-recoverable state is
 * ever reached via the proven WDT path. Tune against observed poll speed. */
#define ENUM_GIVEUP 60000000u

static void put_udec(uint32_t v)
{
	char buf[10];
	int i = 0;

	if (v == 0) {
		musb_putc('0');
		return;
	}
	while (v > 0 && i < (int)sizeof(buf)) {
		buf[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i > 0)
		musb_putc(buf[--i]);
}

int main(void)
{
	int was_ready = 0;
	uint32_t loop_counter = 0;
	uint32_t heartbeat_n = 0;
	uint32_t enum_wait = 0;

	/* Breadcrumb: prove we ran and lay down the magic before anything risky. */
	BC(0, BC_MAGIC);
	BC(1, BC_STAGE_ENTER);

	/* Arm the hardware watchdog BEFORE any risky bring-up. From here on, if
	 * musb_init()/musb_poll() (or later EMAC code) hangs or faults, the board
	 * self-resets in ~16 s back to U-Boot '=>' — the session can never brick.
	 * The poll loop pets it; a clean 'q' return disarms it first. */
	wdt_arm();
	BC(1, BC_STAGE_WDT);

	musb_init();
	BC(1, BC_STAGE_INITED);
	musb_puts("MK: musb_init done (wdt armed)\r\n");
	musb_flush();
	BC(1, BC_STAGE_BANNER);

	for (;;) {
		wdt_pet();               /* healthy loop keeps the reset at bay */
		int ready = musb_poll();

		if (loop_counter == 0 && enum_wait == 0)
			BC(1, BC_STAGE_LOOP);

		if (ready && !was_ready) {
			was_ready = 1;
			BC(1, BC_STAGE_READY);
			/* Early banner: fires exactly once, right as musb_ready()
			 * first turns true. */
			musb_puts("MK: host configured (ttyACM up)\r\n");
			musb_flush();
		}

		/* Not yet enumerated? Bound the wait so a stuck enumeration ends in a
		 * clean WDT reset to a fresh U-Boot (readable breadcrumb) instead of
		 * an unbounded pet-forever spin we can never recover from. */
		if (!was_ready) {
			if (++enum_wait >= ENUM_GIVEUP) {
				BC(1, 99u);      /* 99 = gave up waiting for enumeration */
				for (;;) {       /* stop petting → WDT resets us in ~16 s */
					musb_poll(); /* keep servicing EP0 in case host is late */
					/* NB: intentionally NO wdt_pet() here. */
				}
			}
		}

		if (was_ready) {
			if (++loop_counter >= HEARTBEAT_PERIOD) {
				loop_counter = 0;
				BC(1, BC_STAGE_ALIVE);
				musb_puts("BZDOS-MK-ALIVE ");
				put_udec(heartbeat_n++);
				musb_puts("\r\n");
				musb_flush();
			}
		}

		/* Echo every loop, independent of the heartbeat throttle above;
		 * musb_getc() is non-blocking (-1 when nothing received), so
		 * this costs nothing when idle and never delays musb_poll(). */
		int c = musb_getc();
		if (c >= 0) {
			musb_putc(c);
			if (c == 'q' || c == 'Q') {
				musb_puts("MK: returning to U-Boot\r\n");
				musb_flush();
				wdt_disarm();   /* don't reset U-Boot after we hand back */
				return 0;
			}
		}
	}
}
