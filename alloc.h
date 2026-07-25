/* SPDX-License-Identifier: BSD-2-Clause */

/* alloc.h — deterministic static region / slab allocator for the bzdOS
 * microkernel (AArch64, EL2, bare-metal, MMU on).
 *
 * This is the ONLY allocation primitive the kernel and PaaS containers
 * (.jpk) use: fixed-block pools pre-carved out of caller-supplied static
 * arenas. There is no malloc, no heap, no sbrk/vmem, and no runtime
 * fragmentation — every pool's blocks are the same size, decided at
 * pool_init() time, and handed out/taken back with a single pointer-chase
 * (an intrusive singly-linked free list threaded through the free blocks
 * themselves). Both pool_alloc() and pool_free() are O(1): a fixed number
 * of loads/stores, no scanning, no coalescing, no searching — the
 * determinism an RTOS/hypervisor scheduler needs (bounded worst-case
 * latency for a container to get its buffer).
 *
 * House style: freestanding, no libc, <stdint.h> only (see wdt.c/main.c).
 */
#ifndef BZDOS_ALLOC_H
#define BZDOS_ALLOC_H

#include <stdint.h>

/* Every block handed out is at least this aligned. 16 bytes covers AArch64
 * NEON/SIMD (Q register) natural alignment and is a safe default for any
 * struct a container might place at the front of its buffer. */
#define POOL_ALIGN 16u

/* ------------------------------------------------------------------ *
 * Tier 1: fixed-block pool allocator.
 *
 * pool_init() carves `arena` into `n_blocks` blocks of `block_size` each
 * (both rounded up to POOL_ALIGN — see the .c file for the exact math) and
 * threads them into a free list: the first POOL_ALIGN-aligned word of each
 * free block stores the address of the next free block (or NULL for the
 * last one). No block header, no size class table, no bitmap — the free
 * list pointers live INSIDE the free blocks themselves, so a pool of N
 * blocks costs zero bytes of allocator metadata beyond the `struct pool`
 * itself.
 * ------------------------------------------------------------------ */
struct pool {
	uint8_t  *base;        /* first byte of the (aligned) usable arena */
	uint32_t  block_size;   /* aligned block stride, bytes */
	uint32_t  n_blocks;     /* total blocks carved from the arena */
	uint32_t  free_count;   /* blocks currently on the free list */
	void     *free_head;    /* head of intrusive singly-linked free list */
};

/* Carve `arena` (arena_size bytes) into blocks of at least `block_size`
 * bytes each, both internally rounded up to POOL_ALIGN, and build the free
 * list. Returns 1 on success, 0 if the arena is too small to hold even one
 * block (or a NULL/zero argument is passed) — p is left zeroed either way
 * so pool_alloc() on a failed-init pool deterministically returns NULL
 * rather than reading garbage. */
int pool_init(struct pool *p, void *arena, uint32_t arena_size, uint32_t block_size);

/* O(1): pop the free-list head. Returns NULL if the pool is exhausted. */
void *pool_alloc(struct pool *p);

/* O(1): validate `ptr` lies within [base, base + n_blocks*block_size) and
 * is exactly block-aligned, then push it back onto the free list. A bad
 * pointer (out of range, misaligned) is rejected and ignored — it is
 * dropped on the floor rather than corrupting the free list; the return
 * value flags this so callers/diagnostics can notice.
 * Returns 1 on a valid free, 0 if the pointer was rejected. */
int pool_free(struct pool *p, void *ptr);

/* Snapshot of pool occupancy. Either output pointer may be NULL. */
void pool_stats(struct pool *p, uint32_t *free_out, uint32_t *used_out);

/* ------------------------------------------------------------------ *
 * Tier 2 (optional): tiny bump arena for one-shot startup allocations
 * that are never individually freed (e.g. carving out the pools
 * themselves, or fixed per-container bookkeeping structs at boot). O(1),
 * monotonic — there is no arena_free(); the whole arena resets only via
 * arena_init() (e.g. across a warm restart of the allocator subsystem).
 * ------------------------------------------------------------------ */
struct arena {
	uint8_t  *base;
	uint32_t  size;
	uint32_t  offset;   /* bytes bumped so far */
};

void  arena_init(struct arena *a, void *mem, uint32_t size);

/* Bump-allocate `size` bytes, aligned to POOL_ALIGN. Returns NULL if the
 * arena has insufficient remaining space (no rollback needed — nothing was
 * ever committed on failure). */
void *arena_bump(struct arena *a, uint32_t size);

/* ------------------------------------------------------------------ *
 * Self-test — exercises the pool allocator against a static arena and
 * records pass/fail + diagnostics into a cache-coherent DRAM breadcrumb
 * window, following the same dc-civac-then-dsb pattern used in main.c
 * (the MMU/D-cache are on; a plain store would sit in cache and be lost
 * across a watchdog reset).
 *
 * Breadcrumb window: 0x50000700, 5 x uint32_t words:
 *   [0] magic       0x414c4c43  ("ALLC")  — proves this code ran
 *   [1] result      1 = all checks passed, 0 = a check failed
 *   [2] n_blocks    pool_init()'s reported block count for the test arena
 *   [3] peak_used   max blocks observed allocated at once (n_blocks - free)
 *   [4] fail_code   0 if result==1; otherwise which check failed, see
 *                   ALLC_FAIL_* below
 *
 * This window (0x50000700) is deliberately distinct from main.c's
 * 0x50000000..0x5000001C progress breadcrumb (word0 magic 0xB2D0CAFE) so
 * the two subsystems never collide in DRAM.
 * ------------------------------------------------------------------ */
#define ALLC_BC_BASE   0x50000700UL
#define ALLC_BC_MAGIC  0x414c4c43u   /* "ALLC" */

#define ALLC_FAIL_NONE       0u
#define ALLC_FAIL_INIT       1u   /* pool_init() failed */
#define ALLC_FAIL_ALLOC      2u   /* pool_alloc() returned NULL early */
#define ALLC_FAIL_OVERLAP    3u   /* two allocations aliased */
#define ALLC_FAIL_ALIGN      4u   /* a returned pointer was misaligned */
#define ALLC_FAIL_EXHAUST    5u   /* pool_alloc() didn't return NULL once full */
#define ALLC_FAIL_FREE       6u   /* pool_free() rejected a valid pointer */
#define ALLC_FAIL_REFILL     7u   /* free_count didn't return to n_blocks */
#define ALLC_FAIL_BADFREE    8u   /* pool_free() accepted an invalid pointer */

void alloc_selftest(void);

#endif /* BZDOS_ALLOC_H */
