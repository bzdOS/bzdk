/* ktimer.c — software timers / timeouts. See ktimer.h for the API contract.
 *
 * ---------------------------------------------------------------------------
 * TICK INTEGRATION (paste into el2_exc.c's el2_trap(), the IRQ/FIQ arm — this
 * file and el2_exc.c are owned by different lanes, so this is a SNIPPET, not an
 * edit made here). Place it AFTER gic_timer_irq() (which ACKs/EOIs/re-arms) and
 * BEFORE sched_tick():
 *
 *     if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
 *         ...
 *         gic_timer_irq(frame);
 *         uint64_t __now = timer_now();
 *         ktimer_service(__now);   // fire due software timers (wakes ksleep-ers,
 *                                  //   sem/mbox timeouts) — MUST be OUTSIDE any
 *                                  //   sched lock: its callbacks call sched_wake()
 *         wcet_tick(__now);        // flag a running task that overran its budget
 *         sched_tick(frame);       // preemptive switch (picks up newly-woken tasks)
 *         return;
 *     }
 *
 * WHY NOT INSIDE sched_tick(): sched_tick() holds g_sched_lock across its body,
 * and a fired timer's callback (ksleep wake, sem timeout) calls sched_wake(),
 * which takes g_sched_lock — servicing timers from inside sched_tick would
 * self-deadlock. Servicing them in el2_trap, before sched_tick, holds no sched
 * lock; the woken tasks are READY by the time sched_tick picks the next one, so
 * a task's wake latency is still a single tick.
 *
 * For SMP secondaries (smp_timer_irq path) the same two lines may be added; each
 * due timer still fires exactly once (popped under ktimer's lock).
 * ---------------------------------------------------------------------------
 */
#include <stdint.h>
#include "ktimer.h"
#include "ksync.h"
#include "timer.h"
#include "sched.h"
#include "smp.h"

/* ------------------------------------------------------------------ *
 * Timer table + its own spinlock.
 * ------------------------------------------------------------------ */
struct ktimer_slot {
	uint64_t deadline;       /* absolute fire time, in timer_now() ticks */
	uint64_t period;         /* re-arm interval in ticks; 0 == one-shot   */
	void   (*cb)(void *);
	void    *arg;
	uint32_t active;
	uint32_t gen;            /* bumped each (re)use for stale-handle detection */
};

static struct ktimer_slot g_kt[KTIMER_MAX];
static spinlock_t g_kt_lock = SPINLOCK_INIT;

/* handle <-> slot: handle = (gen << 8) | (idx + 1); 0 == invalid. idx fits in
 * the low byte since KTIMER_MAX <= 255. */
static inline ktimer_t kt_mkhandle(uint32_t idx, uint32_t gen)
{
	return (ktimer_t)((gen << 8) | (idx + 1u));
}
static inline int kt_handle_idx(ktimer_t h) { return (int)((h & 0xffu)) - 1; }
static inline uint32_t kt_handle_gen(ktimer_t h) { return h >> 8; }

static inline uint64_t kt_us_to_ticks(uint64_t us)
{
	/* ticks = us * freq / 1e6, computed without overflow for sane us. */
	uint64_t f = timer_freq();
	return (us * f) / 1000000ull;
}

static ktimer_t
kt_arm(uint64_t us, uint64_t period_us, void (*cb)(void *), void *arg)
{
	ktimer_t h = 0;
	int i;
	uint64_t now = timer_now();
	uint64_t d = kt_us_to_ticks(us);
	uint64_t d64 = smp_irq_save();

	if (d == 0)
		d = 1;   /* never in the past — soonest is the next serviced tick */

	spin_lock(&g_kt_lock);
	for (i = 0; i < KTIMER_MAX; i++) {
		if (!g_kt[i].active) {
			g_kt[i].deadline = now + d;
			g_kt[i].period   = kt_us_to_ticks(period_us);
			g_kt[i].cb       = cb;
			g_kt[i].arg      = arg;
			g_kt[i].active   = 1;
			g_kt[i].gen++;
			h = kt_mkhandle((uint32_t)i, g_kt[i].gen);
			break;
		}
	}
	spin_unlock(&g_kt_lock);
	smp_irq_restore(d64);
	return h;
}

ktimer_t
ktimer_after(uint64_t us, void (*cb)(void *), void *arg)
{
	return kt_arm(us, 0, cb, arg);
}

ktimer_t
ktimer_every(uint64_t us, void (*cb)(void *), void *arg)
{
	return kt_arm(us, us, cb, arg);
}

int
ktimer_cancel(ktimer_t h)
{
	int idx = kt_handle_idx(h);
	int rc = 0;
	uint64_t d;

	if (h == 0 || idx < 0 || idx >= KTIMER_MAX)
		return 0;

	d = smp_irq_save();
	spin_lock(&g_kt_lock);
	if (g_kt[idx].active && g_kt[idx].gen == kt_handle_gen(h)) {
		g_kt[idx].active = 0;
		rc = 1;
	}
	spin_unlock(&g_kt_lock);
	smp_irq_restore(d);
	return rc;
}

