/* SPDX-License-Identifier: BSD-2-Clause */

/* sched.c — fixed-priority PREEMPTIVE scheduler for the bzdOS EL2 microkernel.
 * See sched.h for the API contract; this file is the mechanism.
 *
 * ---------------------------------------------------------------------
 * THE CONTEXT-SWITCH MECHANISM (read this before touching sched_resume)
 * ---------------------------------------------------------------------
 * exceptions.S's el2_common saves a FULL struct el2_frame (x0..x30, kind,
 * elr, spsr, esr, far, sp_at_entry) on the CURRENT stack, calls el2_trap(),
 * and on return restores that frame and erets. Two facts about that frame
 * matter here:
 *
 *   1. It carries the interrupted task's PC/state (elr, spsr) and all 31
 *      GPRs — everything -mgeneral-regs-only needs (no FP/SIMD to save).
 *   2. It does NOT carry a restorable SP for el2_common's own epilogue:
 *      SP_EL2 is a banked architectural register, not a GPR, and
 *      el2_common's epilogue only does `add sp, sp, #FRAME_SIZE` — a
 *      RELATIVE pop off whatever stack the frame happened to be pushed
 *      on. It cannot hop to a different task's stack by itself.
 *
 * So "mutate *frame and let el2_common's restore+eret resume a different
 * task" only works as-is if every task shared one stack. Real tasks need
 * their own stacks (that's the whole point of sched_add's stack_top), so
 * this scheduler performs the stack hop itself, once, explicitly:
 *
 *   sched_resume(task) loads task->frame.x[1..30] from the TCB, sets
 *   `sp` to task->frame.sp_at_entry (that field is already exactly "the
 *   real SP_EL2 this task was using" — el2_common computes it BEFORE
 *   pushing anything, so a struct-copy `*frame` captures it automatically,
 *   no extra bookkeeping needed), writes ELR_EL2/SPSR_EL2, and erets
 *   directly — all inside one inline-asm block, never falling back into
 *   el2_common's own epilogue. This is safe precisely because `eret` does
 *   not unwind the C call stack (unlike `ret`): it only cares about
 *   ELR_EL2/SPSR_EL2/PC, so calling it from deep inside sched_tick's own
 *   call chain (el2_common -> el2_trap -> sched_tick -> sched_resume)
 *   abandons that call chain's stack frames cleanly — there is nothing to
 *   return to on the outgoing task's stack; its live state is already
 *   fully captured in its TCB.
 *
 * A never-yet-run task (sched_add) and a preempted task (sched_tick) are
 * therefore represented identically (both are just "a struct el2_frame in
 * a TCB") and resumed by the exact same sched_resume() — which is also
 * how sched_start() performs the very first switch from ordinary C: eret
 * does not care whether it is "returning from a trap" or invoked fresh.
 *
 * ---------------------------------------------------------------------
 * READY BITMAP (O(1) picking)
 * ---------------------------------------------------------------------
 * One bit per priority level in g_ready_bitmap; bit P is set iff priority
 * P's ready list is non-empty. Picking the next task is find-first-set
 * (__builtin_ctz, a single clz/rbit-class instruction, not a scan) on the
 * bitmap to get the priority, then g_ready_head[prio] (O(1), no scan of
 * tasks). Each priority level owns a small intrusive doubly-linked ready
 * list (next_ready/prev_ready fields in the TCB) so insert-at-tail,
 * remove-from-anywhere (block) and rotate-to-tail (yield, for round-robin
 * among same-priority peers) are all O(1) pointer fixups — bounded
 * regardless of SCHED_MAX_TASKS.
 *
 * ---------------------------------------------------------------------
 * YIELD / BLOCK — no SVC/BRK trap, a flag honored by the next tick
 * ---------------------------------------------------------------------
 * sched_yield() sets a flag and busy-waits on `wfi`; the SPSR built for
 * every task unmasks IRQ only (D=1,A=1,I=0,F=1 — see SPSR_TASK below), so
 * the next 1ms tick always arrives, sched_tick() observes the flag,
 * rotates that task to the back of its own priority's ready list (so a
 * peer — or itself again, if it has no peers — gets picked), and clears
 * it. sched_block() is similar but simpler: it unlinks itself from the
 * ready list immediately (so the very next tick provably cannot repick
 * it) and spins until some other context calls sched_wake(), which
 * re-links it. Both are bounded: at most one extra tick of latency, no
 * loops over other tasks, no allocation.
 */
