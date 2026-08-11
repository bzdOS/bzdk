/* SPDX-License-Identifier: BSD-2-Clause */

/* test_scanout_regs.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the PURE decision logic in scanout.c: the register-read
 * dispatch table (scanout_reg_read()), the flip accept/reject/no-op
 * decision (scanout_decide_flip()), and the register-write dispatch
 * (scanout_reg_write()).
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "scanout.c"` WITH STUBS
 * ============================================================================
 * scanout.c was read in full before writing this file (it did not exist
 * before this task; this test was written alongside it, not against a
 * pre-existing file). The three functions under test are genuinely pure —
 * a switch on an offset, and a few integer comparisons — but scanout.c AS A
 * WHOLE is not includable on x86_64: scan_bc() ("dc civac"/"dsb sy") and
 * scanout_mmio_fault() ("mrs hpfar_el2") are raw AArch64 mnemonics plain
 * host gcc rejects, and scanout_init()/scanout_flip() call hdmi_width()/
 * hdmi_set_scanout_addr(), which touch real MMIO on the host segfaults.
 * None of that affects the arithmetic under test here (cache maintenance
 * changes cache state, never the values read/written; the real DE2 register
 * write is a documented side effect the pure decision layer decides WHETHER
 * to trigger, never how it behaves) — same rationale test_sd_bio_addr.c and
 * test_vnet_ring.c already document for the identical situation.
 *
 * Mirrored VERBATIM in control flow (offsets/comparisons/ordering unchanged)
 * from scanout.c:
 *   - struct scanout_dev                     (scanout.c:61-67)
 *   - scanout_reg_read()                     (scanout.c:109-125)
 *   - scanout_decide_flip()                   (scanout.c:138-149)
 *   - scanout_reg_write()                     (scanout.c:157-170)
 * hdmi_set_scanout_addr()/scan_bc() calls inside the real scanout_reg_write()
 * are replaced by a scripted recorder (`struct hw` below) that remembers
 * whether/what it was called with, so the tests can assert "the hardware
 * write was (not) triggered" without linking any hardware code at all.
 *
 * ============================================================================
 * WHAT THIS FILE PROVES, EXERCISES, AND CANNOT COVER
 * ============================================================================
 * PROVES:
 *   - scanout_decide_flip()'s three-way branch: reject (>1), no-op (already
 *     front), accept (the other buffer) — including that a no-op and a
 *     reject are each counted (or not) exactly once, never both, and that
 *     an accepted flip always returns the CORRECT buffer's address, not a
 *     stale one from a previous call.
 *   - scanout_reg_write() only ever acts on SCANOUT_R_FLIP_REQUEST; every
 *     other offset (a defined read-only register, or a wholly unknown one)
 *     is a byte-for-byte no-op — no field of `struct scanout_dev` changes
 *     and the hardware recorder is never invoked.
 *   - scanout_reg_read()'s full offset table, including that an unknown
 *     offset reads 0 rather than some other register's value by accident.
 *   - A round-trip sequence (flip to 1, flip back to 0) leaves front_index
 *     back where it started but flip_count strictly increasing — i.e. the
 *     counter counts EVENTS, not "is buffer 1 front".
 * CANNOT COVER (deliberately out of scope, needs the board):
 *   - Whether the address scanout_decide_flip() hands back actually reaches
 *     DE_UI1_CFG0_TOP_LADDR correctly, whether DE_GLB_DBUFF's commit is
 *     immediate or vblank-latched, or anything about the real monitor —
 *     see docs/zero-copy-scanout.md's verification section for what a human
 *     with board access should check.
 *   - scanout_mmio_fault()'s ESR/HPFAR decode itself (the untestable half —
 *     same boundary vblk_mmio_fault()/vnet_mmio_fault() draw, and neither of
 *     those has a hosted test of ITS decode either; test_vblk_ring.c/
 *     test_vnet_ring.c test the ring logic downstream of it, not the abort
 *     decode).
 *   - scanout_init()'s geometry cross-check against hdmi_width()/height()/
 *     stride() — needs real hdmi.c state, not a hosted concern.
 *
 * Build: gcc -o test_scanout_regs test_scanout_regs.c && ./test_scanout_regs
 * (wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * VERBATIM MIRROR of scanout.h's register offsets (needed by the dispatch
 * tables below) and scanout.c's pure logic.
 * ============================================================================ */
