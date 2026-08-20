/* SPDX-License-Identifier: BSD-2-Clause */

/* usbacm.c — see usbacm.h for the full design rationale (data flow,
 * threading model, the open MUSB-hardware-ownership risk). Freestanding,
 * no libc: only <stdint.h> plus our own headers.
 */
#include <stdint.h>
#include "usbacm.h"
#include "musb.h"
#include "vconsole.h"
#include "wdt.h"        /* wdt_debug_hold -- fixed-address macro, see wdt.h */

/* Bounded per-poll drain limits, so a single usbacm_poll() call can never
 * spin unboundedly even if one side is producing faster than the other can
 * be serviced -- matches every other poll loop in this tree (musb.c's own
 * EP1-OUT drain is similarly capped at one already-arrived packet).
 *
 * These caps are defense-in-depth, not the fix for the CPU1-starvation bug
 * found live on hardware (that was the RX/TX-tee rings' head/tail counters
 * reading stale, never-zeroed DRAM -- see vconsole_init()'s fix -- which
 * made vconsole_tx_tee_getc() see a bogus, effectively unbounded backlog).
 * With that fixed, a real per-call backlog is at most a few dozen bytes
 * (one guest console line). musb_putc() is now itself fully non-blocking (it
 * only appends to musb.c's TX staging ring; the actual push to the host is
 * driven one bounded 64-byte packet at a time by musb_poll()), so it can no
 * longer spin -- the old ~200000-iteration musb_tx_flush_now() busy-wait that
 * these caps were guarding against is gone. The caps are kept anyway as
 * cheap defense-in-depth so a single usbacm_poll() call stays O(small)
 * regardless of how much either ring has queued. */
#define USBACM_RX_DRAIN_MAX  32
#define USBACM_TX_DRAIN_MAX  16

/* ── Break-glass reset over the USB console ──────────────────────────────
 * The one wedge with NO remote recovery used to be: EMAC/dbgmon dark (so no
 * `w wdt_debug_hold 1` over the net) + guest hung (so no console reaction)
 * + CPU1 dutifully kicking the HW WDOG forever. Happened live 2026-07-21 —
 * cost a physical power-cycle. The USB-ACM RX path stays alive through all
 * of that (CPU1 services it independently of both EMAC and the guest), so:
 * if the host types the magic sequence below into /dev/ttyACM0, we set
 * wdt_debug_hold=1 -> both watchdog-pet paths stop (see wdt.c) -> the HW
 * WDOG fires within ~16 s -> U-Boot -> chimpd/reliable_load reloads. The
 * sequence is deliberately unlikely to appear in interactive console
 * traffic (nobody types 0x00 bytes). Matcher is a plain rolling state
 * machine over the RX byte stream, byte-at-a-time, O(1) per byte. */
static const uint8_t bg_seq[] = { 0x00, '~', 'B', 'Z', 'R', 'S', 'T', 0x00 };
static uint32_t bg_pos;

static void usbacm_breakglass(uint8_t b)
{
	if (b == bg_seq[bg_pos]) {
		if (++bg_pos == (uint32_t)sizeof(bg_seq)) {
			wdt_debug_hold = 1;   /* WDOG fires in <=16 s */
			bg_pos = 0;
		}
	} else {
		/* restart; also handle the first byte re-matching */
		bg_pos = (b == bg_seq[0]) ? 1 : 0;
	}
}

void
usbacm_init(void)
{
	musb_init();
}

void
usbacm_poll(void)
{
	int c;
	uint8_t b;
	int n;

	/* Services EP0 SETUP/enumeration and drains one EP1-OUT packet (if any)
	 * into musb.c's own RX ring, unconditionally -- must run every call
	 * regardless of which direction has traffic (see musb.h's contract),
	 * and regardless of readiness (this is what MAKES the device ready). */
	musb_poll();

	/* Cheap early-out: until the host has finished enumeration (SET_
	 * CONFIGURATION seen), there is no ttyACM on the other end and nothing
	 * useful to drain in either direction -- skip both ring-pump loops below
	 * so a not-yet-enumerated (or unplugged) gadget costs this poll call
	 * one musb_poll() and nothing else. */
	if (!musb_ready())
		return;

	/* Host -> guest: forward up to USBACM_RX_DRAIN_MAX bytes musb.c's RX
	 * ring already has into the guest's virtual UART0 RX ring. musb_getc()
	 * itself also bounds this (it drains exactly what's already buffered,
	 * never blocks) -- the explicit cap here is belt-and-suspenders so a
	 * host paste-flood can't extend a single poll call indefinitely. */
	n = 0;
	while (n < USBACM_RX_DRAIN_MAX && (c = musb_getc()) >= 0) {
		vconsole_rx_push((uint8_t)c);
		usbacm_breakglass((uint8_t)c);
		n++;
	}

	/* Guest -> host: drain up to USBACM_TX_DRAIN_MAX tee'd bytes per poll
	 * call into musb.c's TX staging buffer, then push them out now. Capped
	 * so a guest producing console output faster than USB can carry it
	 * doesn't turn one usbacm_poll() call into an unbounded (or merely
	 * very large) loop -- the remainder just waits in the tee ring for the
	 * next call. */
	n = 0;
	while (n < USBACM_TX_DRAIN_MAX && vconsole_tx_tee_getc(&b)) {
		musb_putc((int)b);
		n++;
	}
	musb_flush();
}

int
usbacm_ready(void)
{
	return musb_ready();
}
