/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vconsole_pm.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests
 * for the postmortem carry-over arithmetic in vconsole.c's
 * vconsole_postmortem_carry() and bmc.c's bmc_con_pm().
 *
 * ============================================================================
 * WHY THIS TEST EXISTS AT ALL
 * ============================================================================
 * The feature under test is the one that gets used when something has ALREADY
 * gone wrong on hardware, which is the worst possible time to discover it is
 * subtly wrong. That is not hypothetical here: the sibling reader for the LIVE
 * ring (bmc_con_read) shipped with exactly this arithmetic inverted — it walked
 * forward from offset 0 instead of backward from the write cursor — and, with a
 * stale buffer address on top, spent months confidently printing the boot
 * banner when asked "what did the guest say before it died?". It cost the
 * 2026-08-10 kldload investigation four passes; the panic backtrace was in DRAM
 * the whole time. vconsole.h and bmc.c both carry that account.
 *
 * So the two properties this file pins down are the two that were got wrong
 * before, and they are pinned down here rather than on hardware because both
 * are pure integer logic:
 *   1. The copy takes the NEWEST bytes, in order, when the ring has wrapped.
 *      "Newest" is the whole point; taking the oldest 64 KiB of a long boot is
 *      the failure mode that looks like it works (you get plausible console
 *      text!) and answers the wrong question.
 *   2. The copy is LINEARISED — offset 0 is the oldest surviving byte and it
 *      runs forward — so no reader ever repeats the modular arithmetic. The
 *      test asserts the reader can be a plain memcmp against the expected
 *      substring, because if that is true then bmc_con_pm() genuinely cannot
 *      get it wrong.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vconsole.c"`
 * ============================================================================
 * Same reason and same convention as test_vconsole_uart.c, test_vblk_ring.c and
 * every other test_*.c here (see the Makefile's comment above the `test`
 * target): vconsole_postmortem_carry() is pure integer logic wrapped in
 * "dc civac, %0\n\tdsb sy" cache maintenance and fixed absolute DRAM addresses,
 * neither of which exists on x86_64. What is mirrored below is the arithmetic —
 * the cold/warm and layout-version guards, the n/start derivation from
 * total_bytes, the copy loop's index mapping, and bmc_con_pm()'s
 * offset/length clamping. The cache maintenance and the absolute addresses are
 * the parts deliberately NOT mirrored: they are what the hardware run proves.
 *
 * Mirrors are kept honest by being small and by naming their source. If
 * vconsole.c's derivation changes, this file must change with it — the same
 * compromise every other test here makes.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- constants mirrored from vconsole.h / hv_addrmap.h ------------------- */
#define VCONSOLE_MAGIC       0x55415254u   /* "UART" */
#define VCONSOLE_LAYOUT_VER  2u
#define VCONSOLE_BUF_SIZE    0x10000u      /* 64 KiB live ring               */
#define VCONSOLE_BUF_BASE    0x50040000u
#define VCPM_MAGIC           0x5643504Du   /* "VCPM" */
#define VCPM_BUF_SIZE        0x10000u      /* carry-over lane, >= ring size  */

/* ---- test doubles for the two DRAM regions ------------------------------ */

/* The live ring, as the PREVIOUS run left it: header words + byte buffer. */
struct ring {
	uint32_t w[8];
	uint8_t  buf[VCONSOLE_BUF_SIZE];
};

/* The carry-over lane. */
struct pm {
	uint32_t w[8];
	uint8_t  buf[VCPM_BUF_SIZE];
};

/* ---- mirror of vconsole.c's vconsole_postmortem_carry() ------------------
 * Returns 1 if a carry-over was performed, 0 if it declined (which is a real
 * outcome the tests check, not just an error path). */