#define SCANOUT_R_MAGIC         0x00u
#define SCANOUT_R_VERSION       0x04u
#define SCANOUT_R_BUF0_ADDR     0x08u
#define SCANOUT_R_BUF1_ADDR     0x0Cu
#define SCANOUT_R_WIDTH         0x10u
#define SCANOUT_R_HEIGHT        0x14u
#define SCANOUT_R_STRIDE        0x18u
#define SCANOUT_R_FORMAT        0x1Cu
#define SCANOUT_R_FRONT_INDEX   0x20u
#define SCANOUT_R_FLIP_REQUEST  0x24u
#define SCANOUT_R_FLIP_COUNT    0x28u
#define SCANOUT_R_REJECT_COUNT  0x2Cu

#define SCANOUT_MAGIC           0x53434e41u
#define SCANOUT_VERSION         1u
#define SCANOUT_FORMAT_XRGB8888 0u

/* Mirror of scanout.c's `struct scanout_dev` (scanout.c:61-67). */
struct scanout_dev {
	uint32_t buf_pa[2];
	uint32_t width, height, stride, format;
	uint32_t front_index;
	uint32_t flip_count;
	uint32_t reject_count;
};

/* Scripted recorder standing in for hdmi_set_scanout_addr() + scan_bc() —
 * the two hardware-touching calls scanout_reg_write()/scanout_flip() make.
 * Neither has any bearing on the decision logic under test; this only lets
 * the tests assert whether/with-what they WOULD have been called. */
struct hw {
	int      set_scanout_calls;
	uint32_t last_pa;
};

/* VERBATIM mirror of scanout.c's scanout_reg_read() (scanout.c:109-125). */
static uint32_t scanout_reg_read(const struct scanout_dev *d, uint32_t off)
{
	switch (off) {
	case SCANOUT_R_MAGIC:        return SCANOUT_MAGIC;
	case SCANOUT_R_VERSION:      return SCANOUT_VERSION;
	case SCANOUT_R_BUF0_ADDR:    return d->buf_pa[0];
	case SCANOUT_R_BUF1_ADDR:    return d->buf_pa[1];
	case SCANOUT_R_WIDTH:        return d->width;
	case SCANOUT_R_HEIGHT:       return d->height;
	case SCANOUT_R_STRIDE:       return d->stride;
	case SCANOUT_R_FORMAT:       return d->format;
	case SCANOUT_R_FRONT_INDEX:  return d->front_index;
	case SCANOUT_R_FLIP_COUNT:   return d->flip_count;
	case SCANOUT_R_REJECT_COUNT: return d->reject_count;
	default:                     return 0u;
	}
}

/* VERBATIM mirror of scanout.c's scanout_decide_flip() (scanout.c:138-149). */
static int scanout_decide_flip(struct scanout_dev *d, uint32_t requested,
                                uint32_t *new_pa)
{
	if (requested > 1u) {
		d->reject_count++;
		return 0;
	}
	if (requested == d->front_index)
		return 0;

	d->front_index = requested;
	d->flip_count++;
	*new_pa = d->buf_pa[requested];
	return 1;
}

/* Mirror of scanout.c's scanout_reg_write() (scanout.c:157-170), with the two
 * hardware calls (hdmi_set_scanout_addr(), scan_bc()) replaced by recording
 * into `struct hw` — see the file banner. Control flow is otherwise
 * identical: same condition, same branches, same order. */
static void scanout_reg_write(struct scanout_dev *d, uint32_t off, uint32_t val,
                               struct hw *hw)
{
	uint32_t new_pa;

	if (off != SCANOUT_R_FLIP_REQUEST)
		return;

	if (scanout_decide_flip(d, val, &new_pa)) {
		hw->set_scanout_calls++;
		hw->last_pa = new_pa;
	}
}

/* ============================================================================
 * Test harness — same CHECK()/g_failed/main() convention as
 * test_vnet_ring.c/test_vblk_ring.c.
 * ============================================================================ */
#define CHECK(cond, name) do { \
	if (cond) { printf("[ OK  ] %s\n", name); } \
	else { printf("[FAIL ] %s\n", name); g_failed = 1; } \
} while (0)

static int g_failed = 0;

#define BUF0_PA 0x4D000000u
#define BUF1_PA 0x4D384000u
#define WIDTH   1280u
#define HEIGHT  720u
#define STRIDE  5120u

static struct scanout_dev mkdev(void)
{
	struct scanout_dev d;
	memset(&d, 0, sizeof(d));
	d.buf_pa[0] = BUF0_PA;
	d.buf_pa[1] = BUF1_PA;
	d.width  = WIDTH;
	d.height = HEIGHT;
	d.stride = STRIDE;
	d.format = SCANOUT_FORMAT_XRGB8888;
	/* front_index/flip_count/reject_count start at 0, matching
	 * scanout_init()'s own boot-time defaults (scanout.c). */
	return d;
}

