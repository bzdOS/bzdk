/* trace.h — event-trace ring (ftrace-lite) for the bzdOS EL2 hypervisor.
 *
 * A single global, SMP-safe, lock-free ring of timestamped kernel/hypervisor
 * events in a FIXED cache-coherent DRAM window that a sibling HUD track and an
 * external host tool both read. The layout below is a HARD contract — do not
 * change offsets/encodings without updating every reader.
 *
 * Window (fixed): base 0x50004000, magic "TRC1" (0x54524331).
 *   Header (5 words @ 0x50004000):
 *     [0] magic          0x54524331 ("TRC1")
 *     [1] total_events   monotonic event count (wraps the ring by modulo);
 *                        also the fetch-add ticket source (SMP-safe slotting)
 *     [2] head           next write slot index (== total_events % capacity)
 *     [3] capacity       number of slots (TRACE_CAP, 512)
 *     [4] timer_freq_lo  timer_freq() low 32 bits (Hz; timestamp -> seconds)
 *   Entries start at 0x50004040 (0x40 header stride). Entry = 16 bytes/4 words:
 *     word0 = timestamp_lo   (CNTPCT low 32, from timer_now())
 *     word1 = timestamp_hi   (CNTPCT high 32)
 *     word2 = (type & 0xff) | ((cpu & 0xff)<<8) | ((arg16 & 0xffff)<<16)
 *     word3 = pc_or_context  (u32 low of a PC, or an id)
 *
 * With TRACE_CAP=512 the ring occupies 0x50004000..0x50006040.
 *
 * SMP model: single global ring, wait-free. trace_emit() does one exclusive
 * fetch-add on total_events to claim a unique ticket; slot = ticket % cap. Each
 * producer therefore owns a distinct 16-byte slot for its write (a slow
 * producer can be lapped after a full wrap, but that is the normal ring
 * semantics and never corrupts a concurrent writer's slot). Every written word
 * is cleaned to PoC (dc cvac + dsb) so the record survives a reset and is
 * readable by the HUD and an external md.l / host reader.
 */
#ifndef BZDOS_TRACE_H
#define BZDOS_TRACE_H

#include <stdint.h>

/* Event types (word2 low byte). Shared contract with the HUD/host reader. */
enum {
	TRACE_CTX_SWITCH = 1,  /* arg=new task id,  word3=prev task id        */
	TRACE_IRQ        = 2,  /* arg=intid,        word3=0                   */
	TRACE_TRAP       = 3,  /* arg=ESR EC,       word3=guest PC low        */
	TRACE_GUEST_EXIT = 4,  /* arg=reason,       word3=guest PC low        */
	TRACE_TICK       = 5,  /* arg=0,            word3=0                   */
	TRACE_PROFILE    = 6,  /* arg=0 guest/1 kernel, word3=sampled PC low  */
	TRACE_USER       = 7,  /* generic                                     */
};

/* Initialize (zero + stamp header) the trace ring. Call once, early, before
 * any trace_emit(). Idempotent. */
void trace_init(void);

/* Emit one event. Lock-free, SMP-safe, bounded (no loops except a short
 * exclusive-store retry on the ticket). Safe from IRQ/trap context. */
void trace_emit(uint8_t type, uint8_t cpu, uint16_t arg, uint32_t ctx);

/* Current core id (MPIDR aff0), used by the convenience inlines below so the
 * cpu field is correct without every call site threading it through. */
static inline uint8_t trace_cpu(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return (uint8_t)(v & 0xffu);
}

/* Convenience inlines (thin wrappers over trace_emit). */
static inline void trace_ctx_switch(int prev, int next)
{
	trace_emit(TRACE_CTX_SWITCH, trace_cpu(),
	           (uint16_t)(uint32_t)next, (uint32_t)prev);
}

static inline void trace_irq(uint16_t intid)
{
	trace_emit(TRACE_IRQ, trace_cpu(), intid, 0u);
}

static inline void trace_trap(uint16_t ec, uint32_t pc)
{
	trace_emit(TRACE_TRAP, trace_cpu(), ec, pc);
}

static inline void trace_tick(void)
{
	trace_emit(TRACE_TICK, trace_cpu(), 0u, 0u);
}

#endif /* BZDOS_TRACE_H */
