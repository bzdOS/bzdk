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
#include "cntpct.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

/* ------------------------------------------------------------------ */
/* Physical bases                                                      */
/* ------------------------------------------------------------------ */
#define EMMC_BASE        SOC_A64_SMHC2_BASE   /* SMHC2/aw_mmc1 (eMMC) controller  */
#define PIO_PC_CFG0      (SOC_A64_PIO_BASE + 0x48)   /* PC5 pinmux nibble (bits[23:20])  */
#define CCU_MMC2_CLK     SOC_A64_CCU_MMC2_CLK   /* CCU MMC2_CLK gate/divider reg    */

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
#define GCTL_DMA_RST       0x00000004u   /* bit2 */
#define GCTL_AHB_INIT      0x80000010u   /* bit31 AHB-FIFO-access | bit4 */

/* CKCR bits */
#define CKCR_CARD_CLK_EN   0x00010000u   /* bit16 */

/* CMDR (command register) control/flag bits */
#define CMDR_LOAD           0x80000000u  /* bit31: start/load this command */
#define CMDR_PRG_CLK        0x00200000u  /* bit21: program-clock cmd */
#define CMDR_SEND_INIT      0x00008000u  /* bit15: send init sequence (CMD0) */
#define CMDR_WAIT_PRE_OVER  0x00002000u  /* bit13 */
#define CMDR_STOP_ABORT     0x00004000u  /* bit14: this is an ABORT command */
#define CMDR_DATA_EXP       0x00000200u  /* bit9: data transfer expected */
#define CMDR_CHK_CRC        0x00000100u  /* bit8 */
#define CMDR_WRITE          0x00000400u  /* bit10: data direction = write */
#define CMDR_LONG_RESP      0x00000080u  /* bit7 */
#define CMDR_RESP_EXP       0x00000040u  /* bit6 */

/* RINT (raw interrupt) bits used by polls below */
#define RINT_CMD_DONE       0x00000004u  /* bit2 */
#define RINT_DATA_OVER      0x00000008u  /* bit3 */
#define RINT_ALL            0xFFFFFFFFu

/* Error bits. PROVENANCE MATTERS HERE, so it is written down rather than
 * assumed: this is the sunxi SDXC layout Linux's sunxi-mmc.c uses, and the two
 * bits this file already relied on before any of these were named -- bit2
 * CMD_DONE and bit3 DATA_OVER -- match it exactly, which is the corroboration.
 * It is NOT read off the A64 manual, so treat a bit that behaves unlike its
 * name as evidence against the name, not against the hardware. */
#define RINT_RESP_ERR       0x00000002u  /* bit1  */
#define RINT_RESP_CRC_ERR   0x00000040u  /* bit6  */
#define RINT_DATA_CRC_ERR   0x00000080u  /* bit7  */
#define RINT_RESP_TIMEOUT   0x00000100u  /* bit8  -- SEE THE WARNING BELOW */
#define RINT_DATA_TIMEOUT   0x00000200u  /* bit9  */
#define RINT_FIFO_RUN_ERR   0x00000800u  /* bit11 */
#define RINT_HW_LOCKER      0x00001000u  /* bit12 */
#define RINT_START_BIT_ERR  0x00002000u  /* bit13 */
#define RINT_END_BIT_ERR    0x00008000u  /* bit15 */

/* The mask emmc_bio_read() fails a read on. DELIBERATELY EXCLUDES bit8.
 *
 * Measured on this board 2026-08-12 (breadcrumb [14], an OR across every
 * SUCCESSFUL write since boot): bit8 was set at the end of at least one write
 * that completed fine. That is one write, not every write -- the accumulator
 * cannot distinguish -- but it is enough to disqualify bit8 from a fatal mask
 * until something explains it, because a bit whose observed behaviour
 * contradicts the name "RESP_TIMEOUT" is exactly the wrong thing to start
 * failing I/O on. The write path's own long-standing 0x0180 check DOES include
 * bit8; that is not evidence it is safe, it is unexamined.
 *
 * Everything in this mask has been observed NEVER to be set at the end of a
 * successful read: [13] holds only {CMD_DONE, DATA_OVER, RX_DATA_REQ} = 0x2c
 * across a full boot plus a 68 MB read workload. So switching this check on
 * cannot break a path that works today -- it can only start reporting a
 * condition that was previously returned to the guest as good data. If [20]
 * ever climbs, read [19] to see WHICH bit fired before assuming the medium is
 * at fault; a benign bit in this mask would look identical to a real error. */
#define RINT_READ_ERR_MASK  (RINT_RESP_ERR | RINT_RESP_CRC_ERR | \
                             RINT_DATA_CRC_ERR | RINT_DATA_TIMEOUT | \
                             RINT_FIFO_RUN_ERR | RINT_HW_LOCKER | \
                             RINT_START_BIT_ERR | RINT_END_BIT_ERR)

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

/* CMD8 (SEND_EXT_CSD, eMMC meaning) full CMDR word: identical flag set to
 * CMD17 (single-block PIO read, no address argument needed -- EXT_CSD is a
 * fixed per-card register, not sector-addressed), command index 8 instead
 * of 17: LOAD | WAIT_PRE_OVER | DATA_EXP | CHK_CRC | RESP_EXP | 8. */
#define CMD8_SEND_EXT_CSD_CMDR  0x80002348u

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
	v = cntpct_read();   /* cntpct.h: Allwinner counter erratum */
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
/* Per-STALL budget for the read drain loop, not a whole-transfer budget: it
 * resets every time a word is drained. Generous on purpose -- an eMMC may pause
 * a read for tens of milliseconds of internal housekeeping, and failing such a
 * read costs the guest an EIO on a disk that is working. CPU1 owns the hardware
 * watchdog and pets it independently of this core, so a long stall here cannot
 * reset the board. */
#define EMMC_READ_STALL_TIMEOUT_MS   500u
/* Deadline for the FIFO tail AFTER DATA_OVER has latched with a short count.
 * The card has stopped sending by then, so the outstanding words are already in
 * flight inside the controller: they show up in microseconds, or the transfer
 * really is short and waiting longer is pointless. Deliberately much tighter
 * than the stall budget above. */
#define EMMC_READ_DRAIN_TAIL_MS      20u
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
 * success (with *rint_out and *resp0_out filled in if non-NULL), -1 on a
 * bounded poll timeout.
 *
 * (The "/" before "*resp0_out" used to sit directly against it, which C reads as
 * a nested comment opener and -Wcomment warns about. Worth fixing rather than
 * tolerating: a standing warning is where a real one goes to hide.) */
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
/* Defined below, next to ebio_bc() (which it uses); forward-declared so
 * emmc_bio_init() can clear the window before anything writes to it. */
static void ebio_bc_reset(void);

