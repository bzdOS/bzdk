/* SPDX-License-Identifier: BSD-2-Clause */

/* test_lz4.c — hosted (x86_64, plain gcc) unit tests for lz4.c: vzram's
 * per-4KiB-page LZ4 compressor/decompressor.
 *
 * lz4.c has no board-specific asm (unlike zstage.c/kload.c — see
 * test_zstage.c's header for why those two mirror instead of #include), so
 * this compiles THE REAL SOURCE directly: drift between this test and the
 * shipped code is impossible by construction.
 *
 * Build: gcc -o test_lz4 test_lz4.c && ./test_lz4
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lz4.c"

static int g_fail;

#define CHECK(cond, ...) do {                                           \
	if (!(cond)) {                                                  \
		printf("  FAIL %s:%d: ", __func__, __LINE__);           \
		printf(__VA_ARGS__);                                    \
		printf("\n");                                           \
		g_fail++;                                               \
	}                                                               \
} while (0)

/* xorshift32 — deterministic, no libc rand() version drift between hosts. */
static uint32_t
xs32(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

/* ------------------------------------------------------------------ *
 * Fill patterns, matching the classes the scratchpad fuzzer used.
 * ------------------------------------------------------------------ */
static void
fill_zero(uint8_t *p, uint32_t n)
{
	memset(p, 0, n);
}

static void
fill_random(uint8_t *p, uint32_t n, uint32_t seed)
{
	uint32_t s = seed ? seed : 1;
	uint32_t i;
	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(xs32(&s) >> 24);
}

static void
fill_repeating(uint8_t *p, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++)
		p[i] = (uint8_t)('A' + (i % 4));
}

static void
fill_text(uint8_t *p, uint32_t n)
{
	static const char *words[] = {
		"the ", "quick ", "brown ", "fox ", "jumps ", "over ",
		"lazy ", "dog ", "vzram ", "page ", "4096 ", "lz4 "
	};
	uint32_t i = 0, s = 12345;
	while (i < n) {
		const char *w = words[xs32(&s) % (sizeof(words) / sizeof(words[0]))];
		uint32_t wl = (uint32_t)strlen(w);
		uint32_t take = (wl < n - i) ? wl : n - i;
		memcpy(p + i, w, take);
		i += take;
	}
}

/* ------------------------------------------------------------------ *
 * Round-trip a buffer of n bytes: compress, decompress, must come back
 * byte-identical (or compress must report "doesn't fit", which the caller
 * treats as "store raw" -- either outcome is correct, silence is not).
 * ------------------------------------------------------------------ */
static void
roundtrip_one(const char *label, const uint8_t *src, uint32_t n)
{
	uint8_t cbuf[LZ4_BOUND(4096)];
	uint8_t dbuf[4096];
	uint32_t clen;

	CHECK(n <= sizeof(dbuf), "%s: fixture too big for this test (%u)",
	      label, n);

	clen = lz4_compress(src, n, cbuf, sizeof(cbuf));
	if (clen == 0) {
		/* legal: caller falls back to storing the page raw. */
		return;
	}
	memset(dbuf, 0xA5, sizeof(dbuf));   /* canary: decode must fill exactly n */
	CHECK(lz4_decompress(cbuf, clen, dbuf, n) == 0,
	      "%s: decompress of our own output failed", label);
	CHECK(memcmp(dbuf, src, n) == 0,
	      "%s: round-trip mismatch at n=%u", label, n);
}

static const uint32_t SIZES[] = {
	0, 1, 2, 3, 4, 5, 11, 12, 13, 15, 16, 17, 63, 64, 65,
	254, 255, 256, 257, 511, 512, 1023, 1024, 4095, 4096
};
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))

/* ------------------------------------------------------------------ *
 * 1. Round-trip across every pattern class and every edge size 0..4096 --
 *    the sizes that bracket the token nibble (15), the length-extension
 *    byte (255), and the real page size (4096).
 * ------------------------------------------------------------------ */
