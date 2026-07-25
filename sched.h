/* SPDX-License-Identifier: BSD-2-Clause */

/* sched.h — fixed-priority PREEMPTIVE scheduler for the bzdOS EL2 microkernel
 * (AArch64, Allwinner A64). RTOS primitive #1: a small, deterministic set of
 * EL2 threads ("tasks"), each with a strict fixed priority, preempted by the
 * verified 1ms CNTP timer tick.
 *
 * Layered on top of (and does NOT modify) exceptions.h/exceptions.S: every
 * EL2 exception saves a FULL integer context (struct el2_frame) on the
 * current stack and, when the C handler returns, el2_common restores that
 * same frame and erets. sched_tick() is called from the timer-IRQ arm of
 * el2_trap() with that live frame; see sched.c's top comment for exactly
 * how mutating it (or bypassing el2_common's epilogue with our own eret)
 * achieves a context switch, and how sched_start()/sched_add() reuse the
 * identical mechanism for the very first task entry.
 *
 * House style: freestanding, no libc, <stdint.h> only, -mgeneral-regs-only
 * (integer context only — no FP/SIMD state to save, matching the build).
 */
#ifndef BZDOS_SCHED_H
#define BZDOS_SCHED_H

#include <stdint.h>
#include "exceptions.h"

/* Fixed, small, deterministic bounds — no dynamic task/priority count. */
#define SCHED_MAX_TASKS 8
#define SCHED_MAX_PRIO  8   /* priorities 0 (highest) .. SCHED_MAX_PRIO-1 (lowest) */

enum sched_state {
	TASK_UNUSED  = 0,   /* slot never used / freed (not implemented: no delete) */
	TASK_READY   = 1,   /* eligible to run, sitting in its priority's ready list */
	TASK_RUNNING = 2,   /* the one currently executing (== sched_current_task()) */
	TASK_BLOCKED = 3,   /* waiting for sched_wake(); not in any ready list */
};

/* One task's control block. Entirely static storage (SCHED_MAX_TASKS of
 * these) — no allocation at scheduling time, pairs fine with alloc.c for
 * the caller-owned stacks but does not depend on it.
 *
 * `frame` is the FULL saved integer context (see exceptions.h) — and it is
 * also this task's live stack pointer: frame.sp_at_entry (a field
 * el2_common already computes for every trap) doubles as "the real SP_EL2
 * this task was using", so a fresh task (sched_add) and a preempted task
 * (sched_tick) are represented identically and resumed by the exact same
 * routine. See sched.c: sched_resume(). */
struct sched_task {
	struct el2_frame frame;
	uint32_t priority;
	volatile uint32_t state;
	int next_ready;         /* intrusive ready-list link (by priority)   */
	int prev_ready;
	volatile uint32_t yield_req; /* sched_yield() sets; sched_tick() clears */
	uint64_t switches;      /* times switched IN (logd accounting)       */
	uint64_t run_ticks;     /* accumulated timer_now() ticks while RUNNING */
	uint64_t last_switch_in;/* timer_now() at the most recent switch-in  */
	void    *stack_top;
	uint32_t stack_size;
	uint32_t used;
};

/* Register a new task. Builds its initial el2_frame so that when first
 * scheduled, eret lands at `entry` with `arg` in x0, SP = stack_top
 * (16-byte aligned down), SPSR = EL2h with only IRQ unmasked (so the
 * preemptive tick keeps firing while a task runs), ELR = entry. The task
 * is inserted READY at the tail of its priority's ready list.
 * Returns the new task id (0..SCHED_MAX_TASKS-1), or -1 on a bad argument
 * or if the fixed task table is full. */
int sched_add(void (*entry)(void *), void *arg, void *stack_top,
              uint32_t stack_size, int priority);

/* THE preemption hook: call from the EL2 timer IRQ arm (see el2_trap),
 * passing the frame el2_common just saved. Saves the interrupted context
 * into the current task's TCB, picks the highest-priority READY task via
 * an O(1) find-first-set on the ready bitmap, and resumes it. Bounded,
 * reentrancy-safe, assumes IRQs are masked (exception context). Does not
 * allocate and does not loop over all tasks — only the bitmap scan plus a
 * fixed handful of pointer chases. See sched.c for why this call almost
 * always does NOT return (it erets directly) instead of mutating *frame
 * and letting el2_common's own epilogue run. */
void sched_tick(struct el2_frame *frame);

/* Cooperative hooks, all O(1)/bounded — see sched.c for the exact
 * mechanism (a flag/state change honored by the NEXT timer tick, not a
 * synchronous SVC/BRK trap; documented trade-off: up to ~1 tick of
 * latency, no exception-path changes required). */
void sched_yield(void);      /* give up this tick; re-queued at the back of
                               * my own priority level (round-robin peers) */
void sched_block(void);      /* the CURRENT task blocks until sched_wake() */
void sched_wake(int taskid); /* move a BLOCKED task back to READY          */
int  sched_block_if(volatile uint32_t *token); /* token-aware block (ksync)  */
int  sched_set_priority(int taskid, int new_prio); /* priority inheritance    */

/* Begin multitasking: pick the highest-priority READY task and enter it.
 * Called from ordinary C (not exception context) — implemented via the
 * same restore-and-eret trampoline sched_tick() uses (see sched_resume()
 * in sched.c). Does not return. */
void sched_start(void);

/* Read-only observability (bounded, no side effects) for logd/REPL. */
int      sched_current_task(void);
uint64_t sched_switch_count(void);
const struct sched_task *sched_task_info(int taskid);

/* Self-contained demo: registers 2-3 tasks at different fixed priorities,
 * each looping {increment its own counter; sched_yield();}. Call this,
 * then sched_start() (and have the IRQ path call sched_tick()) to watch
 * strict-priority preemption on hardware via the SCHD breadcrumb — the
 * highest-priority task should dominate the run-tick counters exactly
 * because priorities here are strict/fixed (no aging, no fair share). */
void sched_selftest_setup(void);

#endif /* BZDOS_SCHED_H */