int emmc_bio_init(void)
{
	uint32_t c, resp0;
	int i;

	/* Clear the breadcrumb window for THIS boot.
	 *
	 * Without this the window is never zeroed, while g_ebio_fails IS (it is
	 * plain .bss and start.S zeroes .bss on every load). So a boot with zero
	 * failures leaves the LAST failing boot's numbers sitting there, and a
	 * reader cannot tell "12 failures this boot" from "0 this boot, 12 in some
	 * earlier one". That ambiguity wasted the verification of c40964b on
	 * 2026-07-30: the window read 12 after a soak in which every post-fix
	 * cycle reported g_ioerrs=0, and the two statements could not be
	 * reconciled from the data. vblk_init() already zeroes its own VBK1 window
	 * for exactly this reason; this makes the EBIO window behave the same, so
	 * a value read out of it always belongs to the boot that is running.
	 *
	 * Post-mortem evidence is NOT lost by doing this: the soak harness reads
	 * the window BEFORE triggering the next reload, which is the same pattern
	 * it already uses for VBK1. */
	ebio_bc_reset();

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
static uint32_t g_settles;        /* [8] error-recovery runs                */
/* [9] was g_settle_clkfail; the clk_update() it counted is gone (see below). */
static uint32_t g_busy_timeouts;  /* [10] post-write CARD_BUSY wait timeouts */

/* [13]/[14]: OR of every RINT bit observed at the END of a SUCCESSFUL transfer,
 * accumulated separately for reads and writes. Pure observation -- neither one
 * changes a return value.
 *
 * Why this exists. emmc_bio_write() bails on `ri & 0x0180`, but emmc_bio_read()
 * checks NO error bits at all: it returns 0 as soon as 128 words drained and
 * DATA_OVER latched. So if this controller can latch a data-integrity error
 * while still delivering a full block, a read returns rc=0 with wrong bytes --
 * silent corruption, invisible to every vblk IOERR counter (they only count
 * non-zero rc). That is a mechanism, not yet a measurement.
 *
 * It is deliberately NOT being "fixed" by adding a mask first. The two error
 * bits the write path does check are commented as DATA_CRC(bit7)/DATA_TIMEOUT
 * (bit8), and in the sunxi layout used elsewhere bit8 is RESP_TIMEOUT while
 * DATA_TIMEOUT is bit9 -- so the existing mask may already be naming the wrong
 * bit, and guessing a wider one from recollection would just add a second guess.
 * Accumulate what the hardware actually sets on THIS board, then mask. */
static uint32_t g_rint_or_read;   /* [13] OR of RINT after successful reads  */
static uint32_t g_rint_or_write;  /* [14] OR of RINT after successful writes */

/* [15]..[17]: the raw inputs of the post-write CARD_BUSY timeout decision,
 * recorded WITHOUT arithmetic, because the derived value already contradicts
 * itself.
 *
 * Measured 2026-08-12: g_busy_timeouts[10] = 14710 while [11] (the elapsed time
 * that same branch computes, in ms) = 0, with [12] = CNTFRQ = 24000000, i.e.
 * correct. Those two cannot both be right. The branch is only entered when
 * `rd_cntpct() - start > cap`, and cap = ms_to_ticks(4000) = 96e6 ticks, so the
 * elapsed time must exceed 96e6 ticks = 4000 ms; the branch then re-reads the
 * counter and gets under 24000 ticks = under 1 ms. Either `cap` is not what the
 * arithmetic says, or two consecutive rd_cntpct() reads disagree by four
 * orders of magnitude.
 *
 * emmc_bio.c's own comment at the branch predicted exactly this fork: "If [11]
 * is ~4000 the card really does stall that long ... if it is tiny, the cap is
 * expiring early and the timeout math is the bug." It is tiny. So record the
 * ingredients rather than a derived number: cap as computed, and el as the raw
 * 64-bit difference split across two words so an unsigned underflow (el near
 * 2^64, high word all ones) is unmistakable. */
static uint32_t g_busy_cap_lo;    /* [15] (uint32_t)cap at the last timeout   */
static uint32_t g_busy_el_lo;     /* [16] (uint32_t)el                       */
static uint32_t g_busy_el_hi;     /* [17] (uint32_t)(el >> 32)               */

/* [18]: consecutive rd_cntpct() reads that DISAGREED about whether the cap had
 * expired -- one read said elapsed > cap, the very next said elapsed < cap.
 *
 * That is measured, not hypothetical. With cap recorded as exactly 96000000
 * ticks (4.000 s, correct) the busy-wait branch fired while its own re-read of
 * `rd_cntpct() - start`, two instructions later, came to 7627 ticks (0.32 ms).
 * Both are the same expression over the same `start`; the reads themselves
 * disagree by roughly 4 seconds. rd_cntpct() already carries the ISB that the
 * architecture requires before reading CNTPCT_EL0, so that is not the gap.
 *
 * Why the counter behaves this way is still open. The fix does not depend on
 * knowing: a bounded wait must not be decidable by a SINGLE read of an
 * unreliable clock. Both timeout loops now require the cap to be exceeded on
 * two consecutive reads, and an over-then-under pair lands here so the anomaly
 * stays visible instead of being smoothed away. If [18] climbs while the
 * timeouts stop firing, that is the anomaly still happening and no longer doing
 * damage -- which is the intended outcome, not a reason to stop looking. */
static uint32_t g_cnt_anom;       /* [18] over-cap read followed by under-cap */

/* [19]/[20]: reads that finished with a full block AND an error bit set --
 * previously returned as rc=0, i.e. silently. [19] is the OR of the offending
 * bits so a climb in [20] can be attributed to a specific bit. */
static uint32_t g_read_err_bits;  /* [19] OR of bits that failed a read       */
static uint32_t g_read_errs;      /* [20] reads failed on RINT_READ_ERR_MASK  */

/* [21]/[22]: how the FIFO reset actually behaves, which is the evidence for
 * replacing the fixed small_delay() with a real wait. [21] is the worst spin
 * count seen before GCTL's self-clearing reset bits went to 0; [22] counts
 * resets that never cleared within the cap. [21] staying 0 would mean the reset
 * is always complete by the first read-back and the old fixed delay was never
 * the problem -- which is a useful negative result, not a wasted change. */
static uint32_t g_gctl_rst_spins;    /* [21] worst-case poll count            */
static uint32_t g_gctl_rst_timeouts; /* [22] resets that never self-cleared   */

/* [23]: error bits observed at the moment the WRITE data phase saw DATA_OVER.
 *
 * Observation only, on purpose. emmc_bio_write()'s loop checks `ri & 0x0180`
 * AFTER its `if (ri & RINT_DATA_OVER) break;`, so any error bit that latches no
 * later than DATA_OVER is never examined -- the same structural hole as the read
 * path's missing check, and it is still open. It is not being closed in the same
 * change that closed the read side, because the write path currently produces
 * ZERO corrupt sectors in a 68 MB round trip and making a mask fatal there could
 * MEASURED 2026-08-20, which was the whole point of recording it: after 512 MiB
 * of raw reads and 64 MiB of file writes on this board, [23] and [24] are BOTH
 * ZERO -- not one error bit has ever been observed latched at the moment
 * DATA_OVER was seen. So on this part the hole is theoretical, and closing it by
 * failing the write would trade a real (retry-storm) risk for no measured
 * benefit. Leave it observational. The READ path's version of the same hole was
 * closed at the same time, and there it was NOT theoretical: the read loop now
 * tests RINT_READ_ERR_MASK before the DATA_OVER check, so a genuine error ends
 * the wait immediately instead of being outlived by it.
 *
 * only regress that. Record which bits actually co-occur with DATA_OVER first;
 * make it fatal when the data says which bits are safe to fail on. */
static uint32_t g_write_dataover_errs; /* [23] err bits seen with DATA_OVER   */

/* [24]: writes failed because an error bit was set at the moment DATA_OVER
 * latched -- the hole that used to be unreachable. See the check itself. */
static uint32_t g_write_dataover_fails;

/* [25]/[26]: the retry path's safety precondition, measured.
 *
 * ebio_fail_settle() used wait_card_idle(), which is bounded by ITERATIONS
 * (EMMC_POLL_CAP register reads), not by time. A flash program can take tens of
 * milliseconds, so that wait could return with the card still busy -- and the
 * caller's next act is a retry, which is precisely how a retried CMD24 lands on
 * a still-programming card. That is the corruption mechanism this whole
 * investigation traced. The settle was never sufficient protection; it only
 * looked like it because the FIFO/DMA reset half of it is real.
 *
 * This matters most right now: making the write path fail on MORE conditions
 * (the DATA_OVER check above) increases retries, so the retry path has to be
 * safe FIRST or the fix makes the disease worse. [25] is the worst-case wait
 * actually observed in ms, [26] counts settles where the card never went idle
 * inside the bound. */
static uint32_t g_settle_busy_ms;       /* [25] worst observed idle wait, ms  */
static uint32_t g_settle_busy_timeouts; /* [26] card never went idle          */

/* ------------------------------------------------------------------ *
 * FAULT INJECTION -- off unless armed over the debug channel.
 *
 * Exists to answer the one question the fix for the retry path could not answer
 * about itself. ebio_fail_settle() now waits for card-idle on a TIME bound
 * instead of a register-read count, so that a retried CMD24 can never land on a
 * still-programming card. But with the corruption fixed there are no failures
 * left, so the settle path is never entered and that safeguard has never
 * executed -- "correct by construction, unproven under load".
 *
 * The injection point is deliberately the DANGEROUS one: bail out of a write
 * AFTER the data phase has completed and DATA_OVER has latched, i.e. exactly
 * when the card has begun programming, returning the same retryable -2 the old
 * spurious busy-timeout returned. That reproduces the original bug's
 * precondition on demand.
 *
 * g_fi_legacy_wait then makes the settle use the OLD iteration-bounded
 * wait_card_idle(), so ONE build can demonstrate both halves:
 *
 *   inject + legacy wait -> corruption should REAPPEAR
 *   inject + timed wait  -> it should not
 *
 * which is a validation of the fix rather than a mere exercise of it. That is
 * also the only remaining caller of wait_card_idle(), which -Wunused-function
 * had started warning about once both real call sites moved to the timed wait.
 *
 * g_fi_min_lba is a SAFETY interlock, not a convenience: injection can produce
 * real corruption, so it is confined to LBAs at or above a floor the operator
 * sets. Point it at the unused swap partition (12863488) and the guest's root
 * filesystem cannot be touched no matter how the run goes wrong.
 * ------------------------------------------------------------------ */
static uint32_t g_fi_every;       /* inject on every Nth eligible write; 0=off */
static uint32_t g_fi_min_lba;     /* never inject below this LBA (interlock)   */
static uint32_t g_fi_legacy_wait; /* 1 = settle with the OLD iteration wait    */
/* WHICH failure to inject. Added after the first A/B came back negative and
 * forced a re-reading of the original numbers.
 *
 *   0 = bail AFTER DATA_OVER latched (the post-write CARD_BUSY path).
 *   1 = bail on the FIRST poll of the DATA-PHASE wait, before DATA_OVER.
 *
 * Point 0 produced zero corruption with BOTH waits, which is not a null result:
 * once DATA_OVER has latched the data phase is complete and all 128 words have
 * reached the card, so bailing there CANNOT leave a partial block -- and a
 * partial block is precisely what the damage signature requires (card holds
 * words[0..N-1] then words[0..127-N]).
 *
 * Point 1 is where the words are still in the FIFO undelivered, and the settle's
 * FIFO reset discards them. The original counters agree: 160 corrupt sectors
 * against ebio_fails=950 is 1 in 6, while against busy_timeouts=14710 it is 1 in
 * 92. The data-phase path was the corrupting one; the busy path supplied the
 * volume. Both came from the same unguarded CNTPCT subtraction, so the fix covers
 * both -- but the causal chain as first written named the wrong loop. */
static uint32_t g_fi_point;
static uint32_t g_fi_injected;    /* [27] injections performed                 */
static uint32_t g_fi_seq;         /* internal: eligible writes since last hit  */

/* [29]/[30]: the CMD12 STOP_TRANSMISSION added to the settle, and its failures.
 *
 * This closes a bug that the CNTPCT fix hid rather than removed, proven by fault
 * injection at 267 out of 267. This file asserted, as a note of fact:
 *
 *   "NOTE the FIFO reset discards the undelivered words. That is correct here and
 *    only here: these are single-block CMD24 transfers, so the caller re-pushes
 *    the whole 512 B sector from the bounce buffer on retry -- nothing is
 *    half-written from the host's side."
 *
 * The host side is indeed not half-written. The CARD side is. Discarding the
 * FIFO abandons a block the card has already begun ACCEPTING, and the retry's
 * 128 words are then appended to what the card already took instead of replacing
 * it -- which is exactly the measured damage signature (the card ends up holding
 * words[0..N-1] followed by words[0..127-N]). Injecting a mid-data-phase bail
 * corrupted the sector every single time, 267 for 267, with the timed card-idle
 * wait in place. So any GENUINE data-phase failure -- a real CRC error, a real
 * timeout -- would still corrupt on retry. Only the spurious trigger was gone.
 *
 * The remedy was already written down two lines below the wrong claim: "an
 * explicit CMD12 STOP_TRANSMISSION (or a controller soft reset) is the next
 * step". Issued AFTER the FIFO/DMA reset, because the controller has to be able
 * to put a command on the bus, and followed by another idle wait because the card
 * may take time to retire the abort. Best-effort by design: [30] counts CMD12
 * failures rather than escalating, since a settle that cannot even stop the card
 * has already lost and the retry is the remaining hope either way. */
#define CMD12_STOP_TRANSMISSION  12u
static uint32_t g_stop_cmds;      /* [29] CMD12s issued from the settle        */
static uint32_t g_stop_fails;     /* [30] of those, ones that did not complete */
static uint32_t g_dma_irqs;       /* [31] real IDMAC completion IRQs observed
                                   * (gic_timer.c's EMMC_DMA_IRQ_INTID arm) --
                                   * cross-check against vblk_emmc.c's g_dma_ok
                                   * to confirm the interrupt fires ~once per
                                   * DMA transfer, not a control-flow signal */

/* See the call site in emmc_bio_init() for why this exists. EBIO_BC_NWORDS
 * covers every slot any ebio_bc() caller writes, so no stale field survives. */
#define EBIO_BC_NWORDS 32u
/* hv_addrmap.h's assert chain proves this window does not overlap its
 * NEIGHBOURS; it cannot know how many slots this file writes. Without this the
 * two drifted: the declared size said 8 words while the code wrote 13, and the
 * only reason nothing broke is that the next window happened to start 0x100
 * away instead of 0x20. Same shape as the vblk [59] slot-aliasing mistake --
 * a window-level guarantee read as an index-level one. */
_Static_assert(EBIO_BC_NWORDS * 4u <= HVMAP_EBIO_BC_SIZE,
               "emmc_bio.c writes more EBIO breadcrumb slots than "
               "HVMAP_EBIO_BC_SIZE reserves");
static void ebio_bc_reset(void)
{
	unsigned i;
	for (i = 0; i < EBIO_BC_NWORDS; i++)
		ebio_bc((int)i, 0);
}

/* Bounded wait for the card to stop being busy. Cheap when the controller is
 * already idle (breaks on the first read), so it is safe on a hot path. */
static void wait_card_idle(void)
{
	int i;
	for (i = 0; i < EMMC_POLL_CAP; i++)
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
			return;
}

/* Leave the controller quiescent after a FAILED transfer (found live
 * 2026-07-30). The "ROOT-CAUSE FIX" below establishes quiescence on the
 * SUCCESS path only: every failure return jumped straight out with the data
 * phase still retiring, so the NEXT call cleared RINT while a stale DATA_OVER
 * was still in flight, saw FIFO_EMPTY+DATA_OVER immediately, drained 0 words
 * and failed too -- one spurious error sustaining itself. Observed as
 * cascading pairs from the CPU1 debug path: two failures back to back, then
 * two more on the next two LBAs, with the documented signature (RINT=0x208,
 * DATA_OVER set, CMD_DONE clear, nwords=0).
 *
 * The same stale-retire window also explains a read that "succeeds" with the
 * PREVIOUS block's bytes: the earlier transfer keeps pushing into the FIFO
 * after this call's reset, so 128 words drain cleanly but belong to the wrong
 * sector. That is a silent wrong-data return, which is worse than the -1.
 *
 * Deliberately does NOT wait for DATA_OVER: on a genuinely stuck card that
 * bit may never arrive and poll_rint() would burn the full EMMC_POLL_CAP. Card
 * idle plus a FIFO reset is enough to stop the leak into the next call. */
/* REWRITTEN 2026-07-30 to match the reference driver instead of a guess.
 *
 * The first version of this function did: wait_card_idle(), set GCTL_FIFO_RST,
 * small_delay(). That was reasoned from first principles and it was wrong in
 * three ways. FreeBSD's own aw_mmc driver -- which is not just "a" reference but
 * the very driver the GUEST runs against this same controller -- does the
 * following on EVERY command error (aw_mmc_req_done(), sys/arm/allwinner/
 * aw_mmc.c, under `if (cmd->error != MMC_ERR_NONE)`):
 *
 *   1. set GCTL_FIFO_RST | GCTL_DMA_RST     <- BOTH, not just FIFO
 *   2. poll until GCTL's reset bits self-clear (up to 1000 x DELAY(100)),
 *      and report "timeout resetting DMA/FIFO" if they never do
 *   3. aw_mmc_update_clock(sc, 1)           <- re-program the internal clock
 *
 * Against that, the old version:
 *   - omitted DMA_RST. We drive this controller in PIO, so this is the least
 *     important of the three, but the reference sets it unconditionally on
 *     error and there is no reason to differ.
 *   - never WAITED for the reset pulse to finish. A fixed small_delay() is not
 *     the same as polling the self-clearing bits: if the reset had not completed
 *     when we returned, the next transfer was issued into a controller that was
 *     still resetting -- which is the exact failure mode this function exists to
 *     prevent. It could therefore fail to fix, or even cause, the thing it was
 *     added for.
 *   - never re-programmed the clock. This is the one that would not have been
 *     guessed: on this DesignWare-derived IP the clock domain has to be
 *     re-programmed after a reset via a CMDR LOAD|PRG_CLK|WAIT_PRE_OVER
 *     command, and the reference does it as an integral part of error recovery,
 *     not as a clock-change-only operation. clk_update() already implements
 *     exactly that handshake here (it was written for emmc_reclock()); this
 *     path simply never called it.
 *
 * Deliberately still does NOT wait for DATA_OVER: on a genuinely stuck card
 * that bit may never arrive and poll_rint() would burn the full EMMC_POLL_CAP.
 *
 * Counted in [8]/[9] so the effect is measurable rather than argued: [8] is how
 * many times recovery ran, [9] how many times its clock re-program timed out
 * (the reference's "timeout updating clock", which is a real reported failure
 * on Allwinner parts -- see the sunxi "fatal err update clk timeout" reports).
 * A [9] that tracks [8] means recovery itself is failing and the card needs a
 * heavier reset, not a retry. */
/* Reset the FIFO and WAIT for the self-clearing pulse to actually finish.
 *
 * This is the fix for a defect ebio_fail_settle() below had already diagnosed in
 * its own comment and fixed only for itself: "never WAITED for the reset pulse to
 * finish. A fixed small_delay() is not the same as polling the self-clearing
 * bits: if the reset had not completed when we returned, the next transfer was
 * issued into a controller that was still resetting -- which is the exact
 * failure mode this function exists to prevent." Both hot paths --
 * emmc_bio_read() and emmc_bio_write() -- went on doing precisely that, with
 * small_delay() being 64 register reads, a few microseconds fixed.
 *
 * The lesson was applied in one function and not in the two that run on every
 * single sector. Now it is shared, so there is one implementation to be right.
 *
 * Bounded, like the sibling: a controller that will not clear these bits is not
 * going to be rescued by waiting longer, and a caller that cannot proceed is
 * better off failing than spinning. Instrumented ([21]/[22]) because the whole
 * point is to find out whether the fixed delay was ever actually too short.
 *
 * NOT used by emmc_bio_init(), which writes GCTL_RESET_ALL wholesale as its
 * bring-up reset rather than OR-ing bits into the live register. That path is
 * hardware-validated and semantically different; left alone deliberately. */
/* Wait for the card to stop programming, bounded by TIME rather than by a
 * register-read count. See g_settle_busy_ms for why the iteration-bounded
 * wait_card_idle() is not good enough on the retry path.
 *
 * Uses the same two-read discipline as the timeout loops in emmc_bio_write():
 * a single CNTPCT read must never decide a bounded wait on this board, because
 * consecutive reads have been measured disagreeing by ~4 s. Writing this loop
 * the naive way would re-introduce, in the fix, the exact bug the fix exists to
 * prevent. Returns 0 once the card is idle, -1 if it never was. */
#define EMMC_SETTLE_BUSY_TIMEOUT_MS  500u

static int wait_card_idle_timed(uint32_t ms)
{
	uint64_t start, cap;
	uint32_t over = 0;

	/* Fast path costs exactly one register read and NO clock reads. This runs on
	 * the entry of every read, where the controller is idle in the normal case;
	 * the original wait_card_idle() was cheap for the same reason and that
	 * property is worth keeping. */
	if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
		return 0;

	start = rd_cntpct();
	cap = ms_to_ticks(ms);

	for (;;) {
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY)) {
			uint64_t now = rd_cntpct();
			uint64_t el  = (now >= start) ? (now - start) : 0ull;
			uint64_t f   = rd_cntfrq();
			uint32_t el_ms = f ? (uint32_t)((el * 1000ull) / f) : 0u;
			if (el_ms > g_settle_busy_ms) {
				g_settle_busy_ms = el_ms;
				ebio_bc(25, el_ms);
			}
			return 0;
		}
		{
			uint64_t now = rd_cntpct();
			uint64_t el  = (now >= start) ? (now - start) : 0ull;
			if (el > cap) {
				if (++over >= 2)
					break;
			} else if (over) {
				ebio_bc(18, ++g_cnt_anom);
				over = 0;
			}
		}
	}
	ebio_bc(26, ++g_settle_busy_timeouts);
	return -1;
}

