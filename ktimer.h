/* SPDX-License-Identifier: BSD-2-Clause */

/* ktimer.h — software timers / timeouts for the bzdOS EL2 microkernel.
 *
 * RTOS primitive: one-shot and periodic callbacks plus task sleep, layered on
 * top of the verified monotonic timebase (timer.c: timer_now()/timer_freq())
 * and the 1 ms CNTP preemption tick (gic_timer.c). A small fixed-size table of
 * (deadline_ticks, period_ticks, callback, arg) records is scanned once per
 * tick by ktimer_service(now), which fires every timer whose deadline has
 * passed; periodic timers re-arm themselves, one-shots deactivate.
 *
 * SMP-safe: all shared timer state is guarded by a dedicated spinlock (the
 * ldaxr/stlxr lock from smp.h). Callbacks are invoked with that lock RELEASED
 * (the due timer is popped/re-armed under the lock, then the callback runs
 * unlocked) so a callback may freely take other locks — e.g. ksleep's wake
 * callback calls sched_wake(), and ksem's timeout callback re-enters ksync —
 * without any lock-ordering hazard against ktimer itself.
 *
 * Freestanding: no libc, <stdint.h> only, -mgeneral-regs-only.
 */
#ifndef BZDOS_KTIMER_H
#define BZDOS_KTIMER_H

#include <stdint.h>

/* Fixed, deterministic bound — no dynamic timer allocation. */
#define KTIMER_MAX 16

/* Opaque handle. 0 == invalid/none. Encodes {generation, slot+1} so a stale
 * handle to a slot that has since been recycled is rejected by ktimer_cancel()
 * (the generation won't match) rather than cancelling an unrelated timer. */
typedef uint32_t ktimer_t;

/* Arm a ONE-SHOT timer: fire cb(arg) once, approximately `us` microseconds
 * from now (rounded up to the next serviced tick). Returns a handle (nonzero)
 * or 0 if the timer table is full. */
ktimer_t ktimer_after(uint64_t us, void (*cb)(void *), void *arg);

/* Arm a PERIODIC timer: fire cb(arg) every `us` microseconds, first firing at
 * now+us. Re-arms itself from its own scheduled deadline (not from callback
 * entry time) so period drift does not accumulate. Returns a handle or 0. */
ktimer_t ktimer_every(uint64_t us, void (*cb)(void *), void *arg);

/* Cancel a timer previously returned by ktimer_after/every. Returns 1 if the
 * timer was live and is now cancelled, 0 if the handle was invalid/stale/
 * already fired (one-shot). Safe to call from any context, incl. from inside
 * another timer's callback. */
int ktimer_cancel(ktimer_t h);

/* Fire all timers whose deadline <= now. Call once per tick from the IRQ/tick
 * path (see the el2_trap integration snippet in ktimer.c's header comment),
 * passing timer_now(). Bounded: at most KTIMER_MAX callbacks per call. SMP-safe
 * (may be called from every core's tick concurrently — each due timer fires
 * exactly once, whichever core pops it first). */
void ktimer_service(uint64_t now);

/* Block the CURRENT task for approximately `us` microseconds: arm a one-shot
 * wake timer, mark the task blocked, and (once the tick fires the timer) wake
 * it. If no scheduler task is current (sched_current_task() < 0) this falls
 * back to timer_delay_us() (a bounded busy-wait) so it is always safe to call.
 */
void ksleep(uint64_t us);

/* Convenience: ksleep in milliseconds. */
static inline void kdelay_ms(uint32_t ms) { ksleep((uint64_t)ms * 1000ull); }

/* ------------------------------------------------------------------ *
 * Deterministic unit self-test — drives ktimer_service() with synthetic
 * timestamps (does NOT require the scheduler or the live tick) and verifies
 * one-shot fire, periodic re-arm, cancel, and ordering. Records pass/fail +
 * counters to the cache-coherent DRAM breadcrumb 0x50005600 ("KTMR"):
 *   [0] magic 0x4b544d52 ("KTMR")
 *   [1] result   1 = all checks passed, 0 = failed
 *   [2] fail_code (0 = none; else which check, see ktimer.c)
 *   [3] oneshot fire count   (expect 1)
 *   [4] periodic fire count  (expect 5)
 *   [5] cancel took effect   (1 = a cancelled periodic stopped firing)
 * ------------------------------------------------------------------ */
#define KTMR_BC_BASE  0x50005600UL
#define KTMR_BC_MAGIC 0x4b544d52u  /* "KTMR" */
void ktimer_selftest(void);

#endif /* BZDOS_KTIMER_H */