/* ---- register read table ------------------------------------------------ */
static void test_reg_read_table(void)
{
	struct scanout_dev d = mkdev();

	CHECK(scanout_reg_read(&d, SCANOUT_R_MAGIC) == SCANOUT_MAGIC,
	      "MAGIC reads back the fixed constant");
	CHECK(scanout_reg_read(&d, SCANOUT_R_VERSION) == SCANOUT_VERSION,
	      "VERSION reads back the fixed constant");
	CHECK(scanout_reg_read(&d, SCANOUT_R_BUF0_ADDR) == BUF0_PA,
	      "BUF0_ADDR reads back the buffer 0 physical address");
	CHECK(scanout_reg_read(&d, SCANOUT_R_BUF1_ADDR) == BUF1_PA,
	      "BUF1_ADDR reads back the buffer 1 physical address");
	CHECK(scanout_reg_read(&d, SCANOUT_R_WIDTH) == WIDTH, "WIDTH");
	CHECK(scanout_reg_read(&d, SCANOUT_R_HEIGHT) == HEIGHT, "HEIGHT");
	CHECK(scanout_reg_read(&d, SCANOUT_R_STRIDE) == STRIDE, "STRIDE");
	CHECK(scanout_reg_read(&d, SCANOUT_R_FORMAT) == SCANOUT_FORMAT_XRGB8888,
	      "FORMAT");
	CHECK(scanout_reg_read(&d, SCANOUT_R_FRONT_INDEX) == 0u,
	      "FRONT_INDEX starts at buffer 0 (matches boot-time TOP_LADDR)");
	CHECK(scanout_reg_read(&d, SCANOUT_R_FLIP_COUNT) == 0u,
	      "FLIP_COUNT starts at 0");
	CHECK(scanout_reg_read(&d, SCANOUT_R_REJECT_COUNT) == 0u,
	      "REJECT_COUNT starts at 0");
	CHECK(scanout_reg_read(&d, 0x30u) == 0u,
	      "an unknown offset reads 0, not a neighbouring register's value");
	CHECK(scanout_reg_read(&d, SCANOUT_R_FLIP_REQUEST) == 0u,
	      "the write-only FLIP_REQUEST register reads 0 (falls to default:)");
}

/* ---- flip decision: accept ---------------------------------------------- */
static void test_flip_accept_0_to_1(void)
{
	struct scanout_dev d = mkdev();
	uint32_t new_pa = 0xDEADBEEFu;
	int rc = scanout_decide_flip(&d, 1u, &new_pa);

	CHECK(rc == 1, "flip 0->1 is accepted");
	CHECK(d.front_index == 1u, "front_index becomes 1");
	CHECK(d.flip_count == 1u, "flip_count increments exactly once");
	CHECK(d.reject_count == 0u, "reject_count is untouched by an accepted flip");
	CHECK(new_pa == BUF1_PA, "the accepted flip's *new_pa is buffer 1's address");
}

/* ---- flip decision: no-op (already front) -------------------------------- */
static void test_flip_noop_already_front(void)
{
	struct scanout_dev d = mkdev();   /* front_index == 0 */
	uint32_t new_pa = 0xDEADBEEFu;
	int rc = scanout_decide_flip(&d, 0u, &new_pa);

	CHECK(rc == 0, "flipping to the CURRENT front buffer is a no-op");
	CHECK(d.front_index == 0u, "front_index is unchanged");
	CHECK(d.flip_count == 0u,
	      "a no-op flip does NOT increment flip_count (it changed nothing)");
	CHECK(d.reject_count == 0u,
	      "a no-op flip is not a rejection either -- it is a third, distinct "
	      "outcome, not an error");
	CHECK(new_pa == 0xDEADBEEFu, "*new_pa is untouched when the call is a no-op");
}

/* ---- flip decision: reject (out of range) -------------------------------- */
static void test_flip_reject_out_of_range(void)
{
	struct scanout_dev d = mkdev();
	uint32_t new_pa = 0xDEADBEEFu;
	int rc2   = scanout_decide_flip(&d, 2u, &new_pa);
	int rcbig = scanout_decide_flip(&d, 0xFFFFFFFFu, &new_pa);

	CHECK(rc2 == 0, "requesting buffer index 2 is rejected");
	CHECK(rcbig == 0, "requesting buffer index 0xFFFFFFFF is rejected");
	CHECK(d.reject_count == 2u,
	      "each rejected request increments reject_count exactly once");
	CHECK(d.front_index == 0u,
	      "a rejected request never changes front_index");
	CHECK(d.flip_count == 0u,
	      "a rejected request never increments flip_count");
	CHECK(new_pa == 0xDEADBEEFu, "*new_pa is untouched when the call is rejected");
}

