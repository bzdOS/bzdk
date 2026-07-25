/* SPDX-License-Identifier: BSD-2-Clause */

/* ksync.c — mutex (priority inheritance), counting semaphore, mailbox, and the
 * robust block/wake core they share. See ksync.h for the API contract.
 *
 * ---------------------------------------------------------------------------
 * REQUIRED SCHEDULER ADDITIONS — paste these two functions into sched.c (that
 * file is owned by another lane; these are the ONLY new scheduler entry points
 * ksync/ktimer need beyond what sched.h already exports). Both are O(1) and use
 * sched.c's existing sched_lock()/rl_unlink()/rl_push_tail()/g_tasks:
 *
 *   // Token-aware block of the CURRENT task, WITHOUT spinning. Under the run-
 *   // queue lock: if *token != 0 a wake already arrived -> do NOT block, return
 *   // 0; else mark the current task BLOCKED and return 1. Because it runs under
 *   // g_sched_lock (which sched_wake() also takes) it serializes with the
 *   // waker, closing the lost-wakeup race with NO per-core assumptions and no
 *   // double-schedule hazard. The caller does its own WFI wait if it returns 1.
 *   int sched_block_if(volatile uint32_t *token)
 *   {
 *       uint32_t cpu = smp_cpu_id();
 *       uint64_t daif = sched_lock();
 *       int id = (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;
 *       int blocked = 0;
 *       if (id >= 0 && *token == 0) {
 *           g_tasks[id].state = TASK_BLOCKED;
 *           blocked = 1;
 *       }
 *       sched_unlock(daif);
 *       return blocked;
 *   }
 *
 *   // Priority inheritance support: change a task's fixed priority and, if it
 *   // is currently READY, move it to the tail of the new priority level.
 *   int sched_set_priority(int taskid, int new_prio)
 *   {
 *       uint64_t daif;
 *       if (taskid < 0 || taskid >= SCHED_MAX_TASKS ||
 *           new_prio < 0 || new_prio >= SCHED_MAX_PRIO)
 *           return -1;
 *       daif = sched_lock();
 *       if (g_tasks[taskid].used) {
 *           if (g_tasks[taskid].state == TASK_READY) {
 *               rl_unlink(taskid);
 *               g_tasks[taskid].priority = (uint32_t)new_prio;
 *               rl_push_tail(taskid);
 *           } else {
 *               g_tasks[taskid].priority = (uint32_t)new_prio;
 *           }
 *       }
 *       sched_unlock(daif);
 *       return 0;
 *   }
 *
 * And declare them in sched.h:
 *   int  sched_block_if(volatile uint32_t *token);
 *   int  sched_set_priority(int taskid, int new_prio);
 * ---------------------------------------------------------------------------
 */
#include <stdint.h>
#include "ksync.h"
#include "ktimer.h"
#include "sched.h"
#include "timer.h"
#include "smp.h"

/* The two scheduler additions documented above (declared here in case sched.h
 * has not yet been updated — the definitions live in sched.c). */
extern int  sched_block_if(volatile uint32_t *token);
extern int  sched_set_priority(int taskid, int new_prio);

/* ------------------------------------------------------------------ *
 * Block/wake core.
 *
 * Per-task wake token: kwake() stores the token BEFORE sched_wake(), and
 * kblock() consumes it. sched_block_if() checks the token AND marks the task
 * BLOCKED atomically under the run-queue lock (which sched_wake() also takes),
 * so the waker and the blocker are fully serialized — a wake that races ahead
 * of the block is never lost, with no per-core assumptions and no chance of the
 * task being run on two cores. kwake() from another core (or the tick) is safe.
 * ------------------------------------------------------------------ */
static volatile uint32_t g_kwake[SCHED_MAX_TASKS];

void
kwake(int taskid)
{
	if (taskid < 0 || taskid >= SCHED_MAX_TASKS)
		return;
	g_kwake[taskid] = 1;                 /* publish token BEFORE the wake */
	__asm__ volatile("dsb sy" ::: "memory");
	sched_wake(taskid);                  /* if already BLOCKED, relink READY */
}

