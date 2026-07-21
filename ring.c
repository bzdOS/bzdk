/* ring.c — lock-free SPSC ring buffer (see ring.h).
 *
 * Freestanding, bare-metal AArch64 (Allwinner A64, Cortex-A53, EL2, MMU on,
 * normal cacheable memory): no libc, no dynamic allocation, only <stdint.h>.
 * Compiles under -mgeneral-regs-only because the C11 atomic builtins used
 * below operate purely on 32-bit integers — no FP/SIMD registers involved.
 *
 * ---------------------------------------------------------------------
 * Why acquire/release (no locks, no seq-cst) is correct AND sufficient
 * for a single-producer/single-consumer ring:
 *
 * head is written ONLY by the producer, tail is written ONLY by the
 * consumer. Each side's "own" field never needs synchronization against
 * itself — only against the OTHER side's view of it. The two hazards to
 * rule out are exactly the classic "publish data, then publish index":
 *
 *   Producer:  write buf[head..head+n)  ; THEN  publish new head
 *   Consumer:  read new head            ; THEN  read buf[old_tail..head)
 *
 * If the consumer could observe the new head value before it observes
 * the buffer writes that preceded it, it would read stale/garbage bytes.
 * __atomic_store_n(&head, ..., __ATOMIC_RELEASE) prevents the compiler
 * and CPU from hoisting the head store above the preceding buffer
 * writes, and on AArch64 lowers to STLR (store-release) — an
 * ARMv8 "load-acquire, store-release" instruction, not a full DMB, so no
 * separate barrier instruction is needed. __atomic_load_n(&head, ...,
 * __ATOMIC_ACQUIRE) on the consumer side lowers to LDAR (load-acquire),
 * which prevents the buffer reads that follow it from being reordered
 * ahead of it. Together, STLR-then-LDAR forms an inter-thread
 * "release/acquire pair" per the ARMv8 memory model: everything the
 * producer did before the STLR is guaranteed visible to the consumer
 * after its matching LDAR observes that value. The identical argument
 * applies in the other direction for tail (consumer publishes "I've
 * freed this space" via release; producer's acquire load of tail
 * ensures it doesn't overwrite bytes the consumer hasn't finished
 * reading yet).
 *
 * Sequential consistency (__ATOMIC_SEQ_CST) would additionally order
 * these operations against every OTHER seq-cst operation in the system,
 * which SPSC never needs — there is exactly one producer and one
 * consumer, so acquire/release is the tightest correct fence and avoids
 * the extra DMB SY overhead SEQ_CST would compile to on AArch64.
 *
 * A thread only ever needs a RELAXED load of its OWN field (e.g. the
 * producer reading back the head value it itself last wrote) since
 * program order on a single core already guarantees that visibility —
 * there is no other writer of that field.
 * ---------------------------------------------------------------------
 */
#include <stdint.h>
#include "ring.h"

void
ring_init(struct ring *r, uint8_t *storage, uint32_t size_pow2)
{
	r->head = 0;
	r->tail = 0;
	r->mask = size_pow2 - 1u;
	r->buf = storage;
}

uint32_t
ring_put(struct ring *r, const uint8_t *data, uint32_t len)
{
	uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED); /* our own last write */
	uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE); /* sync with consumer */
	uint32_t cap = r->mask + 1u;
	uint32_t used = head - tail;              /* unsigned wraparound-safe */
	uint32_t space = cap - used;
	uint32_t n = (len < space) ? len : space;
	uint32_t i;

	for (i = 0; i < n; i++)
		r->buf[(head + i) & r->mask] = data[i];

	/* Publish the new head AFTER the data writes above are visible to a
	 * consumer that observes it (see file header). */
	__atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
	return n;
}

uint32_t
ring_get(struct ring *r, uint8_t *out, uint32_t max)
{
	uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED); /* our own last write */
	uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE); /* sync with producer */
	uint32_t avail = head - tail;             /* unsigned wraparound-safe */
	uint32_t n = (max < avail) ? max : avail;
	uint32_t i;

	for (i = 0; i < n; i++)
		out[i] = r->buf[(tail + i) & r->mask];

	/* Publish the new tail AFTER the reads above complete, so the
	 * producer's acquire load of tail never sees space freed before the
	 * bytes were actually consumed. */
	__atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
	return n;
}