static int gctl_reset_and_wait(uint32_t bits)
{
	uint32_t i;

	wreg(REG_GCTL, rreg(REG_GCTL) | bits);
	for (i = 0; i < EMMC_POLL_CAP; i++) {
		if ((rreg(REG_GCTL) & GCTL_RESET_ALL) == 0) {
			if (i > g_gctl_rst_spins) {
				g_gctl_rst_spins = i;
				ebio_bc(21, i);
			}
			return 0;
		}
	}
	ebio_bc(22, ++g_gctl_rst_timeouts);
	return -1;
}

/* stop_card: issue CMD12 to make the CARD abandon an open data transfer.
 *
 * Must be 0 wherever the data phase already COMPLETED. Sent unconditionally at
 * first, and the measurement said no: CMD12 failed 106 times out of 547 (19%),
 * ebio_fails went from 0 to 354, and one write finally reached the guest as
 * S_IOERR -- because STOP_TRANSMISSION with no transfer open is an illegal
 * command, and this function is called from every failure path, most of which
 * are past DATA_OVER. Targeting it is the difference between aborting a real
 * transfer and generating fresh errors. */
static void ebio_fail_settle_full(int stop_card)
{
	uint32_t i;

	/* (0) STOP THE CARD FIRST, while the data transfer is still live on the bus.
	 *
	 * This used to sit after the FIFO/DMA reset, and there it failed 141 times
	 * out of 268 (53%). The order was the problem: a FIFO+DMA reset takes the
	 * controller's command engine through a reset pulse, after which the
	 * transfer is over as far as the HOST is concerned -- but CMD12 has to reach
	 * the CARD, which is still holding an open write. Issuing it first, before
	 * anything is reset and before waiting on a card that is only "busy" because
	 * it is waiting for the rest of the block, is what the sequence actually
	 * needs.
	 *
	 * Also deliberately BEFORE the idle wait: waiting for CARD_BUSY to clear on
	 * a card mid-receive either spins out the whole bound or returns with the
	 * transfer still open. There is nothing to wait for until the abort is sent. */
	if (stop_card) {
		ebio_bc(29, ++g_stop_cmds);
		/* CMDR_STOP_ABORT is the bit that makes this an ABORT rather than
		 * an ordinary command, and it was missing. Without bit 14 the
		 * controller's command engine queues CMD12 behind the transfer it
		 * is supposed to be aborting -- which is a coherent explanation
		 * for the measured 141-of-268 (53%) failure rate, and for the two
		 * damaged sectors whose signature was "the abort did not take".
		 *
		 * PROVENANCE, because this file's header insists on it: Linux
		 * sunxi-mmc.c's sunxi_mmc_send_manual_stop() (6.12, lines
		 * 451-452) composes exactly
		 *     SDXC_START | SDXC_RESP_EXPIRE | SDXC_STOP_ABORT_CMD |
		 *     SDXC_CHECK_RESPONSE_CRC | MMC_STOP_TRANSMISSION
		 * and this driver was sending RESP_EXPIRE alone. CHK_CRC is added
		 * for the same reason: CMD12 answers R1b, a CRC-protected
		 * response, and every other R1 command in this file already asks
		 * the controller to check it. */
		if (emmc_cmd_done(CMD12_STOP_TRANSMISSION, 0,
		                  CMDR_RESP_EXP | CMDR_CHK_CRC |
		                  CMDR_STOP_ABORT, NULL) != 0)
			ebio_bc(30, ++g_stop_fails);
	}

	/* TIME-bounded, not iteration-bounded -- see g_settle_busy_ms. The caller's
	 * next act after a settle is typically a RETRY, and a retry that lands on a
	 * still-programming card is the corruption mechanism this file's history is
	 * about. wait_card_idle()'s EMMC_POLL_CAP register reads are not a duration.
	 *
	 * g_fi_legacy_wait selects the OLD behaviour on purpose, so a fault-injection
	 * run can show the difference instead of asserting it. Off unless armed. */
	if (g_fi_legacy_wait)
		wait_card_idle();
	else
		wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	/* (1) FIFO + DMA reset. */
	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST | GCTL_DMA_RST);

	/* (2) Wait for the self-clearing reset pulse to actually finish. The
	 * reference polls all three reset bits (GCTL_RESET) even though it sets
	 * only two, so do the same. Bounded: a controller that never clears these
	 * is not going to be rescued by waiting longer. */
	for (i = 0; i < EMMC_POLL_CAP; i++)
		if ((rreg(REG_GCTL) & GCTL_RESET_ALL) == 0)
			break;

	/* (2b) The abort at step (0) can leave the card busy retiring it, and the
	 * reset above can leave the controller settling. Same time bound as
	 * everywhere else here -- an iteration count is not a duration. */
	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	/* (3) Re-program the internal clock after the reset -- REMOVED, it broke
	 * the guest (2026-07-30, same day it was added).
	 *
	 * Deploying the clk_update() call here regressed guest boot immediately and
	 * reproducibly: 5 of 6 soak cycles, the guest stopped at g_writes=0 and
	 * g_reads=715 (against 152/1178 on the build before it), never reaching
	 * fsck or /etc/rc, and never becoming ssh-reachable. triage.py: CPU0 frozen
	 * with CPU1/CPU2 healthy and the eMMC lock free -- the guest wedged, the HV
	 * was fine. Note how that regression MASQUERADED as a fix: write retries
	 * fell from ~50-170 per boot to 0, which looked like the retry cascade had
	 * been cured, when in fact there were no writes left to retry.
	 *
	 * The reference driver genuinely does re-program the clock here
	 * (aw_mmc_req_done -> aw_mmc_update_clock), and that part of the reading was
	 * correct. What was wrong was importing it into THIS driver: clk_update()
	 * issues a command on CMDR, and this file's own header warns, in as many
	 * words, not to re-derive the sequence from a generic sunxi-mmc reading
	 * because subtle reordering has bitten this project before. aw_mmc does it
	 * inside a full clock state machine, including the clock-off/on framing that
	 * emmc_reclock() reproduces and that this path does not have -- and per the
	 * header, NTSR/SAMP_DL only latch while the card clock is DISABLED. A bare
	 * clock-program command with the clock running is exactly the class of thing
	 * that warning is about.
	 *
	 * Kept, because neither introduces a new command and both were genuinely
	 * missing: the DMA_RST bit, and actually WAITING for the reset pulse to
	 * clear instead of a fixed small_delay(). If the clock really must be
	 * re-programmed after a reset, it needs the full clock-off/update/on framing
	 * around it, not this one call, and it needs its own soak. */
	ebio_bc(8, ++g_settles);
}

