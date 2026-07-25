/* SPDX-License-Identifier: BSD-2-Clause */

/* emmc_bio.c — Allwinner A64 SMHC2/eMMC (aw_mmc1 @ 0x01c11000) block-I/O
 * helper. Implements emmc_bio.h. Freestanding, bare-metal AArch64, MMIO via
 * volatile pointers built from absolute physical addresses (identity-mapped
 * Device-nGnRE in stage2 — same contract as emac.c/musb.c: U-Boot leaves the
 * MMU on with a flat device mapping, no pmap games needed here).
 *
 * EVERYTHING in the register map, bit values, command flags and the
 * init/read sequence below is ported VERBATIM from a hardware-verified
 * Python sequence run live over the EMAC debug channel against THIS exact
 * controller (readblk() there returned real data — LBA1 decoded as
 * "EFI PART", i.e. the GPT header). Do NOT "improve" or re-derive the
 * sequence from a generic sunxi-mmc datasheet reading — it works as-is on
 * this silicon and subtle reordering has bitten this project before (see
 * musb.c's header for the analogous USB lesson).
 *
 * The write path (CMD24) is the one part of this file NOT yet confirmed on
 * hardware: it mirrors the read path's FIFO-drain loop in the push
 * direction, using the CMDR WRITE bit and STAR FIFO_FULL flag as described
 * by the source sequence, but has not itself been run against the board.
 *
 * All MMIO polls are iteration-capped (EMMC_POLL_CAP) so a dbgmon `call`
 * into any of these functions can never hang the debug core — a timeout
 * returns a negative code instead of spinning forever.
 */
#include <stdint.h>
#include <stddef.h>
#include "emmc_bio.h"
#include "hv_addrmap.h"     /* HVMAP_EBIO_BC / HVMAP_EMMC_HS_TESTBUF */

/* ------------------------------------------------------------------ */
/* Physical bases                                                      */
/* ------------------------------------------------------------------ */
#define EMMC_BASE        0x01c11000UL   /* SMHC2/aw_mmc1 (eMMC) controller  */
#define PIO_PC_CFG0      0x01C20848UL   /* PC5 pinmux nibble (bits[23:20])  */
#define CCU_MMC2_CLK     0x01c20090UL   /* CCU MMC2_CLK gate/divider reg    */

/* ------------------------------------------------------------------ */
/* Controller register offsets (byte, off EMMC_BASE) — verbatim from the   */
/* verified Python sequence.                                               */
/* ------------------------------------------------------------------ */
#define REG_GCTL   0x00u   /* global control: soft/fifo/dma reset, AHB mode */
#define REG_CKCR   0x04u   /* clock control */
#define REG_TMOR   0x08u   /* timeout */
#define REG_BWDR   0x0Cu   /* bus width */
#define REG_BKSR   0x10u   /* block size */
#define REG_BYCR   0x14u   /* byte count */
#define REG_CMDR   0x18u   /* command */
#define REG_CAGR   0x1Cu   /* command argument */
#define REG_RESP0  0x20u   /* response 0 */
#define REG_IMKR   0x30u   /* interrupt mask */
#define REG_RINT   0x38u   /* raw interrupt status (write-1-to-clear) */
#define REG_STAR   0x3Cu   /* status */
#define REG_NTSR   0x5Cu   /* new timing set register (A64: MUST be set, see below) */
#define REG_SAMP_DL 0x144u /* sample delay / calibration register (A64: MUST be set, see below) */
#define REG_FIFO   0x200u  /* data FIFO (read to drain / write to fill) */

/* NTSR bit31 "MODE_SEL_NEW" + SAMP_DL bit7 "CAL_DL_SW_EN": the A64
 * (MACH_SUN50I) build of U-Boot's sunxi_mmc.c unconditionally selects
 * MMC_SUNXI_HAS_NEW_MODE and is in sunxi_mmc_can_calibrate(), so
 * mmc_config_clock() ORs NTSR_MODE_SEL_NEW into NTSR AND writes
 * CAL_DL_SW_EN into SAMP_DL for every MMC controller, including MMC2/eMMC
 * at this exact base (mmc_no = (0x01c11000-0x01c0f000)/0x1000 = 2).
 *
 * CRITICAL PRECONDITION (found the hard way — a first attempt wrote NTSR
 * with the card clock already running and it read back as 0x0, write still
 * hung): per U-Boot's mmc_config_clock(), NTSR and SAMP_DL only LATCH while
 * the card clock (CKCR bit16 CLK_ENABLE) is DISABLED. The real sequence is:
 *   1) CKCR &= ~CLK_ENABLE (clock off)
 *   2) clock-update command (poll CMDR bit31 clear)
 *   3) program the CCU mod-clock register, THEN NTSR, THEN SAMP_DL
 *   4) CKCR |= CLK_ENABLE (clock back on)
 *   5) clock-update command again (poll CMDR bit31 clear)
 * emmc_bio_init() below follows this exactly, using clk_update() (see its
 * own comment) for steps 2 and 5. The verified read-only Python sequence never
 * did any of this and reads still worked at the crawling 400kHz init clock
 * (read timing is tolerant of the missing new-mode/calibration setup), but
 * CMD24 writes hang forever with RINT stuck at CMD_DONE|TX_DATA_REQ and
 * DATA_OVER never asserting — the card's write CRC-status-token phase
 * needs the new-mode sample-clock alignment latched correctly to be
 * recognised at all. (The CCU-clock register's own "mode select new" bit,
 * CCM_MMC_CTRL_MODE_SEL_NEW, is undefined for every sunxi clock header on
 * this SoC family and is a no-op — only the two controller-side registers,
 * NTSR and SAMP_DL, matter.) */
