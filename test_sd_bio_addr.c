/* SPDX-License-Identifier: BSD-2-Clause */

/* test_sd_bio_addr.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the two pieces of PURE decision logic in sd_bio.c: (1) the
 * SDHC-vs-SDSC card-type detection and the block-vs-byte command-argument
 * translation it feeds (sd_addr()), and (2) sd_bio_write()'s write-completion
 * return-code decision.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "sd_bio.c"` WITH STUBS
 * ============================================================================
 * sd_bio.c was read in full before writing this file. The logic under test is
 * genuinely pure — `g_sd_block_addressed ? lba : (lba * 512u)` is integer
 * arithmetic, and the write-completion decision is a chain of bit tests and one
 * unsigned tick comparison — but the FILE is not includable on x86_64:
 *   - rd_cntpct()    "isb; mrs %0, cntpct_el0"              (sd_bio.c:133-138)
 *   - rd_cntfrq()    "mrs %0, cntfrq_el0"                   (sd_bio.c:140-145)
 *   - sdbc()         "dc civac, %0; dsb sy"                 (sd_bio.c:158-163)
 *   - sd_bio_read/write()  "dsb sy"                     (sd_bio.c:382/399/...)
 * every one of which is a raw AArch64 system-register / cache-maintenance
 * mnemonic the x86_64 assembler rejects. Worse, ALL of the logic under test is
 * interleaved with hard-coded MMIO through rd32()/wr32() on absolute physical
 * addresses (SD_BASE 0x01c0f000, PIO_PF_CFG0 0x01C208B4, CCU_MMC0_CLK
 * 0x01c20088, and the SDBC breadcrumb window from hv_addrmap.h): sd_addr() is
 * only ever called from inside `wreg(REG_CAGR, sd_addr(lba))`
 * (sd_bio.c:350/407), and g_sd_block_addressed is only ever assigned in the
 * middle of sd_bio_init()'s 10-command identification sequence
 * (sd_bio.c:300) — dereferencing address 0x01c0f000 on the host segfaults, and
 * sd_bio.c also #includes hv_addrmap.h. Making any of that overridable would
 * mean editing sd_bio.c, which is a read-only reference here.
 *
 * So: this file hand-transcribes the pure functions verbatim, with an exact
 * sd_bio.c line-number citation on every one, and replaces ONLY the MMIO reads
 * and the two timer reads with scripted test doubles (`struct hw` below) that
 * hand back a caller-supplied sequence of register samples and a virtual
 * CNTPCT. The transcribed control flow — statement order, which register is
 * read when, which check happens first — is character-for-character the
 * source's, because in both suites under test THE ORDER IS THE BEHAVIOUR.
 *
 * ============================================================================
 * WHAT THIS FILE PROVES, EXERCISES, AND CANNOT COVER
 * ============================================================================
 * PROVES (about the mirrored logic, hence about the source only insofar as the
 * transcription is faithful — the line citations are there so that can be
 * re-checked by eye):
 *   - sd_addr()'s two branches, including that they differ by exactly 512x and
 *     that the byte-addressed branch WRAPS (it does not saturate or error) once
 *     lba*512 exceeds 32 bits;
 *   - that the CCS decision is derived from the ACMD41 response word alone, and
 *     specifically from the word that had OCR bit31 (power-up complete) set —
 *     not from any earlier not-ready response;
 *   - the complete return-code map of sd_bio_write()'s tail: 0, the two
 *     POSITIVE 0x4.../0x2... tagged codes, and -2 — and which (rint, elapsed)
 *     input produces each.
 * EXERCISES but does not prove:
 *   - the iteration/time caps. The mirror shows the loops TERMINATE for the
 *     scripted inputs and that ACMD41_RETRY_CAP/SD_POLL_CAP are honoured, but
 *     whether 1000 ms / 4000 ms / 200 retries are the right numbers for real
 *     silicon is an empirical question no host test can answer.
 * CANNOT COVER (deliberately out of scope, needs the board):
 *   - anything about the SMHC controller itself: the clock-config latch dance
 *     (sd_bio.c:265-271), the CMDR command engine, the 128-word FIFO
 *     drain/fill, the pinmux write, and whether a real card actually reports
 *     the OCR/CMD8 responses this file feeds in. In particular a WRONG
 *     addressing decision is invisible here by construction: the test can only
 *     prove "CCS set => arg == lba", never "this card's CCS was set".
 *
 * WHY THIS LOGIC IS WORTH A TEST AT ALL: getting the addressing branch
 * backwards is a 512x address error. On read it returns the wrong sector
 * (garbage); on WRITE it silently destroys an unrelated sector — and because
 * sd_addr() is one ternary buried in a wreg() argument, nothing else in the
 * path would notice. The same is true of the overflow: sd_addr(0x800000) and
 * sd_addr(0) are the SAME byte address on a byte-addressed card, i.e. a large
 * LBA aliases onto sector 0 — the GPT.
 *
 * ============================================================================
 * OPEN CONTRACT MISMATCH FOUND WHILE WRITING THIS FILE (source NOT changed)
 * ============================================================================
 * sd_bio.h:52-56 documents sd_bio_write() as "Returns 0 on success, negative on
 * timeout". sd_bio.c:427-430 returns
 *       (int)(0x40000000u | (ri & 0x3fffu))        /​* data CRC/timeout bits *​/
 *       (int)(0x20000000u | (rreg(REG_RINT) & 0x3fffu))   /​* DATA_OVER timeout *​/
 * both of which are POSITIVE ints (max possible value 0x40003fff, well under
 * INT_MAX, so the conversion is value-preserving — these are not accidentally
 * negative, they are genuinely positive). Only the CARD_BUSY timeout
 * (sd_bio.c:446) returns a negative -2, and the FIFO-fill failure
 * (sd_bio.c:417) returns -1. So a caller written to the header's contract —
 * `if (rc < 0) fail;` — SILENTLY IGNORES the two most interesting write
 * failures and treats a CRC-failed write as success. `if (rc != 0)` is correct
 * and is what a caller must use today.
 * This file does NOT assert the header's promise. It asserts the ACTUAL
 * behaviour (see test_write_error_codes_are_positive_not_negative) so the
 * mismatch is measured and pinned rather than assumed away, and so that
 * whichever side is eventually fixed — widen the header, or make the codes
 * negative — has to be a deliberate change that trips this test.
 *
 * SECOND OBSERVATION, also pinned rather than "fixed": in sd_bio.c:425-428 the
 * DATA_OVER check precedes the error-bit check, so a sample in which the
 * controller has raised BOTH data-over and a data CRC/timeout error is treated
 * as a clean completion and the error bits are discarded (see
 * test_data_over_wins_over_error_bits). Whether that can happen on this IP is
 * a hardware question; the ordering itself is pinned here.
 *
 * Build: gcc -Wall -Wextra -O2 -o test_sd_bio_addr test_sd_bio_addr.c &&
 *        ./test_sd_bio_addr
 * (NOT wired into the Makefile by this file's author — see the task note.)
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>

/* ------------------------------------------------------------------ *
 * Mirrored constants — verbatim from sd_bio.c, with citations.
 * ------------------------------------------------------------------ */