/* Default: no CMD12. Every caller that can leave the CARD mid-transfer calls
 * ebio_fail_settle_full(1) explicitly, so the dangerous case is the one that
 * must be spelled out rather than the safe one. */
static void ebio_fail_settle(void) { ebio_fail_settle_full(0); }

/* Shared by emmc_bio_read() (CMD17, cagr=lba) and emmc_bio_read_ext_csd()
 * (CMD8, cagr=0) -- structurally the exact same single-block PIO drain
 * (same FIFO/stall/DATA_OVER handling that took real hardware debugging to
 * get right, see the comments below), differing only in which command is
 * loaded and whether the argument is a sector address. Factored rather
 * than copied (unlike emmc_bio_write()'s deliberate mirror-copy of this
 * function, per its own header comment) because this is not a structural
 * mirror with real differences to keep readable side by side -- it is the
 * identical operation, and a hand-copy would silently drift from any
 * future fix made here. `lba` in breadcrumbs/comments below is really
 * "the CAGR argument used", which is 0 (meaningless) for EXT_CSD. */
static int emmc_bio_read_block(uint32_t cmdr, uint32_t lba, uint64_t buf_pa)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	unsigned nwords = 0;
	int i;

	/* Do not start on top of a transfer that is still retiring -- see
	 * ebio_fail_settle(). Still costs nothing when the controller is already
	 * idle (wait_card_idle_timed()'s fast path reads one register and no clock),
	 * but when it is NOT idle the bound is now a duration rather than a count of
	 * register reads, which is what "still retiring" actually needs. */
	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	/* FIFO reset, WAITING for the self-clearing pulse -- not a fixed delay.
	 * See gctl_reset_and_wait(): this used small_delay() (64 register reads),
	 * which is the very thing ebio_fail_settle()'s comment documents as wrong. */
	gctl_reset_and_wait(GCTL_FIFO_RST);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, 512);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, cmdr);

	/* ROOT-CAUSE FIX (found live 2026-08-20, from the diagnostics this very
	 * loop records). The bound used to be a single iteration count shared
	 * between two different waits: waiting for the FIFO to refill, and
	 * draining it. A card that pauses mid-block -- eMMC parts do internal
	 * housekeeping whenever they like, for tens of milliseconds -- spends the
	 * whole budget waiting, and the read then fails a few words short WITH NO
	 * ERROR BIT SET ANYWHERE.
	 *
	 * That is exactly what was captured: nwords=127 of 128, RINT=0xC
	 * (CMD_DONE|DATA_OVER, not one error bit), STAR bit2 clear -- i.e. the
	 * missing word had ALREADY ARRIVED by the time the failure was recorded.
	 * The guest saw `vtbd0: hard error cmd=read`, g_vfs_done error=5, and a
	 * module failed to load off a disk that was working perfectly.
	 *
	 * The bound is now a STALL deadline: patience resets every time a word is
	 * drained, so a slow card is waited out while a genuinely dead transfer
	 * still ends in bounded time. Elapsed time uses the same backwards-safe
	 * form and two-read confirmation as the write path's loops (rd_cntpct()
	 * has been observed going backwards on this part -- see slot [18]).
	 * EMMC_POLL_CAP survives only as a backstop for a broken counter, which is
	 * why it is multiplied out: it must not be the binding constraint again. */
	{
		uint64_t patience = ms_to_ticks(EMMC_READ_STALL_TIMEOUT_MS);
		uint64_t last = rd_cntpct();
		unsigned over = 0;
		unsigned over_done = 0;   /* DATA_OVER seen with a short count */
		uint64_t guard = (uint64_t)EMMC_POLL_CAP * 64ull;

		for (i = 0; (uint64_t)i < guard && nwords < 128; i++) {
			uint32_t st = rreg(REG_STAR);
			uint32_t ri;

			if (!(st & STAR_FIFO_EMPTY)) {
				buf[nwords++] = rreg(REG_FIFO);
				last = rd_cntpct();   /* progress: reset the patience */
				over = 0;
				continue;
			}

			ri = rreg(REG_RINT);
			/* A real error ends the wait immediately -- no point spending the
			 * stall budget on a transfer the controller has already failed. */
			if (ri & RINT_READ_ERR_MASK)
				break;
			if (ri & RINT_DATA_OVER) {
				/* DATA_OVER means the CARD has no more data to send. It
				 * does NOT mean the FIFO has already made the last word
				 * visible: the controller latches DATA_OVER while the
				 * final word is still being pushed in, so a read of
				 * STAR taken in that window shows FIFO_EMPTY with a word
				 * about to appear.
				 *
				 * Breaking out here on a SHORT count is what actually
				 * produced `vtbd0: hard error cmd=read`. The captured
				 * evidence is unambiguous: nwords=127, RINT=0xC (no
				 * error bit anywhere), and STAR at the failure showing
				 * FIFO_LEVEL=1 -- the 128th word was sitting in the FIFO
				 * we had just declared empty. Widening the earlier
				 * iteration bound did not help, and could not: the loop
				 * was not running out of time, it was concluding early.
				 *
				 * So DATA_OVER ends the wait only once the block is
				 * complete. Short of that, keep draining under a tight
				 * post-DATA_OVER deadline -- the remaining words are
				 * already in flight, so they arrive in microseconds or
				 * they are never coming. */
				if (nwords >= 128)
					break;
				if (!over_done) {
					over_done = 1;
					last = rd_cntpct();
					patience = ms_to_ticks(
					    EMMC_READ_DRAIN_TAIL_MS);
				}
			}

			{
				uint64_t now = rd_cntpct();
				uint64_t el  = (now >= last) ? (now - last) : 0ull;

				if (el > patience) {
					if (++over < 2)
						continue;      /* re-read before believing it */
					break;
				}
				if (over) {
					ebio_bc(18, ++g_cnt_anom);
					over = 0;
				}
			}
		}
	}

	if (nwords < 128) {
		uint32_t ri = rreg(REG_RINT);

		ebio_bc(1, lba);
		ebio_bc(2, ri);
		ebio_bc(3, rreg(REG_STAR));
		/* Tag 0x50000 means "short block with NOT ONE error bit set" -- the
		 * signature of the 2026-08-20 bug fixed above (the bound expired while
		 * the transfer was merely slow). Distinguishing it matters: a short
		 * block WITH an error bit is the card or the bus, and a short block
		 * WITHOUT one is us giving up too early. If this tag ever appears
		 * again, EMMC_READ_STALL_TIMEOUT_MS is too small -- do not go looking
		 * at the hardware. */
		ebio_bc(4, ((ri & RINT_READ_ERR_MASK) ? 0u : 0x50000u) | nwords);
		ebio_bc(5, rreg(REG_GCTL));
		ebio_bc(0, ++g_ebio_fails);
		ebio_fail_settle();
		return -1; /* short block: see the tag in slot [4] for which kind */
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
		ebio_fail_settle();
		return -1;
	}
	for (i = 0; i < EMMC_POLL_CAP; i++) {
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
			break;
	}

	/* THE READ-SIDE ERROR CHECK. Until now this function returned 0 the moment
	 * 128 words had drained and DATA_OVER had latched, without ever looking at a
	 * single error bit -- so a block the controller had flagged as bad was handed
	 * to the guest as good data, with no counter anywhere able to see it. The
	 * write path has checked (a narrow mask of) error bits all along; the read
	 * path checked none. That asymmetry was the defect.
	 *
	 * Settle before returning, for the same reason every other failure exit here
	 * does: bailing without settling leaves the data phase retiring and poisons
	 * whatever call comes next. Reads are NOT retried below us -- vblk_emmc.c has
	 * VBLK_WRITE_RETRIES and no read equivalent -- so this becomes S_IOERR to the
	 * guest and FreeBSD retries it. That is the correct trade: a reported error
	 * the guest can retry beats silently correct-looking wrong bytes. */
	{
		uint32_t ri = rreg(REG_RINT);

		if ((ri | g_rint_or_read) != g_rint_or_read) {
			g_rint_or_read |= ri;
			ebio_bc(13, g_rint_or_read);
		}

		if (ri & RINT_READ_ERR_MASK) {
			g_read_err_bits |= (ri & RINT_READ_ERR_MASK);
			ebio_bc(19, g_read_err_bits);
			ebio_bc(20, ++g_read_errs);
			ebio_bc(1, lba);
			ebio_bc(2, ri);
			ebio_bc(3, rreg(REG_STAR));
			ebio_bc(4, 0x40000u | (ri & 0x3fffu)); /* tag: read error bits */
			ebio_bc(5, rreg(REG_GCTL));
			ebio_bc(0, ++g_ebio_fails);
			ebio_fail_settle();
			return -1;
		}
	}

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

int emmc_bio_read(uint32_t lba, uint64_t buf_pa)
{
	return emmc_bio_read_block(CMD17_READ_CMDR, lba, buf_pa);
}

int emmc_bio_read_ext_csd(uint64_t buf_pa)
{
	return emmc_bio_read_block(CMD8_SEND_EXT_CSD_CMDR, 0, buf_pa);
}

/* EXT_CSD[267]/[268]/[269] -- PRE_EOL_INFO / DEVICE_LIFE_TIME_EST_TYP_A/B.
 * Bounded and self-zeroing on any failure, matching axp803_read_health()'s
 * contract (bmc.c's bmc_health_snapshot() relies on exactly this shape). */