uint32_t
ring_count(const struct ring *r)
{
	uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
	uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
	return head - tail;
}

uint32_t
ring_space(const struct ring *r)
{
	return (r->mask + 1u) - ring_count(r);
}

/* ------------------------------------------------------------------ *
 * Self-test breadcrumb (survives a watchdog reset — same cache-coherent
 * write pattern as main.c's bc_write: MMU/D-cache are on, so a plain
 * store sits in cache and would be lost to a WDT reset; `dc civac` +
 * `dsb sy` cleans it to the point of coherency so a post-reset `md.l`
 * (or another core) sees it).
 *
 * Layout at DRAM window 0x50000600 (disjoint from main.c's 0x50000000
 * progress breadcrumb):
 *   [0] magic           0x52494e47 ("RING")
 *   [1] pass            1 = all bytes matched expected sequence, in order;
 *                        0 = a mismatch was found (or push/pop count wrong)
 *   [2] bytes pushed    total bytes accepted by ring_put across the test
 *   [3] bytes popped    total bytes returned by ring_get across the test
 *   [4] first mismatch  index (within the popped stream) of the first
 *                        byte that didn't match its expected value, or
 *                        0xffffffff if none
 * ------------------------------------------------------------------ */
#define RING_BC_BASE 0x50000600UL

static inline void
ring_bc_write(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(RING_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

#define RING_ST_CAP   64u   /* power-of-two capacity for the self-test ring */
#define RING_ST_TOTAL 300u  /* > 4x capacity: forces several wraps */

static uint8_t ring_st_storage[RING_ST_CAP];

void
ring_selftest(void)
{
	struct ring r;
	uint8_t tmp_in[40];
	uint8_t tmp_out[40];
	uint32_t push_idx = 0, pop_idx = 0;
	uint32_t total_pushed = 0, total_popped = 0;
	uint32_t mismatch = 0xffffffffu;
	uint32_t step = 0;

	ring_init(&r, ring_st_storage, RING_ST_CAP);

	/* Deterministic producer/consumer interleaving: varying push-chunk
	 * and pop-chunk sizes (relative to the 64-byte capacity) exercise
	 * a full fill, wrap-around across the power-of-two boundary,
	 * partial reads (popping fewer bytes than are available), and a
	 * final full drain once pushing is done. */
	while (push_idx < RING_ST_TOTAL || ring_count(&r) > 0) {
		if (push_idx < RING_ST_TOTAL) {
			uint32_t want = 7u + (step % 5u) * 9u; /* 7,16,25,34,43 */
			uint32_t remaining = RING_ST_TOTAL - push_idx;
			uint32_t chunk = (want < remaining) ? want : remaining;
			uint32_t i, accepted;

			if (chunk > sizeof(tmp_in))
				chunk = sizeof(tmp_in);
			for (i = 0; i < chunk; i++)
				tmp_in[i] = (uint8_t)(push_idx + i);

			accepted = ring_put(&r, tmp_in, chunk);
			push_idx += accepted;
			total_pushed += accepted;
		}

		{
			uint32_t avail = ring_count(&r);
			uint32_t want = 5u + (step % 7u) * 6u; /* 5,11,...,41 */
			uint32_t chunk = (want < avail) ? want : avail;

			if (chunk > sizeof(tmp_out))
				chunk = sizeof(tmp_out);

			if (chunk > 0) {
				uint32_t got = ring_get(&r, tmp_out, chunk);
				uint32_t i;

				for (i = 0; i < got; i++) {
					uint8_t expect = (uint8_t)(pop_idx + i);

					if (tmp_out[i] != expect && mismatch == 0xffffffffu)
						mismatch = total_popped + i;
				}
				pop_idx += got;
				total_popped += got;
			}
		}

		step++;
	}

	{
		uint32_t pass = (mismatch == 0xffffffffu &&
				  total_pushed == RING_ST_TOTAL &&
				  total_popped == RING_ST_TOTAL) ? 1u : 0u;

		ring_bc_write(0, 0x52494e47u);
		ring_bc_write(1, pass);
		ring_bc_write(2, total_pushed);
		ring_bc_write(3, total_popped);
		ring_bc_write(4, mismatch);
	}
}
