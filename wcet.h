/* SPDX-License-Identifier: BSD-2-Clause */

/* wcet.h — WCET / deadline monitor for the bzdOS EL2 microkernel.
 *
 * Lets a task declare a PERIOD and a per-release BUDGET (both in microseconds),
 * timestamps each release, and flags overruns:
 *   - BUDGET overrun : a release's execution (wall time since wcet_release())
 *                      exceeds the declared budget.
 *   - DEADLINE miss  : the current time passes release_time + period before the
 *                      release completes (wcet_complete()).
 * Two entry styles, both supported simultaneously:
 *   - explicit  : the task calls wcet_release() at the top of each period and
 *                 wcet_complete() at the bottom (precise, self-reported).
 *   - tick hook : wcet_tick(now), called from the scheduler tick, catches a
 *                 RUNNING task that has blown its budget/deadline even if it
 *                 never reaches wcet_complete() (e.g. it hung) — the safety net.
 *
 * Overrun events accumulate into a DRAM breadcrumb (0x50005400 "WCET"): a total
 * count, the worst offender's task id, and the worst overrun magnitude, so the
 * result survives a watchdog reset and can be read over the network REPL.
 *
 * Freestanding: no libc, <stdint.h> only. SMP-safe (its own spinlock).
 */
#ifndef BZDOS_WCET_H
#define BZDOS_WCET_H

#include <stdint.h>

/* Declare monitoring for task `taskid` (0..SCHED_MAX_TASKS-1): expected release
 * period and per-release CPU/wall budget, in microseconds. budget_us should be
 * <= period_us. Passing 0/0 disables monitoring for that task. */
void wcet_declare(int taskid, uint64_t period_us, uint64_t budget_us);

/* Mark the start of a new release (job) for the CURRENT task. Records the start
 * timestamp and computes this release's deadline (= now + period). Idempotent-
 * safe to call once per period at the top of the task loop. */
void wcet_release(void);

/* Mark the end of the current task's release. Computes elapsed = now - start;
 * if elapsed > budget records a BUDGET overrun; if now > deadline records a
 * DEADLINE miss. */
void wcet_complete(void);

/* Scheduler-tick safety-net hook: for the task currently RUNNING (per
 * sched_current_task()), if it is monitored and has a live release whose
 * elapsed time already exceeds its budget, or whose deadline has already
 * passed, record the overrun immediately (don't wait for wcet_complete, which
 * a runaway task may never reach). Call once per tick with timer_now(). */
void wcet_tick(uint64_t now);

/* Read-only snapshot for logd/REPL. Any pointer may be NULL. */
void wcet_stats(uint32_t *total_overruns, int *worst_task, uint64_t *worst_us);

/* ------------------------------------------------------------------ *
 * Breadcrumb — DRAM window 0x50005400 ("WCET"):
 *   [0]  magic 0x57434554 ("WCET")
 *   [1]  total overrun events (budget + deadline)
 *   [2]  budget-overrun count
 *   [3]  deadline-miss count
 *   [4]  worst offender task id
 *   [5]  worst overrun magnitude, microseconds (low32)
 *   [6]  last overrun task id
 *   [7]  last overrun kind (1 = budget, 2 = deadline)
 *   [8]  last overrun magnitude, microseconds (low32)
 *   [9]  number of tasks currently declared
 * ------------------------------------------------------------------ */
#define WCET_BC_BASE  0x50005400UL
#define WCET_BC_MAGIC 0x57434554u  /* "WCET" */

#define WCET_KIND_BUDGET   1u
#define WCET_KIND_DEADLINE 2u

/* Deterministic unit self-test: declares a task, simulates an on-time release
 * and an over-budget release using synthetic timestamps (no scheduler needed),
 * and verifies exactly one overrun is recorded. Writes the WCET breadcrumb. */
void wcet_selftest(void);

#endif /* BZDOS_WCET_H */