static int
carry(const struct ring *r, struct pm *p)
{
	uint32_t magic  = r->w[0];
	uint32_t total  = r->w[1];
	uint32_t faults = r->w[2];
	uint32_t ver    = r->w[3];
	uint32_t base   = r->w[4];
	uint32_t size   = r->w[5];
	uint32_t cap    = VCONSOLE_BUF_SIZE;
	uint32_t n, start, gen, i;

	if (magic != VCONSOLE_MAGIC || total == 0u)
		return 0;
	if (ver != VCONSOLE_LAYOUT_VER || base != VCONSOLE_BUF_BASE || size != cap)
		return 0;

	n = (total < cap) ? total : cap;
	start = (total - n) % cap;

	gen = (p->w[0] == VCPM_MAGIC) ? p->w[3] : 0u;

	for (i = 0; i < n; i++)
		p->buf[i] = r->buf[(start + i) % cap];

	p->w[1] = n;
	p->w[2] = total;
	p->w[3] = gen + 1u;
	p->w[4] = 0;            /* buf base -- absolute address, not mirrored */
	p->w[5] = VCPM_BUF_SIZE;
	p->w[6] = faults;
	p->w[7] = 0;
	p->w[0] = VCPM_MAGIC;
	return 1;
}

/* ---- mirror of bmc.c's bmc_con_pm() window selection --------------------
 * Fills out_off and out_n with the byte window the command would print, or
 * returns 0 for the two refusals (no magic / offset past the end). */
static int
pm_window(const struct pm *p, uint32_t n, uint32_t off, int have_off,
          uint32_t *out_off, uint32_t *out_n)
{
	uint32_t len;

	if (p->w[0] != VCPM_MAGIC)
		return 0;

	len = p->w[1];
	if (len > VCPM_BUF_SIZE)
		len = VCPM_BUF_SIZE;

	if (n == 0u || n > 0x1000u)
		n = 0x1000u;

	if (!have_off)
		off = (len > n) ? len - n : 0u;
	if (off >= len)
		return 0;
	if (n > len - off)
		n = len - off;

	*out_off = off;
	*out_n = n;
	return 1;
}

/* ---- helpers ------------------------------------------------------------ */

#define CHECK(cond, ...) do {                                            \
	if (!(cond)) {                                                   \
		printf("[FAIL] %s:%d: ", __func__, __LINE__);            \
		printf(__VA_ARGS__);                                     \
		printf("\n");                                            \
		exit(1);                                                 \
	}                                                                \
} while (0)

/* Lay down a ring holding `nbytes` of a deterministic byte stream, written as
 * the firmware writes it: offset = total % cap, total unclamped. Byte k of the
 * stream is `stream_byte(k)`, so any position can be predicted independently of
 * the ring's state — which is what lets the assertions below name exact bytes
 * rather than compare two implementations of the same loop against each other. */
static uint8_t
stream_byte(uint32_t k)
{
	/* Printable, period 251 (prime, so it does not align with the 64 KiB
	 * capacity and a mis-derived start offset cannot coincidentally match). */
	return (uint8_t)(0x20u + (k % 251u) % 0x5fu);
}

static void
fill_ring(struct ring *r, uint32_t nbytes)
{
	uint32_t i;

	memset(r, 0, sizeof(*r));
	r->w[0] = VCONSOLE_MAGIC;
	r->w[3] = VCONSOLE_LAYOUT_VER;
	r->w[4] = VCONSOLE_BUF_BASE;
	r->w[5] = VCONSOLE_BUF_SIZE;
	r->w[2] = 7;                    /* arbitrary nonzero fault count */

	for (i = 0; i < nbytes; i++)
		r->buf[i % VCONSOLE_BUF_SIZE] = stream_byte(i);
	r->w[1] = nbytes;
}

/* ---- tests -------------------------------------------------------------- */

/* A cold boot has no magic (DRAM holds whatever it held). Declining is the
 * correct answer: copying unvalidated DRAM as if it were console text would
 * produce a plausible-looking lane full of garbage, which is worse than an
 * empty one because a reader would believe it. */
static void test_cold_boot_declines(void)
{
	struct ring r;
	struct pm p;

	fill_ring(&r, 1000);
	r.w[0] = 0xdeadbeefu;           /* no "UART" magic */
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 0, "carried from a ring with no magic");
	CHECK(p.w[0] != VCPM_MAGIC, "stamped a lane it declined to fill");
}

/* A previous run that never printed a byte has nothing to carry, and stamping
 * an empty lane would make `con pm` claim to hold a log that is 0 bytes long
 * rather than say "no carry-over held". */
