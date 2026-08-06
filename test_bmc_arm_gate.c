/* SPDX-License-Identifier: BSD-2-Clause */

/* test_bmc_arm_gate.c -- hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for bmc.c's destructive-verb safety gate: bmc_arm()/bmc_check_armed()
 * (bmc.c:203-235 as of this commit). ROADMAP B1 polish, task priority #3:
 * "verify the arming gate actually gates" -- reset/wdt-hold/flag all call
 * bmc_check_armed() first, so this is the one piece of logic standing
 * between an unauthenticated L2 frame and a board reset.
 *
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "bmc.c"` WITH STUBS
 * ------------------------------------------------------------------------
 * Same house rule as test_snapshot_fmt.c/test_stage2_tables.c/
 * test_vconsole_uart.c: bmc.c is freestanding AArch64
 * (-mgeneral-regs-only, no libc) and bmc_arm()/bmc_check_armed() read the
 * real clock with inline asm --
 *     bmc.c:211  `mrs %0, cntfrq_el0`
 *     bmc.c:215  rd_cntpct() -> `isb; mrs %0, cntpct_el0`
 *     bmc.c:228  rd_cntpct() again
 * -- which gas rejects on x86_64. Making bmc.c includable here would mean
 * editing its real clock reads just to run a host test, which is exactly
 * the thing those earlier test files' house rule forbids. So: transcribe
 * the two functions verbatim (this file cites the exact bmc.c line numbers
 * it mirrors, so a change there is a visible diff here too), with ONE
 * deliberate seam -- `fake_now`/`fake_freq` in place of the two `mrs`
 * reads -- so the 10s window can be exercised without a real clock or a
 * sleep(). The OTHER deliberate deviation: the real bmc_check_armed()
 * returns plain 0/1 and prints its reason via `cputs()` (bmc.c:225,230);
 * this mirror returns three DISTINCT codes (NOT_ARMED/EXPIRED/OK) instead
 * of printing, purely so the tests below can assert WHICH refusal reason
 * fired -- every branch, its order, and the one-shot-consume side effect
 * are otherwise unchanged. Real callers only ever branch on
 * "!= ARM_OK", so this is not a behavior change, just a more legible
 * return value for testing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* abort() */

#define BMC_ARM_WINDOW_S 10u   /* bmc.c:203, verbatim */

static uint32_t bmc_arm_nonce;      /* bmc.c:205 */
static uint64_t bmc_arm_at;         /* bmc.c:206 */
static uint64_t bmc_arm_ticks_win;  /* bmc.c:207 */

/* The seam: stand-ins for bmc.c's `mrs cntfrq_el0` (bmc.c:211) and
 * rd_cntpct() (bmc.c:146-149, called at bmc.c:215 and bmc.c:228). Tests set
 * these directly instead of waiting on a real clock. */
static uint64_t fake_freq = 24000000ull;   /* A64 CNTFRQ, matches bmc.c's own
                                             * fallback constant */
static uint64_t fake_now;
static uint64_t rd_cntpct(void) { return fake_now; }

/* bmc_arm(), transcribed verbatim from bmc.c:209-218 (s/mrs cntfrq_el0/
 * fake_freq/, cputs()/pdec() dropped -- pure state transition only). */
static void bmc_arm(unsigned long nonce)
{
	uint64_t f = fake_freq;
	if (!f) f = 24000000ull;                                  /* bmc.c:212 */
	bmc_arm_ticks_win = f * (uint64_t)BMC_ARM_WINDOW_S;        /* bmc.c:213 */
	bmc_arm_nonce = (uint32_t)nonce ? (uint32_t)nonce : 1u;    /* bmc.c:214 */
	bmc_arm_at = rd_cntpct();                                  /* bmc.c:215 */
}

enum { ARM_NOT_ARMED = 0, ARM_OK = 1, ARM_EXPIRED = 2 };

/* bmc_check_armed(), transcribed verbatim from bmc.c:222-235 -- same three
 * branches, same order, same one-shot consume; see the file header for why
 * the return values differ from the real 0/1. */
static int bmc_check_armed(void)
{
	if (bmc_arm_nonce == 0u)                                   /* bmc.c:224 */
		return ARM_NOT_ARMED;
	if (rd_cntpct() - bmc_arm_at > bmc_arm_ticks_win) {         /* bmc.c:228 */
		bmc_arm_nonce = 0u;                                     /* bmc.c:229 */
		return ARM_EXPIRED;
	}
	bmc_arm_nonce = 0u;   /* one-shot consume -- bmc.c:233 */
	return ARM_OK;
}

static void reset_gate(void)
{
	bmc_arm_nonce = 0u;
	bmc_arm_at = 0u;
	bmc_arm_ticks_win = 0u;
	fake_freq = 24000000ull;
	fake_now = 0u;
}

/* ==================================================================== *
 * test cases
 * ==================================================================== */

static void test_refused_when_never_armed(void)
{
	reset_gate();
	if (bmc_check_armed() != ARM_NOT_ARMED) { fprintf(stderr, "expected NOT_ARMED\n"); abort(); }
}

