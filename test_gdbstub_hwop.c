/* SPDX-License-Identifier: BSD-2-Clause */

/* test_gdbstub_hwop.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the cross-core hw-breakpoint/watchpoint request state machine in
 * gdbstub_hw.c's hwop_run() -- the fix documented in project memory
 * `gdbstub-hwbp-wrong-core.md`.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "gdbstub_hw.c"` WITH STUBS
 * ============================================================================
 * gdbstub_hw.c was re-read in full (current, POST-FIX version) before writing
 * this file. hwop_run() (gdbstub_hw.c:101-127) is a pure request/poll state
 * machine over a handful of shared volatile globals -- exactly the class of
 * logic this project's test suite already extracts (see test_kload_modinfo.c
 * and test_gdbstub_resolve.c's header comments for the established house
 * rule). But gdbstub_hw.c is NOT includable on plain x86_64 gcc: rd_wcr()/
 * wr_wcr() (gdbstub_hw.c:169-189) use raw "mrs .../msr ..." on literal
 * "dbgwcrN_el1" register names, which the assembler rejects outright on this
 * target. hwop_run() itself also contains three ARM-only inline-asm
 * statements -- "dsb sy" (x2, lines 112/114), "dsb sy\n\tsev" (line 116, pokes
 * CPU0's wfe), and "wfe" (line 121, the actual wait-for-event the real spin
 * loop uses) -- none of which assemble on x86_64 either. Per this file's own
 * documented discipline (same rationale test_kload_modinfo.c and
 * test_gdbstub_resolve.c already use for kload_bc()/cache-maintenance calls):
 * these three asm statements are pure memory-ordering/wake-hint side effects
 * with NO effect on the C-visible control flow or the values of the shared
 * request/result fields under test -- a single-threaded hosted test has no
 * real multi-core ordering hazard to begin with, and "sev"/"wfe" are pure
 * power-management/event hints, architecturally inert on data. They are
 * OMITTED from the mirror below; every other line of hwop_run() (the
 * gdb_stop_pending gate, the field-posting order, the spin-and-check loop,
 * the timeout check, the return value) is reproduced verbatim in control
 * flow, with an exact gdbstub_hw.c line-number citation.
 *
 * THE BUG THIS FILE IS AIMED AT: gdbstub_hw_insert()/_remove() used to call
 * hwbp_set()/hwbp_clear()/patch_wp_lsc() DIRECTLY from whichever core was
 * running dispatch() -- always CPU1, the debug-service core, which never
 * hosts the FreeBSD guest. DBGBVR/DBGBCR/DBGWVR/DBGWCR are per-PE BANKED
 * registers, so arming them on CPU1 armed a debug unit the guest never
 * executes on -- every hbreak/watch/rwatch/awatch silently never fired. The
 * fix (gdbstub_hw.c's header block comment) posts the request into shared
 * globals and has CPU0's OWN parked wfe loop (el2_exc.c, not touched by this
 * task) service it via gdbstub_hw_apply_op() -- but ONLY when CPU0 is
 * verifiably parked right now (gdb_stop_pending != 0); otherwise hwop_run()
 * must refuse cleanly (-2) rather than silently arm the wrong core (the
 * original bug) or hang forever waiting for a CPU0 that isn't listening.
 * The three tests below cover exactly those three behaviors.
 *
 * TEST-ONLY "CPU0" SIMULATION: this is a single-threaded hosted test with no
 * real second core, so nothing can concurrently clear gdb_hw_op_pending while
 * hwop_run()'s loop is running the way the real CPU0 would. To exercise the
 * "CPU0 services the request and responds" path (as opposed to "never
 * responds" -- the timeout path, which needs no simulation at all: the
 * mirror's loop just runs to completion since nothing ever clears
 * gdb_hw_op_pending), a small test-only hook function pointer
 * (g_test_cpu0_hook, NOT present in the real source) is invoked once per
 * loop iteration, BEFORE the real `if (!gdb_hw_op_pending) break;` check --
 * standing in for "CPU0 wakes from wfe and services the op" at a chosen
 * iteration. When the hook is NULL (the default), the mirrored loop behaves
 * identically to the real one: nothing ever clears gdb_hw_op_pending, so it
 * spins for its full guard count and returns -2 -- this is precisely the
 * bounded-timeout test case.
 *
 * SHRUNK SPIN-GUARD CONSTANT: the real HWOP_SPIN_GUARD (gdbstub_hw.c:89) is
 * 10,000,000 iterations of a `wfe`-gated spin, sized as "generous headroom"
 * for real hardware where CPU0 responds within a handful of instructions.
 * On a hosted x86_64 test with no real wfe and no real second core, spinning
 * the full 10M iterations to prove the timeout path would waste real wall
 * time for no additional coverage (the loop body is trivial and the outcome
 * is identical at any guard size >= the responding iteration). This file
 * uses TEST_HWOP_SPIN_GUARD = 2000 instead, so the timeout test completes in
 * well under a second; this is a test-speed accommodation only, documented
 * here rather than silently changed.
 *
 * Build: gcc -o test_gdbstub_hwop test_gdbstub_hwop.c && ./test_gdbstub_hwop
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mirrored shared globals -- stand-ins for the `extern volatile` globals
 * gdbstub_hw.c:69-75 declares (real definitions live in el2_exc.c, not
 * touched by this task). Plain file-scope variables here; volatile dropped
 * since this hosted test is single-threaded (no compiler-reordering hazard
 * to guard against without a real concurrent writer).
 * ------------------------------------------------------------------ */
