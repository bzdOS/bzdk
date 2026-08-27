/* SPDX-License-Identifier: BSD-2-Clause */

/* test_gdbstub_wdt_kick.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the watchdog-starvation fix in gdbstub.c's blocking RSP receive
 * loops (gdb_recv() / gdb_recv_body()) -- root cause #1 of the four GDB-stub
 * failures in this task's brief ("the RSP channel goes dark in long
 * sessions, only a board reset recovers it"), and the SAME mechanism behind
 * the separately-reported "pause-gate + an armed Z0 breakpoint kills the
 * board ~15s after release" (both are this one starved wait, reached by
 * different front doors).
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "gdbstub.c"` WITH STUBS
 * ============================================================================
 * Same house rule as test_gdbstub_resolve.c / test_gdbstub_hwop.c (see their
 * header comments): gdbstub.c is not includable on plain x86_64 gcc (raw
 * ARMv8 asm elsewhere in the file), so this mirrors ONLY the two blocking
 * receive loops verbatim in control flow, with exact gdbstub.c line-number
 * citations, current as of this fix:
 *
 *   - gdb_recv()      -- gdbstub.c:409-422 (scans the wire for the opening
 *     '$', spinning on gdb_getc()).
 *   - gdb_recv_body()'s outer byte loop and checksum-digit wait --
 *     gdbstub.c:333-395 (reads the packet body up to '#', then two hex
 *     checksum digits). The escape ('}') and run-length ('*') sub-waits
 *     (gdbstub.c:350-368) are NOT mirrored here: they are structurally
 *     identical `do { x = gdb_getc(); if (x<0) wdt_debug_kick(); } while
 *     (x<0);` retry loops, already covered in spirit by the checksum-digit
 *     wait mirrored below, and reaching them needs a '}'/'*' byte in the
 *     packet body first, which would only add test-harness complexity
 *     without exercising a different kick-cadence code shape.
 *
 * THE BUG THIS FILE IS AIMED AT: before this fix, gdb_getc() returning -1
 * ("no byte yet") inside either loop just did a bare `continue` -- no kick.
 * The ONLY wdt_debug_kick() call in the whole command-loop path
 * (command_loop_ex(), gdbstub.c:946-967) ran ONCE, immediately BEFORE
 * calling gdb_recv()/gdb_recv_body(), which then block with NO further kick
 * for as long as the wire stays quiet. wdt.c's wdt_debug_kick() restarts a
 * hardware timer capped at 16s (the hardware maximum interval, wdt.c's
 * WDOG_MODE_16S_EN) -- so any gap longer than ~16s between two complete RSP
 * packets (a human reading code, thinking, or just not having typed the next
 * command yet -- completely ordinary interactive use) silently reset the
 * board mid-session. The fix adds a kick on every failed gdb_getc() poll
 * inside the wait itself, restoring the "kicked every poll pass,
 * unconditionally" cadence the original dedicated CPU1 loop (smp.c)
 * guaranteed before gdbstub.c's blocking design regressed it.
 *
 * Each test below simulates an arbitrarily long "nothing on the wire yet"
 * stretch (thousands of -1 polls) before a real byte finally arrives, and
 * asserts the kick counter grew in step with the wait -- not just once. A
 * build of these mirrors against the PRE-FIX shape (kick only before the
 * loop, bare `continue` inside it) would fail every assertion below with a
 * kick count of 0 or 1 regardless of wait length, which is exactly the bug.
 *
 * Build: gcc -o test_gdbstub_wdt_kick test_gdbstub_wdt_kick.c &&
 *        ./test_gdbstub_wdt_kick (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Mock transport + watchdog -- stand-ins for main_gdb.c's gdb_getc()/
 * gdb_putc()/gdb_flush() and wdt.c's wdt_debug_kick(). A scripted byte
 * queue: the test pre-loads exactly the bytes gdb_recv()/gdb_recv_body()
 * will consume, interspersed with a controllable number of "-1, nothing yet"
 * polls before each real byte -- exactly what a quiet wire looks like to
 * gdb_getc() while a human is thinking.
 * ------------------------------------------------------------------ */
#define MAX_SCRIPT 256

struct script_ent {
	int      empty_polls;   /* how many -1s to return before the byte */
	int      byte;          /* the real byte delivered after them     */
};