#define NTSR_MODE_SEL_NEW    0x80000000u
#define SAMP_DL_CAL_SW_EN    0x00000080u  /* bit7 */

/* GCTL bits */
#define GCTL_RESET_ALL     0x00000007u   /* bits 0,1,2: soft+fifo+dma reset */
#define GCTL_FIFO_RST      0x00000002u   /* bit1 */
#define GCTL_AHB_INIT      0x80000010u   /* bit31 AHB-FIFO-access | bit4 */

/* CKCR bits */
#define CKCR_CARD_CLK_EN   0x00010000u   /* bit16 */

/* CMDR (command register) control/flag bits */
#define CMDR_LOAD           0x80000000u  /* bit31: start/load this command */
#define CMDR_PRG_CLK        0x00200000u  /* bit21: program-clock cmd */
#define CMDR_SEND_INIT      0x00008000u  /* bit15: send init sequence (CMD0) */
#define CMDR_WAIT_PRE_OVER  0x00002000u  /* bit13 */
#define CMDR_DATA_EXP       0x00000200u  /* bit9: data transfer expected */
#define CMDR_CHK_CRC        0x00000100u  /* bit8 */
#define CMDR_WRITE          0x00000400u  /* bit10: data direction = write */
#define CMDR_LONG_RESP      0x00000080u  /* bit7 */
#define CMDR_RESP_EXP       0x00000040u  /* bit6 */

/* RINT (raw interrupt) bits used by polls below */
#define RINT_CMD_DONE       0x00000004u  /* bit2 */
#define RINT_DATA_OVER      0x00000008u  /* bit3 */
#define RINT_ALL            0xFFFFFFFFu

/* STAR (status) bits */
#define STAR_FIFO_EMPTY     0x00000004u  /* bit2 */
#define STAR_FIFO_FULL      0x00000008u  /* bit3 */
#define STAR_CARD_BUSY      0x00000200u  /* bit9: card still programming/busy */

/* Clock-update command issued directly on REG_CMDR (no CAGR/argument):
 * LOAD | PRG_CLK | WAIT_PRE_OVER */
#define CLK_UPDATE_CMDR    (CMDR_LOAD | CMDR_PRG_CLK | CMDR_WAIT_PRE_OVER)

/* CMD17 (READ_SINGLE_BLOCK) full CMDR word, verbatim from the verified
 * sequence: LOAD | WAIT_PRE_OVER | DATA_EXP | CHK_CRC | RESP_EXP | 17 */
#define CMD17_READ_CMDR    0x80002351u

/* CMD24 (WRITE_BLOCK) full CMDR word, mirrored from CMD17 per the source
 * sequence's note: LOAD | WAIT_PRE_OVER | DATA_EXP | WRITE | CHK_CRC |
 * RESP_EXP | 24 == 0x80000000|0x2000|0x200|0x400|0x100|0x40|24 */
#define CMD24_WRITE_CMDR   0x80002758u

/* Bounded-poll cap: a "few hundred thousand" register reads, per the brief.
 * Cheap MMIO reads at EL2 with no cache misses; this is well under a second
 * of wall time even in the worst case, so it can't visibly wedge the debug
 * core, but it's long enough to cover legitimate multi-hundred-microsecond
 * eMMC command/data latencies. */
#define EMMC_POLL_CAP   400000

/* CMD1 (SEND_OP_COND) OCR-ready retry budget, matching the verified
 * "loop up to 40x" in the source sequence. */
#define CMD1_RETRY_CAP  40

/* ------------------------------------------------------------------ */
/* Raw MMIO helpers                                                    */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(uint64_t pa)
{
	return *(volatile uint32_t *)(unsigned long)pa;
}

static inline void wr32(uint64_t pa, uint32_t v)
{
	*(volatile uint32_t *)(unsigned long)pa = v;
}

static inline uint32_t rreg(uint32_t off) { return rd32(EMMC_BASE + off); }
static inline void wreg(uint32_t off, uint32_t v) { wr32(EMMC_BASE + off, v); }

/* ------------------------------------------------------------------ */
/* D4 fix: TIME-based (CNTPCT_EL0) bounds for the two long post-write polls */
/* below, replacing raw iteration caps. Same rationale as wdt.c's own       */
/* CNTPCT-based software window: an iteration count assumes a fixed        */
/* per-iteration wall-clock cost that real hardware does not guarantee (an */
/* AHB bus stall, a slower clock, or just being on a different core with a */
/* different memory-system path could silently blow the intended bound     */
/* out to many times longer than assumed) — see the task's D4 finding.     */
/* CNTPCT_EL0 is a free-running physical counter, safe to read from any     */
/* core with no locking (each core has its own read-only view of the same  */
/* system counter). Mirrored verbatim into sd_bio.c (same controller IP).  */
/* ------------------------------------------------------------------ */
static inline uint64_t rd_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t rd_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* ms -> counter ticks, with the same 24 MHz fallback wdt.c uses if CNTFRQ_EL0
 * ever reads 0 (should not happen on real hardware, but a 0 divisor would
 * make every wait return instantly, which is the WRONG failure direction for
 * a timeout bound — better to assume the architected A64 default). */
