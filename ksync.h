/* ksync.h — synchronization primitives for the bzdOS EL2 microkernel:
 *   - kmutex : mutual exclusion with PRIORITY INHERITANCE (bounds inversion)
 *   - ksem   : counting semaphore (blocking wait, optional timeout)
 *   - kmbox  : fixed-size message queue / mailbox (blocking recv)
 *
 * All three block waiting tasks through a single robust block/wake core
 * (kblock/kwake below) built on the scheduler's sched_block_prepare() /
 * sched_wake() and a per-task wake token that closes the classic lost-wakeup
 * race (a wake that arrives between "decide to block" and "actually blocked").
 *
 * SMP-safe: each object carries its own spinlock (smp.h ldaxr/stlxr lock);
 * task-context callers mask IRQ around the lock (so a same-core tick cannot
 * self-deadlock on a lock its own preempted thread holds — same discipline as
 * sched.c). Waiter sets are 8-bit bitmaps (SCHED_MAX_TASKS == 8), so "wake the
 * highest-priority waiter" is a bounded scan, no lists, no allocation.
 *
 * REQUIRED SCHEDULER ADDITIONS (see delivery notes — paste into sched.c):
 *   void sched_block_prepare(void);            // mark current task BLOCKED, no spin
 *   int  sched_set_priority(int tid,int prio); // change prio + re-place if READY
 * Everything else uses the already-exported sched_wake()/sched_current_task()/
 * sched_task_info().
 *
 * Freestanding: no libc, <stdint.h> only, -mgeneral-regs-only.
 */
#ifndef BZDOS_KSYNC_H
#define BZDOS_KSYNC_H

#include <stdint.h>
#include "smp.h"   /* spinlock_t, spin_lock/unlock, smp_irq_save/restore */

/* ------------------------------------------------------------------ *
 * Block/wake core (shared by ktimer's ksleep and every ksync object).
 * ------------------------------------------------------------------ */

/* Block the CURRENT task until kwake(current) is called. Returns 0 when woken,
 * -1 immediately if there is no current scheduler task (nothing to block). The
 * per-task wake token makes a kwake() that races ahead of the block non-lossy.
 * The caller must have released any object lock it holds BEFORE calling this
 * (it spins in WFI); it re-enqueues nothing — the object's own state already
 * records why the task is waiting. */
int  kblock(void);

/* Make a blocked (or about-to-block) task runnable again. Sets its wake token
 * then sched_wake()s it. Safe from IRQ/tick context and from other cores. */
void kwake(int taskid);

/* ------------------------------------------------------------------ *
 * Mutex with priority inheritance.
 * ------------------------------------------------------------------ */
struct kmutex {
	spinlock_t lock;         /* guards this object's fields */
	uint32_t   held;         /* 1 if owned by someone (owner id may be -1 when
	                          * locked from non-task/bare context) */
	int        owner;        /* task id holding it, or -1 (free, or bare-ctx) */
	int        owner_base;   /* owner's priority at acquire time (for restore) */
	uint32_t   waiters;      /* bitmap of task ids blocked on this mutex */
	uint32_t   boosted;      /* 1 if we currently hold the owner boosted */
};

void kmutex_init(struct kmutex *m);
/* Acquire; blocks on contention. On contention the holder inherits the
 * (numerically lowest = highest) priority among itself and all waiters, so it
 * cannot be indefinitely preempted by a mid-priority task (bounded inversion).
 * Priority is restored on unlock. Non-recursive: a task must not re-lock a
 * mutex it already holds. */
void kmutex_lock(struct kmutex *m);
/* Try to acquire without blocking. Returns 1 on success, 0 if held by another. */
int  kmutex_trylock(struct kmutex *m);
/* Release. Restores the holder's base priority and, if waiters exist, performs
 * a direct priority-ordered handoff to the highest-priority waiter (it wakes
 * already owning the mutex — no re-acquire race). */
void kmutex_unlock(struct kmutex *m);