void emmc_bio_read_wear(struct emmc_wear *out)
{
	const volatile uint8_t *ecsd =
	    (const volatile uint8_t *)(unsigned long)HVMAP_EMMC_EXTCSD_BUF;

	out->pre_eol_info = 0;
	out->life_est_a = 0;
	out->life_est_b = 0;
	out->ok = 0;

	if (emmc_bio_read_ext_csd(HVMAP_EMMC_EXTCSD_BUF) != 0)
		return;

	out->pre_eol_info = ecsd[267];
	out->life_est_a   = ecsd[268];
	out->life_est_b   = ecsd[269];
	out->ok = 1;
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
	/* Decide ONCE per write whether this call is the one to fault, then let the
	 * selected point consume the decision.
	 *
	 * This used to be decided at each point directly, and point 1 sits INSIDE
	 * the data-phase polling loop -- so `every` counted POLLS, not writes, and a
	 * run armed for "every 500th write" injected 62839 times across ~133632
	 * sectors, roughly every second one. The measurement was still useful but it
	 * was not the experiment that had been armed, and a knob that silently means
	 * something different depending on which point is selected is a trap. */
	uint32_t fi_now = 0;

	if (g_fi_every && lba >= g_fi_min_lba && ++g_fi_seq >= g_fi_every) {
		g_fi_seq = 0;
		fi_now = 1u;
	}

	__asm__ volatile("dsb sy" ::: "memory");

	/* FIFO reset, WAITING for the self-clearing pulse -- not a fixed delay.
	 * See gctl_reset_and_wait(): this used small_delay() (64 register reads),
	 * which is the very thing ebio_fail_settle()'s comment documents as wrong. */
	gctl_reset_and_wait(GCTL_FIFO_RST);

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
	if (nwords < 128) {
		/* Same reason as the read side: bailing mid-transfer leaves the data
		 * phase retiring and poisons whatever call comes next, read or write.
		 * See ebio_fail_settle_full(). CMD12 here: the push never finished, so
		 * the card is holding a partial block that the retry would otherwise
		 * append to. */
		ebio_fail_settle_full(1);
		return -1; /* FIFO never drained enough to accept all words */
	}

	/* Wait for the controller to report the data phase complete. TIME-capped
	 * (D4 fix — see the block comment above EMMC_WRITE_DATA_TIMEOUT_MS): the
	 * init clock is 400 kHz, so pushing 512 B out to the card takes ~10 ms,
	 * far longer than EMMC_POLL_CAP (~0.4 ms) — a normal poll_rint would time
	 * out before DATA_OVER. Also bail on any data error bit. */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(EMMC_WRITE_DATA_TIMEOUT_MS);
		uint32_t over = 0;       /* consecutive over-cap reads; see g_cnt_anom */
		uint32_t ri;
		for (;;) {
			ri = rreg(REG_RINT);

			/* FAULT INJECTION point 1 -- see g_fi_point. Fires on the FIRST
			 * poll, before DATA_OVER, which is exactly what the CNTPCT
			 * underflow did: it made the timeout comparison true immediately.
			 * The 128 words are in the FIFO but not yet all delivered to the
			 * card, and ebio_fail_settle()'s reset discards the remainder. */
			if (fi_now && g_fi_point == 1u) {
				fi_now = 0;
				ebio_bc(27, ++g_fi_injected);
				ebio_fail_settle_full(1);   /* mimic the real data-phase path */
				/* Same encoding the real data-phase timeout returns. */
				return (int)(0x20000000u | (ri & 0x3fffu));
			}

			if (ri & RINT_DATA_OVER) {
				/* THE THIRD HOLE, now closed. The `ri & 0x0180` check further
				 * down is UNREACHABLE for any error bit that latches no later
				 * than DATA_OVER, because this break wins the race -- so a write
				 * the controller had flagged completed as a success. Same defect
				 * as the read path's missing check, in a different shape.
				 *
				 * Why it is safe to make fatal NOW and was not before. Failing a
				 * write here makes vblk_emmc.c retry it, and a retry landing on a
				 * still-programming card is exactly the corruption mechanism this
				 * file's history is about -- so the precondition was making the
				 * retry path safe first: ebio_fail_settle() now waits for
				 * card-idle on a TIME bound (see g_settle_busy_ms), where it
				 * previously used an iteration count that is not a duration.
				 *
				 * And it is measured to be a no-op on healthy hardware: [23],
				 * which accumulated exactly this mask at exactly this point,
				 * read 0x0000 across a boot plus a 68 MB round trip. So this can
				 * only start reporting a condition that was previously completed
				 * as a success -- it cannot break a path that works today. [23]
				 * keeps accumulating, and [24] counts the failures, so if this
				 * ever fires the offending bit is already recorded. */
				uint32_t e = ri & (RINT_READ_ERR_MASK | RINT_RESP_TIMEOUT);
				if (e) {
					if ((e | g_write_dataover_errs) != g_write_dataover_errs) {
						g_write_dataover_errs |= e;
						ebio_bc(23, g_write_dataover_errs);
					}
					ebio_bc(24, ++g_write_dataover_fails);
					ebio_bc(1, lba);
					ebio_bc(2, ri);
					ebio_bc(3, rreg(REG_STAR));
					ebio_bc(4, 0x50000u | (ri & 0x3fffu)); /* tag: err@DATA_OVER */
					ebio_bc(5, rreg(REG_GCTL));
					ebio_bc(0, ++g_ebio_fails);
					ebio_fail_settle();
					/* Same encoding the pre-DATA_OVER error path returns, so
					 * VBLK_RC_WR_RETRYABLE() treats it identically and the
					 * sector is retried rather than handed to the guest. */
					return (int)(0x40000000u | (ri & 0x3fffu));
				}

				/* FAULT INJECTION -- see the g_fi_* block. Off unless armed.
				 *
				 * THIS is the dangerous moment, which is why the injection sits
				 * exactly here: the data phase is done and DATA_OVER has latched,
				 * so the card has begun PROGRAMMING. Bailing out now with a
				 * retryable code reproduces, on demand, the precondition of the
				 * bug this file's history is about -- the old spurious busy
				 * timeout returned -2 from a handful of instructions further down,
				 * with the card in exactly this state.
				 *
				 * The LBA floor is an interlock, not a nicety: this can corrupt
				 * real data, so it must be impossible to reach the guest's root
				 * filesystem with it. */
				if (fi_now && g_fi_point == 0u) {
					fi_now = 0;
					ebio_bc(27, ++g_fi_injected);
					ebio_fail_settle();
					return -2;   /* the code the old busy-timeout returned */
				}
				break;
			}
			/* Record the controller state, like the READ path already does.
			 * Without this a write failure left only its encoded rc and
			 * nothing about the controller, which is why 0x40000104 could be
			 * chased for so long (see 2026-07-30): RINT alone does not say
			 * whether the FIFO had drained, what STAR thought, or whether the
			 * card was still busy. Same slots the read path uses, plus a tag in
			 * [4] so a reader can tell a write record from a read one:
			 * 0x20000 = data-phase error bit, 0x30000 = data-phase timeout.
			 *
			 * The breadcrumb those slots produced immediately paid for itself and
			 * explains the ebio_fail_settle() calls below (added 2026-07-30, same
			 * day, one soak run apart): STAR read 0x0100c301 at the moment of a
			 * real RINT=0x104 DATA_TIMEOUT — FIFO NOT empty and CARD_BUSY set, so
			 * the write had stalled mid-transfer with words still undelivered,
			 * rather than the card simply not responding. Both of these returns
			 * then jumped out leaving exactly that state behind. That is the same
			 * self-sustaining failure ebio_fail_settle()'s own comment describes
			 * for the read path: the next attempt clears RINT and writes a new
			 * CMDR on top of a still-retiring data phase and fails too. It also
			 * explains why raising VBLK_WRITE_RETRIES from 3 to 8 earlier the same
			 * day did not help at all — every retry inherited the dirty controller
			 * its predecessor left, so a bigger budget just bought more attempts
			 * at the same poisoned state.
			 *
			 * NOTE the FIFO reset discards the undelivered words. That is correct
			 * here and only here: these are single-block CMD24 transfers, so the
			 * caller (vblk_emmc.c serve_data) re-pushes the whole 512 B sector
			 * from the bounce buffer on retry — nothing is half-written from the
			 * host's side. This does NOT abort the data phase on the CARD's side;
			 * if ebio fails keeps climbing after this, an explicit CMD12
			 * STOP_TRANSMISSION (or a controller soft reset) is the next step, and
			 * the fails counter staying flat is what says it isn't needed. */
			if (ri & 0x0180u) { /* DATA_CRC(bit7)/DATA_TIMEOUT(bit8) */
				ebio_bc(1, lba);
				ebio_bc(2, ri);
				ebio_bc(3, rreg(REG_STAR));
				ebio_bc(4, 0x20000u | (ri & 0x3fffu));
				ebio_bc(5, rreg(REG_GCTL));
				ebio_bc(0, ++g_ebio_fails);
				ebio_fail_settle_full(1);   /* data phase open: stop the card */
				return (int)(0x40000000u | (ri & 0x3fffu));
			}
			/* Two-read confirmation -- see g_cnt_anom. A single CNTPCT read
			 * is not trusted to end a bounded wait, because consecutive reads
			 * have been measured disagreeing by ~4 s. */
			{
				uint64_t now = rd_cntpct();
				uint64_t el  = (now >= start) ? (now - start) : 0ull;
				if (el > cap) {
					if (++over < 2)
						continue;      /* re-read before believing it */
				} else {
					if (over) {
						ebio_bc(18, ++g_cnt_anom);
						over = 0;
					}
					continue;
				}
			}
			{
				uint32_t r2 = rreg(REG_RINT);
				ebio_bc(1, lba);
				ebio_bc(2, r2);
				ebio_bc(3, rreg(REG_STAR));
				ebio_bc(4, 0x30000u | (r2 & 0x3fffu));
				ebio_bc(5, rreg(REG_GCTL));
				ebio_bc(0, ++g_ebio_fails);
				ebio_fail_settle_full(1);   /* data phase open: stop the card */
				return (int)(0x20000000u | (r2 & 0x3fffu));
			}
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
		uint32_t over = 0;       /* consecutive over-cap reads; see g_cnt_anom */
		for (;;) {
			if ((rreg(REG_STAR) & STAR_CARD_BUSY) == 0)
				break;
			/* Two-read confirmation -- see g_cnt_anom. THIS is the loop where
			 * the disagreement was measured (14710 spurious timeouts, each
			 * recording under 1 ms elapsed against a correct 4 s cap). */
			{
				uint64_t now = rd_cntpct();
				uint64_t el2 = (now >= start) ? (now - start) : 0ull;
				if (el2 > cap) {
					if (++over < 2)
						continue;
				} else {
					if (over) {
						ebio_bc(18, ++g_cnt_anom);
						over = 0;
					}
					continue;
				}
			}
			{
				/* Settle for the same reason as the data-phase returns above:
				 * this bails with CARD_BUSY still set by definition, which is
				 * precisely the state that poisons the next call. -2 is the code
				 * vblk_emmc.c's write-retry loop has always retried, so before
				 * this it was the one retryable write error GUARANTEED to hand
				 * its successor a busy card. wait_card_idle() is iteration-capped
				 * (~0.4 ms), so re-waiting on a card we just timed out on costs a
				 * bounded amount and the FIFO reset happens either way. */
				/* MEASURE this path (2026-07-30). It is by far the most
				 * frequent retry trigger and the only one that writes no
				 * breadcrumb, which is why it stayed invisible: soak10 showed
				 * ~50-170 write retries per boot with ebio_fails at 0, so
				 * essentially all of them came from HERE, not from the
				 * DATA_TIMEOUT this whole investigation was aimed at.
				 *
				 * And the arithmetic does not close: at the nominal 4000 ms cap,
				 * 50 of these would be 200 s, inside a boot that takes ~80 s.
				 * So either they are not really waiting 4000 ms, or they are not
				 * really this path. rd_cntpct() reads CNTPCT_EL0 (physical, so
				 * CNTVOFF_EL2 cannot skew it) and ms_to_ticks() derives from
				 * CNTFRQ_EL0, so both look right by inspection -- which is
				 * exactly why this needs measuring rather than more reasoning.
				 *
				 * [10] counts them, [11] records how long the last one actually
				 * waited in ms, [12] the CNTFRQ it was computed from. If [11] is
				 * ~4000 the card really does stall that long and the retry count
				 * has to be explained some other way; if it is tiny, the cap is
				 * expiring early and the timeout math is the bug. */
				{
					uint64_t el = rd_cntpct() - start;
					uint64_t f = rd_cntfrq();
					ebio_bc(10, ++g_busy_timeouts);
					ebio_bc(11, f ? (uint32_t)((el * 1000ull) / f) : 0xffffffffu);
					ebio_bc(12, (uint32_t)f);
					/* See g_busy_cap_lo: [11] above contradicts the branch
					 * condition that got us here, so record the raw ingredients
					 * too. el is split so an unsigned underflow (high word all
					 * ones) cannot hide inside a truncating cast. */
					g_busy_cap_lo = (uint32_t)cap;
					g_busy_el_lo  = (uint32_t)el;
					g_busy_el_hi  = (uint32_t)(el >> 32);
					ebio_bc(15, g_busy_cap_lo);
					ebio_bc(16, g_busy_el_lo);
					ebio_bc(17, g_busy_el_hi);
				}
				ebio_fail_settle();
				return -2;   /* card never signaled program-done: do NOT claim success */
			}
		}
	}

	/* Observation only -- see g_rint_or_write. Counterpart to the read side, and
	 * the reason both are needed: the write path DOES bail on `ri & 0x0180`, so
	 * comparing the two accumulators says whether the read path's missing check
	 * would ever have fired. If [13] shows bits that 0x0180 would have caught
	 * and [14] does not, the asymmetry is real and reads have been passing
	 * flagged data through as success. */
	{
		uint32_t ri = rreg(REG_RINT);
		if ((ri | g_rint_or_write) != g_rint_or_write) {
			g_rint_or_write |= ri;
			ebio_bc(14, g_rint_or_write);
		}
	}

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* ------------------------------------------------------------------ */
/* emmc_bio_read_multi() / emmc_bio_write_multi() — CMD18/CMD25, the       */
/* controller's own AUTO_STOP raising CMD12 for us. One transaction for a  */
/* whole run of sectors instead of one emmc_bio_read()/write() call each.  */
/*                                                                          */
/* SAME sunxi SMHC IP as sd_bio.c's own read_multi/write_multi (identical   */
/* CMD17/CMD24 CMDR encodings in both files -- CMD17_READ_CMDR/             */
/* CMD24_WRITE_CMDR match byte for byte -- confirms the command layout      */
/* transfers directly), but built on THIS file's OWN hardened idioms, not   */
/* sd_bio.c's simpler ones: a stall-based, two-read-CNTPCT-confirmed        */
/* timeout that resets on every word of progress (see emmc_bio_read()'s own */
/* "ROOT-CAUSE FIX" comment -- an iteration cap or a fixed total-transfer    */
/* deadline both under- and over-fire on a card that pauses mid-block for    */
/* internal housekeeping), and ebio_fail_settle_full() for every abort       */
/* path -- targeted CMD12 only while the data phase is genuinely still open, */
/* never after AUTO_STOP has already retired it (see that function's own     */
/* header: an untargeted STOP_TRANSMISSION measured 19-53% failure rates     */
/* and corrupted sectors on the single-block path this is built next to).    */
/*                                                                          */
/* NOT hardware-verified yet. Validate exactly like sd_bio.c's series was:  */
/* dbgmon `call` first (see emmc_bio.h's header on why these are call-able), */
/* then fsck -n + parallel md5 reads across a remount, before this is wired */
/* into any guest-facing path. See HANDOFF.md. */
/* ------------------------------------------------------------------ */
#define CMDR_AUTO_STOP        0x00001000u   /* bit12, CMDR register */
#define RINT_AUTO_STOP_DONE   0x00004000u   /* bit14, RINT register */
#define CMD18_READ_CMDR       (0x80002352u | CMDR_AUTO_STOP)   /* CMD18 */
#define CMD25_WRITE_CMDR      (0x80002759u | CMDR_AUTO_STOP)   /* CMD25 */

/* Per-word stall budget for the multi push/drain loops -- same values and
 * same rationale as the single-block read path's EMMC_READ_STALL_TIMEOUT_MS/
 * EMMC_READ_DRAIN_TAIL_MS (resets on progress, so total wall time scales
 * naturally with nblk without needing a separate "whole transfer" cap). */
#define EMMC_MULTI_STALL_TIMEOUT_MS  EMMC_READ_STALL_TIMEOUT_MS
#define EMMC_MULTI_TAIL_TIMEOUT_MS   EMMC_READ_DRAIN_TAIL_MS

int emmc_bio_read_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	uint32_t nwords = 0, total, ri = 0;

	if (nblk < 2 || nblk > EMMC_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 128u;

	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);
	gctl_reset_and_wait(GCTL_FIFO_RST);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, nblk * 512u);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD18_READ_CMDR);

	/* Drain, stall-based -- direct generalisation of emmc_bio_read()'s own
	 * loop (see its comment for why DATA_OVER needs a short tail wait rather
	 * than ending the drain the instant it latches: the final word can still
	 * be in flight when DATA_OVER is observed). */
	{
		uint64_t patience = ms_to_ticks(EMMC_MULTI_STALL_TIMEOUT_MS);
		uint64_t last = rd_cntpct();
		unsigned over = 0, over_done = 0;
		uint64_t guard = (uint64_t)EMMC_POLL_CAP * 64ull * (uint64_t)nblk;
		uint64_t i;

		for (i = 0; i < guard && nwords < total; i++) {
			uint32_t st = rreg(REG_STAR);

			if (!(st & STAR_FIFO_EMPTY)) {
				buf[nwords++] = rreg(REG_FIFO);
				last = rd_cntpct();
				over = 0;
				continue;
			}
			ri = rreg(REG_RINT);
			if (ri & RINT_READ_ERR_MASK)
				goto fail_open;
			if (ri & RINT_DATA_OVER) {
				if (nwords >= total)
					break;
				if (!over_done) {
					over_done = 1;
					last = rd_cntpct();
					patience = ms_to_ticks(EMMC_MULTI_TAIL_TIMEOUT_MS);
				}
			}
			{
				uint64_t now = rd_cntpct();
				uint64_t el = (now >= last) ? (now - last) : 0ull;
				if (el > patience) {
					if (++over < 2)
						continue;
					goto fail_open;
				}
				over = 0;
			}
		}
		if (nwords < total)
			goto fail_open;
	}

	/* DATA_OVER + the controller's own AUTO_STOP (CMD12) done. */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(EMMC_WRITE_DATA_TIMEOUT_MS);
		unsigned over = 0;
		for (;;) {
			ri = rreg(REG_RINT);
			if (ri & RINT_READ_ERR_MASK)
				goto fail_open;
			if ((ri & (RINT_DATA_OVER | RINT_AUTO_STOP_DONE)) ==
			    (RINT_DATA_OVER | RINT_AUTO_STOP_DONE))
				break;
			{
				uint64_t now = rd_cntpct();
				uint64_t el = (now >= start) ? (now - start) : 0ull;
				if (el > cap) {
					if (++over < 2)
						continue;
					goto fail_open;
				}
				over = 0;
			}
		}
	}

	/* AUTO_STOP's own CMD12 is R1b: card holds DAT0 low while it retires. */
	if (wait_card_idle_timed(EMMC_WRITE_BUSY_TIMEOUT_MS) != 0)
		goto fail_closed;

	ri = rreg(REG_RINT);
	if (ri & RINT_READ_ERR_MASK)
		goto fail_closed;

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail_open:
	/* Data phase may still be live on the card: targeted CMD12 needed. */
	ebio_fail_settle_full(1);
	return (int)(0x60000000u | ((nwords & 0x3fffu) << 14) | (ri & 0x3fffu));