static inline uint64_t ms_to_ticks(uint32_t ms)
{
	uint64_t f = rd_cntfrq();
	if (f == 0)
		f = 24000000ull;   /* A64 arch timer default, matches wdt.c's fallback */
	return (f * (uint64_t)ms) / 1000ull;
}

/* Generous caps, NOT tight ones: the goal is to bound worst-case wall time
 * (so a single call can never approach the ~16 s HW WDOG window — see wdt.c),
 * not to police "normal" latency. Both are far above the expected duration:
 *   - DATA_OVER wait: pushing 512 B out at the 400 kHz init clock is
 *     ~10 ms (see the original comment this replaces); 1000 ms is ~100x that.
 *   - CARD_BUSY (flash program) wait: typically well under 1 s even on slow
 *     eMMC; 4000 ms leaves generous headroom without the two waits, summed,
 *     coming anywhere close to 16 s even with ZERO watchdog feeding in
 *     between (see vblk_emmc.h's D4 discussion of why no wdt_pet() call was
 *     added here — this file runs on CPU0, CPU1 *and* CPU2, and the per-
 *     sector feed already happens one layer up, in vblk_emmc.c's
 *     serve_data(), after this call returns). */
#define EMMC_WRITE_DATA_TIMEOUT_MS   1000u
#define EMMC_WRITE_BUSY_TIMEOUT_MS   4000u

/* poll(mask, want): spin reading RINT until (RINT & mask) == want, capped at
 * EMMC_POLL_CAP iterations. Returns 0 on success, -1 on timeout. mask==0 is
 * trivially satisfied immediately (matches the source sequence's use of
 * poll(0, 0) as a bounded short delay rather than a real wait-for-bit). */
static int poll_rint(uint32_t mask, uint32_t want)
{
	int i;
	for (i = 0; i < EMMC_POLL_CAP; i++) {
		if ((rreg(REG_RINT) & mask) == want)
			return 0;
	}
	return -1;
}

/* Small bounded busy-wait used where the source sequence relied on a
 * trivial poll(0, 0)/short delay rather than a real condition (e.g. right
 * after kicking the GCTL soft/fifo/dma reset bits, which self-clear in
 * hardware on their own timescale). Not a correctness gate — just avoids
 * racing the very next register write against the reset pulse. */
static void small_delay(void)
{
	volatile int i;
	for (i = 0; i < 64; i++)
		(void)rreg(REG_GCTL);
}

/* clk_update(): issue the MMC "clock update only" command (U-Boot's
 * mmc_update_clk() — START|PRG_CLK|WAIT_PRE_OVER on CMDR) and wait for the
 * LOAD/START bit to self-clear IN CMDR ITSELF (0x18), not RINT — this is
 * how the hardware acks a clock-update command. Returns 0 on success, -1 on
 * a bounded timeout. Also clears RINT afterwards (the clock-update pulse
 * raises assorted RINT bits, matching the reference driver). */
static int clk_update(void)
{
	uint32_t i;

	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CMDR, CLK_UPDATE_CMDR);
	for (i = 0; i < EMMC_POLL_CAP; i++)
		if ((rreg(REG_CMDR) & CMDR_LOAD) == 0)
			break;
	if (i >= EMMC_POLL_CAP)
		return -1;
	wreg(REG_RINT, RINT_ALL);
	return 0;
}

/* cmd(op, arg, fl, dm): issue one MMC command with dm defaulting to
 * RINT_CMD_DONE in the source sequence's cmd() helper. Returns 0 on
 * success (with *rint_out/*resp0_out filled in if non-NULL), -1 on a
 * bounded poll timeout. */
static int emmc_cmd(uint32_t op, uint32_t arg, uint32_t fl, uint32_t dm,
                     uint32_t *rint_out, uint32_t *resp0_out)
{
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, arg);
	wreg(REG_CMDR, CMDR_LOAD | fl | op);
	if (poll_rint(dm, dm) != 0)
		return -1;
	if (rint_out)
		*rint_out = rreg(REG_RINT);
	if (resp0_out)
		*resp0_out = rreg(REG_RESP0);
	return 0;
}

/* Convenience wrapper matching the source sequence's cmd(op,arg,fl) with
 * the default dm=RINT_CMD_DONE. */
static int emmc_cmd_done(uint32_t op, uint32_t arg, uint32_t fl,
                          uint32_t *resp0_out)
{
	return emmc_cmd(op, arg, fl, RINT_CMD_DONE, NULL, resp0_out);
}