#include <stdint.h>
#include "sched.h"
#include "trace.h"
#include "exceptions.h"
#include "timer.h"
#include "smp.h"

/* SPSR_EL2 for every task: mode EL2h (M[3:0]=0b1001=9), 64-bit (M[4]=0),
 * D=1, A=1, I=0 (IRQ UNMASKED — required for preemption), F=1. */
#define SPSR_TASK 0x349UL

/* ------------------------------------------------------------------ *
 * SMP model (v1): ONE global run-queue (the ready lists + bitmap below)
 * protected by g_sched_lock. Each core runs its own task; a task that is
 * RUNNING on some core is UNLINKED from the ready list (so no other core can
 * pick it) and re-linked at the tail of its priority on the next tick. Cores
 * with nothing ready idle in WFI and simply return from the tick.
 *
 * Locking discipline:
 *   - sched_tick() runs in EL2 IRQ context (IRQs already masked) — it takes the
 *     raw spinlock, and RELEASES it before sched_resume()'s eret. It stays
 *     IRQ-masked throughout so a nested tick can't reenter on the same core.
 *   - the cooperative entry points (add/yield/block/wake/start) run in task
 *     context with IRQs unmasked, so they mask IRQ around the lock
 *     (sched_lock/sched_unlock) — otherwise a tick on the same core would spin
 *     forever on a lock its own preempted thread holds (self-deadlock).
 * When only CPU0 is up this all still behaves as the original single-core
 * scheduler (one core, uncontended lock).
 * ------------------------------------------------------------------ */
static struct sched_task g_tasks[SCHED_MAX_TASKS];
static int      g_cpu_current[SMP_MAX_CPUS] = { -1, -1, -1, -1 }; /* per-core RUNNING task */
static uint32_t g_ready_bitmap;      /* bit P set => priority P non-empty */
static int      g_ready_head[SCHED_MAX_PRIO];
static int      g_ready_tail[SCHED_MAX_PRIO];
static uint64_t g_switch_count;
static spinlock_t g_sched_lock = SPINLOCK_INIT;

/* Mask IRQ + take the run-queue lock (task-context callers). Returns the saved
 * DAIF to hand back to sched_unlock(). */
static inline uint64_t sched_lock(void)
{
	uint64_t daif = smp_irq_save();
	spin_lock(&g_sched_lock);
	return daif;
}

static inline void sched_unlock(uint64_t daif)
{
	spin_unlock(&g_sched_lock);
	smp_irq_restore(daif);
}

/* ------------------------------------------------------------------ *
 * Breadcrumb — fixed DRAM window 0x50000a00 ("SCHD"). Distinct from
 * MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300, EL2 exc 0x50000400,
 * TIMR 0x50000500, guest 0x50000b00.
 *   [0]  magic 0x53434844 ("SCHD")
 *   [1]  current task id
 *   [2]  switch count (low32)   [3] switch count (high32)
 *   [4]  ready bitmap
 *   [5]  task count registered
 *   [6..13]  per-task run_ticks, low32, one word per task id 0..7
 * ------------------------------------------------------------------ */
#define SCHD_BC_BASE 0x50000a00UL
#define SCHD_MAGIC   0x53434844u