fail_closed:
	/* AUTO_STOP already retired the transfer on the card's side; a second
	 * CMD12 here is the illegal-command case ebio_fail_settle_full()'s own
	 * header warns about. */
	ebio_fail_settle_full(0);
	return (int)(0x70000000u | (ri & 0x3fffu));
}

int emmc_bio_write_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	uint32_t nwords = 0, total, ri = 0;

	if (nblk < 2 || nblk > EMMC_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 128u;

	__asm__ volatile("dsb sy" ::: "memory");

	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);
	gctl_reset_and_wait(GCTL_FIFO_RST);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, nblk * 512u);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD25_WRITE_CMDR);

	/* Push, stall-based -- direct generalisation of emmc_bio_write()'s own
	 * FIFO-full spin, with the same 0x0180 (DATA_CRC/DATA_TIMEOUT) fatal
	 * check that single-block write already relies on. */
	{
		uint64_t patience = ms_to_ticks(EMMC_MULTI_STALL_TIMEOUT_MS);
		uint64_t last = rd_cntpct();
		unsigned over = 0;
		uint64_t guard = (uint64_t)EMMC_POLL_CAP * 64ull * (uint64_t)nblk;
		uint64_t i;

		for (i = 0; i < guard && nwords < total; i++) {
			uint32_t st = rreg(REG_STAR);

			if (!(st & STAR_FIFO_FULL)) {
				wreg(REG_FIFO, buf[nwords++]);
				last = rd_cntpct();
				over = 0;
				continue;
			}
			ri = rreg(REG_RINT);
			if (ri & 0x0180u)   /* DATA_CRC(bit7)/DATA_TIMEOUT(bit8) */
				goto fail_open;
			{
				uint64_t now = rd_cntpct();
				uint64_t el = (now >= last) ? (now - last) : 0ull;
				if (el > patience) {
					if (++over < 2)
						continue;
					goto fail_open;
				}
				over = 0;
			}
		}
		if (nwords < total)
			goto fail_open;
	}

	/* DATA_OVER + AUTO_STOP_DONE -- same asymmetric-fatality rule as
	 * emmc_bio_write()'s own DATA_OVER wait: RINT_READ_ERR_MASK|RESP_TIMEOUT
	 * bits seen at THIS point are fatal, matching the read-error mask
	 * (excludes bit8, per that file's own measured-false-positive note). */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(EMMC_WRITE_DATA_TIMEOUT_MS);
		unsigned over = 0;
		for (;;) {
			ri = rreg(REG_RINT);
			if (ri & (RINT_READ_ERR_MASK | RINT_RESP_TIMEOUT))
				goto fail_open;
			if ((ri & (RINT_DATA_OVER | RINT_AUTO_STOP_DONE)) ==
			    (RINT_DATA_OVER | RINT_AUTO_STOP_DONE))
				break;
			{
				uint64_t now = rd_cntpct();
				uint64_t el = (now >= start) ? (now - start) : 0ull;
				if (el > cap) {
					if (++over < 2)
						continue;
					goto fail_open;
				}
				over = 0;
			}
		}
	}

	/* AUTO_STOP's own CMD12 is R1b: card holds DAT0 low while it programs. */
	if (wait_card_idle_timed(EMMC_WRITE_BUSY_TIMEOUT_MS) != 0)
		goto fail_closed;

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail_open:
	/* Data phase may still be live: targeted CMD12 needed (see the header
	 * comment on why an untargeted one corrupted sectors before). */
	ebio_fail_settle_full(1);
	return (int)(0x60000000u | ((nwords & 0x3fffu) << 14) | (ri & 0x3fffu));
fail_closed:
	/* AUTO_STOP's CMD12 already ran; sending a second one is the illegal-
	 * command case. Do not claim success: the card never signalled program-
	 * done, exactly the -2 contract emmc_bio_write() already uses. */
	ebio_fail_settle_full(0);
	return -2;
}

/* ------------------------------------------------------------------ */
/* emmc_bio_read_dma() / emmc_bio_write_dma() -- IDMAC descriptor-chain DMA */
/* ------------------------------------------------------------------ */
/* Register map, descriptor format and the enable/reset/wait sequence below
 * are transcribed VERBATIM from FreeBSD's own aw_mmc.c (sys/arm/allwinner/
 * aw_mmc.c in this tree's /opt/bzdos/freebsd-src-bpi checkout) -- the real
 * driver for this exact controller, and this project's standing reference
 * for anything not already covered by the hardware-verified Python sequence
 * (see this file's own top-of-file note, and the "REWRITTEN 2026-07-30 to
 * match the reference driver" precedent above gctl_reset_and_wait()'s
 * GCTL_FIFO_RST|GCTL_DMA_RST pairing -- same source, same method, again).
 *
 * Field/bit names below match aw_mmc.h's AW_MMC_* verbatim (renamed to this
 * file's REG_/bit-macro convention) so a diff against the real driver stays
 * legible. dma_desc_shift=0 for this controller (a64_emmc_conf), i.e. DLBA
 * and every descriptor buf_addr/next field hold UNSHIFTED 32-bit physical
 * addresses -- confirmed safe on this board (GUEST_DRAM_2G, every DMA PA
 * used here comfortably fits 32 bits). */