static void test_arm_then_check_succeeds_exactly_once(void)
{
	reset_gate();
	bmc_arm(0x1234);
	if (bmc_check_armed() != ARM_OK) { fprintf(stderr, "expected OK\n"); abort(); }
	/* The window really CLOSES: a second destructive verb riding the SAME
	 * arm must be refused, not silently allowed through until the 10s
	 * elapse. This is the exact property the task asks to verify. */
	if (bmc_check_armed() != ARM_NOT_ARMED) { fprintf(stderr, "arm was not consumed\n"); abort(); }
}

static void test_zero_nonce_is_coerced_not_rejected(void)
{
	/* bmc.c:214 -- `bmc arm 0` still arms (coerced to 1). The nonce's role
	 * is "was arm() called recently", not "does this value match a later
	 * command" -- bmc.h's design comment says so explicitly, and this
	 * pins that down as intended behavior, not an oversight. */
	reset_gate();
	bmc_arm(0);
	if (bmc_arm_nonce != 1u) { fprintf(stderr, "zero nonce not coerced to 1\n"); abort(); }
	if (bmc_check_armed() != ARM_OK) { fprintf(stderr, "coerced arm did not authorize\n"); abort(); }
}

static void test_window_boundary_exactly_at_limit_still_ok(void)
{
	/* bmc.c:228 tests `> ticks_win`, so equality is still within the
	 * window -- pin the boundary down explicitly (off-by-one here would
	 * either reject a legitimate command at the wire or, worse, extend
	 * the window past its documented ~10s). */
	reset_gate();
	fake_now = 1000;
	bmc_arm(1);
	fake_now = bmc_arm_at + bmc_arm_ticks_win;      /* exactly at the edge */
	if (bmc_check_armed() != ARM_OK) { fprintf(stderr, "boundary should still be armed\n"); abort(); }
}

static void test_window_one_tick_past_limit_expires(void)
{
	reset_gate();
	fake_now = 1000;
	bmc_arm(1);
	fake_now = bmc_arm_at + bmc_arm_ticks_win + 1;  /* one past the edge */
	if (bmc_check_armed() != ARM_EXPIRED) { fprintf(stderr, "expected EXPIRED\n"); abort(); }
}

static void test_expiry_clears_the_stale_nonce(void)
{
	/* bmc.c:229 zeroes bmc_arm_nonce on expiry, so a REPLAYED destructive
	 * frame that arrives after the window cannot ride the stale arm even
	 * once more: the very next check must report NOT_ARMED, not EXPIRED
	 * again (which would imply something is still "armed but too old"
	 * rather than fully disarmed). */
	reset_gate();
	fake_now = 1000;
	bmc_arm(1);
	fake_now = bmc_arm_at + bmc_arm_ticks_win + 1;
	if (bmc_check_armed() != ARM_EXPIRED) { fprintf(stderr, "expected EXPIRED\n"); abort(); }
	if (bmc_arm_nonce != 0u) { fprintf(stderr, "expired arm was not cleared\n"); abort(); }
	if (bmc_check_armed() != ARM_NOT_ARMED) { fprintf(stderr, "stale arm still reachable after expiry\n"); abort(); }
}

static void test_rearm_after_expiry_works(void)
{
	reset_gate();
	fake_now = 1000;
	bmc_arm(1);
	fake_now = bmc_arm_at + bmc_arm_ticks_win + 1;
	(void)bmc_check_armed();                        /* let it expire */
	bmc_arm(2);                                      /* re-arm fresh */
	if (bmc_check_armed() != ARM_OK) { fprintf(stderr, "re-arm after expiry should succeed\n"); abort(); }
}

static void test_zero_freq_falls_back_to_24mhz(void)
{
	/* bmc.c:212 -- CNTFRQ_EL0 reading 0 (never programmed) must not divide
	 * the window down to zero seconds; it falls back to the A64's real
	 * 24 MHz. */
	reset_gate();
	fake_freq = 0;
	bmc_arm(1);
	if (bmc_arm_ticks_win != 24000000ull * BMC_ARM_WINDOW_S) {
		fprintf(stderr, "zero-freq fallback not applied\n"); abort();
	}
}

/* ==================================================================== *
 * main()
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "refused_when_never_armed",              test_refused_when_never_armed },
	{ "arm_then_check_succeeds_exactly_once",   test_arm_then_check_succeeds_exactly_once },
	{ "zero_nonce_is_coerced_not_rejected",     test_zero_nonce_is_coerced_not_rejected },
	{ "window_boundary_exactly_at_limit_still_ok",
	                                            test_window_boundary_exactly_at_limit_still_ok },
	{ "window_one_tick_past_limit_expires",     test_window_one_tick_past_limit_expires },
	{ "expiry_clears_the_stale_nonce",          test_expiry_clears_the_stale_nonce },
	{ "rearm_after_expiry_works",               test_rearm_after_expiry_works },
	{ "zero_freq_falls_back_to_24mhz",          test_zero_freq_falls_back_to_24mhz },
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

	printf("---- bmc arm-gate tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