#define SD_POLL_CAP        400000        /* sd_bio.c:100 */
#define ACMD41_RETRY_CAP   200           /* sd_bio.c:101 */

#define CMD8_ARG_3V3_AA    0x000001AAu   /* sd_bio.c:104 */
#define ACMD41_ARG_HCS     0x40000000u   /* sd_bio.c:105 */
#define ACMD41_ARG_VWIN    0x00FF8000u   /* sd_bio.c:106 */
#define OCR_BUSY_READY     0x80000000u   /* sd_bio.c:107 — OCR bit31 */
#define OCR_CCS_SDHC       0x40000000u   /* sd_bio.c:108 — OCR bit30 */

#define RINT_DATA_OVER     0x00000008u   /* sd_bio.c:86 */
/* sd_bio.c:427's inline literal, named here for legibility only. The source
 * writes it as a bare `0x0180u` with the comment "DATA_CRC / DATA_TIMEOUT". */
#define RINT_DATA_ERR      0x00000180u
/* NOTE: STAR_FIFO_FULL happens to share the value 0x08 with RINT_DATA_OVER
 * (sd_bio.c:86 vs :90) — different registers, no relationship. Kept distinct
 * here so a reader does not conflate them. */
#define STAR_FIFO_FULL     0x00000008u   /* sd_bio.c:90 */
#define STAR_CARD_BUSY     0x00000200u   /* sd_bio.c:91 */

#define SD_WRITE_DATA_TIMEOUT_MS   1000u /* sd_bio.c:155 */
#define SD_WRITE_BUSY_TIMEOUT_MS   4000u /* sd_bio.c:156 */

#define A64_CNTFRQ_DEFAULT  24000000ull  /* sd_bio.c:151's fallback */

/* ------------------------------------------------------------------ *
 * Mirrored module state — sd_bio.c:165-167.
 * ------------------------------------------------------------------ */
static int g_sd_block_addressed;   /* sd_bio.c:166 — 1 = SDHC/SDXC */
static int g_sd_inited;            /* sd_bio.c:167 */

/* ------------------------------------------------------------------ *
 * sd_addr() — mirrors sd_bio.c:169-173 VERBATIM (the real one is `static
 * inline`; that changes nothing observable). This is the entire block-vs-byte
 * addressing decision: the value that goes into REG_CAGR at sd_bio.c:350
 * (CMD17 read) and sd_bio.c:407 (CMD24 write).
 * ------------------------------------------------------------------ */
static uint32_t sd_addr(uint32_t lba)
{
	return g_sd_block_addressed ? lba : (lba * 512u);
}

/* ------------------------------------------------------------------ *
 * The uninited guard shared by both transfer paths — sd_bio.c:341-342 and
 * sd_bio.c:396-397. Mirrored because it is what guarantees sd_addr() is never
 * consulted before sd_bio_init() has decided the card type (i.e. never with a
 * stale/default g_sd_block_addressed).
 * ------------------------------------------------------------------ */
static int sd_xfer_guard(void)
{
	if (!g_sd_inited)
		return -100;
	return 0;
}

/* ------------------------------------------------------------------ *
 * CMD8 check-pattern gate — mirrors sd_bio.c:279-287. `cmd8_ok` stands in for
 * `sd_cmd_done(8, CMD8_ARG_3V3_AA, CMDR_RESP_EXP, &resp0) == 0`; when CMD8
 * times out the source leaves v2 at 0 AND leaves resp0 holding whatever it had
 * before (0 at that point in sd_bio_init()), which is why the "CMD8 failed"
 * case below passes cmd8_ok=0.
 * ------------------------------------------------------------------ */
static int cmd8_says_v2(int cmd8_ok, uint32_t resp0)
{
	int v2 = 0;

	if (cmd8_ok) {
		if ((resp0 & 0xFFu) == 0xAAu)
			v2 = 1;
	}
	return v2;
}

/* ACMD41 argument construction — mirrors sd_bio.c:291 verbatim. */
static uint32_t acmd41_arg(int v2)
{
	return ACMD41_ARG_VWIN | (v2 ? ACMD41_ARG_HCS : 0u);
}

/* ------------------------------------------------------------------ *
 * Scripted OCR source for the ACMD41 poll loop. NOT a mirror of anything in
 * sd_bio.c — it replaces `sd_acmd(0, 41, arg, CMDR_RESP_EXP, &resp0)`, i.e.
 * two MMIO command issues plus a RESP0 read, with a caller-supplied sequence of
 * response words. The final entry is held indefinitely so a script can drive an
 * unbounded number of retries.
 * ------------------------------------------------------------------ */
struct ocr_script {
	const uint32_t *w;
	unsigned n;
	unsigned i;
};

static uint32_t ocr_next(struct ocr_script *s)
{
	uint32_t v = s->w[s->i < s->n ? s->i : s->n - 1u];

	if (s->i < s->n)
		s->i++;
	return v;
}

/* ------------------------------------------------------------------ *
 * ACMD41 power-up poll + CCS latch — mirrors sd_bio.c:290-303. The transport
 * failure path (`if (sd_acmd(...) != 0) return -4;`, sd_bio.c:293-295) is NOT
 * mirrored: it is a command-engine timeout, not part of the addressing
 * decision, and it cannot be expressed with a response-word script. Everything
 * else — the retry cap, the bit31 break condition, the i==CAP -5 return, and
 * crucially the fact that g_sd_block_addressed is assigned AFTER the loop from
 * the last-read resp0 — is transcribed exactly.
 * ------------------------------------------------------------------ */
