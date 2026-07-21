/* alloc.c — deterministic static region / slab allocator (see alloc.h).
 *
 * No dynamic memory, no OS, no libc: pools are carved out of static or
 * caller-owned arenas at init time and blocks are handed out/reclaimed by
 * popping/pushing an intrusive free list. Freestanding build (see the
 * project's aarch64-linux-gnu-gcc -ffreestanding -nostdlib invocation);
 * only <stdint.h> is used, matching wdt.c/main.c house style.
 */
#include <stdint.h>
#include "alloc.h"

/* ------------------------------------------------------------------ *
 * Small local helpers — no libc, so we roll our own.
 * ------------------------------------------------------------------ */
static uint32_t
align_up_u32(uint32_t v, uint32_t align)
{
	return (v + (align - 1u)) & ~(align - 1u);
}

static uintptr_t
align_up_ptr(uintptr_t v, uintptr_t align)
{
	return (v + (align - 1u)) & ~(align - 1u);
}

/* ------------------------------------------------------------------ *
 * Tier 1: fixed-block pool allocator.
 * ------------------------------------------------------------------ */

/*
 * Arena -> blocks math:
 *
 *   1. block_size is rounded UP to a POOL_ALIGN (16-byte) multiple. This
 *      is the "stride" between consecutive blocks; it guarantees every
 *      block start address is 16-byte aligned PROVIDED the arena's base
 *      is also 16-byte aligned (step 2), since base + k*stride stays a
 *      multiple of 16 above an aligned base.
 *
 *   2. The caller's arena pointer is rounded UP to the next POOL_ALIGN
 *      boundary. Any bytes lost to this rounding ("pad") are subtracted
 *      from arena_size before carving — we never carve blocks from
 *      unaligned memory, we just shrink the usable region by at most
 *      (POOL_ALIGN - 1) bytes.
 *
 *   3. n_blocks = floor(usable_size / aligned_block_size). Any remainder
 *      (usable_size % aligned_block_size) is simply unused tail padding —
 *      deterministic and computed once, never scanned for at alloc time.
 *
 *   4. The free list is built once, forward, in O(n_blocks): block i's
 *      first machine word (an intrusive `void *next`) is set to point at
 *      block i+1, and the last block's next pointer is NULL. free_head
 *      starts at block 0.
 *
 * Failure modes (return 0, and *p is left zeroed so a subsequent
 * pool_alloc() deterministically yields NULL instead of touching
 * uninitialized fields):
 *   - p/arena NULL, arena_size == 0, or block_size == 0
 *   - alignment padding alone would consume the whole arena
 *   - the (aligned, padded) usable region is smaller than one aligned block
 */
int
pool_init(struct pool *p, void *arena, uint32_t arena_size, uint32_t block_size)
{
	uintptr_t raw;
	uintptr_t aligned;
	uintptr_t pad;
	uint32_t usable;
	uint32_t aligned_block;
	uint32_t n_blocks;
	uint8_t *base;
	uint32_t i;

	if (p == (void *)0) {
		return 0;
	}

	/* Zero the pool struct up front so any early-return leaves it in a
	 * safe, all-empty state (free_head NULL, n_blocks 0). */
	p->base = (void *)0;
	p->block_size = 0;
	p->n_blocks = 0;
	p->free_count = 0;
	p->free_head = (void *)0;

	if (arena == (void *)0 || arena_size == 0u || block_size == 0u) {
		return 0;
	}

	raw = (uintptr_t)arena;
	aligned = align_up_ptr(raw, POOL_ALIGN);
	pad = aligned - raw;

	if (pad >= (uintptr_t)arena_size) {
		return 0; /* alignment padding alone consumes the whole arena */
	}

	usable = arena_size - (uint32_t)pad;
	aligned_block = align_up_u32(block_size, POOL_ALIGN);

	if (aligned_block == 0u || usable < aligned_block) {
		return 0;
	}

	n_blocks = usable / aligned_block;
	base = (uint8_t *)aligned;

	/* Thread the intrusive free list: each free block's first word holds
	 * the address of the next free block. O(n_blocks), done once, here —
	 * never repeated or rescanned during alloc/free. */
	for (i = 0; i < n_blocks; i++) {
		uint8_t *blk = base + (uintptr_t)i * aligned_block;
		void **next_slot = (void **)(void *)blk;

		if (i + 1u < n_blocks) {
			*next_slot = (void *)(base + (uintptr_t)(i + 1u) * aligned_block);
		} else {
			*next_slot = (void *)0;
		}
	}

	p->base = base;
	p->block_size = aligned_block;
	p->n_blocks = n_blocks;
	p->free_count = n_blocks;
	p->free_head = (n_blocks > 0u) ? (void *)base : (void *)0;

	return 1;
}

