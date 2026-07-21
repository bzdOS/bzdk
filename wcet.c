/* wcet.c — WCET / deadline monitor. See wcet.h for the API contract.
 *
 * Per-task declared period + budget (in timer ticks); each release is
 * timestamped and checked against budget (execution time) and deadline
 * (release_time + period). Overruns accumulate into the WCET breadcrumb.
 *
 * ---------------------------------------------------------------------------
 * TICK INTEGRATION: wcet_tick(timer_now()) is called from the same el2_trap()
 * IRQ arm as ktimer_service() (see ktimer.c's header comment). It is the safety
 * net that catches a RUNNING task overrunning its budget/deadline even if the
 * task never reaches wcet_complete().
 * ---------------------------------------------------------------------------
 */
#include <stdint.h>
#include "wcet.h"
#include "sched.h"
#include "timer.h"
#include "smp.h"

struct wcet_task {
	uint32_t declared;
	uint32_t active;          /* in a release (between release & complete) */
	uint64_t period;          /* ticks */
	uint64_t budget;          /* ticks */
	uint64_t start;           /* release timestamp (ticks) */
	uint64_t deadline;        /* start + period (ticks) */
	uint32_t budget_flagged;  /* already recorded a budget overrun this release */
	uint32_t deadline_flagged;/* already recorded a deadline miss this release */
};

static struct wcet_task g_w[SCHED_MAX_TASKS];
static spinlock_t g_w_lock = SPINLOCK_INIT;

/* Aggregate stats (guarded by g_w_lock). */
static uint32_t g_total;
static uint32_t g_budget_cnt;
static uint32_t g_deadline_cnt;
static int      g_worst_task = -1;
static uint64_t g_worst_us;
static int      g_last_task = -1;
static uint32_t g_last_kind;
static uint64_t g_last_us;
static uint32_t g_declared_n;