/* ------------------------------------------------------------------ *
 * Counting semaphore.
 * ------------------------------------------------------------------ */
struct ksem {
	spinlock_t lock;
	int32_t    count;        /* available permits */
	uint32_t   waiters;      /* bitmap of task ids blocked in ksem_wait */
};

void ksem_init(struct ksem *s, int32_t initial);
/* P(): take a permit, blocking until one is available. */
void ksem_wait(struct ksem *s);
/* P() with timeout: block at most ~`us` microseconds. Returns 0 if a permit
 * was taken, -1 on timeout (arms a ktimer that wakes the waiter). */
int  ksem_wait_timeout(struct ksem *s, uint64_t us);
/* Non-blocking P(): returns 1 if a permit was taken, 0 if none available. */
int  ksem_trywait(struct ksem *s);
/* V(): release a permit; if a task is waiting, hand the permit directly to the
 * highest-priority waiter and wake it (count is not incremented in that case).
 */
void ksem_post(struct ksem *s);

/* ------------------------------------------------------------------ *
 * Mailbox / fixed-size message queue.
 *
 * Copy-by-value ring of `capacity` slots of `msg_size` bytes each, backed by
 * caller-supplied storage (capacity*msg_size bytes). recv blocks until a
 * message; send fails (returns -1) when full (non-blocking producer).
 * ------------------------------------------------------------------ */
struct kmbox {
	spinlock_t lock;
	uint8_t   *buf;          /* caller storage: capacity*msg_size bytes */
	uint32_t   msg_size;
	uint32_t   capacity;
	uint32_t   head;         /* next slot to write */
	uint32_t   tail;         /* next slot to read */
	uint32_t   count;        /* messages queued */
	uint32_t   rx_waiters;   /* bitmap of task ids blocked in kmbox_recv */
};

void kmbox_init(struct kmbox *mb, void *storage, uint32_t msg_size, uint32_t capacity);
/* Enqueue one message (msg_size bytes copied from `msg`). Returns 0 on success,
 * -1 if the mailbox is full. Wakes one blocked receiver on success. */
int  kmbox_send(struct kmbox *mb, const void *msg);
/* Dequeue one message into `out` (msg_size bytes). Blocks until a message is
 * available. */
void kmbox_recv(struct kmbox *mb, void *out);
/* Non-blocking dequeue: 1 if a message was returned, 0 if empty. */
int  kmbox_tryrecv(struct kmbox *mb, void *out);

/* ------------------------------------------------------------------ *
 * Deterministic unit self-test — single-threaded semantic checks of mutex
 * (trylock/lock/unlock ownership), semaphore (count, trywait), and mailbox
 * (send/recv FIFO, full/empty) that do NOT block (so no scheduler required).
 * Records to breadcrumb 0x50005500 ("KSY1"):
 *   [0] magic 0x4b535931 ("KSY1")
 *   [1] result 1=pass 0=fail
 *   [2] fail_code (see ksync.c)
 *   [3] mbox messages round-tripped
 *   [4] sem final count
 * ------------------------------------------------------------------ */
#define KSY_BC_BASE  0x50005500UL
#define KSY_BC_MAGIC 0x4b535931u  /* "KSY1" */
void ksync_selftest(void);

/* ------------------------------------------------------------------ *
 * Live multi-task demo (mutex + semaphore + periodic timer) — registers demo
 * tasks with the scheduler; run it by calling sched_start() afterwards (see
 * delivery notes / REPL snippet). Writes progress + pass/fail to breadcrumb
 * 0x50005500 too (offset words [5..9], magic word [0] reused):
 *   [5] producer iterations
 *   [6] consumer iterations (via mailbox)
 *   [7] mutex critical-section entries (protected shared counter)
 *   [8] timer ticks observed by the periodic timer
 *   [9] demo pass flag (1 once counters agree & no corruption seen)
 * ------------------------------------------------------------------ */
void ksync_demo_setup(void);

#endif /* BZDOS_KSYNC_H */