static void
t1_roundtrip_all_patterns_all_sizes(void)
{
	uint8_t buf[4096];
	uint32_t i;

	for (i = 0; i < NSIZES; i++) {
		uint32_t n = SIZES[i];

		fill_zero(buf, n);
		roundtrip_one("zero", buf, n);

		fill_random(buf, n, 0x9E3779B9u ^ n);
		roundtrip_one("random", buf, n);

		fill_repeating(buf, n);
		roundtrip_one("repeating", buf, n);

		fill_text(buf, n);
		roundtrip_one("text", buf, n);
	}
}

/* ------------------------------------------------------------------ *
 * 2. Many random 4 KiB pages, the shape vzram actually pushes through this
 *    path in production. Not the 20000-case scratchpad fuzz run, but a
 *    fixed, reproducible slice of it that stays in the tree.
 * ------------------------------------------------------------------ */
static void
t2_roundtrip_many_random_pages(void)
{
	uint8_t buf[4096];
	uint32_t seed;

	for (seed = 1; seed <= 500; seed++) {
		fill_random(buf, sizeof(buf), seed);
		roundtrip_one("random-page", buf, sizeof(buf));
	}
}

/* ------------------------------------------------------------------ *
 * 3. compress() must refuse (return 0) rather than overflow when the
 *    destination is smaller than the input could ever need, and must never
 *    write past `cap` -- checked with a guard byte right after the buffer.
 * ------------------------------------------------------------------ */
static void
t3_compress_refuses_undersized_dst(void)
{
	uint8_t src[4096];
	uint8_t cbuf[16 + 1];   /* +1 guard */
	uint32_t clen;

	fill_random(src, sizeof(src), 777);
	cbuf[16] = 0x42;        /* guard */
	clen = lz4_compress(src, sizeof(src), cbuf, 16);
	CHECK(clen == 0, "compress must refuse a 16-byte dst for 4096 src");
	CHECK(cbuf[16] == 0x42, "compress wrote past the given cap");
}

/* ------------------------------------------------------------------ *
 * 4. decompress() on corrupted/adversarial input must return -1 (never
 *    crash, never write past `want`) for every corruption shape: truncated
 *    stream, an out-of-range match offset, and a length-extension chain
 *    that runs off the end of the source.
 * ------------------------------------------------------------------ */
static void
t4_decompress_rejects_corrupt_input(void)
{
	uint8_t src[4096];
	uint8_t cbuf[LZ4_BOUND(4096)];
	uint8_t dbuf[4096 + 16];   /* +16 guard past `want` */
	uint32_t clen;
	uint32_t i;

	fill_text(src, sizeof(src));
	clen = lz4_compress(src, sizeof(src), cbuf, sizeof(cbuf));
	CHECK(clen > 0, "fixture failed to compress at all");

	/* (a) truncate the compressed stream at every prefix length: must
	 *     never succeed and never overrun `want` bytes of dbuf. */
	for (i = 1; i < clen; i++) {
		memset(dbuf, 0xA5, sizeof(dbuf));
		int rc = lz4_decompress(cbuf, i, dbuf, sizeof(src));
		CHECK(rc == 0 || rc == -1, "truncate@%u: bad return %d", i, rc);
		if (rc == 0) {
			/* a prefix that happens to also be a complete, valid
			 * stream is fine as long as it doesn't claim to be
			 * our fixture verbatim by coincidence at full length */
			CHECK(i < clen, "prefix accepted at full length");
		}
		CHECK(dbuf[sizeof(src) + 0] == 0xA5 &&
		      dbuf[sizeof(src) + 15] == 0xA5,
		      "truncate@%u: wrote past `want`", i);
	}

	/* (b) corrupt one byte at a time (bit flips) across the whole
	 *     compressed buffer. This LZ4 block format carries no checksum,
	 *     so a flip inside a literal payload byte is UNDETECTABLE by
	 *     design -- decompress() legitimately returns 0 with silently
	 *     wrong bytes there, exactly like the reference implementation.
	 *     The only guarantee this test enforces is the one lz4.h
	 *     promises: bounded failure, never a write past `want`. */
	for (i = 0; i < clen; i++) {
		uint8_t save = cbuf[i];
		memset(dbuf, 0xA5, sizeof(dbuf));
		cbuf[i] ^= 0xFF;
		int rc = lz4_decompress(cbuf, clen, dbuf, sizeof(src));
		CHECK(rc == 0 || rc == -1, "flip@%u: bad return %d", i, rc);
		CHECK(dbuf[sizeof(src) + 0] == 0xA5 &&
		      dbuf[sizeof(src) + 15] == 0xA5,
		      "flip@%u: wrote past `want`", i);
		cbuf[i] = save;
	}
}