static void test_empty_ring_declines(void)
{
	struct ring r;
	struct pm p;

	fill_ring(&r, 0);
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 0, "carried from a ring with total_bytes == 0");
	CHECK(p.w[0] != VCPM_MAGIC, "stamped a lane it declined to fill");
}

/* THE GUARD THAT MATTERS MOST for a mechanism whose whole job is to read memory
 * written by a DIFFERENT build: if the previous run described a buffer this
 * build does not use, its bytes are not where we would look. vconsole.h's
 * layout-version note exists because a silent host/firmware size disagreement
 * already produced bogus reconstructions once. */
static void test_layout_mismatch_declines(void)
{
	struct ring r;
	struct pm p;

	fill_ring(&r, 1000);
	r.w[3] = 1u;                    /* previous run used layout v1 */
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 0, "carried across a layout-version change");

	fill_ring(&r, 1000);
	r.w[4] = 0x50000f10u;           /* the pre-2026-08-01 buffer address */
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 0, "carried across a buffer-base change");

	fill_ring(&r, 1000);
	r.w[5] = 0x0c00u;               /* the original 3 KiB buffer size */
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 0, "carried across a buffer-size change");
}

/* Not yet wrapped: the copy is the whole log, in order, starting at 0. The
 * start derivation must land exactly on 0 here — that it does is why the
 * wrapped and unwrapped cases share one code path with no branch. */
static void test_unwrapped_copies_whole_log_in_order(void)
{
	struct ring r;
	struct pm p;
	uint32_t i, n = 4096;

	fill_ring(&r, n);
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 1, "declined a valid unwrapped ring");
	CHECK(p.w[1] == n, "len %u, want %u", p.w[1], n);
	CHECK(p.w[2] == n, "prev_total %u, want %u", p.w[2], n);
	for (i = 0; i < n; i++)
		CHECK(p.buf[i] == stream_byte(i),
		      "byte %u: got 0x%02x want 0x%02x", i, p.buf[i],
		      stream_byte(i));
}

/* THE ONE THE SIBLING READER GOT WRONG. After a wrap, offset 0 of the live ring
 * holds the OLDEST surviving bytes, so a copy that starts there captures the
 * beginning of the boot and throws away the end — the exact bytes anyone
 * reading a postmortem wants. Assert the carry-over's first byte is stream
 * position (total - cap), not stream position 0, and its last byte is the
 * newest byte ever written. */
static void test_wrapped_takes_the_newest_bytes(void)
{
	struct ring r;
	struct pm p;
	uint32_t total = VCONSOLE_BUF_SIZE + 5000u;   /* wrapped by 5000 bytes */
	uint32_t i;

	fill_ring(&r, total);
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 1, "declined a valid wrapped ring");
	CHECK(p.w[1] == VCONSOLE_BUF_SIZE, "len %u, want %u", p.w[1],
	      VCONSOLE_BUF_SIZE);
	CHECK(p.w[2] == total, "prev_total %u, want %u", p.w[2], total);

	/* First carried byte is the oldest SURVIVING one, i.e. stream position
	 * total-cap -- NOT position 0, which was overwritten by the wrap. */
	CHECK(p.buf[0] == stream_byte(total - VCONSOLE_BUF_SIZE),
	      "first byte 0x%02x, want 0x%02x (stream pos %u)", p.buf[0],
	      stream_byte(total - VCONSOLE_BUF_SIZE),
	      total - VCONSOLE_BUF_SIZE);
	CHECK(p.buf[0] != stream_byte(0),
	      "first byte equals stream position 0 -- copied the oldest bytes, "
	      "which is the bug this test exists for");

	/* Last carried byte is the newest byte the guest ever wrote. */
	CHECK(p.buf[VCONSOLE_BUF_SIZE - 1] == stream_byte(total - 1),
	      "last byte 0x%02x, want 0x%02x (stream pos %u)",
	      p.buf[VCONSOLE_BUF_SIZE - 1], stream_byte(total - 1), total - 1);

	/* And everything in between is contiguous and in order. */
	for (i = 0; i < VCONSOLE_BUF_SIZE; i++)
		CHECK(p.buf[i] == stream_byte(total - VCONSOLE_BUF_SIZE + i),
		      "byte %u out of order", i);
}

/* Exactly full, no wrap yet: total == cap. The boundary between the two cases
 * above, where an off-by-one in the min() or the modulo shows up. */