int
kblock(void)
{
	int id = sched_current_task();
	const struct sched_task *t;

	if (id < 0)
		return -1;

	for (;;) {
		/* Atomic under the run-queue lock: blocks only if no wake is pending. */
		if (sched_block_if(&g_kwake[id])) {
			/* We are BLOCKED; a tick has (or will) switch us off the core.
			 * We resume here once kwake()->sched_wake() marks us runnable. */
			t = sched_task_info(id);
			while (t && t->state == TASK_BLOCKED)
				__asm__ volatile("wfi" ::: "memory");
		}
		/* Either a wake was already pending (didn't block) or we were woken.
		 * kwake() always sets the token before sched_wake(), so a genuine wake
		 * implies the token is set — consume it and return. */
		if (g_kwake[id]) {
			g_kwake[id] = 0;
			return 0;
		}
		/* No token: sched_block_if declined for another reason (e.g. no task) —
		 * loop; the id<0 case was handled above so this converges. */
	}
}

/* Pick the highest-priority (numerically lowest priority value) task id set in
 * `waiters` (an 8-bit bitmap over task ids). Returns -1 if empty. Bounded scan
 * over SCHED_MAX_TASKS. */
static int
pick_highest_waiter(uint32_t waiters)
{
	int best = -1;
	uint32_t best_prio = 0xffffffffu;
	int i;

	for (i = 0; i < SCHED_MAX_TASKS; i++) {
		if (waiters & (1u << i)) {
			const struct sched_task *t = sched_task_info(i);
			uint32_t p = t ? t->priority : 0xfffffffeu;
			if (p < best_prio) {
				best_prio = p;
				best = i;
			}
		}
	}
	return best;
}

static inline int
cur_prio(int id)
{
	const struct sched_task *t = sched_task_info(id);
	return t ? (int)t->priority : -1;
}

/* ------------------------------------------------------------------ *
 * Mutex with priority inheritance.
 * ------------------------------------------------------------------ */
void
kmutex_init(struct kmutex *m)
{
	m->lock.lock = 0;
	m->held = 0;
	m->owner = -1;
	m->owner_base = -1;
	m->waiters = 0;
	m->boosted = 0;
}

int
kmutex_trylock(struct kmutex *m)
{
	int self = sched_current_task();
	int got = 0;
	uint64_t d = smp_irq_save();

	spin_lock(&m->lock);
	if (!m->held) {
		m->held = 1;
		m->owner = self;
		m->owner_base = (self >= 0) ? cur_prio(self) : -1;
		m->boosted = 0;
		got = 1;
	}
	spin_unlock(&m->lock);
	smp_irq_restore(d);
	return got;
}

void
kmutex_lock(struct kmutex *m)
{
	int self = sched_current_task();

	if (self < 0) {
		/* No task context: degrade to a bounded spin acquire. */
		for (;;) {
			if (kmutex_trylock(m))
				return;
			__asm__ volatile("wfe" ::: "memory");
		}
	}

	for (;;) {
		uint64_t d = smp_irq_save();
		spin_lock(&m->lock);

		if (!m->held) {
			m->held = 1;
			m->owner = self;
			m->owner_base = cur_prio(self);
			m->boosted = 0;
			spin_unlock(&m->lock);
			smp_irq_restore(d);
			return;
		}

		/* Contended. Priority inheritance: if we outrank the holder, boost the
		 * holder to our priority so it can't be preempted by a mid-priority
		 * task while we wait on it (bounds priority inversion). */
		{
			int op = cur_prio(m->owner);
			int sp = cur_prio(self);
			if (sp >= 0 && op >= 0 && sp < op) {
				sched_set_priority(m->owner, sp);
				m->boosted = 1;
			}
		}
		m->waiters |= (1u << self);
		spin_unlock(&m->lock);
		smp_irq_restore(d);

		kblock();                    /* wait for a handoff from kmutex_unlock */

		/* Woken. If unlock handed ownership directly to us, we're done;
		 * otherwise (spurious) loop and retry. */
		d = smp_irq_save();
		spin_lock(&m->lock);
		if (m->owner == self) {
			spin_unlock(&m->lock);
			smp_irq_restore(d);
			return;
		}
		spin_unlock(&m->lock);
		smp_irq_restore(d);
	}
}