static int acmd41_detect(struct ocr_script *s)
{
	uint32_t resp0 = 0;
	int i;

	for (i = 0; i < ACMD41_RETRY_CAP; i++) {
		resp0 = ocr_next(s);
		if (resp0 & OCR_BUSY_READY)
			break;
	}
	if (i == ACMD41_RETRY_CAP)
		return -5;
	g_sd_block_addressed = (resp0 & OCR_CCS_SDHC) ? 1 : 0;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Scripted hardware double for the write-completion logic. Replaces exactly
 * three things and nothing else:
 *   rreg(REG_RINT)  -> hw_rint()    (a caller-supplied sample sequence)
 *   rreg(REG_STAR)  -> hw_star()    (ditto)
 *   rd_cntpct()     -> hw_cntpct()  (a virtual counter that advances by `dt`
 *                                    on every read, so "elapsed ticks" is
 *                                    driven by the number of polls the
 *                                    mirrored loop performs — the same thing
 *                                    that makes the real loop time out)
 * Register sample sequences hold their final value forever, so an "error state
 * that never clears" is a one-element script. The read INDICES are left visible
 * so tests can assert how many polls the loop actually performed.
 * ------------------------------------------------------------------ */
struct hw {
	const uint32_t *rint;
	unsigned nrint, irint;
	const uint32_t *star;
	unsigned nstar, istar;
	uint64_t now;      /* virtual CNTPCT_EL0 */
	uint64_t dt;       /* ticks added per cntpct read */
	uint64_t cntfrq;   /* virtual CNTFRQ_EL0 (0 exercises the fallback) */
};

static uint32_t hw_rint(struct hw *h)
{
	uint32_t v = h->rint[h->irint < h->nrint ? h->irint : h->nrint - 1u];

	if (h->irint < h->nrint)
		h->irint++;
	return v;
}

static uint32_t hw_star(struct hw *h)
{
	uint32_t v = h->star[h->istar < h->nstar ? h->istar : h->nstar - 1u];

	if (h->istar < h->nstar)
		h->istar++;
	return v;
}

static uint64_t hw_cntpct(struct hw *h)
{
	uint64_t v = h->now;

	h->now += h->dt;
	return v;
}

/* ms_to_ticks() — mirrors sd_bio.c:147-153 verbatim, with rd_cntfrq()
 * replaced by the scripted h->cntfrq (including its 0 fallback). */
static uint64_t ms_to_ticks(struct hw *h, uint32_t ms)
{
	uint64_t f = h->cntfrq;

	if (f == 0)
		f = A64_CNTFRQ_DEFAULT;
	return (f * (uint64_t)ms) / 1000ull;
}

/* ------------------------------------------------------------------ *
 * sd_bio_write()'s FIFO-fill loop + its -1 return — mirrors sd_bio.c:410-417.
 * Included because it completes the return-code map (the only other code
 * sd_bio_write() can produce) and because it is the loop whose SD_POLL_CAP
 * bound is the "a dbgmon call can never hang the debug core" guarantee.
 * `*nwords_out` receives the words actually pushed.
 * ------------------------------------------------------------------ */
static int sd_write_fill_fifo(struct hw *h, unsigned *nwords_out)
{
	unsigned nwords = 0;
	int i;

	for (i = 0; i < SD_POLL_CAP && nwords < 128; i++) {
		uint32_t st = hw_star(h);
		if (st & STAR_FIFO_FULL)
			continue;
		nwords++;              /* stands in for wreg(REG_FIFO, buf[nwords++]) */
	}
	if (nwords_out)
		*nwords_out = nwords;
	if (nwords < 128)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * THE WRITE-COMPLETION DECISION — mirrors sd_bio.c:419-449 VERBATIM, including
 * both anonymous blocks, the exact order of the three checks in phase 1, and
 * the fact that the phase-1 timeout return RE-READS REG_RINT (sd_bio.c:430)
 * instead of reusing the `ri` it just tested. That re-read is observable: the
 * bits reported can differ from the bits that were examined.
 * ------------------------------------------------------------------ */
static int sd_write_completion(struct hw *h)
{
	/* sd_bio.c:419-432 — wait for DATA_OVER, time-capped (the D4 fix: an
	 * iteration cap is not a time cap). */
	{
		uint64_t start = hw_cntpct(h);
		uint64_t cap = ms_to_ticks(h, SD_WRITE_DATA_TIMEOUT_MS);
		uint32_t ri;
		for (;;) {
			ri = hw_rint(h);
			if (ri & RINT_DATA_OVER)
				break;
			if (ri & 0x0180u)   /* DATA_CRC / DATA_TIMEOUT */
				return (int)(0x40000000u | (ri & 0x3fffu));
			if (hw_cntpct(h) - start > cap)
				return (int)(0x20000000u | (hw_rint(h) & 0x3fffu));
		}
	}
	/* sd_bio.c:433-448 — then wait out the card's flash program time on a
	 * separate, longer time cap, returning a real error instead of
	 * unconditionally claiming success (the REVIEW-2026-07-24 fix). */
	{
		uint64_t start = hw_cntpct(h);
		uint64_t cap = ms_to_ticks(h, SD_WRITE_BUSY_TIMEOUT_MS);
		for (;;) {
			if ((hw_star(h) & STAR_CARD_BUSY) == 0)
				break;
			if (hw_cntpct(h) - start > cap)
				return -2;   /* card never signaled program-done */
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Test helpers.
 * ------------------------------------------------------------------ */
static void sd_reset_state(void)
{
	g_sd_block_addressed = 0;
	g_sd_inited = 0;
}

/* A hw double with a 24 MHz counter that advances one full 1000 ms cap per
 * cntpct read, so phase 1 times out on its second poll and phase 2 (whose cap
 * is 4x larger) on its fifth — few enough to reason about exactly. */
static void hw_init(struct hw *h,
                    const uint32_t *rint, unsigned nrint,
                    const uint32_t *star, unsigned nstar)
{
	memset(h, 0, sizeof(*h));
	h->rint = rint; h->nrint = nrint;
	h->star = star; h->nstar = nstar;
	h->cntfrq = A64_CNTFRQ_DEFAULT;
	h->now = 0;
	h->dt = (A64_CNTFRQ_DEFAULT * SD_WRITE_DATA_TIMEOUT_MS) / 1000ull;
}

/* ==================================================================== *
 * Tests — part 1: addressing
 * ==================================================================== */

/* SDHC/SDXC (CCS set): the CMD17/CMD24 argument IS the 512-byte sector index,
 * unmodified — sd_bio.c:172's true branch. Identity across the whole 32-bit
 * range, including values that would overflow the other branch. */
static void test_sd_addr_block_addressed_is_identity(void)
{
	static const uint32_t lbas[] = {
		0u, 1u, 2u, 63u, 512u, 8192u, 0x7FFFFFu, 0x800000u,
		0x1000000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
	};
	unsigned i;

	sd_reset_state();
	g_sd_block_addressed = 1;
	for (i = 0; i < sizeof(lbas) / sizeof(lbas[0]); i++)
		assert(sd_addr(lbas[i]) == lbas[i]);
}

/* Legacy SDSC (CCS clear): the argument is a BYTE offset, lba*512 —
 * sd_bio.c:172's false branch. Explicit values, no arithmetic in the
 * expectation, so a transcription slip cannot cancel out. */
static void test_sd_addr_byte_addressed_multiplies_by_512(void)
{
	sd_reset_state();
	g_sd_block_addressed = 0;

	assert(sd_addr(0u) == 0u);
	assert(sd_addr(1u) == 512u);
	assert(sd_addr(2u) == 1024u);
	assert(sd_addr(3u) == 1536u);
	assert(sd_addr(63u) == 32256u);
	assert(sd_addr(0x1000u) == 0x200000u);
	assert(sd_addr(0x100000u) == 0x20000000u);
	/* Largest LBA whose byte address still fits in 32 bits. */
	assert(sd_addr(0x7FFFFFu) == 0xFFFFFE00u);
}

/* The whole point of the branch: for the SAME lba the two branches differ by
 * exactly 512x. Swapping them is not a subtle off-by-one, it is a 512-sector-
 * scale address error — silent corruption on write. */
static void test_sd_addr_branches_differ_by_512x(void)
{
	static const uint32_t lbas[] = { 1u, 2u, 34u, 2048u, 0x7FFFFu };
	unsigned i;

	for (i = 0; i < sizeof(lbas) / sizeof(lbas[0]); i++) {
		uint32_t blk, byt;

		sd_reset_state();
		g_sd_block_addressed = 1;
		blk = sd_addr(lbas[i]);
		g_sd_block_addressed = 0;
		byt = sd_addr(lbas[i]);

		assert(blk == lbas[i]);
		assert(byt == blk * 512u);
		assert(byt / 512u == blk);          /* exact, no overflow at these lbas */
		assert(byt != blk);                 /* they are never interchangeable */
	}

	/* lba 0 is the one value where both branches agree — worth knowing,
	 * because it means a sector-0 test can NEVER detect a swapped branch. */
	sd_reset_state();
	g_sd_block_addressed = 1;
	assert(sd_addr(0u) == 0u);
	g_sd_block_addressed = 0;
	assert(sd_addr(0u) == 0u);
}

/* MEASURED, NOT ASPIRATIONAL: `lba * 512u` is unsigned-int arithmetic
 * (uint32_t * unsigned), so it WRAPS mod 2^32. sd_bio.c has no overflow check
 * and sd_addr() has no way to report one (it returns the argument value, not a
 * status). This test pins the real behaviour, it does NOT assert a fix.
 *
 * Practical severity: byte addressing only applies to SDSC cards, which top out
 * at 2 GB (lba < 0x400000), so a correct caller never reaches the wrap. But the
 * ALIASING is what makes it worth pinning — sd_addr(0x800000) == sd_addr(0), so
 * a caller that passes an out-of-range LBA to a byte-addressed card writes
 * sector 0 (the MBR/GPT) instead of failing. sd_bio_read/write() range-check
 * nothing (sd_bio.c:335-350, 390-408). */
static void test_sd_addr_byte_overflow_wraps_and_aliases(void)
{
	sd_reset_state();
	g_sd_block_addressed = 0;

	/* First LBA whose byte address does not fit: 0x800000 * 512 == 2^32. */
	assert(sd_addr(0x7FFFFFu) == 0xFFFFFE00u);   /* last exact one */
	assert(sd_addr(0x800000u) == 0u);            /* wraps to zero */
	assert(sd_addr(0x800001u) == 512u);          /* ...and keeps going */
	assert(sd_addr(0x800002u) == 1024u);
	assert(sd_addr(0xFFFFFFFFu) == 0xFFFFFE00u);

	/* The aliasing, stated directly. */
	assert(sd_addr(0x800000u) == sd_addr(0u));
	assert(sd_addr(0x800001u) == sd_addr(1u));
	assert(sd_addr(0xFFFFFFFFu) == sd_addr(0x7FFFFFu));

	/* Every wrap is a multiple of 512 (the low 9 bits are always clear), so
	 * the wrapped address is still sector-aligned — which is exactly why it
	 * lands on a real, innocent sector instead of an obviously bogus one. */
	assert((sd_addr(0x800000u) & 511u) == 0u);
	assert((sd_addr(0xFFFFFFFFu) & 511u) == 0u);

	/* The block-addressed branch has no such cliff — pinning that the
	 * overflow is a property of the multiply, not of sd_addr() as such. */
	g_sd_block_addressed = 1;
	assert(sd_addr(0x800000u) == 0x800000u);
	assert(sd_addr(0xFFFFFFFFu) == 0xFFFFFFFFu);
}

/* sd_addr() must never be reached before sd_bio_init() has classified the card:
 * both transfer entry points bail with -100 while g_sd_inited == 0
 * (sd_bio.c:341-342, 396-397). Without that guard a transfer would silently use
 * the default g_sd_block_addressed == 0 (BYTE addressing) on an SDHC card —
 * the 512x error above, on every access. */
static void test_uninited_guard_precedes_addressing(void)
{
	sd_reset_state();
	assert(g_sd_inited == 0);
	assert(sd_xfer_guard() == -100);
	assert(sd_xfer_guard() < 0);        /* honours sd_bio.h's "negative" */

	/* The default state, if it were ever used, is byte addressing. */
	assert(g_sd_block_addressed == 0);
	assert(sd_addr(1u) == 512u);

	g_sd_inited = 1;
	assert(sd_xfer_guard() == 0);
}

/* CMD8 (SEND_IF_COND) gate — sd_bio.c:281-284. v2 is set ONLY when the command
 * succeeded AND the low byte of R7 echoes the 0xAA check pattern we sent. */
static void test_cmd8_check_pattern_gate(void)
{
	/* The pattern we send and the pattern we test for must agree; the source
	 * hard-codes 0xAA at :282 rather than deriving it from CMD8_ARG_3V3_AA,
	 * so pin that they match today. */
	assert((CMD8_ARG_3V3_AA & 0xFFu) == 0xAAu);

	/* Textbook success: R7 echoes VHS=1 | pattern 0xAA. */
	assert(cmd8_says_v2(1, 0x000001AAu) == 1);

	/* Any non-0xAA low byte is a reject, including one-bit-off. */
	assert(cmd8_says_v2(1, 0x000001ABu) == 0);
	assert(cmd8_says_v2(1, 0x000001A9u) == 0);
	assert(cmd8_says_v2(1, 0x000001AAu ^ 0x80u) == 0);
	assert(cmd8_says_v2(1, 0x00000000u) == 0);
	assert(cmd8_says_v2(1, 0xFFFFFFFFu) == 0);   /* low byte 0xFF */

	/* MEASURED: ONLY the low byte is examined. The VHS/voltage-accepted
	 * nibble in R7[11:8] is not checked at all, so these all read as v2. */
	assert(cmd8_says_v2(1, 0x000000AAu) == 1);   /* VHS nibble zero */
	assert(cmd8_says_v2(1, 0x000002AAu) == 1);   /* VHS nibble 2 */
	assert(cmd8_says_v2(1, 0xDEADBEAAu) == 1);   /* everything else garbage */

	/* CMD8 timing out is NOT fatal (sd_bio.c:277-278: a v1 card has no
	 * CMD8); it just leaves v2 clear, whatever resp0 happens to hold. */
	assert(cmd8_says_v2(0, 0x000001AAu) == 0);
	assert(cmd8_says_v2(0, 0x00000000u) == 0);
}

/* The HCS bit in the ACMD41 argument is gated on that CMD8 result
 * (sd_bio.c:291), and HCS-in-the-argument is the SAME bit position as
 * CCS-in-the-response. Same value, opposite directions: bit30 out = "host
 * supports high capacity", bit30 back = "card is block-addressed". */
static void test_acmd41_hcs_arg_gated_on_cmd8(void)
{
	assert(acmd41_arg(1) == 0x40FF8000u);   /* VWIN | HCS */
	assert(acmd41_arg(0) == 0x00FF8000u);   /* VWIN only  */
	assert((acmd41_arg(1) & ACMD41_ARG_HCS) != 0u);
	assert((acmd41_arg(0) & ACMD41_ARG_HCS) == 0u);
	/* The voltage window is unconditional in both. */
	assert((acmd41_arg(1) & ACMD41_ARG_VWIN) == ACMD41_ARG_VWIN);
	assert((acmd41_arg(0) & ACMD41_ARG_VWIN) == ACMD41_ARG_VWIN);

	/* Same bit, two meanings. If these two constants ever diverge, the CCS
	 * read at sd_bio.c:300 and the HCS write at :291 stop being about the
	 * same OCR bit and this whole detection is wrong. */
	assert(ACMD41_ARG_HCS == OCR_CCS_SDHC);
	assert(ACMD41_ARG_HCS == (1u << 30));
	assert(OCR_BUSY_READY == (1u << 31));
}

/* The CCS decision is derived from the response word EXACTLY as sd_bio.c:300
 * derives it: `(resp0 & OCR_CCS_SDHC) ? 1 : 0` — bit30 of the OCR, nothing
 * else, no dependence on the requested HCS or on the card's capacity. */
static void test_ocr_ccs_bit_selects_addressing(void)
{
	static const uint32_t ready_sdhc[]  = { 0xC0FF8000u }; /* bit31|bit30|VWIN */
	static const uint32_t ready_sdsc[]  = { 0x80FF8000u }; /* bit31|VWIN       */
	struct ocr_script s;

	/* Realistic SDHC OCR: powered up (bit31) and CCS (bit30) set. */
	sd_reset_state();
	s.w = ready_sdhc; s.n = 1; s.i = 0;
	assert(acmd41_detect(&s) == 0);
	assert(g_sd_block_addressed == 1);
	assert(sd_addr(12345u) == 12345u);           /* => block addressing */

	/* Realistic SDSC OCR: powered up, CCS clear. */
	sd_reset_state();
	s.w = ready_sdsc; s.n = 1; s.i = 0;
	assert(acmd41_detect(&s) == 0);
	assert(g_sd_block_addressed == 0);
	assert(sd_addr(12345u) == 12345u * 512u);    /* => byte addressing */

	/* Only bit30 matters. Every other bit set, bit30 clear -> byte. */
	{
		static const uint32_t all_but_ccs[] = { 0xFFFFFFFFu & ~OCR_CCS_SDHC };
		sd_reset_state();
		s.w = all_but_ccs; s.n = 1; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(g_sd_block_addressed == 0);
	}
	/* Only bit31|bit30 set, nothing else -> block. */
	{
		static const uint32_t bare_ccs[] = { OCR_BUSY_READY | OCR_CCS_SDHC };
		sd_reset_state();
		s.w = bare_ccs; s.n = 1; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(g_sd_block_addressed == 1);
	}
}

/* CCS must be taken from the OCR word that reported power-up complete, not
 * from any earlier not-ready response — CCS is only defined once bit31 is set.
 * sd_bio.c gets this right by assigning g_sd_block_addressed AFTER the poll
 * loop (:300) from the resp0 that broke it (:296-297); this test would fail if
 * the assignment ever moved inside the loop. */
static void test_ccs_read_from_the_ready_word_only(void)
{
	struct ocr_script s;

	/* Two not-ready responses that (spuriously) have bit30 set, then a ready
	 * response with CCS CLEAR. Correct answer: byte addressing. */
	{
		static const uint32_t script[] = {
			0x40FF8000u,          /* bit31 clear -> still busy, CCS meaningless */
			0x40FF8000u,
			0x80FF8000u,          /* ready, CCS clear */
		};
		sd_reset_state();
		s.w = script; s.n = 3; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(s.i == 3);                    /* it really polled three times */
		assert(g_sd_block_addressed == 0);
	}

	/* Mirror image: not-ready responses with bit30 clear, then ready with CCS
	 * SET. Correct answer: block addressing. */
	{
		static const uint32_t script[] = {
			0x00FF8000u, 0x00FF8000u, 0x00FF8000u,
			0xC0FF8000u,
		};
		sd_reset_state();
		s.w = script; s.n = 4; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(s.i == 4);
		assert(g_sd_block_addressed == 1);
	}

	/* Ready on the very first poll: exactly one response consumed. */
	{
		static const uint32_t script[] = { 0xC0FF8000u, 0x80FF8000u };
		sd_reset_state();
		s.w = script; s.n = 2; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(s.i == 1);                    /* the second entry is never read */
		assert(g_sd_block_addressed == 1);
	}
}

/* A card that never powers up: the loop is capped at ACMD41_RETRY_CAP
 * (sd_bio.c:292/299) and returns -5 WITHOUT latching an addressing mode, so
 * g_sd_block_addressed stays at its reset value and the uninited guard keeps
 * every transfer out. Bounded: it does not spin forever. */
static void test_acmd41_never_ready_returns_minus5(void)
{
	static const uint32_t never[] = { 0x00FF8000u };   /* bit31 never set */
	struct ocr_script s;

	sd_reset_state();
	s.w = never; s.n = 1; s.i = 0;
	assert(acmd41_detect(&s) == -5);
	assert(g_sd_block_addressed == 0);   /* untouched by the failure path */
	assert(g_sd_inited == 0);
	assert(sd_xfer_guard() == -100);

	/* Also pin the cap itself: a CCS-set response arriving on retry 200
	 * (one past the budget) is never seen. */
	{
		static uint32_t late[ACMD41_RETRY_CAP + 1];
		int i;
		for (i = 0; i < ACMD41_RETRY_CAP; i++)
			late[i] = 0x00FF8000u;
		late[ACMD41_RETRY_CAP] = 0xC0FF8000u;

		sd_reset_state();
		s.w = late; s.n = ACMD41_RETRY_CAP + 1u; s.i = 0;
		assert(acmd41_detect(&s) == -5);
		assert(s.i == (unsigned)ACMD41_RETRY_CAP);
		assert(g_sd_block_addressed == 0);
	}
	/* ...whereas arriving on retry 199 (the last one in budget) is. */
	{
		static uint32_t just[ACMD41_RETRY_CAP];
		int i;
		for (i = 0; i < ACMD41_RETRY_CAP - 1; i++)
			just[i] = 0x00FF8000u;
		just[ACMD41_RETRY_CAP - 1] = 0xC0FF8000u;

		sd_reset_state();
		s.w = just; s.n = ACMD41_RETRY_CAP; s.i = 0;
		assert(acmd41_detect(&s) == 0);
		assert(g_sd_block_addressed == 1);
	}
}

/* ==================================================================== *
 * Tests — part 2: sd_bio_write()'s completion return-code decision
 * ==================================================================== */

/* The D4 time-cap arithmetic (sd_bio.c:147-153): a real time budget, not an
 * iteration count, and a hard fallback when CNTFRQ_EL0 reads 0. */
static void test_ms_to_ticks_and_the_two_caps(void)
{
	struct hw h;
	static const uint32_t r[] = { RINT_DATA_OVER };
	static const uint32_t st[] = { 0u };

	hw_init(&h, r, 1, st, 1);
	assert(h.cntfrq == 24000000ull);
	assert(ms_to_ticks(&h, SD_WRITE_DATA_TIMEOUT_MS) == 24000000ull);
	assert(ms_to_ticks(&h, SD_WRITE_BUSY_TIMEOUT_MS) == 96000000ull);
	assert(ms_to_ticks(&h, 1u) == 24000ull);
	assert(ms_to_ticks(&h, 0u) == 0ull);

	/* The program-time cap is deliberately 4x the data-phase cap. */
	assert(SD_WRITE_BUSY_TIMEOUT_MS == 4u * SD_WRITE_DATA_TIMEOUT_MS);
	assert(ms_to_ticks(&h, SD_WRITE_BUSY_TIMEOUT_MS) ==
	       4ull * ms_to_ticks(&h, SD_WRITE_DATA_TIMEOUT_MS));

	/* CNTFRQ_EL0 == 0 falls back to the A64's 24 MHz (sd_bio.c:150-151), so
	 * the cap is never accidentally 0 (which would time out instantly). */
	h.cntfrq = 0;
	assert(ms_to_ticks(&h, SD_WRITE_DATA_TIMEOUT_MS) == 24000000ull);
	assert(ms_to_ticks(&h, SD_WRITE_BUSY_TIMEOUT_MS) == 96000000ull);
	assert(ms_to_ticks(&h, SD_WRITE_DATA_TIMEOUT_MS) != 0ull);
}

/* Happy path: DATA_OVER already latched, card not busy -> 0, and it took
 * exactly one RINT poll and one STAR poll. */
static void test_write_success_returns_zero(void)
{
	struct hw h;
	static const uint32_t r[] = { RINT_DATA_OVER };
	static const uint32_t st[] = { 0u };

	hw_init(&h, r, 1, st, 1);
	assert(sd_write_completion(&h) == 0);
	assert(h.irint == 1);
	assert(h.istar == 1);
}

/* A slow-but-successful write: DATA_OVER shows up after several polls and the
 * card releases CARD_BUSY after several more, both inside their caps -> 0.
 * This is the case the pre-fix code and an over-eager timeout would both get
 * wrong in opposite directions. */
static void test_write_slow_but_successful_returns_zero(void)
{
	struct hw h;
	static const uint32_t r[] = { 0u, 0u, RINT_DATA_OVER };
	static const uint32_t st[] = { STAR_CARD_BUSY, STAR_CARD_BUSY, 0u };

	hw_init(&h, r, 3, st, 3);
	/* Slow the virtual clock right down so neither cap is reached. */
	h.dt = 1;
	assert(sd_write_completion(&h) == 0);
	assert(h.irint == 3);       /* it really waited for DATA_OVER */
	assert(h.istar == 3);       /* ...and really waited out CARD_BUSY */
}

/* Data CRC / data-timeout bits (0x0180) with DATA_OVER clear -> the 0x4-tagged
 * code carrying the low 14 RINT bits (sd_bio.c:427-428). */
static void test_write_data_error_returns_0x4_tagged_code(void)
{
	struct hw h;
	static const uint32_t st[] = { 0u };

	{   /* bit7 alone */
		static const uint32_t r[] = { 0x0080u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == 0x40000080);
	}
	{   /* bit8 alone */
		static const uint32_t r[] = { 0x0100u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == 0x40000100);
	}
	{   /* both */
		static const uint32_t r[] = { 0x0180u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == 0x40000180);
	}
	{   /* the 0x3fff mask really discards the high RINT bits: 0x80000180
		 * reports as 0x40000180, i.e. bit31 of RINT is NOT preserved. */
		static const uint32_t r[] = { 0x80000180u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == 0x40000180);
	}
	{   /* ...and only the low 14 bits survive: 0x7F80 -> 0x3F80. */
		static const uint32_t r[] = { 0x00007F80u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == (int)(0x40000000u | 0x3F80u));
	}
	/* Returning on the error means the CARD_BUSY phase is never entered. */
	{
		static const uint32_t r[] = { 0x0180u };
		hw_init(&h, r, 1, st, 1);
		(void)sd_write_completion(&h);
		assert(h.istar == 0);
	}
}

/* MEASURED ORDERING, NOT AN ENDORSEMENT (see the banner's second observation):
 * sd_bio.c:425 tests DATA_OVER before :427 tests the error bits, so a sample
 * with BOTH set is treated as a clean completion and the error bits are thrown
 * away — the write then reports success if the card is not busy. */
static void test_data_over_wins_over_error_bits(void)
{
	struct hw h;
	static const uint32_t r[] = { RINT_DATA_OVER | 0x0180u };
	static const uint32_t st[] = { 0u };

	hw_init(&h, r, 1, st, 1);
	assert(sd_write_completion(&h) == 0);   /* the error bits are discarded */
	assert(h.irint == 1);

	/* Whereas if the very next sample (with DATA_OVER clear) carries them,
	 * they ARE reported — proving it is purely the check order, not a
	 * deliberate mask. */
	{
		static const uint32_t r2[] = { 0u, 0x0180u };
		hw_init(&h, r2, 2, st, 1);
		h.dt = 1;                       /* don't let the time cap interfere */
		assert(sd_write_completion(&h) == 0x40000180);
	}
}

/* DATA_OVER never arrives and no error bits are set: the TIME cap fires and
 * returns the 0x2-tagged code (sd_bio.c:429-430). Note that path RE-READS
 * REG_RINT rather than reusing the `ri` it tested, so the bits it reports are
 * a LATER sample — pinned here with a script whose third sample differs. */
static void test_write_data_over_timeout_returns_0x2_tagged_code(void)
{
	struct hw h;
	static const uint32_t st[] = { 0u };

	{
		/* dt == one full data cap per cntpct read: poll 1 sees elapsed ==
		 * cap (not > cap, so it keeps waiting), poll 2 sees 2*cap and
		 * times out, then re-reads RINT -> the third script entry. */
		static const uint32_t r[] = { 0x0000u, 0x0000u, 0x2222u };
		hw_init(&h, r, 3, st, 1);
		assert(sd_write_completion(&h) == 0x20002222);
		assert(h.irint == 3);           /* 2 loop reads + 1 re-read */
		assert(h.istar == 0);           /* never reached the busy phase */
	}
	{
		/* Same shape, all-zero RINT: the tag is returned with no bits. */
		static const uint32_t r[] = { 0x0000u };
		hw_init(&h, r, 1, st, 1);
		assert(sd_write_completion(&h) == 0x20000000);
	}
	{
		/* The re-read is masked the same way (0x3fff), so a high bit that
		 * appears between the two reads is dropped. */
		static const uint32_t r[] = { 0u, 0u, 0xFFFF0000u | 0x1234u };
		hw_init(&h, r, 3, st, 1);
		assert(sd_write_completion(&h) == (int)(0x20000000u | 0x1234u));
	}
	{
		/* Boundary: elapsed == cap exactly is NOT a timeout (`>`), so with a
		 * dt of exactly one cap, DATA_OVER arriving on the second poll still
		 * succeeds. */
		static const uint32_t r[] = { 0u, RINT_DATA_OVER };
		hw_init(&h, r, 2, st, 1);
		assert(sd_write_completion(&h) == 0);
		assert(h.irint == 2);
	}
}

/* The REVIEW-2026-07-24 fix itself: a card that never releases CARD_BUSY must
 * NOT be reported as a successful write. Pre-fix, this path fell out of a
 * bounded loop and returned 0 — "silently claim success on a still-programming
 * card", which then let the next command hit a busy card. It now returns -2
 * (sd_bio.c:440-447). */
static void test_write_card_busy_timeout_returns_minus2(void)
{
	struct hw h;
	static const uint32_t r[] = { RINT_DATA_OVER };
	/* A long all-busy script (rather than a one-element held sample) so the
	 * read index below counts real polls instead of saturating at 1. */
	static uint32_t st_busy[32];
	unsigned i;

	for (i = 0; i < 32; i++)
		st_busy[i] = STAR_CARD_BUSY;      /* never clears */

	hw_init(&h, r, 1, st_busy, 32);
	assert(sd_write_completion(&h) == -2);
	assert(sd_write_completion(&h) != 0);   /* THE regression: not success */

	/* Poll count, derived from the mirrored arithmetic rather than asserted
	 * from prose: hw_init sets dt == one DATA cap (24e6 ticks) and the BUSY
	 * cap is 4x that (96e6). Phase 1 consumes one cntpct read (start), so
	 * phase 2's own start reads 1*dt and its k-th poll compares
	 * (k+1)*dt - 1*dt == k*dt against 4*dt. k=1..4 are within budget (k==4 is
	 * equal, and the test is strictly `>`), k=5 exceeds it. */
	hw_init(&h, r, 1, st_busy, 32);
	assert(sd_write_completion(&h) == -2);
	assert(h.istar == 5);

	/* And it is bounded in TIME, not just iterations: the virtual clock
	 * advanced past the 4000 ms budget. */
	assert(h.now > ms_to_ticks(&h, SD_WRITE_BUSY_TIMEOUT_MS));

	/* A card that releases CARD_BUSY on the very last in-budget poll (k==4)
	 * must still succeed — the timeout must not fire early. */
	{
		static uint32_t st_late[] = {
			STAR_CARD_BUSY, STAR_CARD_BUSY, STAR_CARD_BUSY, 0u,
		};
		hw_init(&h, r, 1, st_late, 4);
		assert(sd_write_completion(&h) == 0);
		assert(h.istar == 4);
	}
}

/* THE HEADER/IMPLEMENTATION MISMATCH, MEASURED (see the banner). sd_bio.h:52-56
 * promises "negative on timeout"; two of the four failure codes are POSITIVE.
 * This test asserts what the code DOES, and demonstrates the consequence for
 * both plausible caller idioms. If the contract is ever reconciled — either by
 * making these codes negative or by widening the header's wording — this test
 * is where that decision surfaces. */
static void test_write_error_codes_are_positive_not_negative(void)
{
	struct hw h;
	static const uint32_t st_idle[] = { 0u };
	static const uint32_t st_busy[] = { STAR_CARD_BUSY };
	static const uint32_t r_crc[]  = { 0x0180u };
	static const uint32_t r_stall[] = { 0x0000u };
	static const uint32_t r_ok[]   = { RINT_DATA_OVER };
	int rc_crc, rc_stall, rc_busy, rc_ok;
	unsigned nwords = 0;
	int rc_fifo;

	hw_init(&h, r_crc, 1, st_idle, 1);
	rc_crc = sd_write_completion(&h);
	hw_init(&h, r_stall, 1, st_idle, 1);
	rc_stall = sd_write_completion(&h);
	hw_init(&h, r_ok, 1, st_busy, 1);
	rc_busy = sd_write_completion(&h);
	hw_init(&h, r_ok, 1, st_idle, 1);
	rc_ok = sd_write_completion(&h);

	/* The full return-code map of sd_bio_write()'s tail. */
	assert(rc_ok == 0);
	assert(rc_crc == 0x40000180);
	assert(rc_stall == 0x20000000);
	assert(rc_busy == -2);

	/* Both tagged codes are POSITIVE. The largest either can be is
	 * 0x40003fff, comfortably below INT_MAX, so the (int) casts at
	 * sd_bio.c:428/430 are value-preserving — these are not
	 * implementation-defined negatives, they are honestly positive. */
	assert((0x40000000u | 0x3fffu) == 0x40003fffu);
	assert(0x40003fffu < (unsigned)INT_MAX);
	assert(rc_crc > 0);
	assert(rc_stall > 0);

	/* Consequence for a caller written to sd_bio.h's stated contract: a
	 * `rc < 0` test MISSES a CRC-failed and a stalled write entirely. */
	assert(!(rc_crc < 0));
	assert(!(rc_stall < 0));
	/* Whereas `rc != 0` — what a caller must actually use today — catches
	 * every failure. */
	assert(rc_crc != 0 && rc_stall != 0 && rc_busy != 0);

	/* The other two codes DO satisfy the header: the busy timeout above and
	 * the short-FIFO failure (sd_bio.c:416-417). */
	assert(rc_busy < 0);
	{
		static const uint32_t st_full[] = { STAR_FIFO_FULL };   /* never drains */
		hw_init(&h, r_ok, 1, st_full, 1);
		rc_fifo = sd_write_fill_fifo(&h, &nwords);
		assert(rc_fifo == -1);
		assert(rc_fifo < 0);
		assert(nwords == 0);
		/* Bounded: it gave up after SD_POLL_CAP polls rather than hanging
		 * the debug core (sd_bio.c's file-header guarantee). The PROOF of
		 * boundedness is that this call returned at all — the FIFO is
		 * permanently full, so only the `i < SD_POLL_CAP` bound can end the
		 * loop. (The read index saturates at 1 for a held one-entry script,
		 * so it is deliberately not asserted here.) */
	}
	/* And the uninited guard's -100 (sd_bio.c:397). */
	sd_reset_state();
	assert(sd_xfer_guard() == -100);
	assert(sd_xfer_guard() < 0);
}

/* A FIFO that is never full accepts all 128 words in 128 polls and returns 0;
 * one that frees up intermittently still completes. Pins that the -1 above is
 * a real failure signal and not the normal outcome. */
static void test_write_fifo_fill_completes_when_fifo_drains(void)
{
	struct hw h;
	static const uint32_t r_ok[] = { RINT_DATA_OVER };
	unsigned nwords = 0;

	{
		static const uint32_t st_never_full[] = { 0u };
		hw_init(&h, r_ok, 1, st_never_full, 1);
		assert(sd_write_fill_fifo(&h, &nwords) == 0);
		assert(nwords == 128);
	}
	{
		/* Alternating full/empty: 128 words still get in, just slower. */
		static const uint32_t st_alt[] = { STAR_FIFO_FULL, 0u };
		unsigned i;
		static uint32_t st_long[512];
		for (i = 0; i < 512; i++)
			st_long[i] = st_alt[i & 1u];
		hw_init(&h, r_ok, 1, st_long, 512);
		nwords = 0;
		assert(sd_write_fill_fifo(&h, &nwords) == 0);
		assert(nwords == 128);
		assert(h.istar == 256);        /* took twice as many polls */
	}
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "sd_addr_block_addressed_is_identity",      test_sd_addr_block_addressed_is_identity },
	{ "sd_addr_byte_addressed_multiplies_by_512", test_sd_addr_byte_addressed_multiplies_by_512 },
	{ "sd_addr_branches_differ_by_512x",          test_sd_addr_branches_differ_by_512x },
	{ "sd_addr_byte_overflow_wraps_and_aliases",  test_sd_addr_byte_overflow_wraps_and_aliases },
	{ "uninited_guard_precedes_addressing",       test_uninited_guard_precedes_addressing },
	{ "cmd8_check_pattern_gate",                  test_cmd8_check_pattern_gate },
	{ "acmd41_hcs_arg_gated_on_cmd8",             test_acmd41_hcs_arg_gated_on_cmd8 },
	{ "ocr_ccs_bit_selects_addressing",           test_ocr_ccs_bit_selects_addressing },
	{ "ccs_read_from_the_ready_word_only",        test_ccs_read_from_the_ready_word_only },
	{ "acmd41_never_ready_returns_minus5",        test_acmd41_never_ready_returns_minus5 },
	{ "ms_to_ticks_and_the_two_caps",             test_ms_to_ticks_and_the_two_caps },
	{ "write_success_returns_zero",               test_write_success_returns_zero },
	{ "write_slow_but_successful_returns_zero",   test_write_slow_but_successful_returns_zero },
	{ "write_data_error_returns_0x4_tagged_code", test_write_data_error_returns_0x4_tagged_code },
	{ "data_over_wins_over_error_bits",           test_data_over_wins_over_error_bits },
	{ "write_data_over_timeout_returns_0x2_tagged_code",
	  test_write_data_over_timeout_returns_0x2_tagged_code },
	{ "write_card_busy_timeout_returns_minus2",   test_write_card_busy_timeout_returns_minus2 },
	{ "write_error_codes_are_positive_not_negative",
	  test_write_error_codes_are_positive_not_negative },
	{ "write_fifo_fill_completes_when_fifo_drains",
	  test_write_fifo_fill_completes_when_fifo_drains },
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

	printf("---- sd_bio addressing + write-completion tests: %d/%d passed ----\n",
	       passed, n);
	return (passed == n) ? 0 : 1;
}
