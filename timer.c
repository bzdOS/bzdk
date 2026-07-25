/* SPDX-License-Identifier: BSD-2-Clause */

/* timer.c — ARM Generic Timer timebase + jitter meter (see timer.h).
 *
 * Bare-metal, freestanding, EL2, MMU on. Timebase reads the ARM Generic
 * Timer system registers directly (mrs) — no MMIO, no GIC, no interrupt
 * installed here. This module is the measurement primitive the interrupt
 * lane (owned elsewhere) calls into from its own timer IRQ handler once the
 * GIC is wired up; it must never mutate any state the caller doesn't own.
 *
 * System registers used:
 *   CNTFRQ_EL0  — counter frequency in Hz, set by earlier boot firmware
 *                 (U-Boot/BL31). Read-only from EL2 in the normal case; we
 *                 read it live rather than hardcode 24 MHz so this stays
 *                 correct if boot firmware ever reprograms it.
 *   CNTPCT_EL0  — free-running 64-bit physical counter, the monotonic
 *                 timebase. Available at EL2 without trapping (no need to
 *                 go through CNTHCTL_EL2 gymnastics for a plain read on
 *                 this core). We `isb` immediately before each read: the
 *                 core can otherwise reorder the mrs ahead of preceding
 *                 instructions (e.g. a caller that just reprogrammed a
 *                 comparator/control register), which would read a stale
 *                 or premature count. isb flushes the pipeline so the mrs
 *                 executes strictly after everything before it in program
 *                 order.
 */
#include <stdint.h>
#include "timer.h"

static inline uint64_t
read_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline uint64_t
read_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb sy" ::: "memory");
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

uint64_t
timer_freq(void)
{
	return read_cntfrq();
}

uint64_t
timer_now(void)
{
	return read_cntpct();
}

uint64_t
timer_us(uint64_t ticks)
{
	uint64_t freq = read_cntfrq();

	if (freq == 0)
		return 0;
	/* ticks * 1e6 / freq, ordered to avoid overflow at typical 24 MHz
	 * counters for any run short enough to matter (ticks up to ~2^44
	 * before the *1000000 would overflow 64 bits at freq=24MHz, i.e.
	 * multi-thousand-year uptimes — a non-issue in practice). */
	return (ticks * 1000000ull) / freq;
}

uint64_t
timer_ns(uint64_t ticks)
{
	uint64_t freq = read_cntfrq();

	if (freq == 0)
		return 0;
	return (ticks * 1000000000ull) / freq;
}

void
timer_delay_us(uint32_t us)
{
	uint64_t freq = read_cntfrq();
	uint64_t start = read_cntpct();
	uint64_t ticks = (freq / 1000000ull) * (uint64_t)us;

	/* Guard against a bogus zero frequency: fall back to a fixed
	 * (generous, A64-typical 24 MHz) tick count rather than spin
	 * forever or return instantly. */
	if (freq == 0)
		ticks = 24ull * (uint64_t)us;

	while ((read_cntpct() - start) < ticks)
		;
}

/* ------------------------------------------------------------------ *
 * Jitter meter — pure computation, no globals mutated: `j` is entirely
 * caller-owned storage.
 * ------------------------------------------------------------------ */

void
jitter_init(struct jitter *j, uint64_t expected_period_ticks)
{
	j->expected = expected_period_ticks;
	j->last     = 0;
	j->period   = 0;
	j->min_d    = 0;
	j->max_d    = 0;
	j->dev      = 0;
	j->max_dev  = 0;
	j->sum_d    = 0;
	j->n        = 0;
}

void
jitter_sample(struct jitter *j, uint64_t now_ticks)
{
	uint64_t delta;
	uint64_t dev;

	if (j->n == 0 && j->last == 0) {
		/* First sample after init: nothing to diff against yet, just
		 * prime the reference point. */
		j->last = now_ticks;
		return;
	}

	delta = now_ticks - j->last;
	j->last = now_ticks;
	j->period = delta;
	j->sum_d += delta;

	if (j->n == 0) {
		j->min_d = delta;
		j->max_d = delta;
	} else {
		if (delta < j->min_d)
			j->min_d = delta;
		if (delta > j->max_d)
			j->max_d = delta;
	}

	dev = (delta > j->expected) ? (delta - j->expected) : (j->expected - delta);
	j->dev = dev;
	if (dev > j->max_dev)
		j->max_dev = dev;

	j->n++;
}

/* ------------------------------------------------------------------ *
 * Jitter breadcrumb — fixed DRAM scratch window at 0x50000500.
 *
 * Distinct from the other fixed breadcrumb windows already in use by this
 * codebase: MUSB console at 0x50000000, EMAC at 0x50000100, REPL at
 * 0x50000300, EL2 exception vectors at 0x50000400. This one is "TIMR".
 *
 * D-cache is on (MMU on, per house style) so a plain store can sit in
 * cache and be lost across a watchdog reset; every word is cleaned to the
 * point of coherency with `dc civac` + `dsb sy` after writing, matching
 * the bc_write() pattern in main.c/wdt.c.
 *
 * Word layout (uint32_t each, offsets from 0x50000500):
 *   [0] magic       0x54494d52 ("TIMR")
 *   [1] freq        CNTFRQ_EL0, Hz (truncated to 32 bits; A64 values fit)
 *   [2] n           sample count (jitter->n, truncated to 32 bits)
 *   [3] last_delta  low 32 bits of jitter->period (last observed delta)
 *   [4] min_delta   low 32 bits of jitter->min_d
 *   [5] max_delta   low 32 bits of jitter->max_d
 *   [6] last_dev    low 32 bits of jitter->dev (deviation of last sample)
 *   [7] max_dev     low 32 bits of jitter->max_dev
 *
 * Ticks at 24 MHz stay well within 32 bits for any deltas/deviations of
 * interest (a full 32-bit tick span is ~178 s), so truncation only loses
 * information for pathological multi-minute gaps, not for the periodic
 * jitter this module targets.
 */
#define TIMR_BC_BASE 0x50000500UL

static inline void
timr_bc_write(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(TIMR_BC_BASE + (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

#define TIMR_BC_MAGIC 0x54494d52u /* "TIMR" */

void
jitter_report_bc(struct jitter *j)
{
	timr_bc_write(0, TIMR_BC_MAGIC);
	timr_bc_write(1, (uint32_t)read_cntfrq());
	timr_bc_write(2, (uint32_t)j->n);
	timr_bc_write(3, (uint32_t)j->period);
	timr_bc_write(4, (uint32_t)j->min_d);
	timr_bc_write(5, (uint32_t)j->max_d);
	timr_bc_write(6, (uint32_t)j->dev);
	timr_bc_write(7, (uint32_t)j->max_dev);
}