/* ------------------------------------------------------------------ */
/* emmc_bio_init() — one-time bring-up + card-identification sequence      */
/* ------------------------------------------------------------------ */
int emmc_bio_init(void)
{
	uint32_t c, resp0;
	int i;

	/* PC5 -> func3 (eMMC clock pinmux fix). */
	c = rd32(PIO_PC_CFG0);
	wr32(PIO_PC_CFG0, (c & ~(0xFu << 20)) | (3u << 20));

	/* Mask controller interrupts so nothing else eats RINT bits we poll. */
	wreg(REG_IMKR, 0);

	/* Soft/FIFO/DMA reset (GCTL bits 0,1,2), then a short bounded delay
	 * for the self-clearing reset pulse to settle. */
	wreg(REG_GCTL, GCTL_RESET_ALL);
	small_delay();

	/* AHB-FIFO-access mode + bit4. */
	wreg(REG_GCTL, GCTL_AHB_INIT);

	/* Max timeout. */
	wreg(REG_TMOR, 0xffffffffu);

	/* 1-bit bus width. */
	wreg(REG_BWDR, 0);

	/* --- Clock configuration: mirrors U-Boot's mmc_config_clock() exactly,
	 * see the REG_NTSR/REG_SAMP_DL comment above for why. --- */

	/* a) Card clock OFF, latch with a clock-update command. */
	wreg(REG_CKCR, 0);
	if (clk_update() != 0)
		return -1;

	/* b) Program the CCU mod-clock (400kHz init clock), then NTSR
	 * "new mode", then SAMP_DL calibration-delay-enable — all three
	 * MUST happen while the card clock is off (step a) for NTSR/SAMP_DL
	 * to actually latch (verified live: writing them with the clock
	 * already running left NTSR reading back 0x0). */
	wr32(CCU_MMC2_CLK, 0x8002000eu);       /* 24MHz/4/15 = 400kHz */
	wreg(REG_NTSR, rreg(REG_NTSR) | NTSR_MODE_SEL_NEW);
	wreg(REG_SAMP_DL, SAMP_DL_CAL_SW_EN);

	/* c) Card clock back ON, divider 0, latch with a second clock-update
	 * command. NOTE: the clock-update LOAD/START bit self-clears in CMDR
	 * (0x18) itself, NOT in RINT — clk_update() polls CMDR. (In the
	 * reference Python the inter-op network latency masked this; C is
	 * fast enough to race ahead of the hardware ack if RINT were polled
	 * instead.) */
	wreg(REG_CKCR, CKCR_CARD_CLK_EN);
	if (clk_update() != 0)
		return -1;

	/* CMD0 GO_IDLE with SEND_INIT_SEQ (bit15). */
	if (emmc_cmd_done(0, 0, CMDR_SEND_INIT, NULL) != 0)
		return -2;

	/* CMD1 SEND_OP_COND, sector mode, up to 40 retries until RESP0 bit31
	 * (ready) is set. */
	for (i = 0; i < CMD1_RETRY_CAP; i++) {
		if (emmc_cmd_done(1, 0x40FF8000u, CMDR_RESP_EXP, &resp0) != 0)
			return -3;
		if (resp0 & 0x80000000u)
			break;
	}
	if (i == CMD1_RETRY_CAP)
		return -4; /* card never reported ready */

	/* CMD2 ALL_SEND_CID (long response). */
	if (emmc_cmd_done(2, 0, CMDR_LONG_RESP | CMDR_RESP_EXP, NULL) != 0)
		return -5;

	/* CMD3 SET_RCA = 1. */
	if (emmc_cmd_done(3, 0x00010000u, CMDR_RESP_EXP, NULL) != 0)
		return -6;

	/* CMD9 SEND_CSD (long response), rca=1. */
	if (emmc_cmd_done(9, 0x00010000u, CMDR_LONG_RESP | CMDR_RESP_EXP, NULL) != 0)
		return -7;

	/* CMD7 SELECT, rca=1. */
	if (emmc_cmd_done(7, 0x00010000u, CMDR_RESP_EXP, NULL) != 0)
		return -8;

	/* CMD16 SET_BLOCKLEN 512. */
	if (emmc_cmd_done(16, 512, CMDR_RESP_EXP, NULL) != 0)
		return -9;

	/* Best-effort HS reclock (25 MHz + wide bus). Fail-safe by construction
	 * (see emmc_bio.h / the function's own header comment below): any
	 * internal failure restores 400 kHz/1-bit and this still returns 0, so
	 * it can never turn a successful slow-path bring-up into an init()
	 * failure. */
	(void)emmc_bio_set_highspeed();

	return 0;
}

/* ------------------------------------------------------------------ */
/* emmc_bio_read() — CMD17 single-block read, hardware-verified sequence   */
/* ------------------------------------------------------------------ */
/* Failure diagnostics window @0x50020200 (inside the DTB-reserved hv-scratch,
 * clear of vblk's 0x50020000-0x5002017f): [0] fail count, [1] last fail LBA,
 * [2] RISR at fail, [3] STAR at fail, [4] nwords drained before the stall,
 * [5] GCTL at fail. nwords discriminates: 0 + no CMD_DONE in RISR = command
 * never completed; 0 + CMD_DONE = card took the command but sent no data;
 * 1..127 = stall mid-drain (timing/FIFO). */
#define EBIO_BC_BASE HVMAP_EBIO_BC   /* see hv_addrmap.h */
static inline void ebio_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(EBIO_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}
static uint32_t g_ebio_fails;