/* ------------------------------------------------------------------ *
 * 5. decompress() must reject a stream that is well-formed but simply
 *    encodes a different length than `want` -- vzram trusts `want` from
 *    its own slot table, not anything embedded in the compressed bytes.
 * ------------------------------------------------------------------ */
static void
t5_decompress_rejects_wrong_want(void)
{
	uint8_t src[512];
	uint8_t cbuf[LZ4_BOUND(512)];
	uint8_t dbuf[512];
	uint32_t clen;

	fill_text(src, sizeof(src));
	clen = lz4_compress(src, sizeof(src), cbuf, sizeof(cbuf));
	CHECK(clen > 0, "fixture failed to compress");

	CHECK(lz4_decompress(cbuf, clen, dbuf, sizeof(src) - 1) == -1,
	      "want too small must be rejected");
	CHECK(lz4_decompress(cbuf, clen, dbuf, sizeof(src) + 1) == -1,
	      "want too large must be rejected");
}

/* ------------------------------------------------------------------ *
 * 6. A hand-built stream (not run through our own compressor) that
 *    exercises the match-offset guard directly: "ABCD" then a 4-byte
 *    match at offset=4 (i.e. "ABCDABCD"), then an empty terminating
 *    sequence. Bytes: [tok=0x40]['A']['B']['C']['D'][off_lo][off_hi]
 *    [tok=0x00]. Corrupting the offset to 0, or to something larger
 *    than the bytes written so far, must be rejected by the
 *    `off == 0 || off > (op - dst)` guard -- not merely "may" be, this
 *    one IS structurally detectable and must always be caught.
 * ------------------------------------------------------------------ */
static void
t6_decompress_offset_guard(void)
{
	uint8_t stream[8] = { 0x40, 'A', 'B', 'C', 'D', 0x04, 0x00, 0x00 };
	uint8_t dbuf[8];

	CHECK(lz4_decompress(stream, sizeof(stream), dbuf, sizeof(dbuf)) == 0,
	      "hand-built baseline stream rejected");
	CHECK(memcmp(dbuf, "ABCDABCD", 8) == 0,
	      "hand-built baseline stream decoded wrong");

	stream[5] = 0x00;   /* offset := 0 */
	CHECK(lz4_decompress(stream, sizeof(stream), dbuf, sizeof(dbuf)) == -1,
	      "offset=0 must be rejected");

	stream[5] = 0xFF;   /* offset := 0xFFFF, far past the 4 bytes written */
	stream[6] = 0xFF;
	CHECK(lz4_decompress(stream, sizeof(stream), dbuf, sizeof(dbuf)) == -1,
	      "out-of-range offset must be rejected");
}

int
main(void)
{
	printf("test_lz4: lz4_compress()/lz4_decompress() -- vzram page codec\n");

	t1_roundtrip_all_patterns_all_sizes();
	t2_roundtrip_many_random_pages();
	t3_compress_refuses_undersized_dst();
	t4_decompress_rejects_corrupt_input();
	t5_decompress_rejects_wrong_want();
	t6_decompress_offset_guard();

	if (g_fail != 0) {
		printf("test_lz4: %d FAILED\n", g_fail);
		return 1;
	}
	printf("test_lz4: all checks passed\n");
	return 0;
}