static uint32_t gdb_stop_pending;    /* mirrors gdbstub_hw.c:69 */
static uint32_t gdb_hw_op_pending;   /* mirrors gdbstub_hw.c:70 */
static int32_t  gdb_hw_op_kind;      /* mirrors gdbstub_hw.c:71 */
static int32_t  gdb_hw_op_slot;      /* mirrors gdbstub_hw.c:72 */
static uint64_t gdb_hw_op_va;        /* mirrors gdbstub_hw.c:73 */
static uint32_t gdb_hw_op_lsc;       /* mirrors gdbstub_hw.c:74 */
static int32_t  gdb_hw_op_result;    /* mirrors gdbstub_hw.c:75 */

/* Mirrors gdbstub_hw.c:77-82. */
enum {
	HWOP_BP_SET   = 0,
	HWOP_BP_CLEAR = 1,
	HWOP_WP_SET   = 2,
	HWOP_WP_CLEAR = 3,
};

/* Shrunk for test speed -- see the file header's "SHRUNK SPIN-GUARD
 * CONSTANT" discussion. Real value: gdbstub_hw.c:89 HWOP_SPIN_GUARD =
 * 10000000L. */
#define TEST_HWOP_SPIN_GUARD 2000L

/* Test-only "concurrent CPU0" hook -- see the file header's "TEST-ONLY 'CPU0'
 * SIMULATION" discussion. NOT a mirror of anything in gdbstub_hw.c. */
static void (*g_test_cpu0_hook)(long iter) = 0;

static void reset_state(void)
{
	gdb_stop_pending  = 0;
	gdb_hw_op_pending = 0;
	gdb_hw_op_kind    = -99;
	gdb_hw_op_slot    = -99;
	gdb_hw_op_va      = 0xdeadbeefdeadbeefull;
	gdb_hw_op_lsc     = 0xdeadu;
	gdb_hw_op_result  = -99;
	g_test_cpu0_hook  = 0;
}

/* ------------------------------------------------------------------ *
 * hwop_run() -- mirrors gdbstub_hw.c:101-127 verbatim in control flow,
 * MINUS the three ARM-only inline-asm statements (lines 112/114/116/121:
 * pure memory-ordering/wake-hint side effects, see the file header's
 * rationale) PLUS the single test-only hook call noted above (the one
 * addition not present in the real source, needed only because this hosted
 * test has no real second core).
 * ------------------------------------------------------------------ */
