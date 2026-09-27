/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vzram_pool.c — hosted unit tests for vzram_pool.c (HANDOFF item 3's
 * compressed-page store). Compiles the REAL vzram_pool.c and lz4.c
 * directly (neither has board asm), same reasoning as test_lz4.c/
 * test_zstage.c: drift between this test and the shipped code is
 * impossible by construction.
 *
 * Build: gcc -o test_vzram_pool test_vzram_pool.c && ./test_vzram_pool
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lz4.c"
#include "vzram_pool.c"

static int g_fail;

#define CHECK(cond, ...) do {                                           \
	if (!(cond)) {                                                  \
		printf("  FAIL %s:%d: ", __func__, __LINE__);           \
		printf(__VA_ARGS__);                                    \
		printf("\n");                                           \
		g_fail++;                                               \
	}                                                               \
} while (0)

static uint32_t
xs32(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

static void
fill_random(uint8_t *p, uint32_t n, uint32_t seed)
{
	uint32_t s = seed ? seed : 1, i;
	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(xs32(&s) >> 24);
}

static void
fill_repeating(uint8_t *p, uint32_t n, uint8_t byte)
{
	memset(p, byte, n);
}

/* A generously oversized pool -- these tests probe the allocator's own
 * logic, not real-hardware carve sizing (that number is chosen at
 * board-config.xml wiring time, not here). */
#define POOL_SIZE   (1u << 20)   /* 1 MiB */
#define CEIL_SLOTS  4096u        /* room to grow to 16 MiB exposed */

typedef struct {
	vzram_pool_t p;
	uint8_t pool[POOL_SIZE];
	vzram_slot_t slots[CEIL_SLOTS];
} fixture_t;

static fixture_t *
fix_new(uint32_t nslots_initial)
{
	fixture_t *f = calloc(1, sizeof(*f));
	if (!f) {
		printf("calloc failed\n");
		exit(1);
	}
	vzram_pool_init(&f->p, f->pool, POOL_SIZE, f->slots, CEIL_SLOTS,
	                nslots_initial);
	return f;
}

/* ------------------------------------------------------------------ *
 * 1. A never-written page reads as all-zero.
 * ------------------------------------------------------------------ */
static void
t1_unwritten_reads_zero(void)
{
	fixture_t *f = fix_new(16);
	uint8_t dst[VZRAM_PAGE];
	uint32_t i;

	memset(dst, 0xA5, sizeof(dst));
	CHECK(vzram_page_read(&f->p, 3, dst) == 0, "read of unwritten page failed");
	for (i = 0; i < VZRAM_PAGE; i++)
		if (dst[i] != 0) {
			CHECK(0, "unwritten page not all-zero at byte %u", i);
			break;
		}
	free(f);
}

/* ------------------------------------------------------------------ *
 * 2. Round-trip across pages with very different compressibility, then
 *    confirm every page still reads back correctly after ALL writes (no
 *    cross-page corruption from the shared pool/allocator).
 * ------------------------------------------------------------------ */
static void
t2_roundtrip_mixed_pages(void)
{
	fixture_t *f = fix_new(64);
	uint8_t srcs[64][VZRAM_PAGE];
	uint8_t dst[VZRAM_PAGE];
	uint32_t i;

	for (i = 0; i < 64; i++) {
		switch (i % 4) {
		case 0: fill_repeating(srcs[i], VZRAM_PAGE, (uint8_t)i); break;
		case 1: fill_random(srcs[i], VZRAM_PAGE, 1000 + i); break;
		case 2: memset(srcs[i], 0, VZRAM_PAGE); break;
		default:
			fill_repeating(srcs[i], VZRAM_PAGE, 0x00);
			memset(srcs[i], (uint8_t)(0xC0 + i), 17); /* short run,
			       then zero -- exercises a small literal+match mix */
			break;
		}
		CHECK(vzram_page_write(&f->p, i, srcs[i]) == 0,
		      "write %u failed", i);
	}
	for (i = 0; i < 64; i++) {
		memset(dst, 0xA5, sizeof(dst));
		CHECK(vzram_page_read(&f->p, i, dst) == 0, "read %u failed", i);
		CHECK(memcmp(dst, srcs[i], VZRAM_PAGE) == 0,
		      "page %u corrupted by neighbours", i);
	}
	free(f);
}

/* ------------------------------------------------------------------ *
 * 3. Overwriting the same page many times with alternating
 *    compressible/incompressible content must free the old slab back to
 *    its class's free list (verified indirectly: this must not exhaust
 *    the pool, and the page must always read back as the LATEST write).
 * ------------------------------------------------------------------ */
static void
t3_overwrite_reclaims_old_slab(void)
{
	fixture_t *f = fix_new(1);
	uint8_t compressible[VZRAM_PAGE], incompressible[VZRAM_PAGE], dst[VZRAM_PAGE];
	uint32_t iter;

	fill_repeating(compressible, VZRAM_PAGE, 'Z');
	fill_random(incompressible, VZRAM_PAGE, 42);

	/* Far more iterations than POOL_SIZE could hold without reclaiming. */
	for (iter = 0; iter < 5000; iter++) {
		const uint8_t *src = (iter & 1) ? incompressible : compressible;
		CHECK(vzram_page_write(&f->p, 0, src) == 0,
		      "overwrite %u failed (old slab not reclaimed?)", iter);
		CHECK(vzram_page_read(&f->p, 0, dst) == 0, "read after overwrite %u failed", iter);
		CHECK(memcmp(dst, src, VZRAM_PAGE) == 0,
		      "overwrite %u read back wrong content", iter);
	}
	free(f);
}

/* ------------------------------------------------------------------ *
 * 4. Pool exhaustion for one class must fail the write and leave the
 *    page's PRIOR contents completely intact -- never corrupt or lose
 *    data on a failed write.
 * ------------------------------------------------------------------ */
static void
t4_exhaustion_leaves_old_content_intact(void)
{
	fixture_t *f = fix_new(CEIL_SLOTS);
	uint8_t incompressible[VZRAM_PAGE], dst[VZRAM_PAGE];
	uint32_t i, wrote = 0;
	int rc;

	fill_random(incompressible, VZRAM_PAGE, 7);

	/* Every incompressible page costs a full 4096-byte slab; POOL_SIZE
	 * (1 MiB) / 4096 = 256 slabs before the shared bump pointer is
	 * exhausted (this class has no pre-existing free slabs to draw on). */
	for (i = 0; i < CEIL_SLOTS; i++) {
		/* vary content slightly per page so a bug that silently
		 * aliased two pages to the same slab would be visible below */
		incompressible[0] = (uint8_t)i;
		incompressible[1] = (uint8_t)(i >> 8);
		rc = vzram_page_write(&f->p, i, incompressible);
		if (rc != 0)
			break;
		wrote++;
	}
	CHECK(wrote > 0 && wrote < CEIL_SLOTS,
	      "expected exhaustion partway through, wrote=%u", wrote);

	/* The write that failed (`i`) had no prior content -- must still
	 * read as all-zero (unwritten), not as garbage or a partial slab. */
	CHECK(vzram_page_read(&f->p, i, dst) == 0, "read after failed write errored");
	{
		uint32_t j;
		for (j = 0; j < VZRAM_PAGE; j++)
			if (dst[j] != 0) {
				CHECK(0, "failed write left partial garbage at byte %u", j);
				break;
			}
	}

	/* Now overwrite an EARLIER, already-stored page with new content --
	 * this too must fail (pool still full) and must leave that page's
	 * ORIGINAL content readable, not blank and not the new attempt. */
	{
		uint8_t new_content[VZRAM_PAGE], readback[VZRAM_PAGE];
		fill_random(new_content, VZRAM_PAGE, 99);
		rc = vzram_page_write(&f->p, 0, new_content);
		CHECK(rc == -1, "overwrite of a full pool unexpectedly succeeded");
		CHECK(vzram_page_read(&f->p, 0, readback) == 0, "read of page 0 failed");
		{
			uint8_t expect[VZRAM_PAGE];
			fill_random(expect, VZRAM_PAGE, 7);
			expect[0] = 0; expect[1] = 0;
			CHECK(memcmp(readback, expect, VZRAM_PAGE) == 0,
			      "failed overwrite corrupted page 0's original content");
		}
	}
	free(f);
}

/* ------------------------------------------------------------------ *
 * 5. Growth: new slots appear zeroed, old slots are untouched by the
 *    grow, and shrinking is refused.
 * ------------------------------------------------------------------ */
static void
t5_grow(void)
{
	fixture_t *f = fix_new(4);
	uint8_t src[VZRAM_PAGE], dst[VZRAM_PAGE];
	uint32_t i;

	fill_repeating(src, VZRAM_PAGE, 0x11);
	CHECK(vzram_page_write(&f->p, 2, src) == 0, "seed write failed");

	CHECK(vzram_pool_grow(&f->p, 32) == 0, "grow failed");
	CHECK(f->p.nslots == 32, "nslots not updated after grow");

	CHECK(vzram_page_read(&f->p, 2, dst) == 0, "read of pre-grow page failed");
	CHECK(memcmp(dst, src, VZRAM_PAGE) == 0,
	      "grow corrupted a page that predates it");

	for (i = 4; i < 32; i++) {
		memset(dst, 0xA5, sizeof(dst));
		CHECK(vzram_page_read(&f->p, i, dst) == 0, "read of new slot %u failed", i);
		CHECK(dst[0] == 0 && dst[VZRAM_PAGE - 1] == 0,
		      "newly grown slot %u not zeroed", i);
	}

	CHECK(vzram_pool_grow(&f->p, 8) == -1, "shrink via grow() must be refused");
	CHECK(f->p.nslots == 32, "nslots changed despite refused shrink");
	free(f);
}

int
main(void)
{
	printf("test_vzram_pool: vzram_pool.c -- compressed-page store\n");

	t1_unwritten_reads_zero();
	t2_roundtrip_mixed_pages();
	t3_overwrite_reclaims_old_slab();
	t4_exhaustion_leaves_old_content_intact();
	t5_grow();

	if (g_fail != 0) {
		printf("test_vzram_pool: %d FAILED\n", g_fail);
		return 1;
	}
	printf("test_vzram_pool: all checks passed\n");
	return 0;
}
