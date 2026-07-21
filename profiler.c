/* profiler.c — non-halting sampling profiler for the bzdOS EL2 hypervisor.
 *
 * See profiler.h for the fixed DRAM layout (base 0x50006800, "PROF"). Each
 * tick's interrupted PC is bucketed by PC & ~0xF into an open-addressed hash
 * table (linear probe, fixed bound) and also emitted as a TRACE_PROFILE event.
 * Everything here is wait-free and bounded: the bucket-claim CAS and the
 * count increment use exclusive pairs that retry only on real contention, and
 * the probe walk is capped at PROF_PROBE slots.
 */
#include <stdint.h>
#include "profiler.h"
#include "trace.h"
#include "timer.h"

#define PROF_BASE     0x50006800UL
#define PROF_MAGIC    0x50524F46u        /* "PROF" */
#define PROF_BUCKETS  1024u              /* must be a power of two (mask below) */
#define PROF_HDR      0x20UL             /* 8 header words; buckets start +0x20 */
#define PROF_PROBE    8u                 /* max linear-probe steps (bounded) */
#define PC_MASK       (~0xFULL)          /* bucket granularity: 16-byte PCs */

/* Header word indices. */
#define P_MAGIC   0u
#define P_NBUCK   1u
#define P_TOTAL   2u
#define P_GUEST   3u
#define P_KERNEL  4u
#define P_DROP    5u
#define P_FREQ    6u
#define P_MASK    7u

static volatile uint32_t *const prof = (volatile uint32_t *)PROF_BASE;
/* Bucket array base as a word pointer (0x50006820). */
static volatile uint32_t *const prof_bkt =
	(volatile uint32_t *)(PROF_BASE + PROF_HDR);

static int prof_ready;

static inline void clean_word(volatile uint32_t *p)
{
	__asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void hdr_set(unsigned i, uint32_t v)
{
	prof[i] = v;
	clean_word(&prof[i]);
}

/* Atomic 32-bit fetch-add; bounded exclusive retry (see trace.c). */
static inline uint32_t fetch_add32(volatile uint32_t *p, uint32_t inc)
{
	uint32_t old, nw, ok;

	do {
		__asm__ volatile("ldxr %w0, %2" : "=&r"(old) : "Q"(*p), "m"(*p));
		nw = old + inc;
		__asm__ volatile("stxr %w0, %w2, %1"
		                 : "=&r"(ok), "=Q"(*p) : "r"(nw));
	} while (ok != 0u);
	return old;
}

/* Atomically set *p to nw iff it currently reads `exp`. Returns 1 on success,
 * 0 if the observed value was not `exp` (someone else claimed it). Bounded:
 * a spurious stxr failure retries, a value mismatch returns immediately. */
static inline int cas32(volatile uint32_t *p, uint32_t exp, uint32_t nw)
{
	uint32_t cur, ok;

	for (;;) {
		__asm__ volatile("ldxr %w0, %2" : "=&r"(cur) : "Q"(*p), "m"(*p));
		if (cur != exp) {
			__asm__ volatile("clrex" ::: "memory");
			return 0;
		}
		__asm__ volatile("stxr %w0, %w2, %1"
		                 : "=&r"(ok), "=Q"(*p) : "r"(nw));
		if (ok == 0u)
			return 1;
		/* stxr failed spuriously/contended: re-read and re-check exp. */
	}
}

/* Cheap PC hash -> starting bucket index. Multiplicative mix of the
 * 16-byte-granular PC, masked to the (power-of-two) table size. */
static inline uint32_t pc_hash(uint32_t pc_bucket)
{
	uint32_t h = pc_bucket >> 4;
	h *= 2654435761u;              /* Knuth multiplicative */
	return (h >> 6) & (PROF_BUCKETS - 1u);
}

void
profiler_init(void)
{
	unsigned i;

	for (i = 0; i < (PROF_HDR / 4u) + PROF_BUCKETS * 2u; i++) {
		prof[i] = 0u;
		clean_word(&prof[i]);
	}
	hdr_set(P_NBUCK, PROF_BUCKETS);
	hdr_set(P_FREQ,  (uint32_t)timer_freq());
	hdr_set(P_MASK,  (uint32_t)(PC_MASK & 0xffffffffull));
	hdr_set(P_MAGIC, PROF_MAGIC);  /* magic last: reader sees a ready table */
	prof_ready = 1;
}

void
profiler_sample(uint64_t pc, int is_guest)
{
	uint32_t key = (uint32_t)(pc & PC_MASK & 0xffffffffull);
	uint32_t idx, i;

	/* Always emit the sample into the trace ring (arg: 0 guest / 1 kernel,
	 * word3: sampled PC low). This works even before profiler_init(). */
	trace_emit(TRACE_PROFILE, trace_cpu(),
	           (uint16_t)(is_guest ? 0u : 1u), (uint32_t)pc);

	if (!prof_ready)
		return;

	if (key == 0u)
		key = 0x10u;   /* keep 0 reserved as the "free" sentinel */

	fetch_add32(&prof[P_TOTAL], 1u);
	fetch_add32(is_guest ? &prof[P_GUEST] : &prof[P_KERNEL], 1u);

	/* Open-addressed linear probe, bounded to PROF_PROBE slots. Claim a free
	 * bucket for `key` (CAS 0 -> key) or increment the one already holding it;
	 * if the chain is saturated, count a drop rather than loop unbounded. */
	idx = pc_hash(key);
	for (i = 0; i < PROF_PROBE; i++) {
		volatile uint32_t *b = &prof_bkt[((idx + i) & (PROF_BUCKETS - 1u)) * 2u];
		uint32_t cur = b[0];

		if (cur == key) {
			fetch_add32(&b[1], 1u);
			clean_word(&b[1]);
			return;
		}
		if (cur == 0u) {
			if (cas32(&b[0], 0u, key)) {
				clean_word(&b[0]);
				fetch_add32(&b[1], 1u);
				clean_word(&b[1]);
				return;
			}
			/* Lost the claim race: re-read this slot (it may now hold key). */
			if (b[0] == key) {
				fetch_add32(&b[1], 1u);
				clean_word(&b[1]);
				return;
			}
		}
	}

	fetch_add32(&prof[P_DROP], 1u);
	clean_word(&prof[P_DROP]);
}