/* O(1): pop the free-list head — a single load to fetch the next pointer,
 * a single store to advance free_head, a decrement. No scanning, no
 * branching on block state beyond "is the list empty". Constant-time
 * regardless of pool size or occupancy. */
void *
pool_alloc(struct pool *p)
{
	void *blk;
	void **next_slot;

	if (p == (void *)0 || p->free_head == (void *)0) {
		return (void *)0;
	}

	blk = p->free_head;
	next_slot = (void **)blk;
	p->free_head = *next_slot;
	p->free_count--;

	return blk;
}

/* O(1): bounds/alignment check (fixed arithmetic, no loops) then push onto
 * the free list. Rejects (and does not corrupt state for) any pointer that
 * isn't exactly one of the blocks this pool carved out. */
int
pool_free(struct pool *p, void *ptr)
{
	uintptr_t base;
	uintptr_t addr;
	uintptr_t off;
	void **next_slot;

	if (p == (void *)0 || ptr == (void *)0 || p->base == (void *)0) {
		return 0;
	}

	base = (uintptr_t)p->base;
	addr = (uintptr_t)ptr;

	if (addr < base) {
		return 0;
	}

	off = addr - base;

	/* Must land exactly on a block boundary and inside the carved range. */
	if ((off % p->block_size) != 0u) {
		return 0;
	}
	if ((off / p->block_size) >= (uintptr_t)p->n_blocks) {
		return 0;
	}

	next_slot = (void **)ptr;
	*next_slot = p->free_head;
	p->free_head = ptr;
	p->free_count++;

	return 1;
}

void
pool_stats(struct pool *p, uint32_t *free_out, uint32_t *used_out)
{
	uint32_t free_count = 0u;
	uint32_t n_blocks = 0u;

	if (p != (void *)0) {
		free_count = p->free_count;
		n_blocks = p->n_blocks;
	}

	if (free_out != (void *)0) {
		*free_out = free_count;
	}
	if (used_out != (void *)0) {
		*used_out = n_blocks - free_count;
	}
}

/* ------------------------------------------------------------------ *
 * Tier 2: bump arena for one-shot, never-freed startup allocations.
 * ------------------------------------------------------------------ */
void
arena_init(struct arena *a, void *mem, uint32_t size)
{
	if (a == (void *)0) {
		return;
	}
	a->base = (uint8_t *)mem;
	a->size = (mem != (void *)0) ? size : 0u;
	a->offset = 0u;
}

void *
arena_bump(struct arena *a, uint32_t size)
{
	uint32_t start;
	uint32_t end;

	if (a == (void *)0 || a->base == (void *)0 || size == 0u) {
		return (void *)0;
	}

	start = align_up_u32(a->offset, POOL_ALIGN);
	end = start + size; /* startup-scale sizes only; not guarding wrap */

	if (end > a->size) {
		return (void *)0;
	}

	a->offset = end;
	return (void *)(a->base + start);
}

/* ------------------------------------------------------------------ *
 * Self-test.
 * ------------------------------------------------------------------ */