void
ktimer_service(uint64_t now)
{
	int i;
	uint64_t d;

	/* Bounded loop: each pass fires at most one timer with the ktimer lock
	 * RELEASED during the callback (so callbacks may take other locks). We
	 * cap iterations at KTIMER_MAX so a pathological periodic timer with a
	 * tiny period can't spin us forever within one service call. */
	for (i = 0; i < KTIMER_MAX; i++) {
		void (*cb)(void *) = 0;
		void *arg = 0;
		int fired = 0;
		int j;

		d = smp_irq_save();
		spin_lock(&g_kt_lock);
		for (j = 0; j < KTIMER_MAX; j++) {
			if (g_kt[j].active && (int64_t)(now - g_kt[j].deadline) >= 0) {
				cb  = g_kt[j].cb;
				arg = g_kt[j].arg;
				if (g_kt[j].period) {
					/* Periodic: re-arm from the SCHEDULED deadline so the
					 * period doesn't drift with service latency. Skip missed
					 * periods if we fell far behind (deadline stays ahead of
					 * now, so we don't fire a burst). */
					do {
						g_kt[j].deadline += g_kt[j].period;
					} while ((int64_t)(now - g_kt[j].deadline) >= 0);
				} else {
					g_kt[j].active = 0;   /* one-shot: consume */
				}
				fired = 1;
				break;
			}
		}
		spin_unlock(&g_kt_lock);
		smp_irq_restore(d);

		if (!fired)
			break;
		if (cb)
			cb(arg);           /* lock released: safe to sched_wake(), etc. */
	}
}

/* ------------------------------------------------------------------ *
 * ksleep — block the current task on a one-shot wake timer.
 * ------------------------------------------------------------------ */
static void
ksleep_wake_cb(void *arg)
{
	kwake((int)(intptr_t)arg);
}

void
ksleep(uint64_t us)
{
	int id = sched_current_task();

	if (id < 0) {
		/* No scheduler task context — fall back to a bounded busy-wait. */
		while (us > 0) {
			uint32_t chunk = (us > 1000000ull) ? 1000000u : (uint32_t)us;
			timer_delay_us(chunk);
			us -= chunk;
		}
		return;
	}

	/* Arm the wake, then block. kblock()'s per-task wake token makes this
	 * race-free even if the timer fires before we finish blocking. */
	ktimer_after(us, ksleep_wake_cb, (void *)(intptr_t)id);
	kblock();
}

/* ------------------------------------------------------------------ *
 * Breadcrumb helper (dc civac + dsb — MMU/D-cache are on).
 * ------------------------------------------------------------------ */
static inline void
ktmr_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(KTMR_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Deterministic self-test — drives ktimer_service() with synthetic `now`
 * values, so it needs neither the scheduler nor the live tick.
 * ------------------------------------------------------------------ */
static volatile uint32_t st_oneshot_fires;
static volatile uint32_t st_periodic_fires;
static ktimer_t st_periodic_h;

static void st_oneshot_cb(void *a) { (void)a; st_oneshot_fires++; }
static void st_periodic_cb(void *a) { (void)a; st_periodic_fires++; }

void
ktimer_selftest(void)
{
	uint32_t fail = 0;
	uint32_t cancel_ok = 0;
	int i;
	uint64_t f = timer_freq();
	uint64_t base = timer_now();

	/* Reset table + counters. */
	for (i = 0; i < KTIMER_MAX; i++) {
		g_kt[i].active = 0;
		g_kt[i].period = 0;
	}
	st_oneshot_fires = 0;
	st_periodic_fires = 0;

	/* One-shot 1000 us out; periodic every 1000 us. */
	{
		ktimer_t oh = ktimer_after(1000, st_oneshot_cb, 0);
		st_periodic_h = ktimer_every(1000, st_periodic_cb, 0);
		if (oh == 0 || st_periodic_h == 0)
			fail = 1;
	}

	/* Before the first deadline: nothing should fire. `base` was captured
	 * BEFORE arming, and kt_arm() timestamps from its own (slightly later)
	 * timer_now(), so the real deadlines are at >= base+1000us. base+500us is
	 * safely before the first one. */
	ktimer_service(base + (f / 1000000ull) * 500ull);   /* +500us */
	if (st_oneshot_fires != 0 || st_periodic_fires != 0)
		fail = fail ? fail : 2;

	/* Service at the HALF-period point of each of the 5 periods
	 * (base + 1000*i + 500 us). The +500us offset absorbs the arm-time drift
	 * (kt_arm's now is a few ticks past `base`) so the i-th service reliably
	 * lands inside the i-th period window — one periodic fire per call, five
	 * total. Each service crosses exactly one boundary (steps are one full
	 * period apart), and the one-shot fires exactly once on the first call. */
	for (i = 1; i <= 5; i++)
		ktimer_service(base + (f / 1000000ull) * (uint64_t)(1000 * i + 500));

	if (st_oneshot_fires != 1)
		fail = fail ? fail : 3;
	if (st_periodic_fires != 5)
		fail = fail ? fail : 4;

	/* Cancel the periodic; further service calls must not fire it. */
	if (ktimer_cancel(st_periodic_h)) {
		uint32_t before = st_periodic_fires;
		ktimer_service(base + (f / 1000000ull) * 100000ull);   /* far future */
		cancel_ok = (st_periodic_fires == before) ? 1u : 0u;
		if (!cancel_ok)
			fail = fail ? fail : 5;
	} else {
		fail = fail ? fail : 6;
	}

	ktmr_bc(0, KTMR_BC_MAGIC);
	ktmr_bc(1, fail ? 0u : 1u);
	ktmr_bc(2, fail);
	ktmr_bc(3, st_oneshot_fires);
	ktmr_bc(4, st_periodic_fires);
	ktmr_bc(5, cancel_ok);

	/* Leave the table clean for real use. */
	for (i = 0; i < KTIMER_MAX; i++)
		g_kt[i].active = 0;
}