int emmc_bio_read(uint32_t lba, uint64_t buf_pa)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	unsigned nwords = 0;
	int i;

	/* FIFO reset (GCTL bit1) + tiny settle delay. */
	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, 512);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD17_READ_CMDR);

	for (i = 0; i < EMMC_POLL_CAP && nwords < 128; i++) {
		uint32_t st = rreg(REG_STAR);
		if (st & STAR_FIFO_EMPTY) {
			if (rreg(REG_RINT) & RINT_DATA_OVER)
				break; /* DATA_OVER + FIFO empty -> transfer done */
			continue;
		}
		buf[nwords++] = rreg(REG_FIFO);
	}

	if (nwords < 128) {
		ebio_bc(1, lba);
		ebio_bc(2, rreg(REG_RINT));
		ebio_bc(3, rreg(REG_STAR));
		ebio_bc(4, nwords);
		ebio_bc(5, rreg(REG_GCTL));
		ebio_bc(0, ++g_ebio_fails);
		return -1; /* timed out before draining a full 512B block */
	}

	/* ROOT-CAUSE FIX (found live 2026-07-20, virtio-blk "hard error" hunt):
	 * draining 128 words is NOT the end of the transfer — the controller
	 * still runs the CRC/end phase and latches DATA_OVER slightly LATER.
	 * Returning here let a BACK-TO-BACK next call (virtio-blk's serve_data
	 * issues sector reads in a tight loop) FIFO-reset the controller and
	 * write a new CMDR while this block was still retiring; the previous
	 * block's late DATA_OVER then landed AFTER that call's RINT clear, so
	 * its drain loop saw FIFO_EMPTY+DATA_OVER immediately and broke out
	 * with nwords=0 -> spurious -1 for virtually every back-to-back read.
	 * (Diagnosed from the EBIO breadcrumb: RISR=0x8 = DATA_OVER WITHOUT
	 * CMD_DONE, nwords=0. dbgmon/CPU1 calls never hit this because EMAC
	 * round-trips pace them ~80 ms apart.) Wait for DATA_OVER + card idle
	 * before returning, exactly like emmc_bio_write already does. */
	if (poll_rint(RINT_DATA_OVER, RINT_DATA_OVER) != 0) {
		ebio_bc(1, lba);
		ebio_bc(2, rreg(REG_RINT));
		ebio_bc(3, rreg(REG_STAR));
		ebio_bc(4, 0x10000u | nwords);   /* tag: failed at post-drain wait */
		ebio_bc(5, rreg(REG_GCTL));
		ebio_bc(0, ++g_ebio_fails);
		return -1;
	}
	for (i = 0; i < EMMC_POLL_CAP; i++) {
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
			break;
	}

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* ------------------------------------------------------------------ */
/* emmc_bio_write() — CMD24 single-block write. Structural mirror of the   */
/* read path (per the brief); NOT independently hardware-verified.         */
/* ------------------------------------------------------------------ */
int emmc_bio_write(uint32_t lba, uint64_t buf_pa)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	unsigned nwords = 0;
	int i;

	__asm__ volatile("dsb sy" ::: "memory");

	/* FIFO reset (GCTL bit1) + tiny settle delay. */
	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, 512);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD24_WRITE_CMDR);

	/* Push all 128 words, waiting out FIFO_FULL (STAR bit3) between
	 * words — the write-direction mirror of readblk()'s FIFO_EMPTY
	 * spin. */
	for (i = 0; i < EMMC_POLL_CAP && nwords < 128; i++) {
		uint32_t st = rreg(REG_STAR);
		if (st & STAR_FIFO_FULL)
			continue;
		wreg(REG_FIFO, buf[nwords++]);
	}
	if (nwords < 128)
		return -1; /* FIFO never drained enough to accept all words */

	/* Wait for the controller to report the data phase complete. TIME-capped
	 * (D4 fix — see the block comment above EMMC_WRITE_DATA_TIMEOUT_MS): the
	 * init clock is 400 kHz, so pushing 512 B out to the card takes ~10 ms,
	 * far longer than EMMC_POLL_CAP (~0.4 ms) — a normal poll_rint would time
	 * out before DATA_OVER. Also bail on any data error bit. */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(EMMC_WRITE_DATA_TIMEOUT_MS);
		uint32_t ri;
		for (;;) {
			ri = rreg(REG_RINT);
			if (ri & RINT_DATA_OVER)
				break;
			if (ri & 0x0180u)   /* DATA_CRC(bit7)/DATA_TIMEOUT(bit8) */
				return (int)(0x40000000u | (ri & 0x3fffu));
			if (rd_cntpct() - start > cap)
				return (int)(0x20000000u | (rreg(REG_RINT) & 0x3fffu));
		}
	}

	/* Card may be busy programming (DAT0 low) — wait STAR CARD_BUSY(bit9)
	 * clear.
	 *
	 * BUG FIXED HERE (found live 2026-07-24 chasing why `fsck -y
	 * /dev/vtbd0p3` — the FIRST real write workload this path has EVER
	 * seen; every prior test this project ran was read-only — corrupted
	 * the guest): this used EMMC_POLL_CAP (400000 register-read iterations,
	 * fine for the DATA-phase polls above and for emmc_bio_read()'s OWN
	 * post-transfer busy-wait, since reads don't have a comparable
	 * latency) for the POST-WRITE flash PROGRAM time — which is a
	 * genuinely different, typically much longer latency than anything a
	 * read incurs, especially at the 400 kHz init clock this controller
	 * has only ever run at (see emmc_bio_set_highspeed()'s own comment: HS
	 * mode has never been exercised). Worse, the old code didn't even
	 * check whether the wait actually succeeded — it fell out of the loop
	 * and returned 0 (success) UNCONDITIONALLY, whether CARD_BUSY had
	 * cleared or not. If the card was still internally busy programming
	 * when this returned, the caller's NEXT command (another sector's
	 * write, or a completely unrelated read) hit the controller/card
	 * while it was still mid-program — exactly the same class of bug
	 * emmc_bio_read()'s own "ROOT-CAUSE FIX" comment above describes for
	 * back-to-back reads, just on the write side, where it's far more
	 * likely to actually manifest. Now: a generous TIME cap (D4 fix — see
	 * EMMC_WRITE_BUSY_TIMEOUT_MS above) instead of an iteration count whose
	 * real-world duration is not actually bounded (real card program time
	 * can exceed the raw transfer time), and a genuine timeout error instead
	 * of a silent lie. */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(EMMC_WRITE_BUSY_TIMEOUT_MS);
		for (;;) {
			if ((rreg(REG_STAR) & STAR_CARD_BUSY) == 0)
				break;
			if (rd_cntpct() - start > cap)
				return -2;   /* card never signaled program-done: do NOT claim success */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* ------------------------------------------------------------------ */
/* emmc_bio_set_highspeed() — best-effort HS reclock, FAIL-SAFE.           */
/*                                                                          */
/* Sequence and register math are ported from the two reference drivers    */
/* named in the design brief (NOT hardware-verified yet — this controller  */
/* has only ever been run at the 400 kHz identification clock so far):     */
/*                                                                          */
/*  - Ordering (bus-width switch, THEN HS_TIMING switch, THEN raise the    */
/*    host clock LAST) matches U-Boot's generic mmc.c                      */
/*    mmc_select_mode_and_width(): both CMD6 SWITCH commands are sent      */
/*    while the host clock is UNCHANGED (still the old/safe rate), and     */
/*    mmc_set_clock() to the new rate happens only after both switches     */
/*    have been confirmed (mmc_poll_for_busy()). We follow the same order  */
/*    for the same reason: isolate causes (a CMD6 timeout can never be         */
/*    blamed on a bad new clock, because the clock hasn't moved yet).      */
/*  - CMD6 argument encoding (access=WRITE_BYTE=3, index<<16, value<<8) is */
/*    the standard JEDEC eMMC SWITCH format — EXT_CSD_HS_TIMING=185,       */
/*    EXT_CSD_BUS_WIDTH=183, matching the brief's arg values exactly       */
/*    (0x03B90100, 0x03B70100/0x03B70200).                                 */
/*  - CCU MMC2_CLK register field layout (bit31 ENABLE, bit24 clock-source */
/*    select 0=OSC24M/1=PLL6(PLL_PERIPH0), bits[17:16] N raw = pre-divide  */
/*    exponent 2^N, bits[3:0] M raw = divisor-1) is U-Boot's               */
/*    CCM_MMC_CTRL_* layout (arch-sunxi/clock_sun6i.h) — and is EMPIRICALLY*/
/*    confirmed against THIS controller: emmc_bio_init()'s own known-good  */
/*    0x8002000eu decodes under this exact layout as OSC24M / 2^2 / 15 =   */
/*    24 MHz/4/15 = 400 kHz, matching its own comment above verbatim.      */
/*  - clk_update()'s CMDR LOAD|PRG_CLK|WAIT_PRE_OVER handshake, polled on  */
/*    CMDR itself (not RINT), is REUSED as-is (see clk_update()'s own      */
/*    comment) for both the HS-up and the fallback-down reclock.          */
/*  - REG_BWDR values 0/1/2 for 1/4/8-bit match U-Boot's                   */
/*    sunxi_mmc_set_ios_common() (writel(0x0|0x1|0x2, &priv->reg->width)). */
/*                                                                          */
/* PLL6/PLL_PERIPH0 is assumed fixed at 600 MHz as tapped by the MMC clock */
/* mux (linux-sunxi wiki / mainline sun50i-a64-ccu "pll_periph0" 1X parent)*/
/* -- this assumption is NOT independently re-verified against the A64     */
/* datasheet PDF in this pass; if wrong, the resulting card clock is off   */
/* target and the mandatory post-switch test read (step 5) will fail its   */
/* drain/DATA_OVER poll, which triggers the fail-safe fallback exactly as  */
/* designed — a wrong-frequency guess degrades to "HS stayed off", not to  */
/* a wedged or corrupted disk.                                            */
/* ------------------------------------------------------------------ */

/* CMD6 (SWITCH) argument encoding, standard JEDEC eMMC: access=WRITE_BYTE
 * (3) << 24 | index << 16 | value << 8. */
#define CMD6_SWITCH_HS_TIMING     0x03B90100u  /* idx185 (HS_TIMING) = 1 (HS)   */
#define CMD6_SWITCH_BUSWIDTH_1    0x03B70000u  /* idx183 (BUS_WIDTH) = 0 (1-bit)*/
#define CMD6_SWITCH_BUSWIDTH_4    0x03B70100u  /* idx183 (BUS_WIDTH) = 1 (4-bit)*/
#define CMD6_SWITCH_BUSWIDTH_8    0x03B70200u  /* idx183 (BUS_WIDTH) = 2 (8-bit)*/

/* Bus width to negotiate if HS_TIMING succeeds. BPI-M64's eMMC is wired
 * 8-bit per schematic, but whether the D4-D7 pinmux (beyond the PC5 clock
 * fix already applied in emmc_bio_init()) is actually muxed to the SMHC2
 * function on this board has NOT been confirmed here — see the risk note
 * in the task report. Default to the safe 4-bit; flip this to 8 to try
 * wide-8. Either way the post-switch test read (step 5) gates it: if the
 * data pins aren't muxed, the test read fails and we fall back to 1-bit. */
#ifndef EMMC_BUS_WIDTH
#define EMMC_BUS_WIDTH 4
#endif
#if EMMC_BUS_WIDTH != 4 && EMMC_BUS_WIDTH != 8
#error "EMMC_BUS_WIDTH must be 4 or 8"
#endif

/* REG_BWDR (controller bus-width register) values. */
#define BWDR_1BIT   0u
#define BWDR_4BIT   1u
#define BWDR_8BIT   2u

/* CCU MMC2_CLK register values, computed exactly like U-Boot's
 * mmc_set_mod_clk(): div = pll_hz/hz; n=0; while (div>16) { n++;
 * div=(div+1)/2; } -> ENABLE | src | N(n)<<16 | M(div-1).
 *   400 kHz (safe/init, OSC24M path): 24 MHz/2^2/15 == the literal
 *     emmc_bio_init() already uses (0x8002000eu) — duplicated here as a
 *     named constant so the fallback path below provably restores the
 *     IDENTICAL value, not a hand-copied one.
 *   HS-25 (PLL6/PLL_PERIPH0=600MHz assumed): div=600/25=24 -> n=1,div=12
 *     -> N=1 (/2), M=11 (/12) -> 600/2/12 = 25.000 MHz exact.
 *   HS-50 (same PLL): div=600/50=12 -> n=0,div=12 -> N=0, M=11 (/12) ->
 *     600/1/12 = 50.000 MHz exact. Not used by default (#define
 *     EMMC_HS_CLK_REG CCU_MMC2_CLK_HS50 to try it). */
#define CCU_MMC2_CLK_400K   0x8002000eu   /* == emmc_bio_init()'s literal   */
#define CCU_MMC2_CLK_HS25   0x8101000Bu   /* PLL6 | N=1 | M=11 -> 25 MHz    */
#define CCU_MMC2_CLK_HS50   0x8000000Bu   /* PLL6 | N=0 | M=11 -> 50 MHz    */

#ifndef EMMC_HS_CLK_REG
#define EMMC_HS_CLK_REG CCU_MMC2_CLK_HS25   /* conservative default */
#endif

/* HV-local scratch buffer for the mandatory post-switch test read (step 5
 * below). Lives in the same DTB-reserved hv-scratch DRAM window as the
 * VBLK breadcrumb/lock (0x50020000-0x5002017f) and this file's own EBIO
 * breadcrumbs (0x50020200-0x5002021f, words 0-7) — placed well clear of
 * both (0x50020300, 512 B), so it can never collide with either. */
#define EMMC_HS_TESTBUF_PA  HVMAP_EMMC_HS_TESTBUF   /* see hv_addrmap.h */

/* Breadcrumb word indices in the EBIO window (@EBIO_BC_BASE, see above):
 * word[6] = HS state (0=never run,1=HS active,2=fell back after failure);
 * word[7] = which step failed when word[6]==2 (0 otherwise). */
#define EBIO_BC_HS_STATE   6
#define EBIO_BC_HS_STEP    7

#define HS_FAIL_CMD6_TIMING  1  /* CMD6 HS_TIMING switch command timed out   */
#define HS_FAIL_BUSY_TIMING  2  /* post HS_TIMING busy-wait timed out        */
#define HS_FAIL_CMD6_WIDTH   3  /* CMD6 BUS_WIDTH switch command timed out   */
#define HS_FAIL_BUSY_WIDTH   4  /* post BUS_WIDTH busy-wait timed out        */
#define HS_FAIL_RECLOCK      5  /* controller clk_update() handshake timeout */
#define HS_FAIL_TESTREAD     6  /* post-switch LBA0 test read failed         */

/* Bounded wait for STAR_CARD_BUSY (bit9) to clear, used after each CMD6
 * SWITCH (R1b — busy signalled on DAT0). Identical idiom to the STAR
 * CARD_BUSY waits already used after CMD24 (emmc_bio_write) and after the
 * DATA_OVER poll in emmc_bio_read(); EMMC_POLL_CAP left as-is per the
 * timeout-dimensioning note (sized for 400 kHz, generous at 25 MHz). */
static int hs_busy_wait(void)
{
	int i;
	for (i = 0; i < EMMC_POLL_CAP; i++)
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
			return 0;
	return -1;
}

/* Reclock the CONTROLLER only (CCU divider + CKCR card-clock-enable),
 * mirroring emmc_bio_init()'s own clock-config block verbatim: clock OFF +
 * clk_update(), program CCU (NTSR/SAMP_DL re-asserted while the clock is
 * off, per the hardware precondition documented above emmc_bio_init()),
 * clock back ON + clk_update(). Used both to raise the clock for HS and,
 * by the fallback below, to restore the safe 400 kHz rate. Returns 0 on
 * success, -1 if either clk_update() handshake times out. */
static int emmc_reclock(uint32_t ccu_val)
{
	wreg(REG_CKCR, 0);
	if (clk_update() != 0)
		return -1;
	wr32(CCU_MMC2_CLK, ccu_val);
	wreg(REG_NTSR, rreg(REG_NTSR) | NTSR_MODE_SEL_NEW);
	wreg(REG_SAMP_DL, SAMP_DL_CAL_SW_EN);
	wreg(REG_CKCR, CKCR_CARD_CLK_EN);
	if (clk_update() != 0)
		return -1;
	return 0;
}

/* Unconditional fail-safe restore: bring the CARD back to 1-bit (a no-op,
 * spec-legal SWITCH if it was never widened — best-effort, return value
 * ignored on purpose: there is nothing further to do if even this fails,
 * see the risk note in the task report) BEFORE reverting the HOST's own
 * bus-width register and clock, so the two sides can never disagree about
 * bus width. HS_TIMING is deliberately left set on the card: a card in
 * high-speed timing mode remains fully spec-legal (and this driver's own
 * verified init/read path proves it works) at 400 kHz — JEDEC backward
 * compatibility — so there is nothing unsafe about leaving it as-is.
 * Always returns 0 (matches emmc_bio_set_highspeed()'s own contract: a
 * failed HS attempt is a fully-handled non-event, never an init failure). */
static int emmc_hs_fallback(int fail_step)
{
	(void)emmc_cmd_done(6, CMD6_SWITCH_BUSWIDTH_1, CMDR_RESP_EXP, NULL);
	(void)hs_busy_wait();

	wreg(REG_BWDR, BWDR_1BIT);
	(void)emmc_reclock(CCU_MMC2_CLK_400K);

	ebio_bc(EBIO_BC_HS_STATE, 2);
	ebio_bc(EBIO_BC_HS_STEP, (uint32_t)fail_step);
	return 0;
}

int emmc_bio_set_highspeed(void)
{
	uint32_t width_arg = (EMMC_BUS_WIDTH == 8) ? CMD6_SWITCH_BUSWIDTH_8
	                                            : CMD6_SWITCH_BUSWIDTH_4;
	uint32_t bwdr_val  = (EMMC_BUS_WIDTH == 8) ? BWDR_8BIT : BWDR_4BIT;

	/* 1) CMD6 SWITCH HS_TIMING=1, still at the current safe 400 kHz/1-bit
	 * clock (see the ordering rationale in the block comment above), then
	 * wait for the card to drop DAT0 busy. */
	if (emmc_cmd_done(6, CMD6_SWITCH_HS_TIMING, CMDR_RESP_EXP, NULL) != 0)
		return emmc_hs_fallback(HS_FAIL_CMD6_TIMING);
	if (hs_busy_wait() != 0)
		return emmc_hs_fallback(HS_FAIL_BUSY_TIMING);

	/* 2) CMD6 SWITCH BUS_WIDTH, still at 400 kHz, then wait busy. */
	if (emmc_cmd_done(6, width_arg, CMDR_RESP_EXP, NULL) != 0)
		return emmc_hs_fallback(HS_FAIL_CMD6_WIDTH);
	if (hs_busy_wait() != 0)
		return emmc_hs_fallback(HS_FAIL_BUSY_WIDTH);

	/* 3) Host side: match the controller's bus-width register to what the
	 * card just switched to (pure register write, no failure mode). */
	wreg(REG_BWDR, bwdr_val);

	/* 4) Reclock the CONTROLLER to the HS target LAST, exactly mirroring
	 * the reference drivers' order (bus width + timing switched first,
	 * clock raised only once both are confirmed). */
	if (emmc_reclock(EMMC_HS_CLK_REG) != 0)
		return emmc_hs_fallback(HS_FAIL_RECLOCK);

	/* 5) Mandatory post-switch test read of LBA 0: rc==0 is the only thing
	 * that matters (a bad sample point/CRC mismatch at the new clock/width
	 * shows up as a drain timeout inside emmc_bio_read(), exactly like any
	 * other failure of that already-hardware-verified path — including its
	 * DATA_OVER-after-drain fix, untouched here). */
	if (emmc_bio_read(0, EMMC_HS_TESTBUF_PA) != 0)
		return emmc_hs_fallback(HS_FAIL_TESTREAD);

	ebio_bc(EBIO_BC_HS_STATE, 1);
	ebio_bc(EBIO_BC_HS_STEP, 0);
	return 0;
}