static void test_exactly_full_boundary(void)
{
	struct ring r;
	struct pm p;
	uint32_t i;

	fill_ring(&r, VCONSOLE_BUF_SIZE);
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 1, "declined an exactly-full ring");
	CHECK(p.w[1] == VCONSOLE_BUF_SIZE, "len %u", p.w[1]);
	for (i = 0; i < VCONSOLE_BUF_SIZE; i++)
		CHECK(p.buf[i] == stream_byte(i), "byte %u wrong", i);
}

/* One byte past full: the first wrap. start must be 1, not 0. */
static void test_one_byte_past_full(void)
{
	struct ring r;
	struct pm p;

	fill_ring(&r, VCONSOLE_BUF_SIZE + 1u);
	memset(&p, 0, sizeof(p));

	CHECK(carry(&r, &p) == 1, "declined a just-wrapped ring");
	CHECK(p.buf[0] == stream_byte(1), "first byte is stream pos %s",
	      p.buf[0] == stream_byte(0) ? "0 (start not advanced past the wrap)"
	                                 : "neither 0 nor 1");
	CHECK(p.buf[VCONSOLE_BUF_SIZE - 1] == stream_byte(VCONSOLE_BUF_SIZE),
	      "last byte is not the newest");
}

/* gen counts captures and SURVIVES them, which is the only way a reader can
 * tell "this is the crash I want" from "I reloaded twice and lost it". */
static void test_gen_increments_across_captures(void)
{
	struct ring r;
	struct pm p;

	memset(&p, 0, sizeof(p));
	fill_ring(&r, 100);
	CHECK(carry(&r, &p) == 1, "first carry declined");
	CHECK(p.w[3] == 1, "gen %u after one capture, want 1", p.w[3]);

	fill_ring(&r, 200);
	CHECK(carry(&r, &p) == 1, "second carry declined");
	CHECK(p.w[3] == 2, "gen %u after two captures, want 2", p.w[3]);
	CHECK(p.w[1] == 200, "second capture did not replace len");

	/* A declined carry must not bump gen -- otherwise gen would count HV
	 * starts rather than captures, and "did my crash get saved?" becomes
	 * unanswerable. */
	fill_ring(&r, 0);
	CHECK(carry(&r, &p) == 0, "carried from an empty ring");
	CHECK(p.w[3] == 2, "gen bumped by a declined carry");
}

/* `bmc con pm` with no arguments shows the TAIL. Same property as the wrap
 * test, one layer up: the reader must default to the end of the log. */
static void test_pm_default_window_is_the_tail(void)
{
	struct ring r;
	struct pm p;
	uint32_t off = 0, n = 0;

	fill_ring(&r, VCONSOLE_BUF_SIZE + 5000u);
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 1, "carry declined");

	CHECK(pm_window(&p, 0, 0, 0, &off, &n) == 1, "window refused");
	CHECK(n == 0x1000u, "default n %u, want 4096", n);
	CHECK(off == VCONSOLE_BUF_SIZE - 0x1000u,
	      "default off %u, want %u (len - n)", off,
	      VCONSOLE_BUF_SIZE - 0x1000u);
	CHECK(off + n == p.w[1], "default window does not end at the newest byte");
}

/* A carry-over shorter than one page must not produce off = len - n underflowed
 * into a huge unsigned offset -- the classic form of this bug. */
static void test_pm_short_carryover_clamps(void)
{
	struct ring r;
	struct pm p;
	uint32_t off = 0xffffffffu, n = 0xffffffffu;

	fill_ring(&r, 100);
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 1, "carry declined");

	CHECK(pm_window(&p, 0, 0, 0, &off, &n) == 1, "window refused");
	CHECK(off == 0, "off %u for a 100-byte carry-over, want 0", off);
	CHECK(n == 100, "n %u, want 100", n);
}

/* Explicit offsets page the whole lane without gaps or overlaps, and the last
 * page is short rather than running past len. This is what bzdctl's --all loop
 * depends on to know when to stop. */