static inline void
w_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(WCET_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Publish the whole breadcrumb (caller holds g_w_lock). */
static void
w_publish(void)
{
	w_bc(0, WCET_BC_MAGIC);
	w_bc(1, g_total);
	w_bc(2, g_budget_cnt);
	w_bc(3, g_deadline_cnt);
	w_bc(4, (uint32_t)g_worst_task);
	w_bc(5, (uint32_t)g_worst_us);
	w_bc(6, (uint32_t)g_last_task);
	w_bc(7, g_last_kind);
	w_bc(8, (uint32_t)g_last_us);
	w_bc(9, g_declared_n);
}

/* Record one overrun event (caller holds g_w_lock). over_ticks is the amount by
 * which budget/deadline was exceeded. */
static void
w_record(int id, uint32_t kind, uint64_t over_ticks)
{
	uint64_t over_us = timer_us(over_ticks);

	g_total++;
	if (kind == WCET_KIND_BUDGET)
		g_budget_cnt++;
	else
		g_deadline_cnt++;

	g_last_task = id;
	g_last_kind = kind;
	g_last_us = over_us;

	if (over_us >= g_worst_us) {
		g_worst_us = over_us;
		g_worst_task = id;
	}
	w_publish();
}

void
wcet_declare(int taskid, uint64_t period_us, uint64_t budget_us)
{
	uint64_t d;
	uint64_t f = timer_freq();

	if (taskid < 0 || taskid >= SCHED_MAX_TASKS)
		return;

	d = smp_irq_save();
	spin_lock(&g_w_lock);
	if (period_us == 0 && budget_us == 0) {
		if (g_w[taskid].declared && g_declared_n)
			g_declared_n--;
		g_w[taskid].declared = 0;
		g_w[taskid].active = 0;
	} else {
		if (!g_w[taskid].declared)
			g_declared_n++;
		g_w[taskid].declared = 1;
		g_w[taskid].active = 0;
		g_w[taskid].period = (period_us * f) / 1000000ull;
		g_w[taskid].budget = (budget_us * f) / 1000000ull;
		g_w[taskid].budget_flagged = 0;
		g_w[taskid].deadline_flagged = 0;
	}
	w_publish();
	spin_unlock(&g_w_lock);
	smp_irq_restore(d);
}

/* Internal, timestamp-explicit (testable + reused by the public wrappers). */
static void
wcet_release_at(int id, uint64_t now)
{
	uint64_t d;

	if (id < 0 || id >= SCHED_MAX_TASKS)
		return;
	d = smp_irq_save();
	spin_lock(&g_w_lock);
	if (g_w[id].declared) {
		g_w[id].start = now;
		g_w[id].deadline = now + g_w[id].period;
		g_w[id].active = 1;
		g_w[id].budget_flagged = 0;
		g_w[id].deadline_flagged = 0;
	}
	spin_unlock(&g_w_lock);
	smp_irq_restore(d);
}

static void
wcet_complete_at(int id, uint64_t now)
{
	uint64_t d;

	if (id < 0 || id >= SCHED_MAX_TASKS)
		return;
	d = smp_irq_save();
	spin_lock(&g_w_lock);
	if (g_w[id].declared && g_w[id].active) {
		uint64_t elapsed = now - g_w[id].start;
		if (elapsed > g_w[id].budget && !g_w[id].budget_flagged) {
			g_w[id].budget_flagged = 1;
			w_record(id, WCET_KIND_BUDGET, elapsed - g_w[id].budget);
		}
		if ((int64_t)(now - g_w[id].deadline) > 0 && !g_w[id].deadline_flagged) {
			g_w[id].deadline_flagged = 1;
			w_record(id, WCET_KIND_DEADLINE, now - g_w[id].deadline);
		}
		g_w[id].active = 0;
	}
	spin_unlock(&g_w_lock);
	smp_irq_restore(d);
}

void
wcet_release(void)
{
	wcet_release_at(sched_current_task(), timer_now());
}

void
wcet_complete(void)
{
	wcet_complete_at(sched_current_task(), timer_now());
}

void
wcet_tick(uint64_t now)
{
	int id = sched_current_task();
	uint64_t d;

	if (id < 0 || id >= SCHED_MAX_TASKS)
		return;

	d = smp_irq_save();
	spin_lock(&g_w_lock);
	if (g_w[id].declared && g_w[id].active) {
		uint64_t elapsed = now - g_w[id].start;
		if (elapsed > g_w[id].budget && !g_w[id].budget_flagged) {
			g_w[id].budget_flagged = 1;
			w_record(id, WCET_KIND_BUDGET, elapsed - g_w[id].budget);
		}
		if ((int64_t)(now - g_w[id].deadline) > 0 && !g_w[id].deadline_flagged) {
			g_w[id].deadline_flagged = 1;
			w_record(id, WCET_KIND_DEADLINE, now - g_w[id].deadline);
		}
	}
	spin_unlock(&g_w_lock);
	smp_irq_restore(d);
}

void
wcet_stats(uint32_t *total_overruns, int *worst_task, uint64_t *worst_us)
{
	if (total_overruns) *total_overruns = g_total;
	if (worst_task)     *worst_task = g_worst_task;
	if (worst_us)       *worst_us = g_worst_us;
}

/* ------------------------------------------------------------------ *
 * Deterministic self-test with synthetic timestamps (no scheduler needed):
 * one on-time release (no overrun) followed by one over-budget release
 * (exactly one budget overrun expected).
 * ------------------------------------------------------------------ */
void
wcet_selftest(void)
{
	uint64_t f = timer_freq();
	uint64_t per_us = f / 1000000ull;   /* ticks per microsecond */
	uint64_t base = timer_now();
	uint32_t start_total;
	int i;

	/* Reset monitor state. */
	{
		uint64_t d = smp_irq_save();
		spin_lock(&g_w_lock);
		for (i = 0; i < SCHED_MAX_TASKS; i++) {
			g_w[i].declared = 0;
			g_w[i].active = 0;
		}
		g_total = 0;
		g_budget_cnt = 0;
		g_deadline_cnt = 0;
		g_worst_task = -1;
		g_worst_us = 0;
		g_last_task = -1;
		g_last_kind = 0;
		g_last_us = 0;
		g_declared_n = 0;
		spin_unlock(&g_w_lock);
		smp_irq_restore(d);
	}

	wcet_declare(0, 1000, 500);   /* period 1000us, budget 500us */
	start_total = g_total;

	/* Release 1: completes in 400us — within budget, before deadline. */
	wcet_release_at(0, base);
	wcet_complete_at(0, base + per_us * 400ull);

	/* Release 2: completes in 700us — 200us over budget (but still before the
	 * 1000us deadline, so exactly ONE overrun: budget). */
	wcet_release_at(0, base + per_us * 2000ull);
	wcet_complete_at(0, base + per_us * 2000ull + per_us * 700ull);

	{
		uint32_t pass = (g_total == start_total + 1) &&
		                (g_budget_cnt == 1) &&
		                (g_deadline_cnt == 0) &&
		                (g_last_kind == WCET_KIND_BUDGET) &&
		                (g_last_task == 0);
		uint64_t d = smp_irq_save();
		spin_lock(&g_w_lock);
		/* Overwrite word [1] region already reflects real counts; add a clear
		 * pass marker in an unused slot is unnecessary — the counts ARE the
		 * result. Re-publish to be safe, then stash pass in worst-unused? Keep
		 * the canonical layout; readers check [1]==1 && [2]==1 && [3]==0. */
		(void)pass;
		w_publish();
		spin_unlock(&g_w_lock);
		smp_irq_restore(d);
	}
}
