/* SPDX-License-Identifier: BSD-2-Clause */

/* vzram_pool.h — the compressed-page store behind vzram (HANDOFF item 3),
 * split out from the virtio-blk device itself so it can be hosted-tested
 * like lz4.c/zstage.c: no board asm, no MMIO, just the slab allocator and
 * the per-logical-page slot table.
 *
 * DESIGN CHOSEN FOR "dynamic" SIZING (2026-09-27 decision): the exposed
 * virtio-blk capacity is NOT a fixed literal. `vzram_pool_init()` takes
 * `nslots` (the logical page count == exposed capacity / 4 KiB) as a
 * parameter the caller can grow over the device's life by re-calling with
 * a larger `nslots` against the SAME backing pool and slot array (the array
 * must already be allocated for the eventual ceiling — see vblk_zram.c's
 * config-change wiring, not yet written). The *physical* pool a page can
 * ever be compressed into is capped by `pool_size`, chosen once at boot;
 * that number is arbitrary for now (HANDOFF's "one number still open" is
 * resolved as "start small, grow live" rather than "pick a permanent size"
 * -- see HANDOFF item 3).
 *
 * ALLOCATOR: fixed size classes (256, 512, 1024, 2048, 4096 bytes), each
 * served by its own free list plus a shared bump pointer for never-touched
 * pool bytes. This gives real over-commit (a compressible page costs far
 * less than 4096 bytes) at the cost of a real, accepted limitation: once
 * pool bytes are carved for one class they are never returned to the
 * shared bump pool, even after every slab in that class is freed -- no
 * cross-class coalescing. A workload whose compressibility distribution
 * shifts hard over time can fragment the pool below its nominal capacity.
 * Documented, not hidden; a proper slab allocator (zsmalloc-style) is a
 * fine future upgrade if that turns out to matter in practice.
 *
 * LOCKING: none in here -- exactly like lz4.c's static hash table, this
 * expects ONE caller at a time (vblk_zram.c's single device lock across the
 * whole compress/decompress/copy path, per this project's one proven bug
 * class here -- see the SD gather postmortem, and vblk-sd-shared-bounce
 * -under-lock).
 */
#ifndef BZDOS_VZRAM_POOL_H
#define BZDOS_VZRAM_POOL_H
#include <stdint.h>

#define VZRAM_PAGE     4096u
#define VZRAM_NCLASS   5u
#define VZRAM_NONE     0xFFFFFFFFu

extern const uint32_t vzram_class_size[VZRAM_NCLASS]; /* 256,512,1024,2048,4096 */

typedef struct {
	uint32_t off;        /* offset of this page's slab within the pool,
	                       * VZRAM_NONE if the page was never written */
	uint16_t clen;        /* stored length: compressed length, or exactly
	                       * VZRAM_PAGE when `raw` */
	uint8_t  class_idx;   /* index into vzram_class_size[]; valid only
	                       * when off != VZRAM_NONE */
	uint8_t  raw;         /* 1 = stored uncompressed (didn't fit or wasn't
	                       * worth it) */
} vzram_slot_t;

typedef struct {
	uint8_t       *pool;              /* backing bytes, size pool_size */
	uint32_t       pool_size;
	uint32_t       bump_next;         /* first never-touched pool offset */
	uint32_t       free_head[VZRAM_NCLASS]; /* intrusive free lists */
	vzram_slot_t  *slots;             /* caller-owned array, ceiling-sized */
	uint32_t       nslots;            /* logical pages currently exposed */
} vzram_pool_t;

/* `slots` must already be sized for the largest `nslots` this pool will
 * ever be grown to (vzram_pool_grow() only ever raises `nslots`, it never
 * reallocates `slots`). Every slot starts unwritten (reads as zero). */
void vzram_pool_init(vzram_pool_t *p, uint8_t *pool, uint32_t pool_size,
                      vzram_slot_t *slots, uint32_t nslots_ceiling_backed,
                      uint32_t nslots_initial);

/* Raise the exposed logical page count. `new_nslots` must be <= the
 * ceiling `slots` was allocated for; the newly exposed slots start
 * unwritten. Returns 0 on success, -1 if new_nslots < current nslots
 * (this layer never shrinks -- see vblk_zram.c for why shrinking a live
 * swap device is refused at the virtio layer too). */
int vzram_pool_grow(vzram_pool_t *p, uint32_t new_nslots);

/* Read logical page `idx` into dst[VZRAM_PAGE]. A page never written reads
 * as all-zero. Returns 0 on success, -1 for idx >= nslots or corrupt
 * bookkeeping (defensive only -- should be unreachable). */
int vzram_page_read(const vzram_pool_t *p, uint32_t idx, uint8_t *dst);

/* Compress and store src[VZRAM_PAGE] as logical page `idx`. On failure
 * (pool exhausted for the needed size class) the page's PRIOR contents are
 * left completely intact -- a write that can't be stored must not corrupt
 * or lose the page it was replacing. Returns 0 on success, -1 on failure. */
int vzram_page_write(vzram_pool_t *p, uint32_t idx, const uint8_t *src);

#endif /* BZDOS_VZRAM_POOL_H */
