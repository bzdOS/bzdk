/* ring.h — lock-free single-producer/single-consumer (SPSC) byte ring for
 * the bzdOS microkernel's EL2<->EL1 (microkernel<->guest, driver<->consumer)
 * zero-lock data path. One side is the sole producer, the other the sole
 * consumer; no other synchronization primitive is required — see ring.c for
 * why plain acquire/release atomics on head/tail are sufficient.
 *
 * Freestanding, no libc, no dynamic allocation: the caller supplies the
 * backing storage (typically a static array) and its power-of-two size.
 */
#ifndef BZDOS_RING_H
#define BZDOS_RING_H

#include <stdint.h>

struct ring {
	volatile uint32_t head;  /* next slot the producer will write; producer-owned */
	volatile uint32_t tail;  /* next slot the consumer will read;  consumer-owned */
	uint32_t mask;           /* size_pow2 - 1, for fast index wraparound */
	uint8_t *buf;            /* caller-provided storage, size_pow2 bytes */
};

/* Initialize r to use `storage` (size_pow2 bytes, size_pow2 a power of two,
 * e.g. 256, 4096) as backing store. Resets head/tail to empty. Does NOT
 * allocate — storage must outlive r and must already be size_pow2 bytes. */
void ring_init(struct ring *r, uint8_t *storage, uint32_t size_pow2);

/* Producer side. Copies up to len bytes of data into the ring and returns
 * the number of bytes actually accepted (less than len if the ring is
 * full). Safe to call concurrently with exactly one consumer calling
 * ring_get on the same ring from another core — never call this from more
 * than one producer context. */
uint32_t ring_put(struct ring *r, const uint8_t *data, uint32_t len);

/* Consumer side. Copies up to max bytes out of the ring into out and
 * returns the number of bytes actually returned (less than max if the ring
 * has less than max bytes available). Safe to call concurrently with
 * exactly one producer calling ring_put — never call this from more than
 * one consumer context. */
uint32_t ring_get(struct ring *r, uint8_t *out, uint32_t max);

/* Bytes currently queued (readable by the consumer). Uses acquire loads of
 * both head and tail, so the value is a snapshot that may be stale by the
 * time it is used — fine for capacity/telemetry checks, not for control
 * flow that needs exact synchronization (ring_put/ring_get already handle
 * that internally). */
uint32_t ring_count(const struct ring *r);

/* Free space currently available to the producer (capacity - count). Same
 * snapshot caveat as ring_count. */
uint32_t ring_space(const struct ring *r);

/* Deterministic self-test: exercises fill, drain, wrap-around across the
 * power-of-two boundary, and partial reads, verifying no bytes are lost or
 * reordered. Writes a pass/fail record to the cache-coherent breadcrumb
 * window at 0x50000600 (magic/pass/pushed/popped/first-mismatch — see
 * ring.c for the exact layout) so the result survives a watchdog reset.
 * Does not touch any device other than that DRAM window. */
void ring_selftest(void);

#endif /* BZDOS_RING_H */