/* ---- flip decision: round trip (0->1->0) --------------------------------- */
static void test_flip_round_trip(void)
{
	struct scanout_dev d = mkdev();
	uint32_t pa1 = 0, pa0 = 0;
	int rc1 = scanout_decide_flip(&d, 1u, &pa1);
	int rc0 = scanout_decide_flip(&d, 0u, &pa0);

	CHECK(rc1 == 1 && rc0 == 1, "both legs of a round trip are accepted");
	CHECK(d.front_index == 0u, "front_index is back where it started");
	CHECK(d.flip_count == 2u,
	      "flip_count counts EVENTS (2), not \"is buffer 1 front\" (which "
	      "would read back as 0/false after the round trip)");
	CHECK(pa1 == BUF1_PA && pa0 == BUF0_PA,
	      "each leg hands back the buffer it actually flipped to, not a "
	      "stale value from the previous call");
}

/* ---- register write dispatch: FLIP_REQUEST drives the hardware call ----- */
static void test_reg_write_flip_request_drives_hw(void)
{
	struct scanout_dev d = mkdev();
	struct hw hw; memset(&hw, 0, sizeof(hw));

	scanout_reg_write(&d, SCANOUT_R_FLIP_REQUEST, 1u, &hw);

	CHECK(d.front_index == 1u, "an accepted FLIP_REQUEST write updates front_index");
	CHECK(d.flip_count == 1u, "...and flip_count");
	CHECK(hw.set_scanout_calls == 1u,
	      "...and triggers exactly one hardware scanout-address write");
	CHECK(hw.last_pa == BUF1_PA,
	      "...with the correct (buffer 1) physical address");
}

static void test_reg_write_flip_request_rejected_skips_hw(void)
{
	struct scanout_dev d = mkdev();
	struct hw hw; memset(&hw, 0, sizeof(hw));

	scanout_reg_write(&d, SCANOUT_R_FLIP_REQUEST, 7u, &hw);

	CHECK(d.reject_count == 1u, "an out-of-range FLIP_REQUEST write is rejected");
	CHECK(hw.set_scanout_calls == 0u,
	      "...and does NOT touch the real hardware -- a bad write from a "
	      "buggy/malicious guest can never reprogram the scanout address");
}

/* ---- register write dispatch: every other offset is a pure no-op -------- */
static void test_reg_write_other_offsets_are_noop(void)
{
	struct scanout_dev before = mkdev();
	struct scanout_dev d = before;
	struct hw hw; memset(&hw, 0, sizeof(hw));
	uint32_t offsets[] = { SCANOUT_R_MAGIC, SCANOUT_R_VERSION,
	                        SCANOUT_R_BUF0_ADDR, SCANOUT_R_BUF1_ADDR,
	                        SCANOUT_R_WIDTH, SCANOUT_R_HEIGHT, SCANOUT_R_STRIDE,
	                        SCANOUT_R_FORMAT, SCANOUT_R_FRONT_INDEX,
	                        SCANOUT_R_FLIP_COUNT, SCANOUT_R_REJECT_COUNT,
	                        0x30u /* wholly unknown */ };
	size_t i;

	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
		scanout_reg_write(&d, offsets[i], 0xFFFFFFFFu, &hw);

	CHECK(memcmp(&before, &d, sizeof(d)) == 0,
	      "writing any register except FLIP_REQUEST changes NOTHING in the "
	      "device state, for every defined offset plus one unknown one");
	CHECK(hw.set_scanout_calls == 0u,
	      "...and never touches the real hardware either");
}

int main(void)
{
	test_reg_read_table();
	test_flip_accept_0_to_1();
	test_flip_noop_already_front();
	test_flip_reject_out_of_range();
	test_flip_round_trip();
	test_reg_write_flip_request_drives_hw();
	test_reg_write_flip_request_rejected_skips_hw();
	test_reg_write_other_offsets_are_noop();

	if (g_failed) {
		printf("---- scanout register tests: FAILED ----\n");
		return 1;
	}
	printf("---- scanout register tests: passed ----\n");
	return 0;
}