static struct script_ent g_script[MAX_SCRIPT];
static int g_script_len;
static int g_script_pos;
static int g_empty_left;

static int g_kick_count;
static int g_putc_count;    /* not asserted on below, just kept honest */
static int g_primed;        /* has g_empty_left been loaded from script[0]? */

static void reset_state(void)
{
	memset(g_script, 0, sizeof(g_script));
	g_script_len  = 0;
	g_script_pos  = 0;
	g_empty_left  = 0;
	g_kick_count  = 0;
	g_putc_count  = 0;
	g_primed      = 0;
}

static void script_push(int empty_polls, int byte)
{
	assert(g_script_len < MAX_SCRIPT);
	g_script[g_script_len].empty_polls = empty_polls;
	g_script[g_script_len].byte        = byte;
	g_script_len++;
}

/* Mirrors main_gdb.c's `int gdb_getc(void) { emac_poll(); return
 * emac_getc(); }` contract from the CALLER's point of view: returns -1 when
 * nothing is buffered, else the next byte. The real emac_poll() pumps the
 * RX ring on every call regardless of outcome; this mock doesn't need to
 * simulate that plumbing, only its -1-vs-byte return shape. */
static int gdb_getc(void)
{
	if (g_script_pos >= g_script_len)
		return -1;   /* script exhausted: treat as still-quiet wire */

	if (!g_primed) {
		/* Load the CURRENT entry's own empty-poll count before its byte
		 * is ever delivered -- this must happen once, lazily, on the
		 * very first call, since reset_state() runs before script_push()
		 * populates g_script[0]. */
		g_empty_left = g_script[g_script_pos].empty_polls;
		g_primed = 1;
	}

	if (g_empty_left > 0) {
		g_empty_left--;
		return -1;
	}
	{
		int b = g_script[g_script_pos].byte;
		g_script_pos++;
		if (g_script_pos < g_script_len)
			g_empty_left = g_script[g_script_pos].empty_polls;
		return b;
	}
}

static void gdb_putc(int c) { (void)c; g_putc_count++; }
static void gdb_flush(void) { }
static void wdt_debug_kick(void) { g_kick_count++; }