static inline void
allc_bc_write(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(ALLC_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}
#define ALLC_BC(i, v) allc_bc_write((i), (uint32_t)(v))

#define ALLC_TEST_BLOCK_SIZE 64u
#define ALLC_TEST_MAX_BLOCKS 256u /* upper bound for the pointer-tracking array below */

static uint8_t allc_test_arena[4096];

void
alloc_selftest(void)
{
	struct pool p;
	void *slots[ALLC_TEST_MAX_BLOCKS];
	uint32_t n;
	uint32_t i;
	uint32_t j;
	uint32_t free_c, used_c;
	uint32_t peak_used = 0u;
	uint32_t fail_code = ALLC_FAIL_NONE;

	ALLC_BC(0, ALLC_BC_MAGIC);
	ALLC_BC(1, 0u); /* assume fail until proven otherwise */
	ALLC_BC(2, 0u);
	ALLC_BC(3, 0u);
	ALLC_BC(4, ALLC_FAIL_NONE);

	if (!pool_init(&p, allc_test_arena, (uint32_t)sizeof(allc_test_arena),
		       ALLC_TEST_BLOCK_SIZE)) {
		fail_code = ALLC_FAIL_INIT;
		goto done;
	}

	n = p.n_blocks;
	ALLC_BC(2, n);

	if (n == 0u || n > ALLC_TEST_MAX_BLOCKS) {
		fail_code = ALLC_FAIL_INIT;
		goto done;
	}

	/* 1. Allocate every block; verify each pointer is non-NULL, 16-byte
	 *    aligned, and distinct/non-overlapping vs. every prior pointer. */
	for (i = 0; i < n; i++) {
		void *blk = pool_alloc(&p);

		if (blk == (void *)0) {
			fail_code = ALLC_FAIL_ALLOC;
			goto done;
		}
		if (((uintptr_t)blk & (POOL_ALIGN - 1u)) != 0u) {
			fail_code = ALLC_FAIL_ALIGN;
			goto done;
		}
		for (j = 0; j < i; j++) {
			uintptr_t a = (uintptr_t)slots[j];
			uintptr_t b = (uintptr_t)blk;
			uintptr_t lo = (a < b) ? a : b;
			uintptr_t hi = (a < b) ? b : a;

			if (hi - lo < (uintptr_t)p.block_size) {
				fail_code = ALLC_FAIL_OVERLAP;
				goto done;
			}
		}
		slots[i] = blk;

		pool_stats(&p, &free_c, &used_c);
		if (used_c > peak_used) {
			peak_used = used_c;
		}
	}

	/* 2. Pool must now be exhausted. */
	if (pool_alloc(&p) != (void *)0) {
		fail_code = ALLC_FAIL_EXHAUST;
		goto done;
	}
	pool_stats(&p, &free_c, &used_c);
	if (free_c != 0u || used_c != n) {
		fail_code = ALLC_FAIL_EXHAUST;
		goto done;
	}

	/* 2b. An invalid pointer (outside the arena) must be rejected. */
	{
		uint8_t bogus;
		if (pool_free(&p, &bogus) != 0) {
			fail_code = ALLC_FAIL_BADFREE;
			goto done;
		}
	}

	/* 3. Free every block back. */
	for (i = 0; i < n; i++) {
		if (!pool_free(&p, slots[i])) {
			fail_code = ALLC_FAIL_FREE;
			goto done;
		}
	}
	pool_stats(&p, &free_c, &used_c);
	if (free_c != n || used_c != 0u) {
		fail_code = ALLC_FAIL_REFILL;
		goto done;
	}

	/* 4. Re-allocate all blocks again — proves the free list is intact
	 *    and the pool is fully reusable, not just single-shot. */
	for (i = 0; i < n; i++) {
		void *blk = pool_alloc(&p);

		if (blk == (void *)0) {
			fail_code = ALLC_FAIL_ALLOC;
			goto done;
		}
		slots[i] = blk;
	}
	pool_stats(&p, &free_c, &used_c);
	if (free_c != 0u || used_c != n) {
		fail_code = ALLC_FAIL_EXHAUST;
		goto done;
	}

	/* Return the pool to the full-free state we found it in. */
	for (i = 0; i < n; i++) {
		pool_free(&p, slots[i]);
	}
	pool_stats(&p, &free_c, &used_c);
	if (free_c != n) {
		fail_code = ALLC_FAIL_REFILL;
		goto done;
	}

done:
	ALLC_BC(3, peak_used);
	ALLC_BC(4, fail_code);
	ALLC_BC(1, (fail_code == ALLC_FAIL_NONE) ? 1u : 0u);
}