static inline void
schd_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(SCHD_BC_BASE + (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static int g_ntasks;

static void
bc_update(int next)
{
	int i;

	schd_bc(0, SCHD_MAGIC);
	schd_bc(1, (uint32_t)next);
	schd_bc(2, (uint32_t)g_switch_count);
	schd_bc(3, (uint32_t)(g_switch_count >> 32));
	schd_bc(4, g_ready_bitmap);
	schd_bc(5, (uint32_t)g_ntasks);
	for (i = 0; i < SCHED_MAX_TASKS; i++)
		schd_bc(6 + i, (uint32_t)g_tasks[i].run_ticks);
}

/* ------------------------------------------------------------------ *
 * Ready lists: one intrusive doubly-linked list per priority level.
 * All three ops are O(1) — fixed number of pointer stores, no scanning.
 * ------------------------------------------------------------------ */
static void
rl_push_tail(int id)
{
	struct sched_task *t = &g_tasks[id];
	int p = (int)t->priority;

	t->next_ready = -1;
	t->prev_ready = g_ready_tail[p];
	if (g_ready_tail[p] >= 0)
		g_tasks[g_ready_tail[p]].next_ready = id;
	else
		g_ready_head[p] = id;
	g_ready_tail[p] = id;
	g_ready_bitmap |= (1u << p);
}

static void
rl_unlink(int id)
{
	struct sched_task *t = &g_tasks[id];
	int p = (int)t->priority;

	if (t->prev_ready >= 0)
		g_tasks[t->prev_ready].next_ready = t->next_ready;
	else
		g_ready_head[p] = t->next_ready;

	if (t->next_ready >= 0)
		g_tasks[t->next_ready].prev_ready = t->prev_ready;
	else
		g_ready_tail[p] = t->prev_ready;

	t->next_ready = -1;
	t->prev_ready = -1;

	if (g_ready_head[p] < 0)
		g_ready_bitmap &= ~(1u << p);
}

/* (Round-robin among same-priority peers is now handled directly in
 * sched_tick(): a RUNNING task is unlinked from the ready list while it owns a
 * core and re-pushed at the TAIL on its next tick, so a peer is naturally
 * picked next. The old rl_rotate() helper is therefore no longer needed.) */

static void
frame_zero(struct el2_frame *f)
{
	uint64_t *p = (uint64_t *)(void *)f;
	uint32_t n = (uint32_t)(sizeof(*f) / sizeof(uint64_t));
	uint32_t i;

	for (i = 0; i < n; i++)
		p[i] = 0;
}

/* ------------------------------------------------------------------ *
 * sched_resume — the ONE place a task's saved context is actually loaded
 * into the CPU and eret'd into. Used by sched_tick() (preemptive switch),
 * sched_start() (the very first switch, from ordinary C). Never returns.
 *
 * Loads x1..x30 using x0 as a scratch base pointer to task->frame.x[],
 * then loads the real x0 value LAST from frame.x[0] (so the base pointer
 * itself is only needed up to that final load) — the standard trick for
 * restoring a full register file including the register you had to use
 * as your own scratch/base, with -mgeneral-regs-only (no spare non-GPR
 * scratch register available).
 * ------------------------------------------------------------------ */
static void __attribute__((noreturn))
sched_resume(struct sched_task *t)
{
	uint64_t elr  = t->frame.elr;
	uint64_t spsr = t->frame.spsr;
	uint64_t sp_  = t->frame.sp_at_entry;
	uint64_t base = (uint64_t)(void *)t->frame.x;

	__asm__ volatile(
		"msr    elr_el2, %0\n\t"
		"msr    spsr_el2, %1\n\t"
		"mov    sp, %2\n\t"
		"mov    x0, %3\n\t"
		"ldp    x1,  x2,  [x0, #8]\n\t"
		"ldp    x3,  x4,  [x0, #24]\n\t"
		"ldp    x5,  x6,  [x0, #40]\n\t"
		"ldp    x7,  x8,  [x0, #56]\n\t"
		"ldp    x9,  x10, [x0, #72]\n\t"
		"ldp    x11, x12, [x0, #88]\n\t"
		"ldp    x13, x14, [x0, #104]\n\t"
		"ldp    x15, x16, [x0, #120]\n\t"
		"ldp    x17, x18, [x0, #136]\n\t"
		"ldp    x19, x20, [x0, #152]\n\t"
		"ldp    x21, x22, [x0, #168]\n\t"
		"ldp    x23, x24, [x0, #184]\n\t"
		"ldp    x25, x26, [x0, #200]\n\t"
		"ldp    x27, x28, [x0, #216]\n\t"
		"ldp    x29, x30, [x0, #232]\n\t"
		"ldr    x0, [x0, #0]\n\t"
		"eret\n\t"
		:
		: "r" (elr), "r" (spsr), "r" (sp_), "r" (base)
		: "memory", "cc"
		/* Deliberately NOT clobbering x0-x30: with -mgeneral-regs-only
		 * those are the only registers GCC has to hold the four inputs
		 * above, so listing them all as clobbered leaves it with
		 * nowhere to put an input ("impossible constraints"). This is
		 * safe precisely because sched_resume() is noreturn and this
		 * asm block is the last thing that ever executes in this
		 * function — nothing downstream reads a register expecting it
		 * to have survived the block. */
	);
	__builtin_unreachable();
}

int
sched_add(void (*entry)(void *), void *arg, void *stack_top,
          uint32_t stack_size, int priority)
{
	int id;
	struct sched_task *t;
	union { void (*fn)(void *); uint64_t u; } conv;
	uint64_t daif;

	if (!entry || !stack_top || priority < 0 || priority >= SCHED_MAX_PRIO)
		return -1;

	daif = sched_lock();

	for (id = 0; id < SCHED_MAX_TASKS; id++)
		if (!g_tasks[id].used)
			break;
	if (id == SCHED_MAX_TASKS) {
		sched_unlock(daif);
		return -1;
	}

	t = &g_tasks[id];
	frame_zero(&t->frame);

	conv.fn = entry;
	t->frame.x[0]       = (uint64_t)(uintptr_t)arg;
	t->frame.elr        = conv.u;
	t->frame.spsr       = SPSR_TASK;
	/* 16-byte align the stack top down, per the AArch64 SP alignment rule. */
	t->frame.sp_at_entry = ((uint64_t)(uintptr_t)stack_top) & ~0xFULL;

	t->priority     = (uint32_t)priority;
	t->state        = TASK_READY;
	t->next_ready   = -1;
	t->prev_ready   = -1;
	t->yield_req    = 0;
	t->switches     = 0;
	t->run_ticks    = 0;
	t->last_switch_in = 0;
	t->stack_top    = stack_top;
	t->stack_size   = stack_size;
	t->used         = 1;

	rl_push_tail(id);
	g_ntasks++;

	sched_unlock(daif);
	return id;
}

void
sched_tick(struct el2_frame *frame)
{
	uint64_t now = timer_now();
	uint32_t cpu = smp_cpu_id();
	int cur, prio, next;

	/* IRQ context: IRQs are already masked; take the raw lock (no DAIF dance)
	 * and hold it while we mutate the shared run-queue. */
	spin_lock(&g_sched_lock);

	cur = (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;

	if (cur >= 0 && g_tasks[cur].used) {
		/* Save the interrupted context verbatim — the outgoing task's full
		 * state incl. its real SP (sp_at_entry, computed by el2_common before
		 * it pushed anything). */
		g_tasks[cur].frame = *frame;
		g_tasks[cur].run_ticks += now - g_tasks[cur].last_switch_in;
		g_tasks[cur].yield_req = 0;   /* consumed: whether it yielded or not */

		if (g_tasks[cur].state == TASK_RUNNING) {
			/* A RUNNING task is UNLINKED from the ready list while it owns a
			 * core (so no other core repicks it). Re-link it at the TAIL of
			 * its priority now that it's giving up the core — round-robin
			 * among peers, and it becomes eligible for any core again. */
			g_tasks[cur].state = TASK_READY;
			rl_push_tail(cur);
		}
		/* TASK_BLOCKED: sched_block() left it out of the ready list — leave it. */
	}

	/* Publish this core's heartbeat every tick (all cores, incl. CPU0). */
	smp_heartbeat(cpu, cur);

	if (g_ready_bitmap == 0) {
		/* No ready task for this core — idle. Leave *frame untouched so
		 * el2_common's epilogue resumes whatever we interrupted (the per-core
		 * WFI idle loop, or a task that just called sched_block() and is
		 * spinning in its own WFI wait — both are correct to resume). */
		if (cpu < SMP_MAX_CPUS)
			g_cpu_current[cpu] = -1;
		bc_update(cur);
		spin_unlock(&g_sched_lock);
		return;
	}

	prio = __builtin_ctz(g_ready_bitmap);
	next = g_ready_head[prio];

	/* Take ownership: unlink it from the ready list so no other core can pick
	 * the same task while this core runs it. */
	rl_unlink(next);
	g_tasks[next].state = TASK_RUNNING;
	g_tasks[next].last_switch_in = now;
	g_tasks[next].switches++;
	if (next != cur)
		g_switch_count++;
	if (cpu < SMP_MAX_CPUS)
		g_cpu_current[cpu] = next;

	bc_update(next);

	/* Feed the HUD's SCHED GANTT lane. Emitted only on a real switch, while
	 * still holding the lock, so the ring's order matches the switch order
	 * exactly. trace_emit() is weak: in builds without trace.o this is a call
	 * to an empty function, which is why no target needs a new object. */
	if (next != cur)
		trace_ctx_switch(cur, next);

	/* Release the lock BEFORE the eret (sched_resume never returns). We remain
	 * IRQ-masked until the eret restores this task's SPSR, so no nested tick
	 * reenters on this core in the window. */
	spin_unlock(&g_sched_lock);

	sched_resume(&g_tasks[next]); /* noreturn: erets directly */
}

void
sched_yield(void)
{
	uint32_t cpu = smp_cpu_id();
	uint64_t daif = sched_lock();
	int id = (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;

	if (id < 0) {
		sched_unlock(daif);
		return;
	}

	g_tasks[id].yield_req = 1;
	sched_unlock(daif);

	/* Wait for the next tick to rotate us to the back of our priority. */
	while (g_tasks[id].yield_req)
		__asm__ volatile("wfi" ::: "memory");
}

void
sched_block(void)
{
	uint32_t cpu = smp_cpu_id();
	uint64_t daif = sched_lock();
	int id = (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;

	if (id < 0) {
		sched_unlock(daif);
		return;
	}

	/* A RUNNING task is already out of the ready list (owned by this core), so
	 * just mark it BLOCKED — the next tick sees this and does NOT re-link it. */
	g_tasks[id].state = TASK_BLOCKED;
	sched_unlock(daif);

	while (g_tasks[id].state == TASK_BLOCKED)
		__asm__ volatile("wfi" ::: "memory");
}

void
sched_wake(int taskid)
{
	uint64_t daif;

	if (taskid < 0 || taskid >= SCHED_MAX_TASKS)
		return;

	daif = sched_lock();
	if (g_tasks[taskid].used && g_tasks[taskid].state == TASK_BLOCKED) {
		g_tasks[taskid].state = TASK_READY;
		rl_push_tail(taskid);
	}
	sched_unlock(daif);
}

void
sched_start(void)
{
	uint32_t cpu = smp_cpu_id();
	uint64_t now = timer_now();
	int prio, next;
	uint64_t daif = sched_lock();

	if (g_ready_bitmap == 0) {
		sched_unlock(daif);
		return; /* nothing registered — nothing to start */
	}

	prio = __builtin_ctz(g_ready_bitmap);
	next = g_ready_head[prio];

	rl_unlink(next);                 /* own it (see sched_tick) */
	g_tasks[next].state = TASK_RUNNING;
	g_tasks[next].last_switch_in = now;
	g_tasks[next].switches++;
	if (cpu < SMP_MAX_CPUS)
		g_cpu_current[cpu] = next;

	bc_update(next);

	/* Release before the eret. Stay IRQ-masked (the eret loads the task's SPSR
	 * which unmasks IRQ for preemption); do NOT smp_irq_restore here. */
	spin_unlock(&g_sched_lock);
	(void)daif;

	sched_resume(&g_tasks[next]); /* noreturn */
}

int
sched_current_task(void)
{
	uint32_t cpu = smp_cpu_id();
	return (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;
}

uint64_t
sched_switch_count(void)
{
	return g_switch_count;
}

const struct sched_task *
sched_task_info(int taskid)
{
	if (taskid < 0 || taskid >= SCHED_MAX_TASKS || !g_tasks[taskid].used)
		return (const struct sched_task *)0;
	return &g_tasks[taskid];
}

/* ------------------------------------------------------------------ *
 * Self-test: 3 tasks (priorities 0=high, 1=mid, 2=low), each looping
 * {counter++; sched_yield();}. Priorities are STRICT/fixed (no aging) —
 * so once sched_start()/sched_tick() are wired in, the highest-priority
 * task, being always READY, is always the find-first-set winner and
 * dominates st_counter_hi in the SCHD breadcrumb; st_counter_mid/lo only
 * advance if hi is ever not READY (it never blocks here, so in this
 * demo they should barely move at all) — that IS the point: it proves
 * strict fixed-priority determinism, not fairness.
 * ------------------------------------------------------------------ */
#define SELFTEST_STACK_SIZE 2048

static uint8_t st_stack_hi[SELFTEST_STACK_SIZE]  __attribute__((aligned(16)));
static uint8_t st_stack_mid[SELFTEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t st_stack_lo[SELFTEST_STACK_SIZE]  __attribute__((aligned(16)));

static volatile uint32_t st_counter_hi;
static volatile uint32_t st_counter_mid;
static volatile uint32_t st_counter_lo;

static void
st_task_hi(void *arg)
{
	(void)arg;
	for (;;) {
		st_counter_hi++;
		sched_yield();
	}
}

static void
st_task_mid(void *arg)
{
	(void)arg;
	for (;;) {
		st_counter_mid++;
		sched_yield();
	}
}

static void
st_task_lo(void *arg)
{
	(void)arg;
	for (;;) {
		st_counter_lo++;
		sched_yield();
	}
}

void
sched_selftest_setup(void)
{
	sched_add(st_task_hi,  (void *)0, st_stack_hi  + sizeof(st_stack_hi),  sizeof(st_stack_hi),  0);
	sched_add(st_task_mid, (void *)0, st_stack_mid + sizeof(st_stack_mid), sizeof(st_stack_mid), 1);
	sched_add(st_task_lo,  (void *)0, st_stack_lo  + sizeof(st_stack_lo),  sizeof(st_stack_lo),  2);
}

/* ------------------------------------------------------------------ *
 * Additions for the RTOS sync primitives (ksync.c) — they must live here
 * to reach the static run-queue state under g_sched_lock.
 * ------------------------------------------------------------------ */

/* Token-aware block: atomically (under the lock) check the wake token and,
 * only if it's still 0, mark the current task BLOCKED. Returns 1 if it
 * blocked (caller then WFI-waits), 0 otherwise. Closes the lost-wakeup race
 * against sched_wake() with no per-core assumptions. */
int sched_block_if(volatile uint32_t *token)
{
	uint32_t cpu = smp_cpu_id();
	uint64_t daif = sched_lock();
	int id = (cpu < SMP_MAX_CPUS) ? g_cpu_current[cpu] : -1;
	int blocked = 0;
	if (id >= 0 && *token == 0) {
		g_tasks[id].state = TASK_BLOCKED;
		blocked = 1;
	}
	sched_unlock(daif);
	return blocked;
}

/* Priority inheritance support: change a task's fixed priority and, if it is
 * currently READY, re-place it at the tail of the new priority level. */
int sched_set_priority(int taskid, int new_prio)
{
	uint64_t daif;
	if (taskid < 0 || taskid >= SCHED_MAX_TASKS ||
	    new_prio < 0 || new_prio >= SCHED_MAX_PRIO)
		return -1;
	daif = sched_lock();
	if (g_tasks[taskid].used) {
		if (g_tasks[taskid].state == TASK_READY) {
			rl_unlink(taskid);
			g_tasks[taskid].priority = (uint32_t)new_prio;
			rl_push_tail(taskid);
		} else {
			g_tasks[taskid].priority = (uint32_t)new_prio;
		}
	}
	sched_unlock(daif);
	return 0;
}
