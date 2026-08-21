/* SPDX-License-Identifier: BSD-2-Clause */

/* cntpct.h — the ONE way this hypervisor reads the architected counter.
 *
 * WHY THIS FILE EXISTS
 *
 * On the Allwinner A64 the counter registers are not safe to read naively.
 * Upstream Linux carries this as a named erratum -- "Allwinner erratum
 * UNKNOWN1", CONFIG_SUN50I_ERRATUM_UNKNOWN1, device-tree property
 * `allwinner,erratum-unknown1` -- whose binding says, in as many words, that
 * "reading certain values from the counter is unreliable".
 *
 * The mechanism, from arm_arch_timer.c's own comment (6.12, above
 * __sun50i_a64_read_reg):
 *
 *     The low bits of the counter registers are indeterminate while bit 10 or
 *     greater is rolling over. Since the counter value can jump both BACKWARD
 *     (7ff -> 000 -> 800) and forward (7ff -> fff -> 800), ignore register
 *     values with all ones or all zeros in the low bits.
 *
 * That is the answer to a question this project chased for a long time: why
 * CNTPCT_EL0 reads backwards. It is not a banked-register misread, not a
 * compiler reordering, and not our arithmetic -- the hardware really does
 * return a value below the previous one, and it does so around a carry.
 *
 * Reading backwards is not a cosmetic problem here. Every deadline in this
 * tree is `now - start > cap` on unsigned 64-bit values, so a single backwards
 * read UNDERFLOWS to ~2^64 and makes the comparison true immediately: a
 * spurious timeout, at an arbitrary moment, in whatever loop happened to be
 * running. In emmc_bio.c that fired mid-data-phase and cost real filesystem
 * corruption before it was understood (see the CNTPCT history in that file and
 * the `cnt_anom` breadcrumb it still keeps).
 *
 * Individual call sites had grown their own defences -- backwards-safe
 * subtraction, two-read confirmation, anomaly counters. Those stay where they
 * are (they are cheap and they document real incidents), but they were each
 * guarding against a bad value that this header now refuses to return in the
 * first place. Before this file there were EIGHT separate hand-written
 * `mrs cntpct_el0` sites, three of them without even the ISB.
 *
 * Bound: 150 retries, upstream's number -- "the maximum number of CPU cycles
 * in 3 consecutive 24 MHz counter periods". If they are exhausted the value is
 * returned anyway: a bounded wrong answer beats an unbounded loop inside a
 * fault handler, and the callers' own guards remain as the second line.
 */
#ifndef BZDOS_CNTPCT_H
#define BZDOS_CNTPCT_H

#include <stdint.h>

/* All-zeros or all-ones in the low 9 bits == the indeterminate window.
 * `(v + 1) & 0x1ff <= 1` catches both, exactly as upstream's
 * `((_val + 1) & GENMASK(8, 0)) <= 1` does. */
#define CNTPCT_BAD_LOW(v)   ((((v) + 1u) & 0x1ffull) <= 1ull)
#define CNTPCT_RETRIES      150

static inline uint64_t cntpct_read(void)
{
	uint64_t v;
	int retries = CNTPCT_RETRIES;

	do {
		/* ISB before the read: the counter read is not ordered against
		 * preceding instructions without it, which is a separate hazard
		 * from the erratum and was missing at three of the old sites. */
		__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
	} while (CNTPCT_BAD_LOW(v) && --retries);

	return v;
}

/* The virtual counter has the same erratum -- upstream installs
 * sun50i_a64_read_cntvct_el0 alongside the physical one. */
static inline uint64_t cntvct_read(void)
{
	uint64_t v;
	int retries = CNTPCT_RETRIES;

	do {
		__asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
	} while (CNTPCT_BAD_LOW(v) && --retries);

	return v;
}

#endif /* BZDOS_CNTPCT_H */