#define REG_FWLR   0x40u   /* FIFO threshold watermark (DMA burst config) */
#define REG_DMAC   0x80u   /* IDMAC control */
#define REG_DLBA   0x84u   /* IDMAC descriptor-list base address */
#define REG_IDST   0x88u   /* IDMAC status, write-1-to-clear */
#define REG_IDIE   0x8Cu   /* IDMAC interrupt enable */

#define GCTL_FIFO_AC_MOD   0x80000000u  /* bit31: 1=AHB/PIO FIFO access, 0=DMA */
#define GCTL_DMA_ENB       0x00000020u  /* bit5 */

#define DMAC_IDMAC_SOFT_RST   0x00000001u  /* bit0 */
#define DMAC_IDMAC_FIX_BURST  0x00000002u  /* bit1 */
#define DMAC_IDMAC_IDMA_ON    0x00000080u  /* bit7 */

#define IDST_TX_INT        0x00000001u  /* bit0: write-direction completion */
#define IDST_RX_INT        0x00000002u  /* bit1: read-direction completion  */
#define IDST_FATAL_BERR    0x00000004u  /* bit2 */
#define IDST_DES_UNAVL     0x00000010u  /* bit4 */
#define IDST_ERR_FLAG_SUM  0x00000020u  /* bit5 */
#define IDST_ABN_INT_SUM   0x00000200u  /* bit9 */
#define IDST_ERROR   (IDST_FATAL_BERR | IDST_ERR_FLAG_SUM | \
                      IDST_DES_UNAVL | IDST_ABN_INT_SUM)

/* Descriptor `config` word bits (aw_mmc.h AW_MMC_DMA_CONFIG_*). */
#define DESC_DIC   0x00000002u   /* bit1: disable-interrupt-on-completion   */
#define DESC_LD    0x00000004u   /* bit2: last descriptor                  */
#define DESC_FD    0x00000008u   /* bit3: first descriptor                 */
#define DESC_CH    0x00000010u   /* bit4: chained-descriptor mode           */
#define DESC_ER    0x00000020u   /* bit5: end of ring                       */
#define DESC_OWN   0x80000000u   /* bit31: 1 = owned by the IDMAC           */

#define EMMC_DMA_FTRGLEVEL  0x20070008u  /* FWLR magic, matches aw_mmc.c's own
                                          * AW_MMC_DMA_FTRGLEVEL verbatim --
                                          * not independently derived, same
                                          * "it works as shipped, don't
                                          * re-derive it" rule as the rest of
                                          * this file's register values. */

/* One descriptor per up-to-8192-byte (16-sector) segment -- matches
 * a64_emmc_conf.dma_xferlen (0x2000) in aw_mmc.c EXACTLY (same controller,
 * same conf struct), rather than guessing the hardware's real per-descriptor
 * size-field width. EMMC_MULTI_MAX_BLOCKS (128 * 512 = 65536 B) needs
 * exactly 8 such segments. */
#define EMMC_DMA_SEG_BYTES    0x2000u
#define EMMC_DMA_DESC_COUNT   ((EMMC_MULTI_MAX_BLOCKS * 512u + \
                                EMMC_DMA_SEG_BYTES - 1u) / EMMC_DMA_SEG_BYTES)

struct emmc_dma_desc {
	uint32_t config;
	uint32_t buf_size;
	uint32_t buf_addr;
	uint32_t next;
};
/* HV-owned, cache-line aligned: the IDMAC fetches these via real bus-master
 * DMA, so (unlike every PIO path in this file, where the CPU's own load/
 * store is trivially same-core-coherent) this buffer needs EXPLICIT cache
 * maintenance before the hardware reads it -- see emmc_dma_cmo() below. */
static struct emmc_dma_desc g_emmc_dma_desc[EMMC_DMA_DESC_COUNT]
	__attribute__((aligned(64)));
#define EMMC_DMA_DESC_PA  ((uint64_t)(uintptr_t)&g_emmc_dma_desc[0])

/* Clean+invalidate [pa, pa+len) to PoC. Exactly vblk_emmc.c's gmem_cmo()
 * minus the `ic ivau` half: that half exists there because a guest buffer
 * touched via block I/O can hold code the guest is about to execute; the
 * buffers this function guards (our own descriptor ring, and the caller's
 * buf_pa -- an EL2 scratch buffer at every call site wired so far) are pure
 * data, never fetched as instructions, so the I-cache half would be dead
 * work here. If a future caller ever hands buf_pa a guest-executable
 * region directly (true zero-copy), add it back first. */
static void emmc_dma_cmo(uint64_t pa, uint32_t len)
{
	uint64_t p = pa & ~63ULL;
	uint64_t end = pa + len;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Build the descriptor chain for one [buf_pa, buf_pa+total_bytes) transfer,
 * chunked into <= EMMC_DMA_SEG_BYTES segments, CH set on every descriptor,
 * FD on the first, LD|ER (and DIC cleared) on the last -- byte-for-byte the
 * same shape aw_dma_cb() builds in the reference driver. Publishes the
 * chain to PoC before returning (the IDMAC's descriptor fetch is a second
 * independent DMA read, on top of the data-buffer DMA this function does
 * not touch -- callers clean/invalidate buf_pa separately). Returns the
 * descriptor count (always <= EMMC_DMA_DESC_COUNT by construction, since
 * total_bytes <= EMMC_MULTI_MAX_BLOCKS*512). */
static uint32_t emmc_dma_build_desc(uint64_t buf_pa, uint32_t total_bytes)
{
	uint32_t ndesc = 0, done = 0;

	while (done < total_bytes) {
		uint32_t seg = total_bytes - done;
		if (seg > EMMC_DMA_SEG_BYTES)
			seg = EMMC_DMA_SEG_BYTES;
		g_emmc_dma_desc[ndesc].buf_size = seg;
		g_emmc_dma_desc[ndesc].buf_addr = (uint32_t)(buf_pa + done);
		g_emmc_dma_desc[ndesc].config = DESC_CH | DESC_OWN | DESC_DIC;
		g_emmc_dma_desc[ndesc].next = (uint32_t)(EMMC_DMA_DESC_PA +
			(uint64_t)(ndesc + 1) * sizeof(struct emmc_dma_desc));
		done += seg;
		ndesc++;
	}
	g_emmc_dma_desc[0].config |= DESC_FD;
	g_emmc_dma_desc[ndesc - 1].config |= DESC_LD | DESC_ER;
	g_emmc_dma_desc[ndesc - 1].config &= ~(uint32_t)DESC_DIC;
	g_emmc_dma_desc[ndesc - 1].next = 0;

	emmc_dma_cmo(EMMC_DMA_DESC_PA, ndesc * (uint32_t)sizeof(struct emmc_dma_desc));
	return ndesc;
}

/* Switch the controller from its default AHB/PIO FIFO-access mode into
 * IDMAC mode and arm one descriptor-chain transfer. Mirrors aw_dma_prepare()
 * or aw_mmc.c, with the reset step upgraded from that function's un-polled
 * pulse to this file's own hardened gctl_reset_and_wait() -- the same
 * upgrade already applied once to the PIO path's error-recovery reset (see
 * the "REWRITTEN 2026-07-30" comment above), for the same reason: a fixed
 * delay is not the same as confirming the reset pulse actually finished. */
static void emmc_dma_arm(uint32_t ndesc)
{
	uint32_t gctl;

	gctl_reset_and_wait(GCTL_FIFO_RST | GCTL_DMA_RST);

	gctl = rreg(REG_GCTL);
	gctl &= ~GCTL_FIFO_AC_MOD;
	gctl |= GCTL_DMA_ENB;
	wreg(REG_GCTL, gctl);

	wreg(REG_DMAC, DMAC_IDMAC_SOFT_RST);
	small_delay();
	wreg(REG_DMAC, DMAC_IDMAC_IDMA_ON | DMAC_IDMAC_FIX_BURST);
	wreg(REG_IDIE, rreg(REG_IDIE) | IDST_RX_INT | IDST_TX_INT);
	wreg(REG_DLBA, (uint32_t)EMMC_DMA_DESC_PA);
	wreg(REG_FWLR, EMMC_DMA_FTRGLEVEL);
	wreg(REG_IDST, 0xFFFFFFFFu);
	(void)ndesc;
}

/* Restore the controller to plain AHB/PIO FIFO-access mode and switch the
 * IDMAC off, so every OTHER function in this file (emmc_bio_read/write,
 * *_multi, emmc_bio_set_highspeed's own test read) keeps working exactly as
 * hardware-verified -- none of them expect GCTL_DMA_ENB set. Called on both
 * the success and the failure exit of a DMA transfer; idempotent. */
static void emmc_dma_disarm(void)
{
	uint32_t gctl = rreg(REG_GCTL);
	gctl |= GCTL_FIFO_AC_MOD;
	gctl &= ~GCTL_DMA_ENB;
	wreg(REG_GCTL, gctl);
	wreg(REG_DMAC, 0);
}

/* Who finishes a split-phase transfer (vblk_emmc.c). A hook, not a direct
 * call, so every build that links this file but not vblk_emmc.o still
 * links. NULL until the owner registers. */
static void (*volatile g_dma_done_hook)(uint32_t spin_us);

void emmc_bio_set_dma_done_hook(void (*fn)(uint32_t spin_us))
{
	g_dma_done_hook = fn;
}

void emmc_bio_dma_irq_note(void)
{
	void (*fn)(uint32_t) = g_dma_done_hook;

	ebio_bc(31, ++g_dma_irqs);
	if (fn)
		fn(EMMC_DMA_IRQ_SPIN_US);
}

void emmc_bio_dma_tick(void)
{
	void (*fn)(uint32_t) = g_dma_done_hook;

	if (fn)
		fn(0);
}

/* Stall-based wait for the IDMAC's own completion (IDST TX/RX_INT) alongside
 * the data-phase RINT the PIO paths already check -- aw_mmc_intr() treats
 * `idst & AW_MMC_IDST_ERROR` and `rint & AW_MMC_INT_ERR_BIT` as two
 * independent failure sources (see aw_mmc.c's interrupt handler), so this
 * checks both every iteration rather than assuming one implies the other.
 *
 * WFI BETWEEN ITERATIONS (2026-09-30, HANDOFF item 2's open follow-up): the
 * correctness logic above is UNCHANGED -- every register read, every error
 * check, the timeout cap, all exactly as hardware-verified before. The only
 * change is what the core does between one check and the next: instead of
 * spinning back to the top of the loop immediately (burning cycles hammering
 * REG_IDST/REG_RINT), it executes `wfi` first. gic_timer.c now arms the
 * controller's real hardware SPI (EMMC_DMA_IRQ_INTID) at the distributor, so
 * a genuine IDMAC completion wakes this core almost immediately; if that
 * interrupt is ever missed or misrouted (see gic_timer.c's dispatch comment
 * for why that is a live, hardware-observed failure mode for a level line --
 * MUSB and HDMI both needed a throttle for exactly this), the very next
 * regular ~10 ms CNTP tick wakes it anyway, since WFI wakes on ANY pending
 * exception whether or not it is separately masked -- so the timeout cap
 * above remains the actual correctness backstop, completely unaffected by
 * whether the new interrupt path works at all. This is deliberately NOT the
 * bigger change HANDOFF's own wording points at ("vCPU handed back to the
 * guest during a transfer") -- the guest's trap into this code is still
 * synchronous end-to-end, so its vCPU is still blocked for the whole
 * transfer. That would need the request's completion (used-ring publish +
 * virtio IRQ injection) to happen FROM the real IRQ handler, asynchronously,
 * after an early return to the guest -- a materially bigger change (new
 * per-request state surviving across a trap boundary, reusing vblk_async.c's
 * already-proven used-ring-lock discipline rather than inventing a new one)
 * that deserves its own careful, separately-verified step, not a rider on
 * this one. See HANDOFF.md. */
static int emmc_dma_wait_complete(uint32_t is_read, uint32_t *ri_out)
{
	uint64_t start = rd_cntpct();
	uint64_t cap = ms_to_ticks(EMMC_WRITE_DATA_TIMEOUT_MS);
	uint32_t want = is_read ? IDST_RX_INT : IDST_TX_INT;

	for (;;) {
		uint32_t idst = rreg(REG_IDST);
		uint32_t ri = rreg(REG_RINT);

		if ((idst & IDST_ERROR) || (ri & (RINT_READ_ERR_MASK | RINT_RESP_TIMEOUT))) {
			*ri_out = ri;
			return -1;
		}
		if ((idst & want) && (ri & RINT_DATA_OVER)) {
			wreg(REG_IDST, idst);   /* write-1-to-clear, ack what we saw */
			*ri_out = ri;
			return 0;
		}
		{
			uint64_t now = rd_cntpct();
			uint64_t el = (now >= start) ? (now - start) : 0ull;
			if (el > cap) {
				*ri_out = ri;
				return -1;
			}
		}
		__asm__ volatile("wfi" ::: "memory");
	}
}

int emmc_bio_read_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	uint32_t total, ndesc, ri = 0;

	if (nblk < 2 || nblk > EMMC_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;
	/* The IDMAC writes buf_pa behind the cache: a partial first/last line
	 * can hold a neighbour another core dirties mid-transfer, and its
	 * write-back would overwrite the DMA'd bytes. Whole lines only;
	 * the caller falls back to PIO. */
	if ((buf_pa | total) & 63u)
		return -102;

	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	ndesc = emmc_dma_build_desc(buf_pa, total);
	/* The IDMAC is about to WRITE buf_pa; make sure no stale EL2 cache
	 * line for it survives to be read after completion (mirrors the PIO
	 * multi-read path's implicit same-core-store coherency -- explicit
	 * here because the writer is the DMA engine, not this CPU). */
	emmc_dma_cmo(buf_pa, total);
	emmc_dma_arm(ndesc);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD18_READ_CMDR);

	if (emmc_dma_wait_complete(1, &ri) != 0)
		goto fail_open;

	/* AUTO_STOP's own CMD12 is R1b: card holds DAT0 low while it retires. */
	if (wait_card_idle_timed(EMMC_WRITE_BUSY_TIMEOUT_MS) != 0)
		goto fail_closed;
	ri = rreg(REG_RINT);
	if (ri & RINT_READ_ERR_MASK)
		goto fail_closed;

	emmc_dma_disarm();
	/* Publish the IDMAC's write for any EL2 reader (the caller, or a
	 * subsequent PIO call reusing the same buffer). */
	emmc_dma_cmo(buf_pa, total);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail_open:
	emmc_dma_disarm();
	ebio_fail_settle_full(1);
	return (int)(0x60000000u | (ri & 0x3fffu));
fail_closed:
	emmc_dma_disarm();
	ebio_fail_settle_full(0);
	return (int)(0x70000000u | (ri & 0x3fffu));
}