static int unhex(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* ------------------------------------------------------------------ *
 * gdb_recv() -- mirrors gdbstub.c:409-422 (FIXED shape: kicks on c<0).
 * ------------------------------------------------------------------ */
static int gdb_recv_body(void);   /* forward decl, defined below */

static int gdb_recv(void)
{
	int c;
	for (;;) {
		c = gdb_getc();
		if (c < 0) {
			wdt_debug_kick();   /* mirrors gdbstub.c:344 (recv) analogue */
			continue;
		}
		if (c == '$')
			return gdb_recv_body();
	}
}

/* ------------------------------------------------------------------ *
 * gdb_recv_body() -- mirrors gdbstub.c:333-395's outer byte loop and
 * checksum-digit wait (FIXED shape). The escape/run-length sub-waits and the
 * full packet semantics (rx_buf, sum accumulation) are intentionally
 * simplified away -- see the file header for why only the kick CADENCE
 * matters to this test, not the full RSP framing (already covered by
 * gdbstub's own on-target behaviour and by GDB's own retry-on-bad-checksum
 * protocol).
 * ------------------------------------------------------------------ */
static int gdb_recv_body(void)
{
	int c, expect, len = 0;
	unsigned sum = 0;

	for (;;) {
		c = gdb_getc();
		if (c < 0) {
			wdt_debug_kick();   /* mirrors gdbstub.c:344 */
			continue;
		}
		if (c == '#')
			break;
		sum += (unsigned)(c & 0xff);
		len++;
	}

	{
		int h, l;
		do {
			h = gdb_getc();
			if (h < 0)
				wdt_debug_kick();   /* mirrors gdbstub.c:386-387 */
		} while (h < 0);
		do {
			l = gdb_getc();
			if (l < 0)
				wdt_debug_kick();   /* mirrors gdbstub.c:391-392 */
		} while (l < 0);
		expect = (unhex(h) << 4) | unhex(l);
	}

	if ((int)(sum & 0xff) != expect) {
		gdb_putc('-');
		gdb_flush();
		return -1;
	}
	gdb_putc('+');
	gdb_flush();
	return len;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* THE core regression test: gdb_recv() waiting an arbitrarily long time
 * (thousands of empty polls -- standing in for a human thinking at a
 * breakpoint far longer than the 16s hardware watchdog window) for the '$'
 * that opens the next packet must call wdt_debug_kick() once per empty
 * poll, not once total. A pre-fix mirror (bare `continue`, no kick inside
 * the loop) would leave g_kick_count at 0 after this call. */
static void test_gdb_recv_kicks_watchdog_throughout_a_long_idle_wait(void)
{
	reset_state();
	/* 5000 empty polls before the human finally sends "$#00" (the smallest
	 * legal-shaped packet: empty body, checksum 00). 5000 is arbitrary and
	 * only needs to be "large enough to distinguish a per-wait-iteration
	 * kick from a once-per-packet kick" -- it is not a stand-in for any
	 * real time duration since this mock has no clock. */
	script_push(5000, '$');
	script_push(0, '#');
	script_push(0, '0');
	script_push(0, '0');

	int n = gdb_recv();

	assert(n == 0);                  /* empty body, valid checksum 0x00 */
	assert(g_kick_count >= 5000);    /* kicked at least once per empty poll */
}

/* Same property, but for gdb_recv_body()'s own byte-loop wait (the case
 * where '$' already arrived and the body itself is slow to complete --
 * e.g. a flaky link dribbling bytes one at a time with long gaps, not just
 * a human pausing between whole packets). */
static void test_gdb_recv_body_byte_loop_kicks_during_a_long_gap(void)
{
	reset_state();
	script_push(0,    '$');   /* consumed by gdb_recv() itself */
	script_push(3000, 'a');   /* long gap BEFORE the first body byte */
	script_push(0,    '#');
	/* checksum of "a" == 'a' == 0x61 */
	script_push(0, '6');
	script_push(0, '1');

	int n = gdb_recv();

	assert(n == 1);
	assert(g_kick_count >= 3000);
}

/* Same property for the checksum-digit wait specifically (gdbstub.c:381-395)
 * -- a packet whose BODY arrived promptly but whose two trailing checksum
 * hex digits are slow (e.g. the host paused mid-transmit, or a bridge script
 * is momentarily stalled). This is the exact shape of "GDB sent most of a
 * packet and then went quiet" -- previously left CPU1 spinning with zero
 * kicks until the bytes resumed. */
static void test_gdb_recv_body_checksum_wait_kicks_during_a_long_gap(void)
{
	reset_state();
	script_push(0,    '$');
	script_push(0,    '#');   /* empty body -> straight to checksum wait */
	script_push(4000, '0');   /* long gap before the checksum's first digit */
	script_push(0,    '0');

	int n = gdb_recv();

	assert(n == 0);
	assert(g_kick_count >= 4000);
}

/* Sanity check in the OTHER direction: when bytes arrive promptly (no idle
 * gap at all), the kick count should still be small and bounded -- this
 * fix must not turn into "kick on literally every byte read" wastefulness
 * beyond what a genuinely empty poll needs. Guards against a sloppy fix that
 * kicks unconditionally on every gdb_getc() call regardless of its result. */
static void test_no_spurious_kicks_when_wire_is_never_idle(void)
{
	reset_state();
	script_push(0, '$');
	script_push(0, '#');
	script_push(0, '0');
	script_push(0, '0');

	int n = gdb_recv();

	assert(n == 0);
	assert(g_kick_count == 0);   /* not one empty poll occurred */
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "gdb_recv_kicks_watchdog_throughout_a_long_idle_wait",
	  test_gdb_recv_kicks_watchdog_throughout_a_long_idle_wait },
	{ "gdb_recv_body_byte_loop_kicks_during_a_long_gap",
	  test_gdb_recv_body_byte_loop_kicks_during_a_long_gap },
	{ "gdb_recv_body_checksum_wait_kicks_during_a_long_gap",
	  test_gdb_recv_body_checksum_wait_kicks_during_a_long_gap },
	{ "no_spurious_kicks_when_wire_is_never_idle",
	  test_no_spurious_kicks_when_wire_is_never_idle },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- gdbstub wdt-kick tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
