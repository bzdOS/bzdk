/* SPDX-License-Identifier: BSD-2-Clause */

/* timer.h — ARM Generic Timer timebase + jitter meter for the bzdOS
 * microkernel/hypervisor.
 *
 * This is the measurement foundation for logd's deterministic timing
 * profiles: a monotonic high-resolution timebase (CNTPCT_EL0, driven by
 * CNTFRQ_EL0) plus pure-computation jitter tracking so callers can quantify
 * how far real interrupt/loop periods deviate from an expected period.
 *
 * Freestanding, no libc: only <stdint.h>. Does NOT touch the GIC and does
 * NOT install any interrupt handler — the EL2-vectors lane owns IRQ routing
 * and is expected to call timer_now() / jitter_sample() from its own timer
 * IRQ handler. Everything here is reentrancy-safe in the sense that no
 * function mutates any global: the `struct jitter` instance is entirely
 * caller-owned (caller decides storage/locking/ownership across cores).
 */
#ifndef BZDOS_TIMER_H
#define BZDOS_TIMER_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Timebase — ARM Generic Timer, physical counter/frequency.
 * ------------------------------------------------------------------ */

/* Counter frequency in Hz, read straight from CNTFRQ_EL0 (typically 24 MHz
 * on the Allwinner A64, but we read the register rather than hardcode it —
 * firmware/board config is the source of truth). */
uint64_t timer_freq(void);

/* Current monotonic 64-bit physical count, read from CNTPCT_EL0. An isb
 * precedes the mrs so the read is ordered after any preceding instructions
 * that might affect timer state (e.g. CNTP_CTL_EL0 writes) — without it the
 * out-of-order pipeline can hoist the counter read too early. */
uint64_t timer_now(void);

/* Convert a tick delta (as returned by timer_now()) to microseconds /
 * nanoseconds, using the live CNTFRQ_EL0 value. */
uint64_t timer_us(uint64_t ticks);
uint64_t timer_ns(uint64_t ticks);

/* Bounded busy-wait for approximately `us` microseconds, polling CNTPCT_EL0.
 * For the rare spot that genuinely needs a real-time delay (not a loop
 * throttle) — most of the codebase should prefer counting real events. */
void timer_delay_us(uint32_t us);

/* ------------------------------------------------------------------ *
 * Jitter meter — pure computation over caller-supplied timestamps.
 *
 * The caller (e.g. a timer-IRQ handler once the EL2-vectors lane wires the
 * GIC) owns a `struct jitter` instance and feeds it timer_now() values on
 * every period tick via jitter_sample(). No global state is touched here.
 * ------------------------------------------------------------------ */
struct jitter {
	uint64_t expected;   /* expected period, in ticks */
	uint64_t last;       /* timestamp of the previous sample (ticks) */
	uint64_t period;     /* delta of the most recent sample (ticks) */
	uint64_t min_d;      /* smallest observed delta (ticks) */
	uint64_t max_d;      /* largest observed delta (ticks) */
	uint64_t dev;        /* |period - expected| of the most recent sample */
	uint64_t max_dev;    /* largest observed deviation from expected */
	uint64_t sum_d;      /* running sum of deltas, for a mean if wanted */
	uint64_t n;          /* number of samples taken (0 = not yet primed) */
};

/* Initialize/reset a jitter tracker. `expected_period_ticks` is the nominal
 * period (e.g. the programmed timer reload value) that deviation is
 * measured against. Does not take a timestamp yet — the first
 * jitter_sample() call only primes `last` and does not count as a delta
 * (there is nothing to take a delta against). */
void jitter_init(struct jitter *j, uint64_t expected_period_ticks);

/* Feed one new timestamp (ticks, e.g. from timer_now()). Computes
 * delta = now - last, updates min_d/max_d/last/sum_d/n, and sets
 * dev = |delta - expected| (also folded into max_dev). The very first call
 * after jitter_init() only primes `last` (n stays 0, no delta computed —
 * there is no previous sample to diff against). */
void jitter_sample(struct jitter *j, uint64_t now_ticks);

/* Write a cache-coherent breadcrumb snapshot of `j` to the fixed DRAM
 * window at 0x50000500 (see .c file for the exact word layout). Safe to
 * call any time after jitter_init(); reflects whatever j currently holds. */
void jitter_report_bc(struct jitter *j);

#endif /* BZDOS_TIMER_H */