int emmc_bio_write_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	uint32_t total, ndesc, ri = 0;

	if (nblk < 2 || nblk > EMMC_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;

	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	ndesc = emmc_dma_build_desc(buf_pa, total);
	/* The IDMAC is about to READ buf_pa; clean it to PoC first so the
	 * hardware (a real bus-master, no cache snooping) sees fresh bytes
	 * rather than whatever stale line EL2 might still hold. */
	emmc_dma_cmo(buf_pa, total);
	emmc_dma_arm(ndesc);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);
	wreg(REG_CMDR, CMD25_WRITE_CMDR);

	if (emmc_dma_wait_complete(0, &ri) != 0)
		goto fail_open;

	/* AUTO_STOP's own CMD12 is R1b: card holds DAT0 low while it programs. */
	if (wait_card_idle_timed(EMMC_WRITE_BUSY_TIMEOUT_MS) != 0)
		goto fail_closed;

	emmc_dma_disarm();
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail_open:
	emmc_dma_disarm();
	/* Data phase may still be live: targeted CMD12 needed (same rationale
	 * as emmc_bio_write_multi()'s own fail_open -- see the header comment
	 * on why an untargeted one corrupted sectors before). */
	ebio_fail_settle_full(1);
	return (int)(0x60000000u | (ri & 0x3fffu));
fail_closed:
	emmc_dma_disarm();
	/* AUTO_STOP's CMD12 already ran; sending a second one is the illegal-
	 * command case. Do not claim success: the -2 contract matches
	 * emmc_bio_write()/emmc_bio_write_multi(). */
	ebio_fail_settle_full(0);
	return -2;
}

/* Split-phase IDMAC (HANDOFF item 2, IRQ phase 2): the same register
 * sequence as emmc_bio_read_dma()/write_dma() above, cut at the one place
 * the caller has to wait. emmc_bio_dma_start() is everything up to and
 * including the CMDR write; emmc_bio_dma_poll() is everything after it,
 * one non-blocking step at a time, so vblk_emmc.c can return to the guest
 * while the transfer runs and finish it from the completion IRQ.
 *
 * Deliberately NOT a rewrite of the synchronous pair: those stay the
 * hardware-verified path (the sync fallback, dbgmon `call`, SD's mirror),
 * and this is compared against them rather than replacing them.
 *
 * Two phases, because a transfer has two waits and only one raises an IRQ:
 *   DATA -- IDST RX/TX_INT plus RINT DATA_OVER (the IDMAC IRQ fires here);
 *   BUSY -- AUTO_STOP's CMD12 is R1b, the card holds DAT0 until it is done
 *           (programming, for a write). Nothing interrupts on that, so it
 *           is polled, from the IRQ handler's short spin or the next tick.
 * Timeouts are the synchronous path's own caps, measured from the start of
 * each phase, and a cap must be seen exceeded on two separate polls before
 * it counts -- the same CNTPCT two-read discipline as wait_card_idle_timed(),
 * for the same reason (consecutive reads have disagreed by seconds here).
 *
 * The caller must hold the eMMC lock from start until poll returns != 1.
 * Only one transfer can be in flight: start refuses (-103) while one is. */
#define ADMA_DATA 0u
#define ADMA_BUSY 1u
static struct {
	volatile uint32_t active;
	uint32_t is_read, phase, total, over;
	uint64_t buf_pa, t0;
} g_adma;

int emmc_bio_dma_start(uint32_t is_read, uint32_t lba, uint64_t buf_pa,
                       uint32_t nblk)
{
	uint32_t total, ndesc;

	if (g_adma.active)
		return -103;
	if (nblk < 2 || nblk > EMMC_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;
	/* Same whole-cache-line rule as emmc_bio_read_dma(). */
	if (is_read && ((buf_pa | total) & 63u))
		return -102;

	wait_card_idle_timed(EMMC_SETTLE_BUSY_TIMEOUT_MS);

	ndesc = emmc_dma_build_desc(buf_pa, total);
	emmc_dma_cmo(buf_pa, total);
	emmc_dma_arm(ndesc);

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, lba);

	g_adma.is_read = is_read ? 1u : 0u;
	g_adma.phase = ADMA_DATA;
	g_adma.total = total;
	g_adma.over = 0;
	g_adma.buf_pa = buf_pa;
	g_adma.t0 = rd_cntpct();
	g_adma.active = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	wreg(REG_CMDR, is_read ? CMD18_READ_CMDR : CMD25_WRITE_CMDR);
	return 0;
}

/* 1 = over the phase's cap on this poll, confirmed by the previous one too. */
static int adma_timed_out(uint32_t ms)
{
	uint64_t now = rd_cntpct();
	uint64_t el = (now >= g_adma.t0) ? (now - g_adma.t0) : 0ull;

	if (el <= ms_to_ticks(ms)) {
		if (g_adma.over)
			ebio_bc(18, ++g_cnt_anom);
		g_adma.over = 0;
		return 0;
	}
	return ++g_adma.over >= 2u;
}

/* One step. 1 = still running, 0 = done OK, anything else = done with the
 * synchronous pair's own packed failure code, controller already settled. */
static int adma_step(void)
{
	uint32_t ri = 0;

	if (g_adma.phase == ADMA_DATA) {
		uint32_t want = g_adma.is_read ? IDST_RX_INT : IDST_TX_INT;
		uint32_t idst = rreg(REG_IDST);

		ri = rreg(REG_RINT);
		if ((idst & IDST_ERROR) ||
		    (ri & (RINT_READ_ERR_MASK | RINT_RESP_TIMEOUT)))
			goto fail_open;
		if (!((idst & want) && (ri & RINT_DATA_OVER))) {
			if (adma_timed_out(EMMC_WRITE_DATA_TIMEOUT_MS))
				goto fail_open;
			return 1;
		}
		/* W1C drops the IDMAC's level line: this ack is what ends the
		 * interrupt, the GIC EOI alone does not. */
		wreg(REG_IDST, idst);
		g_adma.phase = ADMA_BUSY;
		g_adma.over = 0;
		g_adma.t0 = rd_cntpct();
	}

	if (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (adma_timed_out(EMMC_WRITE_BUSY_TIMEOUT_MS))
			goto fail_closed;
		return 1;
	}
	if (g_adma.is_read) {
		ri = rreg(REG_RINT);
		if (ri & RINT_READ_ERR_MASK)
			goto fail_closed;
	}

	emmc_dma_disarm();
	if (g_adma.is_read)
		emmc_dma_cmo(g_adma.buf_pa, g_adma.total);
	__asm__ volatile("dsb sy" ::: "memory");
	g_adma.active = 0;
	return 0;

fail_open:
	emmc_dma_disarm();
	ebio_fail_settle_full(1);
	g_adma.active = 0;
	return (int)(0x60000000u | (ri & 0x3fffu));
fail_closed:
	emmc_dma_disarm();
	ebio_fail_settle_full(0);
	g_adma.active = 0;
	return g_adma.is_read ? (int)(0x70000000u | (ri & 0x3fffu)) : -2;
}

int emmc_bio_dma_poll(uint32_t spin_us)
{
	uint64_t start = rd_cntpct();
	uint64_t cap = ms_to_ticks(1) * spin_us / 1000ull;

	if (!g_adma.active)
		return -103;
	for (;;) {
		int rc = adma_step();
		uint64_t now;

		if (rc != 1)
			return rc;
		now = rd_cntpct();
		if (now < start || now - start >= cap)
			return 1;
	}
}

/* Arm or disarm write fault injection. See the g_fi_* block for the rationale.
 * every == 0 disarms. Echoes the armed configuration into [28] so the host can
 * confirm what is actually live rather than what it believes it asked for.
 * Callable over the debug channel via dbgmon `call`, and from dbgmon's own
 * `fi` command. */
void emmc_bio_fault_inject(uint32_t every, uint32_t min_lba, uint32_t point,
                           uint32_t legacy_wait)
{
	g_fi_every = every;
	g_fi_min_lba = min_lba;
	g_fi_point = point ? 1u : 0u;
	g_fi_legacy_wait = legacy_wait ? 1u : 0u;
	g_fi_seq = 0;
	g_fi_injected = 0;
	ebio_bc(27, 0);
	/* [28]: 0 = disarmed. Otherwise every | (legacy<<16) | 0x8000 as an
	 * armed marker, so a reader cannot mistake "disarmed" for "unwritten". */
	ebio_bc(28, every ? (every | (g_fi_legacy_wait << 16) |
	                     (g_fi_point << 17) | 0x8000u) : 0u);
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