static void test_pm_explicit_paging_covers_exactly(void)
{
	struct ring r;
	struct pm p;
	uint32_t total = VCONSOLE_BUF_SIZE + 5000u;
	uint32_t off = 0, n = 0, cursor = 0, pages = 0;

	fill_ring(&r, total);
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 1, "carry declined");

	while (pm_window(&p, 0x1000u, cursor, 1, &off, &n)) {
		CHECK(off == cursor, "page %u started at %u, want %u", pages, off,
		      cursor);
		/* Every byte in the page is the stream byte the linearisation
		 * promises -- checked through the reader, not the copier, so a
		 * reader-side offset error cannot hide behind a correct copy. */
		for (uint32_t i = 0; i < n; i++)
			CHECK(p.buf[off + i] ==
			      stream_byte(total - VCONSOLE_BUF_SIZE + off + i),
			      "page %u byte %u wrong", pages, i);
		cursor += n;
		pages++;
		CHECK(pages < 64, "paging did not terminate");
	}
	CHECK(cursor == p.w[1], "paging covered %u of %u bytes", cursor, p.w[1]);
	CHECK(pages == 16, "expected 16 4 KiB pages for a 64 KiB lane, got %u",
	      pages);
}

/* No magic => refuse, so `con pm` says "no carry-over held" instead of dumping
 * an uninitialised lane. */
static void test_pm_refuses_without_magic(void)
{
	struct pm p;
	uint32_t off = 0, n = 0;

	memset(&p, 0, sizeof(p));
	p.w[1] = 4096;                  /* a plausible len, but no magic */
	CHECK(pm_window(&p, 0, 0, 0, &off, &n) == 0,
	      "read a lane with no magic");
}

/* An offset past the end is an operator error, not something to clamp silently
 * -- clamping would print the tail and look like the offset was honoured. */
static void test_pm_offset_past_end_refuses(void)
{
	struct ring r;
	struct pm p;
	uint32_t off = 0, n = 0;

	fill_ring(&r, 4096);
	memset(&p, 0, sizeof(p));
	CHECK(carry(&r, &p) == 1, "carry declined");
	CHECK(pm_window(&p, 0x100u, 4096u, 1, &off, &n) == 0,
	      "accepted off == len");
	CHECK(pm_window(&p, 0x100u, 99999u, 1, &off, &n) == 0,
	      "accepted off far past len");
}

/* A stale/absurd len word (the lane is DRAM; nothing stops a previous build or
 * a wild write from leaving nonsense there) must be clamped to the lane, not
 * used to read past it. */
static void test_pm_absurd_len_clamped(void)
{
	struct pm p;
	uint32_t off = 0, n = 0;

	memset(&p, 0, sizeof(p));
	p.w[0] = VCPM_MAGIC;
	p.w[1] = 0xffffffffu;
	CHECK(pm_window(&p, 0x1000u, 0, 1, &off, &n) == 1, "window refused");
	CHECK(off + n <= VCPM_BUF_SIZE, "window [%u,%u) escapes the lane", off,
	      off + n);
}

struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "cold_boot_declines",                 test_cold_boot_declines },
	{ "empty_ring_declines",                test_empty_ring_declines },
	{ "layout_mismatch_declines",           test_layout_mismatch_declines },
	{ "unwrapped_copies_whole_log_in_order",
	  test_unwrapped_copies_whole_log_in_order },
	{ "wrapped_takes_the_newest_bytes",     test_wrapped_takes_the_newest_bytes },
	{ "exactly_full_boundary",              test_exactly_full_boundary },
	{ "one_byte_past_full",                 test_one_byte_past_full },
	{ "gen_increments_across_captures",     test_gen_increments_across_captures },
	{ "pm_default_window_is_the_tail",      test_pm_default_window_is_the_tail },
	{ "pm_short_carryover_clamps",          test_pm_short_carryover_clamps },
	{ "pm_explicit_paging_covers_exactly",  test_pm_explicit_paging_covers_exactly },
	{ "pm_refuses_without_magic",           test_pm_refuses_without_magic },
	{ "pm_offset_past_end_refuses",         test_pm_offset_past_end_refuses },
	{ "pm_absurd_len_clamped",              test_pm_absurd_len_clamped },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- vconsole postmortem carry-over tests: %d/%d passed ----\n",
	       passed, n);
	return (passed == n) ? 0 : 1;
}
