/* SPDX-License-Identifier: BSD-2-Clause */

/* usbacm.c — see usbacm.h for the full design rationale (data flow,
 * threading model, the open MUSB-hardware-ownership risk). Freestanding,
 * no libc: only <stdint.h> plus our own headers.
 */
#include <stdint.h>
#include "usbacm.h"
#include "musb.h"
#include "vconsole.h"
#include "dbgmon.h"      /* dbgmon_bzdbg_post / bzdbg_reply_* -- BZDBG lifeline */

/* Weak: the lifeline is dbgmon's; a build without dbgmon.o (fbsd, gdb, repl)
 * still gets the ACM console, and BZDBG lines are simply dropped. */
#pragma weak dbgmon_bzdbg_post
#pragma weak bzdbg_reply_ready
#pragma weak bzdbg_reply_len
#pragma weak bzdbg_reply_buf
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
#define USBACM_BZDBG_DRAIN_MAX 48  /* reply bytes per poll: musb_putc drops
                                    * on a full ring, so chunk the reply */

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

/* Programmatic break-glass (internal-note): same endpoint as the byte
 * matcher above, but callable from smp.c supervision loop when the
 * EMAC-dark detector fires. Keeps the reset policy in one place
 * (wdt_debug_hold) so both the host-typed bg_seq and the autonomous
 * EMAC-dark wire converge. */
void usbacm_force_breakglass(void)
{
	wdt_debug_hold = 1;
}

/* ── BZDBG: HV dbgmon over the USB console ───────────────────────────────
 * The EMAC-dark lifeline's other half: when the wire is down, the monitor
 * is still fully drivable over the USB console. Host sends
 * `~BZDBG<line>\n` into /dev/ttyACM0; this matcher (same rolling-machine
 * shape as break-glass, plus a line buffer) captures the line and hands it
 * to dbgmon_bzdbg_post(); dbgmon_service drains it with its output
 * CAPTURED (not printed to the EMAC console), and the answer drains back
 * out here, each line prefixed `~BZDBG< ` so the host can tell monitor
 * output apart from guest console traffic. Line-oriented: the guest's own
 * typed input can never form the prefix (it starts with a tilde+capital
 * sequence nobody types), and partial lines time nothing out -- a line
 * simply completes whenever its last byte arrives.
 * 2026-08-27: closes the "EMAC dark, board alive, HV unreadable" gap. */
static const uint8_t bz_pre[] = { '~', 'B', 'Z', 'D', 'B', 'G' };
static uint32_t bz_pos;
static uint8_t  bz_seen[8];          /* matched-prefix bytes, replayed on break */
static char     bz_line[128];
static uint32_t bz_len;

static void usbacm_bzdbg_feed(uint8_t b)
{
	if (bz_pos < (uint32_t)sizeof(bz_pre)) {
		if (b == bz_pre[bz_pos]) {
			bz_seen[bz_pos++] = b;   /* still matching: hold the byte */
			return;
		}
		/* Break: replay what we held (a guest typing '~' must see it),
		 * then re-check THIS byte as a potential prefix start. */
		for (uint32_t k = 0; k < bz_pos; k++)
			vconsole_rx_push(bz_seen[k]);
		bz_pos = 0;
		if (b == bz_pre[0]) {
			bz_seen[0] = b;
			bz_pos = 1;
			return;
		}
		vconsole_rx_push(b);
		return;
	}
	if (bz_pos == (uint32_t)sizeof(bz_pre)) {
		bz_pos++;                    /* separator byte -- consumed silently */
		return;
	}
	/* inside the line body */
	if (b == '\r' || b == '\n') {
		bz_line[bz_len] = '\0';
		if (dbgmon_bzdbg_post)
			dbgmon_bzdbg_post(bz_line);
		bz_len = 0;
		bz_pos = 0;
		return;                      /* line is ours; nothing reaches the guest */
	}
	if (bz_len < sizeof(bz_line) - 1)
		bz_line[bz_len++] = (char)b;
	/* else: drop silently -- DBGMON drops overlong lines too */
}

static uint32_t bz_reply_off;        /* drain cursor into the reply buffer */

static void usbacm_bzdbg_tx_drain(void)
{
	/* Push the pending reply into the ACM TX ring, COURSORNED: musb_putc
	 * drops bytes on a full ring, so at most USBACM_BZDBG_DRAIN_MAX bytes
	 * per poll call and resume from bz_reply_off next tick. Each line is
	 * prefixed `~BZDBG< ` so the host can frame monitor output apart from
	 * the guest console stream sharing this pipe. */
	static const char tag[] = "~BZDBG< ";
	uint32_t budget;

	if (!&bzdbg_reply_ready || !bzdbg_reply_ready)
		return;
	if (bz_reply_off == 0) {
		for (uint32_t k = 0; k < sizeof(tag) - 1; k++)
			musb_putc((int)tag[k]);          /* first line's prefix */
	}
	budget = USBACM_BZDBG_DRAIN_MAX;
	while (bz_reply_off < bzdbg_reply_len && budget--) {
		char ch = bzdbg_reply_buf[bz_reply_off++];
		if (ch == '\n') {
			musb_putc('\r');
			musb_putc('\n');
			for (uint32_t k = 0; k < sizeof(tag) - 1; k++)
				musb_putc((int)tag[k]);
		} else if (ch != '\r') {
			musb_putc((int)ch);
		}
	}
	if (bz_reply_off >= bzdbg_reply_len) {
		musb_putc('\r');
		musb_putc('\n');
		bz_reply_off = 0;
		bzdbg_reply_ready = 0;
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
	 * host paste-flood can't extend a single poll call indefinitely.
	 * Bytes that match a `~BZDBG` prefix are diverted to the monitor
	 * queue and never reach the guest; a byte that BREAKS a match replays
	 * the buffered prefix first, then the byte -- so a guest user typing a
	 * literal tilde sees every keystroke exactly once. */
	n = 0;
	while (n < USBACM_RX_DRAIN_MAX && (c = musb_getc()) >= 0) {
		usbacm_breakglass((uint8_t)c);
		usbacm_bzdbg_feed((uint8_t)c);
		n++;
	}

	/* Guest -> host: drain up to USBACM_TX_DRAIN_MAX tee'd bytes per poll
	 * call into musb.c's TX staging buffer, then push them out now. Capped
	 * so a guest producing console output faster than USB can carry it
	 * doesn't turn one usbacm_poll() call into an unbounded (or merely
	 * very large) loop -- the remainder just waits in the tee ring for the
	 * next call. Any pending BZDBG reply goes FIRST (it is short, and the
	 * host is polling for it). */
	usbacm_bzdbg_tx_drain();
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