static int hwop_run(int kind, int slot, uint64_t va, uint32_t lsc)
{
	long guard;

	if (!gdb_stop_pending)
		return -2;                      /* CPU0 not parked -- refuse cleanly */

	gdb_hw_op_slot   = slot;
	gdb_hw_op_va     = va;
	gdb_hw_op_lsc    = lsc;
	gdb_hw_op_result = -2;
	gdb_hw_op_kind = kind;
	gdb_hw_op_pending = 1u;

	for (guard = 0; guard < TEST_HWOP_SPIN_GUARD; guard++) {
		if (g_test_cpu0_hook)            /* test-only: see file header */
			g_test_cpu0_hook(guard);
		if (!gdb_hw_op_pending)
			break;
	}
	if (gdb_hw_op_pending)
		return -2;                      /* timed out: never touched HW state */

	return gdb_hw_op_result;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* When gdb_stop_pending is 0 (CPU0 not parked), hwop_run() must return -2
 * IMMEDIATELY and must NEVER touch the request fields at all -- otherwise a
 * caller reading stale request state could think a request was posted when
 * it wasn't (gdbstub_hw.c:105-106). */
static void test_hwop_run_refuses_immediately_when_cpu0_not_parked(void)
{
	reset_state();
	gdb_stop_pending = 0;

	/* Poison the fields BEFORE the call so any accidental write is
	 * detectable. */
	gdb_hw_op_pending = 0xAAu;
	gdb_hw_op_kind    = 0x1111;
	gdb_hw_op_slot    = 0x2222;
	gdb_hw_op_va      = 0x3333333333333333ull;
	gdb_hw_op_lsc     = 0x4444u;
	gdb_hw_op_result  = 0x5555;

	int rc = hwop_run(HWOP_BP_SET, 2, 0xffff000000abcd00ull, 0);
	assert(rc == -2);

	/* Not one of the request fields was touched. */
	assert(gdb_hw_op_pending == 0xAAu);
	assert(gdb_hw_op_kind    == 0x1111);
	assert(gdb_hw_op_slot    == 0x2222);
	assert(gdb_hw_op_va      == 0x3333333333333333ull);
	assert(gdb_hw_op_lsc     == 0x4444u);
	assert(gdb_hw_op_result  == 0x5555);
}

/* Test-only hook: simulate CPU0 servicing the request on its very first
 * "wake" by writing a chosen result and clearing gdb_hw_op_pending -- mirrors
 * gdbstub_hw_apply_op()'s real effect (gdbstub_hw.c:340-364) without
 * reproducing its own asm-laden hwbp_set()/hwbp_clear() bodies (out of scope
 * for this file; hwop_run() only cares that *something* eventually clears
 * pending and leaves a result behind). */
static int g_fake_cpu0_result;
static void fake_cpu0_responds_immediately(long iter)
{
	if (iter == 0) {
		gdb_hw_op_result  = g_fake_cpu0_result;
		gdb_hw_op_pending = 0;
	}
}

/* Second test-only hook: capture what hwop_run() posted (kind/slot/va/lsc)
 * before simulating CPU0's response, proving the post happened BEFORE the
 * loop starts polling -- exactly the real field-then-pending-flag ordering
 * (gdbstub_hw.c:108-115). File-scope (not nested) to keep this plain C89/C99,
 * matching the rest of the tree's style. */
static int      g_captured_kind = -1;
static int      g_captured_slot = -1;
static uint64_t g_captured_va;
static uint32_t g_captured_lsc;
static void capture_and_respond(long iter)
{
	if (iter == 0) {
		g_captured_kind = gdb_hw_op_kind;
		g_captured_slot = gdb_hw_op_slot;
		g_captured_va   = gdb_hw_op_va;
		g_captured_lsc  = gdb_hw_op_lsc;
		g_fake_cpu0_result = 0;   /* hwbp_set()'s own "success" value */
		gdb_hw_op_result   = g_fake_cpu0_result;
		gdb_hw_op_pending  = 0;
	}
}

/* When gdb_stop_pending is 1 (CPU0 parked), hwop_run() posts kind/slot/va/lsc
 * into the shared fields correctly (gdbstub_hw.c:108-113), sets
 * gdb_hw_op_pending (line 115), and -- once "CPU0" (the test hook) services
 * it and clears pending -- returns exactly the result CPU0 left behind
 * (line 126), not -2. */
static void test_hwop_run_posts_request_and_returns_cpu0_result(void)
{
	reset_state();
	gdb_stop_pending = 1;
	g_captured_kind = -1; g_captured_slot = -1; g_captured_va = 0; g_captured_lsc = 0;
	g_test_cpu0_hook = capture_and_respond;

	int rc = hwop_run(HWOP_WP_SET, 3, 0xffff000000112233ull, 0x2u /* LSC_STORE */);

	assert(g_captured_kind == HWOP_WP_SET);
	assert(g_captured_slot == 3);
	assert(g_captured_va   == 0xffff000000112233ull);
	assert(g_captured_lsc  == 0x2u);
	assert(rc == 0);   /* exactly the result "CPU0" left behind */

	/* A second call with a DIFFERENT simulated CPU0 result confirms
	 * hwop_run() returns whatever gdb_hw_op_result holds, not a fixed
	 * constant -- e.g. hwbp_set()'s "-1 = slot not implemented" case. */
	reset_state();
	gdb_stop_pending = 1;
	g_fake_cpu0_result = -1;
	g_test_cpu0_hook = fake_cpu0_responds_immediately;
	rc = hwop_run(HWOP_BP_CLEAR, 0, 0, 0);
	assert(rc == -1);
}

/* The bounded-spin-guard timeout path: CPU0 "never" responds (the test
 * harness deliberately installs NO hook, so nothing ever clears
 * gdb_hw_op_pending). hwop_run() must eventually give up after
 * TEST_HWOP_SPIN_GUARD iterations and return -2, rather than looping forever
 * -- proving the bound in gdbstub_hw.c:118-124 actually terminates instead of
 * being an unreachable dead branch. */
static void test_hwop_run_gives_up_after_spin_guard_when_cpu0_never_responds(void)
{
	reset_state();
	gdb_stop_pending = 1;
	g_test_cpu0_hook = 0;   /* explicit: no simulated CPU0 response at all */

	int rc = hwop_run(HWOP_BP_SET, 1, 0xffff000000445566ull, 0);

	assert(rc == -2);
	/* pending was posted (the request WAS made)... */
	/* ...but never got serviced -- still 1 after the loop gave up. */
	assert(gdb_hw_op_pending == 1u);
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "hwop_run_refuses_immediately_when_cpu0_not_parked",
	  test_hwop_run_refuses_immediately_when_cpu0_not_parked },
	{ "hwop_run_posts_request_and_returns_cpu0_result",
	  test_hwop_run_posts_request_and_returns_cpu0_result },
	{ "hwop_run_gives_up_after_spin_guard_when_cpu0_never_responds",
	  test_hwop_run_gives_up_after_spin_guard_when_cpu0_never_responds },
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

	printf("---- gdbstub hwop tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