void
kmutex_unlock(struct kmutex *m)
{
	int self = sched_current_task();
	int next;
	int restore_prio;
	uint64_t d = smp_irq_save();

	spin_lock(&m->lock);
	if (m->owner != self && self >= 0) {
		/* Not the owner — ignore (defensive). */
		spin_unlock(&m->lock);
		smp_irq_restore(d);
		return;
	}

	/* Restore our (the outgoing holder's) base priority if we were boosted. */
	restore_prio = m->owner_base;
	if (m->boosted && self >= 0 && restore_prio >= 0)
		sched_set_priority(self, restore_prio);
	m->boosted = 0;

	next = pick_highest_waiter(m->waiters);
	if (next >= 0) {
		/* Direct priority-ordered handoff: the highest-priority waiter becomes
		 * the new owner and is woken already holding the mutex (no re-acquire
		 * race). Its base priority is recorded for its own eventual unlock. */
		m->waiters &= ~(1u << next);
		m->held = 1;                 /* ownership passes directly, stays held */
		m->owner = next;
		m->owner_base = cur_prio(next);
		spin_unlock(&m->lock);
		smp_irq_restore(d);
		kwake(next);
	} else {
		m->held = 0;
		m->owner = -1;
		m->owner_base = -1;
		spin_unlock(&m->lock);
		smp_irq_restore(d);
	}
}

/* ------------------------------------------------------------------ *
 * Counting semaphore.
 * ------------------------------------------------------------------ */
void
ksem_init(struct ksem *s, int32_t initial)
{
	s->lock.lock = 0;
	s->count = initial;
	s->waiters = 0;
}

int
ksem_trywait(struct ksem *s)
{
	int got = 0;
	uint64_t d = smp_irq_save();

	spin_lock(&s->lock);
	if (s->count > 0) {
		s->count--;
		got = 1;
	}
	spin_unlock(&s->lock);
	smp_irq_restore(d);
	return got;
}

void
ksem_wait(struct ksem *s)
{
	int self = sched_current_task();

	for (;;) {
		uint64_t d = smp_irq_save();
		spin_lock(&s->lock);
		if (s->count > 0) {
			s->count--;
			spin_unlock(&s->lock);
			smp_irq_restore(d);
			return;
		}
		if (self < 0) {
			/* No task context — spin-wait for a permit. */
			spin_unlock(&s->lock);
			smp_irq_restore(d);
			__asm__ volatile("wfe" ::: "memory");
			continue;
		}
		s->waiters |= (1u << self);
		spin_unlock(&s->lock);
		smp_irq_restore(d);

		kblock();
		/* Woken by ksem_post's direct handoff (it cleared our waiter bit and
		 * did NOT increment count — the permit is ours). Confirm and return. */
		d = smp_irq_save();
		spin_lock(&s->lock);
		if (!(s->waiters & (1u << self))) {
			spin_unlock(&s->lock);
			smp_irq_restore(d);
			return;
		}
		/* Still marked waiting => spurious wake; loop. */
		spin_unlock(&s->lock);
		smp_irq_restore(d);
	}
}

/* Timeout support: the ktimer callback flags the waiter and wakes it. */
struct sem_to_ctx {
	struct ksem *s;
	int taskid;
	volatile uint32_t timed_out;
	volatile uint32_t done;    /* 1 once resolved (took permit or timed out) */
};

static void
sem_timeout_cb(void *arg)
{
	struct sem_to_ctx *c = (struct sem_to_ctx *)arg;
	uint64_t d = smp_irq_save();

	spin_lock(&c->s->lock);
	if (!c->done && (c->s->waiters & (1u << c->taskid))) {
		c->s->waiters &= ~(1u << c->taskid);   /* pull the waiter */
		c->timed_out = 1;
		c->done = 1;
	}
	spin_unlock(&c->s->lock);
	smp_irq_restore(d);

	if (c->timed_out)
		kwake(c->taskid);
}

