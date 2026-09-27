/* SPDX-License-Identifier: BSD-2-Clause */
/* vzram_pool.c — see vzram_pool.h. No board asm; hosted-testable like lz4.c
 * (test_vzram_pool.c compiles this file directly, same as test_lz4.c does
 * for lz4.c). */
#include "vzram_pool.h"
#include "lz4.h"
#include <string.h>

const uint32_t vzram_class_size[VZRAM_NCLASS] = { 256, 512, 1024, 2048, 4096 };

/* One caller at a time (see vzram_pool.h's LOCKING note) -- a static
 * scratch buffer avoids putting LZ4_BOUND(VZRAM_PAGE) (~4128) bytes on
 * EL2's per-core stack, same reasoning as lz4.c's own static g_hash. */
static uint8_t g_zc_scratch[LZ4_BOUND(VZRAM_PAGE)];

static inline uint32_t
class_for_len(uint32_t n)
{
	uint32_t c;
	for (c = 0; c < VZRAM_NCLASS; c++)
		if (n <= vzram_class_size[c])
			return c;
	return VZRAM_NCLASS;   /* unreachable: raw store is always <= VZRAM_PAGE
	                        * and VZRAM_PAGE is the largest class */
}

static inline uint32_t
rd_next(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void
wr_next(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Every size class is >= 256 bytes, so a freed slab always has room for the
 * 4-byte free-list link stashed at its own start. */
static int
slot_alloc(vzram_pool_t *p, uint32_t class_idx, uint32_t *out_off)
{
	uint32_t sz;

	if (p->free_head[class_idx] != VZRAM_NONE) {
		uint32_t off = p->free_head[class_idx];
		p->free_head[class_idx] = rd_next(p->pool + off);
		*out_off = off;
		return 0;
	}
	sz = vzram_class_size[class_idx];
	if (p->pool_size - p->bump_next < sz)
		return -1;
	*out_off = p->bump_next;
	p->bump_next += sz;
	return 0;
}

static void
slot_free(vzram_pool_t *p, uint32_t class_idx, uint32_t off)
{
	wr_next(p->pool + off, p->free_head[class_idx]);
	p->free_head[class_idx] = off;
}

void
vzram_pool_init(vzram_pool_t *p, uint8_t *pool, uint32_t pool_size,
                vzram_slot_t *slots, uint32_t nslots_ceiling_backed,
                uint32_t nslots_initial)
{
	uint32_t i, c;

	p->pool = pool;
	p->pool_size = pool_size;
	p->bump_next = 0;
	for (c = 0; c < VZRAM_NCLASS; c++)
		p->free_head[c] = VZRAM_NONE;
	p->slots = slots;
	p->nslots = nslots_initial;
	(void)nslots_ceiling_backed; /* the caller's contract, not checked here
	                              * (no way to know the true backing size
	                              * from this layer alone) */
	for (i = 0; i < nslots_initial; i++) {
		slots[i].off = VZRAM_NONE;
		slots[i].clen = 0;
		slots[i].class_idx = 0;
		slots[i].raw = 0;
	}
}

int
vzram_pool_grow(vzram_pool_t *p, uint32_t new_nslots)
{
	uint32_t i;

	if (new_nslots < p->nslots)
		return -1;
	for (i = p->nslots; i < new_nslots; i++) {
		p->slots[i].off = VZRAM_NONE;
		p->slots[i].clen = 0;
		p->slots[i].class_idx = 0;
		p->slots[i].raw = 0;
	}
	p->nslots = new_nslots;
	return 0;
}

int
vzram_page_read(const vzram_pool_t *p, uint32_t idx, uint8_t *dst)
{
	const vzram_slot_t *s;

	if (idx >= p->nslots)
		return -1;
	s = &p->slots[idx];
	if (s->off == VZRAM_NONE) {
		memset(dst, 0, VZRAM_PAGE);
		return 0;
	}
	if (s->raw) {
		memcpy(dst, p->pool + s->off, VZRAM_PAGE);
		return 0;
	}
	return lz4_decompress(p->pool + s->off, s->clen, dst, VZRAM_PAGE);
}

int
vzram_page_write(vzram_pool_t *p, uint32_t idx, const uint8_t *src)
{
	vzram_slot_t *s;
	uint32_t clen, store_len, class_idx, new_off;
	uint8_t raw;

	if (idx >= p->nslots)
		return -1;

	/* cap < VZRAM_PAGE: a result only counts as "compressed" if it is
	 * strictly smaller than storing the page raw. */
	clen = lz4_compress(src, VZRAM_PAGE, g_zc_scratch, VZRAM_PAGE - 1);
	if (clen == 0) {
		raw = 1;
		store_len = VZRAM_PAGE;
	} else {
		raw = 0;
		store_len = clen;
	}
	class_idx = class_for_len(store_len);
	if (slot_alloc(p, class_idx, &new_off) != 0)
		return -1;   /* pool full for this class; `idx`'s old slot,
		              * if any, is untouched */

	if (raw)
		memcpy(p->pool + new_off, src, VZRAM_PAGE);
	else
		memcpy(p->pool + new_off, g_zc_scratch, clen);

	s = &p->slots[idx];
	if (s->off != VZRAM_NONE)
		slot_free(p, s->class_idx, s->off);
	s->off = new_off;
	s->clen = (uint16_t)store_len;
	s->class_idx = (uint8_t)class_idx;
	s->raw = raw;
	return 0;
}