int
ksem_wait_timeout(struct ksem *s, uint64_t us)
{
	int self = sched_current_task();
	struct sem_to_ctx ctx;
	ktimer_t th;
	uint64_t d;

	if (self < 0)
		return ksem_trywait(s) ? 0 : -1;

	d = smp_irq_save();
	spin_lock(&s->lock);
	if (s->count > 0) {
		s->count--;
		spin_unlock(&s->lock);
		smp_irq_restore(d);
		return 0;
	}
	ctx.s = s;
	ctx.taskid = self;
	ctx.timed_out = 0;
	ctx.done = 0;
	s->waiters |= (1u << self);
	spin_unlock(&s->lock);
	smp_irq_restore(d);

	th = ktimer_after(us, sem_timeout_cb, &ctx);

	kblock();

	/* Resolve: either ksem_post handed us the permit (cleared our bit,
	 * !timed_out) or the timeout fired (timed_out set). Cancel the timer if it
	 * is still pending, and settle any last race under the lock. */
	ktimer_cancel(th);
	d = smp_irq_save();
	spin_lock(&s->lock);
	if (ctx.timed_out) {
		spin_unlock(&s->lock);
		smp_irq_restore(d);
		return -1;
	}
	if (!(s->waiters & (1u << self))) {
		/* Permit handed to us by post(). */
		ctx.done = 1;
		spin_unlock(&s->lock);
		smp_irq_restore(d);
		return 0;
	}
	/* Still queued and not timed out (spurious) — pull ourselves and report a
	 * timeout rather than block again (bounded, deterministic). */
	s->waiters &= ~(1u << self);
	spin_unlock(&s->lock);
	smp_irq_restore(d);
	return -1;
}

void
ksem_post(struct ksem *s)
{
	int next;
	uint64_t d = smp_irq_save();

	spin_lock(&s->lock);
	next = pick_highest_waiter(s->waiters);
	if (next >= 0) {
		/* Direct handoff: clear the waiter, do NOT bump count — the permit goes
		 * straight to the woken task. */
		s->waiters &= ~(1u << next);
		spin_unlock(&s->lock);
		smp_irq_restore(d);
		kwake(next);
	} else {
		s->count++;
		spin_unlock(&s->lock);
		smp_irq_restore(d);
	}
}

/* ------------------------------------------------------------------ *
 * Mailbox / fixed-size message queue.
 * ------------------------------------------------------------------ */
static void
mb_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

void
kmbox_init(struct kmbox *mb, void *storage, uint32_t msg_size, uint32_t capacity)
{
	mb->lock.lock = 0;
	mb->buf = (uint8_t *)storage;
	mb->msg_size = msg_size;
	mb->capacity = capacity;
	mb->head = 0;
	mb->tail = 0;
	mb->count = 0;
	mb->rx_waiters = 0;
}

int
kmbox_send(struct kmbox *mb, const void *msg)
{
	int wake = -1;
	uint64_t d = smp_irq_save();

	spin_lock(&mb->lock);
	if (mb->count >= mb->capacity) {
		spin_unlock(&mb->lock);
		smp_irq_restore(d);
		return -1;   /* full */
	}
	mb_copy(mb->buf + (uint64_t)mb->head * mb->msg_size,
	        (const uint8_t *)msg, mb->msg_size);
	mb->head = (mb->head + 1) % mb->capacity;
	mb->count++;
	if (mb->rx_waiters) {
		wake = pick_highest_waiter(mb->rx_waiters);
		if (wake >= 0)
			mb->rx_waiters &= ~(1u << wake);
	}
	spin_unlock(&mb->lock);
	smp_irq_restore(d);

	if (wake >= 0)
		kwake(wake);
	return 0;
}

int
kmbox_tryrecv(struct kmbox *mb, void *out)
{
	int got = 0;
	uint64_t d = smp_irq_save();

	spin_lock(&mb->lock);
	if (mb->count > 0) {
		mb_copy((uint8_t *)out,
		        mb->buf + (uint64_t)mb->tail * mb->msg_size, mb->msg_size);
		mb->tail = (mb->tail + 1) % mb->capacity;
		mb->count--;
		got = 1;
	}
	spin_unlock(&mb->lock);
	smp_irq_restore(d);
	return got;
}

void
kmbox_recv(struct kmbox *mb, void *out)
{
	int self = sched_current_task();

	for (;;) {
		uint64_t d = smp_irq_save();
		spin_lock(&mb->lock);
		if (mb->count > 0) {
			mb_copy((uint8_t *)out,
			        mb->buf + (uint64_t)mb->tail * mb->msg_size, mb->msg_size);
			mb->tail = (mb->tail + 1) % mb->capacity;
			mb->count--;
			spin_unlock(&mb->lock);
			smp_irq_restore(d);
			return;
		}
		if (self < 0) {
			spin_unlock(&mb->lock);
			smp_irq_restore(d);
			__asm__ volatile("wfe" ::: "memory");
			continue;
		}
		mb->rx_waiters |= (1u << self);
		spin_unlock(&mb->lock);
		smp_irq_restore(d);

		kblock();
		/* Loop: re-check for a message (send() cleared our waiter bit and woke
		 * us, but does not stash the message — we dequeue on the next pass). */
	}
}

/* ================================================================== *
 * Breadcrumb helper.
 * ================================================================== */
static inline void
ksy_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(KSY_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ================================================================== *
 * Deterministic single-threaded unit self-test (no scheduler needed): checks
 * the NON-blocking semantics of each primitive.
 * ================================================================== */
#define KSY_FAIL_NONE       0u
#define KSY_FAIL_MUTEX      1u
#define KSY_FAIL_SEM        2u
#define KSY_FAIL_MBOX_FIFO  3u
#define KSY_FAIL_MBOX_FULL  4u
#define KSY_FAIL_MBOX_EMPTY 5u

void
ksync_selftest(void)
{
	uint32_t fail = KSY_FAIL_NONE;
	uint32_t roundtrip = 0;
	int32_t sem_final = 0;

	/* --- mutex: trylock/ownership (no task context => spin-degrade path is
	 * avoided by using only trylock/unlock here). --- */
	{
		struct kmutex m;
		kmutex_init(&m);
		if (!kmutex_trylock(&m))            fail = KSY_FAIL_MUTEX; /* free -> ok */
		else if (kmutex_trylock(&m))        fail = KSY_FAIL_MUTEX; /* held -> fail */
		else {
			kmutex_unlock(&m);
			if (!kmutex_trylock(&m))        fail = KSY_FAIL_MUTEX; /* free again */
			else kmutex_unlock(&m);
		}
	}

	/* --- semaphore: count + trywait boundary. --- */
	if (!fail) {
		struct ksem s;
		ksem_init(&s, 2);
		if (!ksem_trywait(&s))              fail = KSY_FAIL_SEM;   /* 2->1 */
		else if (!ksem_trywait(&s))         fail = KSY_FAIL_SEM;   /* 1->0 */
		else if (ksem_trywait(&s))          fail = KSY_FAIL_SEM;   /* 0 -> none */
		else {
			ksem_post(&s);                  /* 0->1 (no waiters) */
			ksem_post(&s);                  /* 1->2 */
			if (!ksem_trywait(&s))          fail = KSY_FAIL_SEM;
			sem_final = s.count;            /* expect 1 */
		}
	}

	/* --- mailbox: FIFO ordering, full, empty. --- */
	if (!fail) {
		static uint8_t store[4 * sizeof(uint32_t)];
		struct kmbox mb;
		uint32_t i, v;
		kmbox_init(&mb, store, sizeof(uint32_t), 4);

		for (i = 0; i < 4; i++) {
			uint32_t msg = 0x1000u + i;
			if (kmbox_send(&mb, &msg))      { fail = KSY_FAIL_MBOX_FIFO; break; }
		}
		if (!fail) {
			uint32_t extra = 0xdead;
			if (kmbox_send(&mb, &extra) != -1)   /* 5th must fail (full) */
				fail = KSY_FAIL_MBOX_FULL;
		}
		if (!fail) {
			for (i = 0; i < 4; i++) {
				if (!kmbox_tryrecv(&mb, &v)) { fail = KSY_FAIL_MBOX_FIFO; break; }
				if (v != 0x1000u + i)        { fail = KSY_FAIL_MBOX_FIFO; break; }
				roundtrip++;
			}
		}
		if (!fail) {
			if (kmbox_tryrecv(&mb, &v))          /* now empty */
				fail = KSY_FAIL_MBOX_EMPTY;
		}
	}

	ksy_bc(0, KSY_BC_MAGIC);
	ksy_bc(1, fail ? 0u : 1u);
	ksy_bc(2, fail);
	ksy_bc(3, roundtrip);
	ksy_bc(4, (uint32_t)sem_final);
}

/* ================================================================== *
 * Live multi-task demo: producer + consumer over a mailbox, a shared counter
 * under a mutex (contended by both, exercising priority inheritance), and a
 * periodic timer. Bounded: each task runs DEMO_ITERS then parks (sched_block).
 *
 * Requires, before sched_start(): the EL2 tick armed (`tick`) AND the el2_trap
 * ktimer_service()/wcet_tick() integration snippet applied (so ksleep and the
 * periodic timer make progress). Read results at bc 0x50005500 words [5..9].
 * ================================================================== */
#define DEMO_ITERS       50
#define DEMO_STACK_SIZE  2048

extern void sched_block(void);   /* park forever after finishing */

static struct kmutex demo_mtx;
static struct ksem   demo_slots;           /* backpressure: free mailbox slots */
static struct kmbox  demo_mb;
static uint32_t      demo_mb_store[8];      /* 8 msgs x uint32_t */

static volatile uint32_t demo_shared;       /* protected by demo_mtx */
static volatile uint32_t demo_prod_iters;
static volatile uint32_t demo_cons_iters;
static volatile uint32_t demo_cs_entries;
static volatile uint32_t demo_ticks;
static volatile uint32_t demo_pass;

static uint8_t demo_stack_prod[DEMO_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t demo_stack_cons[DEMO_STACK_SIZE] __attribute__((aligned(16)));

static void
demo_bc(void)
{
	ksy_bc(0, KSY_BC_MAGIC);
	ksy_bc(5, demo_prod_iters);
	ksy_bc(6, demo_cons_iters);
	ksy_bc(7, demo_cs_entries);
	ksy_bc(8, demo_ticks);
	ksy_bc(9, demo_pass);
}

static void
demo_timer_cb(void *a)
{
	(void)a;
	demo_ticks++;
}

static void
demo_producer(void *arg)
{
	uint32_t i;
	(void)arg;

	ktimer_every(5000, demo_timer_cb, 0);   /* periodic 5 ms heartbeat */

	for (i = 0; i < DEMO_ITERS; i++) {
		uint32_t msg = i;

		ksem_wait(&demo_slots);              /* wait for a free mailbox slot */

		/* Contended critical section (consumer also takes demo_mtx). */
		kmutex_lock(&demo_mtx);
		demo_shared++;
		demo_cs_entries++;
		kmutex_unlock(&demo_mtx);

		kmbox_send(&demo_mb, &msg);
		demo_prod_iters++;
		demo_bc();
		ksleep(1000);                        /* 1 ms pacing (exercises ktimer) */
	}
	sched_block();                           /* park */
}

static void
demo_consumer(void *arg)
{
	uint32_t i, v;
	(void)arg;

	for (i = 0; i < DEMO_ITERS; i++) {
		kmbox_recv(&demo_mb, &v);            /* blocks until a message */

		kmutex_lock(&demo_mtx);
		demo_shared++;
		demo_cs_entries++;
		kmutex_unlock(&demo_mtx);

		demo_cons_iters++;
		ksem_post(&demo_slots);              /* return the slot */
		demo_bc();
	}

	/* Pass: consumer drained everything the producer sent, and the mutex-
	 * protected counter equals the total critical-section entries (no lost
	 * updates / no torn increments). */
	if (demo_cons_iters == DEMO_ITERS &&
	    demo_shared == demo_cs_entries &&
	    demo_cs_entries == 2u * DEMO_ITERS)
		demo_pass = 1;
	demo_bc();
	sched_block();                           /* park */
}

void
ksync_demo_setup(void)
{
	kmutex_init(&demo_mtx);
	ksem_init(&demo_slots, 8);               /* == mailbox capacity */
	kmbox_init(&demo_mb, demo_mb_store, sizeof(uint32_t), 8);

	demo_shared = 0;
	demo_prod_iters = 0;
	demo_cons_iters = 0;
	demo_cs_entries = 0;
	demo_ticks = 0;
	demo_pass = 0;
	demo_bc();

	/* Consumer higher priority (0) than producer (1): consumer preempts to
	 * drain, then blocks on the empty mailbox, letting the producer run. */
	sched_add(demo_consumer, 0, demo_stack_cons + sizeof(demo_stack_cons),
	          sizeof(demo_stack_cons), 0);
	sched_add(demo_producer, 0, demo_stack_prod + sizeof(demo_stack_prod),
	          sizeof(demo_stack_prod), 1);
}
