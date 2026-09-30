/* SPDX-License-Identifier: BSD-2-Clause */

/* sd_bio.c — Allwinner A64 SMHC0 / SD-card (mmc0 @ 0x01c0f000) block-I/O.
 * Implements sd_bio.h. Freestanding bare-metal AArch64, MMIO via volatile
 * pointers on identity-mapped Device-nGnRE addresses (same contract as
 * emmc_bio.c / emac.c: U-Boot leaves the MMU on with a flat device mapping).
 *
 * The SMHC controller IP is IDENTICAL to the eMMC one driven by emmc_bio.c,
 * so the register map, bit values, clock-config latch sequence (NTSR/SAMP_DL
 * only latch while the card clock is off), CMDR command engine and 128-word
 * FIFO drain/fill loops are all ported VERBATIM from emmc_bio.c — see that
 * file's header for the hardware-verification provenance and the citations
 * for every magic value here. Do NOT re-derive from a generic datasheet.
 *
 * What is SD-SPECIFIC (and the reason this is a separate file):
 *   - base 0x01c0f000, pinmux PF0..PF5 -> func2 ("mmc0"), CCU MMC0_CLK
 *     @ 0x01c20088;
 *   - card identification uses the SD protocol: CMD8 SEND_IF_COND, ACMD41
 *     SD_SEND_OP_COND with the HCS high-capacity bit, and CMD3 where the CARD
 *     returns its own RCA (vs eMMC's CMD1 + host-assigned RCA). The OCR CCS
 *     bit selects block- vs byte-addressing for CMD17/CMD24.
 *
 * Runs at the 400 kHz identification clock, 1-bit bus — no high-speed reclock
 * (emmc_bio.c proved the read path is tolerant of the slow init clock; HS/4-bit
 * is a later optimization, deliberately omitted from this first cut to keep the
 * SD bring-up as close to guaranteed-working as possible).
 *
 * All polls are iteration-capped (SD_POLL_CAP) so a dbgmon `call` can never
 * hang the debug core.
 */
#include <stdint.h>
#include <stddef.h>
#include "sd_bio.h"
#include "hv_addrmap.h"     /* HVMAP_SD_BC */
#include "cntpct.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

/* ------------------------------------------------------------------ */
/* Physical bases (SD-specific)                                        */
/* ------------------------------------------------------------------ */
#define SD_BASE          SOC_A64_SMHC0_BASE   /* SMHC0/aw_mmc0 (SD card) controller */
#define PIO_PF_CFG0      (SOC_A64_PIO_BASE + 0xB4)   /* Port F config reg 0 (PF0..PF7)     */
#define CCU_MMC0_CLK     SOC_A64_CCU_MMC0_CLK   /* CCU MMC0_CLK gate/divider          */

/* PF0..PF5 -> function 2 (mmc0), preserving PF6 (card-detect) / PF7 nibbles. */
#define PF_MMC0_CFG0     0x00222222u
#define PF_KEEP_MASK     0xFF000000u

/* ------------------------------------------------------------------ */
/* Controller register offsets — identical to emmc_bio.c (same IP).    */
/* ------------------------------------------------------------------ */
#define REG_GCTL   0x00u
#define REG_CKCR   0x04u
#define REG_TMOR   0x08u
#define REG_BWDR   0x0Cu
#define REG_BKSR   0x10u
#define REG_BYCR   0x14u
#define REG_CMDR   0x18u
#define REG_CAGR   0x1Cu
#define REG_RESP0  0x20u
#define REG_RESP1  0x24u
#define REG_RESP2  0x28u
#define REG_RESP3  0x2Cu
#define REG_IMKR   0x30u
#define REG_RINT   0x38u
#define REG_STAR   0x3Cu
#define REG_NTSR   0x5Cu
#define REG_SAMP_DL 0x144u
#define REG_FIFO   0x200u

#define NTSR_MODE_SEL_NEW    0x80000000u
#define SAMP_DL_CAL_SW_EN    0x00000080u

#define GCTL_RESET_ALL     0x00000007u
#define GCTL_FIFO_RST      0x00000002u
#define GCTL_DMA_RST       0x00000004u   /* bit2, needed by sd_bio_read_dma/write_dma */
#define GCTL_AHB_INIT      0x80000010u

#define CKCR_CARD_CLK_EN   0x00010000u

#define CMDR_LOAD           0x80000000u
#define CMDR_PRG_CLK        0x00200000u
#define CMDR_SEND_INIT      0x00008000u
#define CMDR_WAIT_PRE_OVER  0x00002000u
#define CMDR_DATA_EXP       0x00000200u
#define CMDR_CHK_CRC        0x00000100u
#define CMDR_WRITE          0x00000400u
#define CMDR_LONG_RESP      0x00000080u
#define CMDR_RESP_EXP       0x00000040u

#define RINT_CMD_DONE       0x00000004u
#define RINT_DATA_OVER      0x00000008u
#define RINT_ALL            0xFFFFFFFFu

#define STAR_FIFO_EMPTY     0x00000004u
#define STAR_FIFO_FULL      0x00000008u
#define STAR_CARD_BUSY      0x00000200u

#define CLK_UPDATE_CMDR    (CMDR_LOAD | CMDR_PRG_CLK | CMDR_WAIT_PRE_OVER)

/* CMD17 (READ_SINGLE_BLOCK) / CMD24 (WRITE_BLOCK) full CMDR words — identical
 * to emmc_bio.c's (the data-transfer command encoding is card-agnostic). */
#define CMD17_READ_CMDR    0x80002351u
#define CMD24_WRITE_CMDR   0x80002758u

#define SD_POLL_CAP    400000
#define ACMD41_RETRY_CAP  200      /* ACMD41 can take up to ~1 s to power up */

/* SD command / OCR magic. */
#define CMD8_ARG_3V3_AA    0x000001AAu  /* VHS=1 (2.7-3.6V) | check pattern AA */
#define ACMD41_ARG_HCS     0x40000000u  /* host supports high capacity (SDHC)  */
#define ACMD41_ARG_VWIN    0x00FF8000u  /* 2.7-3.6V voltage window             */
#define OCR_BUSY_READY     0x80000000u  /* OCR bit31: power-up complete        */
#define OCR_CCS_SDHC       0x40000000u  /* OCR bit30: card is block-addressed  */

/* SD breadcrumb window: distinct from vblk (0x50020000-17f), EBIO
 * (0x50020200-21f) and emmc HS testbuf (0x50020300-4ff). */
#define SDBC_BASE HVMAP_SD_BC   /* see hv_addrmap.h */

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
static inline uint32_t rreg(uint32_t off) { return rd32(SD_BASE + off); }
static inline void wreg(uint32_t off, uint32_t v) { wr32(SD_BASE + off, v); }

/* D4 fix, mirrored VERBATIM from emmc_bio.c (same controller IP, same
 * iteration-cap-is-not-a-time-cap issue in the two post-write polls below —
 * see that file's block comment above its EMMC_WRITE_DATA_TIMEOUT_MS for the
 * full rationale). CNTPCT_EL0 is safe to read with no locking from any core/
 * context (dbgmon `call` on CPU1 is this file's only caller today). */
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

static inline uint64_t ms_to_ticks(uint32_t ms)
{
	uint64_t f = rd_cntfrq();
	if (f == 0)
		f = 24000000ull;   /* A64 arch timer default, matches wdt.c's fallback */
	return (f * (uint64_t)ms) / 1000ull;
}

#define SD_WRITE_DATA_TIMEOUT_MS   1000u
#define SD_WRITE_BUSY_TIMEOUT_MS   4000u

static void sdbc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(SDBC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Card type detected by sd_bio_init(); 1 = SDHC/SDXC (block addressing). */
static int g_sd_block_addressed;
static int g_sd_inited;

/* Real capacity, parsed from CSD (sd_bio_init()'s CMD9) when the card is a
 * CSD-2.0 (SDHC/SDXC) card -- see sd_bio_capacity_sectors()'s own comment.
 * Defaults to the same conservative 1 GiB stub vblk_sd.h used to hardcode
 * unconditionally, so a parse failure degrades to the old, always-safe
 * behavior rather than an unbounded/wrong guess. */
#define SD_CAPACITY_FALLBACK_SECTORS  ((uint64_t)1024 * 1024 * 1024 / 512u)
static uint64_t g_sd_capacity_sectors = SD_CAPACITY_FALLBACK_SECTORS;

/* Translate a 512-byte sector index to the controller command argument. */
static inline uint32_t sd_addr(uint32_t lba)
{
	return g_sd_block_addressed ? lba : (lba * 512u);
}

static int poll_rint(uint32_t mask, uint32_t want)
{
	int i;
	for (i = 0; i < SD_POLL_CAP; i++)
		if ((rreg(REG_RINT) & mask) == want)
			return 0;
	return -1;
}

static void small_delay(void)
{
	volatile int i;
	for (i = 0; i < 64; i++)
		(void)rreg(REG_GCTL);
}

static int clk_update(void)
{
	uint32_t i;
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CMDR, CLK_UPDATE_CMDR);
	for (i = 0; i < SD_POLL_CAP; i++)
		if ((rreg(REG_CMDR) & CMDR_LOAD) == 0)
			break;
	if (i >= SD_POLL_CAP)
		return -1;
	wreg(REG_RINT, RINT_ALL);
	return 0;
}

/* Issue one command; wait for `dm` bits in RINT. Fills *resp0_out if non-NULL.
 * Returns 0 on success, -1 on a bounded poll timeout. */
static int sd_cmd(uint32_t op, uint32_t arg, uint32_t fl, uint32_t dm,
                  uint32_t *resp0_out)
{
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, arg);
	wreg(REG_CMDR, CMDR_LOAD | fl | op);
	if (poll_rint(dm, dm) != 0)
		return -1;
	if (resp0_out)
		*resp0_out = rreg(REG_RESP0);
	return 0;
}

static int sd_cmd_done(uint32_t op, uint32_t arg, uint32_t fl, uint32_t *resp0_out)
{
	return sd_cmd(op, arg, fl, RINT_CMD_DONE, resp0_out);
}

/* ACMD<n> = CMD55 (APP_CMD, arg = rca<<16) followed by CMD<n>. During
 * identification rca is 0. */
static int sd_acmd(uint32_t rca, uint32_t acmd, uint32_t arg, uint32_t fl,
                   uint32_t *resp0_out)
{
	if (sd_cmd_done(55, rca << 16, CMDR_RESP_EXP, NULL) != 0)
		return -1;
	return sd_cmd_done(acmd, arg, fl, resp0_out);
}

static int sd_set_hs50(void);

/* ------------------------------------------------------------------ */
/* sd_set_bus4() — 4-bit data bus, FAIL-SAFE (same contract as          */
/* sd_bio_set_highspeed(): it can only leave the card on a WORKING bus). */
/*                                                                      */
/* WHY: at 25 MHz the 1-bit bus caps the card at ~3 MB/s; measured      */
/* 2.07 MB/s reads, 1.50 MB/s writes (CMD25) on 2026-09-26. Every SD     */
/* card supports 4-bit (SCR bit only matters for SD v1.0 MMC-ish cards,  */
/* and PF0..PF5 already carry D0..D3). The switch is ACMD6 (arg 2) to    */
/* the card, BWDR=1 on the host. The check is stronger than an rc: LBA 0 */
/* is read on the 1-bit bus first and must come back identical on the    */
/* 4-bit one -- a wrong-width bus fails CRC, but a wired-but-dead line  */
/* is exactly what should never be trusted on rc alone.                  */
/* SD window [7]: 4 = 4-bit active, 0x10|step = stayed on 1-bit.         */
/* ------------------------------------------------------------------ */
#define SD_BC_BUS  7

static int sd_bus1_fallback(uint32_t rca, uint32_t step)
{
	wreg(REG_BWDR, 0);
	(void)sd_acmd(rca, 6, 0, CMDR_RESP_EXP, NULL);   /* card back to 1-bit */
	sdbc(SD_BC_BUS, 0x10u | step);
	return -1;
}

static int sd_set_bus4(uint32_t rca)
{
	static uint32_t ref[128];
	volatile uint32_t *t = (volatile uint32_t *)(unsigned long)HVMAP_SD_TESTBUF;
	uint32_t r1 = 0;
	int i;

	if (sd_bio_read(0, (uint64_t)(uintptr_t)ref) != 0)
		return sd_bus1_fallback(rca, 1);
	if (sd_acmd(rca, 6, 2, CMDR_RESP_EXP | CMDR_CHK_CRC, &r1) != 0 ||
	    (r1 & 0xFDF80008u) != 0)   /* R1 error bits 31-26, 24-19, 3 */
		return sd_bus1_fallback(rca, 2);
	wreg(REG_BWDR, 1);
	for (i = 0; i < 128; i++)
		t[i] = 0;
	if (sd_bio_read(0, (uint64_t)HVMAP_SD_TESTBUF) != 0)
		return sd_bus1_fallback(rca, 3);
	for (i = 0; i < 128; i++)
		if (t[i] != ref[i])
			return sd_bus1_fallback(rca, 4);
	sdbc(SD_BC_BUS, 4);
	return 0;
}

/* ------------------------------------------------------------------ */
/* sd_bio_init() — controller bring-up + SD card identification.       */
/* Negative return codes are step-specific for diagnosis (see below);  */
/* also mirrored into the SD breadcrumb window word[0].                */
/* ------------------------------------------------------------------ */
int sd_bio_init(void)
{
	uint32_t c, resp0 = 0;
	uint32_t rca = 0;
	int i;

	g_sd_inited = 0;
	g_sd_block_addressed = 0;

	/* PF0..PF5 -> mmc0 function (U-Boot usually already did this if it tried
	 * to boot mmc0, but assert it explicitly). */
	c = rd32(PIO_PF_CFG0);
	wr32(PIO_PF_CFG0, (c & PF_KEEP_MASK) | PF_MMC0_CFG0);

	wreg(REG_IMKR, 0);

	wreg(REG_GCTL, GCTL_RESET_ALL);
	small_delay();
	wreg(REG_GCTL, GCTL_AHB_INIT);
	wreg(REG_TMOR, 0xffffffffu);
	wreg(REG_BWDR, 0);                 /* 1-bit bus */

	/* Clock config — identical latch dance to emmc_bio_init(): card clock
	 * off + clk_update, program CCU + NTSR + SAMP_DL while the clock is off,
	 * clock back on + clk_update. */
	wreg(REG_CKCR, 0);
	if (clk_update() != 0)              { sdbc(0, (uint32_t)-1); return -1; }
	wr32(CCU_MMC0_CLK, 0x8002000eu);    /* 24MHz/4/15 = 400 kHz */
	wreg(REG_NTSR, rreg(REG_NTSR) | NTSR_MODE_SEL_NEW);
	wreg(REG_SAMP_DL, SAMP_DL_CAL_SW_EN);
	wreg(REG_CKCR, CKCR_CARD_CLK_EN);
	if (clk_update() != 0)              { sdbc(0, (uint32_t)-2); return -2; }

	/* CMD0 GO_IDLE (send-init sequence). */
	if (sd_cmd_done(0, 0, CMDR_SEND_INIT, NULL) != 0) { sdbc(0, (uint32_t)-3); return -3; }

	/* CMD8 SEND_IF_COND (R7). If it answers and echoes the check pattern the
	 * card is SD v2.0+ and MAY be SDHC -> set HCS in ACMD41. If it times out
	 * (v1 card / no CMD8), proceed with HCS clear. Not fatal either way. */
	{
		int v2 = 0;
		if (sd_cmd_done(8, CMD8_ARG_3V3_AA, CMDR_RESP_EXP, &resp0) == 0) {
			if ((resp0 & 0xFFu) == 0xAAu)
				v2 = 1;
		}
		sdbc(2, resp0);
		sdbc(3, (uint32_t)v2);

		/* ACMD41 SD_SEND_OP_COND, loop until OCR ready (bit31). R3 (OCR) has
		 * no CRC, short response. */
		{
			uint32_t arg = ACMD41_ARG_VWIN | (v2 ? ACMD41_ARG_HCS : 0u);
			for (i = 0; i < ACMD41_RETRY_CAP; i++) {
				if (sd_acmd(0, 41, arg, CMDR_RESP_EXP, &resp0) != 0) {
					sdbc(0, (uint32_t)-4); return -4;
				}
				if (resp0 & OCR_BUSY_READY)
					break;
			}
			if (i == ACMD41_RETRY_CAP) { sdbc(0, (uint32_t)-5); return -5; }
			g_sd_block_addressed = (resp0 & OCR_CCS_SDHC) ? 1 : 0;
			sdbc(4, resp0);
			sdbc(5, (uint32_t)g_sd_block_addressed);
		}
	}

	/* CMD2 ALL_SEND_CID (R2, long). */
	if (sd_cmd_done(2, 0, CMDR_LONG_RESP | CMDR_RESP_EXP, NULL) != 0) {
		sdbc(0, (uint32_t)-6); return -6;
	}

	/* CMD3 SEND_RELATIVE_ADDR (R6): the CARD returns its RCA in RESP0[31:16]. */
	if (sd_cmd_done(3, 0, CMDR_RESP_EXP, &resp0) != 0) { sdbc(0, (uint32_t)-7); return -7; }
	rca = resp0 >> 16;
	sdbc(6, rca);

	/* CMD9 SEND_CSD (R2, long), rca<<16. sd_cmd_done()'s resp0_out param only
	 * captures RESP0; a long (136-bit) response spans RESP0..RESP3, so read
	 * the other three directly -- same register block, no extra command. */
	if (sd_cmd_done(9, rca << 16, CMDR_LONG_RESP | CMDR_RESP_EXP, NULL) != 0) {
		sdbc(0, (uint32_t)-8); return -8;
	}
	{
		/* CORRECTED 2026-08-25 (live): first attempt assumed resp[0] was
		 * CSD bits[127:96] (MSB) — SANITY-CHECKED WRONG on a real card
		 * (csd_structure decoded as 0, impossible for a confirmed SDHC/
		 * SDXC card whose OCR CCS bit already read 1). This SMHC IP
		 * instead follows the SDHCI-standard convention: resp[0] = bits
		 * [31:0] (LSB, ascending significance with register number), so
		 * resp[3] = bits[127:96] (MSB) and CSD_STRUCTURE lives there, not
		 * in resp[0]. C_SIZE (bits[69:48]) spans resp[2] bits[5:0] (its
		 * high 6 bits) and resp[1] bits[31:16] (its low 16 bits) under
		 * this corrected mapping. CSD_STRUCTURE must be 1 (version 2.0)
		 * for this C_SIZE-only formula to apply -- true for every real
		 * SDHC/SDXC card by spec, and independently corroborated by the
		 * OCR CCS bit already read via ACMD41 above. Fall back to the
		 * conservative 1 GiB stub (g_sd_capacity_sectors' static
		 * initializer) rather than trust a v1.0 CSD's different
		 * encoding. */
		uint32_t r1 = rreg(REG_RESP1);
		uint32_t r2 = rreg(REG_RESP2);
		uint32_t r3 = rreg(REG_RESP3);
		uint32_t csd_structure = (r3 >> 30) & 0x3u;
		/* idx 10..15: distinct from idx 8/9 (SD_BC_HS_STATE/STEP, written
		 * later in this same function by sd_bio_set_highspeed()) and from
		 * idx 0..6 already used above in this function. */
		sdbc(10, r1); sdbc(11, r2); sdbc(12, r3);
		sdbc(13, csd_structure);
		if (csd_structure == 1u) {
			uint32_t c_size = ((r2 & 0x3Fu) << 16) | ((r1 >> 16) & 0xFFFFu);
			uint64_t sectors = ((uint64_t)c_size + 1u) * 1024u;
			g_sd_capacity_sectors = sectors;
			sdbc(14, (uint32_t)sectors);
			sdbc(15, (uint32_t)(sectors >> 32));
		}
	}

	/* CMD7 SELECT_CARD (R1b), rca<<16. */
	if (sd_cmd_done(7, rca << 16, CMDR_RESP_EXP, NULL) != 0) { sdbc(0, (uint32_t)-9); return -9; }

	/* CMD16 SET_BLOCKLEN 512 (harmless/ignored for SDHC block length). */
	if (sd_cmd_done(16, 512, CMDR_RESP_EXP, NULL) != 0) { sdbc(0, (uint32_t)-10); return -10; }

	g_sd_inited = 1;
	sdbc(0, 0);
	if (sd_bio_set_highspeed() == 0 &&  /* best-effort; see its own header comment */
	    sd_set_bus4(rca) == 0)          /* only on a clock that already works */
		(void)sd_set_hs50();            /* only on a 4-bit bus that works */
	return 0;
}

/* ------------------------------------------------------------------ */
/* sd_bio_set_highspeed() — best-effort 25 MHz reclock, FAIL-SAFE.     */
/*                                                                      */
/* WHY: sd_bio_write() was confirmed live (2026-08-24) to either time  */
/* out on RINT_DATA_OVER outright, or -- worse -- report rc==0 while   */
/* the data never actually landed on the card (read-back showed the    */
/* card's pre-write content, not what was written). Isolated via      */
/* direct EL2 sd_bio_read()/sd_bio_write() calls, bypassing vblk_sd.c  */
/* and the guest entirely, so the bug is in THIS file, not the virtio  */
/* plumbing above it. The one thing this controller has NEVER been run */
/* at is anything other than the 400 kHz identification clock -- see   */
/* the module header's own "no high-speed reclock ... deliberately     */
/* omitted from this first cut" note -- and 400 kHz is far below what  */
/* real SDHC/SDXC cards are designed/tested against, which is a        */
/* plausible and cheap-to-try explanation before suspecting the FIFO/  */
/* DATA_OVER timing logic itself (ported verbatim from the ALREADY     */
/* hardware-verified emmc_bio.c read path).                            */
/*                                                                      */
/* UNLIKE eMMC: raising an SD card to its Default Speed range (0-25    */
/* MHz) needs NO CMD6 SWITCH_FUNC negotiation at all -- every SD card   */
/* supports the whole range unconditionally by spec. (SD's own         */
/* High-Speed mode, 50 MHz, DOES need a CMD6 SWITCH_FUNC handshake,     */
/* structurally different from eMMC's byte-indexed EXT_CSD CMD6 SWITCH  */
/* -- not attempted here; 25 MHz is the safe, negotiation-free step.)  */
/* So this is JUST emmc_bio.c's emmc_reclock() (CCU divider math is the */
/* same CCU IP, same field layout, only the mod-clock register address  */
/* differs: CCU_MMC0_CLK here vs CCU_MMC2_CLK there) plus a mandatory   */
/* post-reclock test read, with the same fail-safe-back-to-400kHz       */
/* contract: this function can only ever leave the card at a WORKING    */
/* clock, never a broken one, no matter how the PLL6=600MHz assumption  */
/* below turns out. */
#define CCU_MMC0_CLK_400K  0x8002000eu   /* == sd_bio_init()'s own literal */
#define CCU_MMC0_CLK_HS25  0x8101000Bu   /* PLL6(600MHz) | N=1 | M=11 -> 25.000 MHz, same math as emmc_bio.c's CCU_MMC2_CLK_HS25 */

#define SD_BC_HS_STATE  8   /* 0=never run, 1=25MHz active, 2=fell back */
#define SD_BC_HS_STEP   9   /* which step failed when [8]==2, 0 otherwise */
#define SD_HS_FAIL_RECLOCK   1
#define SD_HS_FAIL_TESTREAD  2

static int sd_reclock(uint32_t ccu_val)
{
	wreg(REG_CKCR, 0);
	if (clk_update() != 0)
		return -1;
	wr32(CCU_MMC0_CLK, ccu_val);
	wreg(REG_NTSR, rreg(REG_NTSR) | NTSR_MODE_SEL_NEW);
	wreg(REG_SAMP_DL, SAMP_DL_CAL_SW_EN);
	wreg(REG_CKCR, CKCR_CARD_CLK_EN);
	if (clk_update() != 0)
		return -1;
	return 0;
}

static int sd_hs_fallback(int fail_step)
{
	(void)sd_reclock(CCU_MMC0_CLK_400K);
	sdbc(SD_BC_HS_STATE, 2);
	sdbc(SD_BC_HS_STEP, (uint32_t)fail_step);
	return -1;
}

int sd_bio_set_highspeed(void)
{
	if (sd_reclock(CCU_MMC0_CLK_HS25) != 0)
		return sd_hs_fallback(SD_HS_FAIL_RECLOCK);

	/* Mandatory post-reclock test read of LBA 0 -- rc==0 is all that
	 * matters here (content is whatever a prior test left there); a bad
	 * sample point at the new clock shows up as sd_bio_read()'s own
	 * already-verified drain/DATA_OVER timeout, same as any other
	 * failure of that path. */
	if (sd_bio_read(0, (uint64_t)HVMAP_SD_TESTBUF) != 0)
		return sd_hs_fallback(SD_HS_FAIL_TESTREAD);

	sdbc(SD_BC_HS_STATE, 1);
	sdbc(SD_BC_HS_STEP, 0);
	return 0;
}

/* Real capacity in 512-byte sectors, parsed from the card's own CSD during
 * sd_bio_init() (CSD version 2.0 / SDHC-SDXC C_SIZE formula: (C_SIZE+1) *
 * 1024 sectors). Falls back to the old conservative 1 GiB stub if parsing
 * never ran (sd_bio_init() not yet called / failed) or the card reported a
 * CSD version other than 2.0 -- see the parse site's own comment for why
 * that should never happen for a real SDHC/SDXC card. Callers must not
 * call this before a successful sd_bio_init(). */
uint64_t sd_bio_capacity_sectors(void)
{
	return g_sd_capacity_sectors;
}

/* ------------------------------------------------------------------ */
/* sd_bio_read() — CMD17 single-block read (mirrors emmc_bio_read()).  */
/* ------------------------------------------------------------------ */
int sd_bio_read(uint32_t lba, uint64_t buf_pa)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	unsigned nwords = 0;
	int i;

	if (!g_sd_inited)
		return -100;

	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, 512);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD17_READ_CMDR);

	for (i = 0; i < SD_POLL_CAP && nwords < 128; i++) {
		uint32_t st = rreg(REG_STAR);
		if (st & STAR_FIFO_EMPTY) {
			if (rreg(REG_RINT) & RINT_DATA_OVER)
				break;
			continue;
		}
		buf[nwords++] = rreg(REG_FIFO);
	}
	if (nwords < 128) {
		sdbc(8, lba);
		sdbc(9, rreg(REG_RINT));
		sdbc(10, rreg(REG_STAR));
		sdbc(11, nwords);
		return -1;
	}

	/* Wait for the late DATA_OVER (CRC/end phase) before returning — same
	 * back-to-back-read hazard fix as emmc_bio_read(). */
	if (poll_rint(RINT_DATA_OVER, RINT_DATA_OVER) != 0) {
		sdbc(8, lba);
		sdbc(9, rreg(REG_RINT));
		sdbc(11, 0x10000u | nwords);
		return -1;
	}
	for (i = 0; i < SD_POLL_CAP; i++)
		if (!(rreg(REG_STAR) & STAR_CARD_BUSY))
			break;

	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* ------------------------------------------------------------------ */
/* sd_bio_write() — CMD24 single-block write (mirrors emmc_bio_write()).*/
/* CONFIRMED on silicon 2026-08-24 at 25 MHz (post sd_bio_set_highspeed()):*/
/* 45/45 direct write/read-back round trips matched, plus a guest-level   */
/* virtio dd write/read/md5 round trip. At the 400 kHz identification    */
/* clock this same function either timed out outright or reported rc==0 */
/* while the data never actually landed -- see sd_bio_set_highspeed()'s  */
/* own comment. Do not remove the reclock call thinking this comment     */
/* means the 400 kHz path was ever made reliable; it was not tried again.*/
/* ------------------------------------------------------------------ */
int sd_bio_write(uint32_t lba, uint64_t buf_pa)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	unsigned nwords = 0;
	int i;

	if (!g_sd_inited)
		return -100;

	__asm__ volatile("dsb sy" ::: "memory");

	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, 512);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD24_WRITE_CMDR);

	for (i = 0; i < SD_POLL_CAP && nwords < 128; i++) {
		uint32_t st = rreg(REG_STAR);
		if (st & STAR_FIFO_FULL)
			continue;
		wreg(REG_FIFO, buf[nwords++]);
	}
	if (nwords < 128)
		return -1;

	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(SD_WRITE_DATA_TIMEOUT_MS);
		uint32_t ri;
		for (;;) {
			ri = rreg(REG_RINT);
			if (ri & RINT_DATA_OVER)
				break;
			if (ri & 0x0180u)   /* DATA_CRC / DATA_TIMEOUT */
				return (int)(0x40000000u | (ri & 0x3fffu));
			if (rd_cntpct() - start > cap)
				return (int)(0x20000000u | (rreg(REG_RINT) & 0x3fffu));
		}
	}
	/* Mirror of emmc_bio_write()'s CARD_BUSY fix: the post-write flash
	 * program time is a genuinely longer latency than the read-side polls,
	 * so use the same generous TIME cap (D4 fix) as the DATA_OVER wait
	 * above, and return a real timeout error instead of unconditionally
	 * claiming success (which would let the next command hit a still-
	 * programming card). */
	{
		uint64_t start = rd_cntpct();
		uint64_t cap = ms_to_ticks(SD_WRITE_BUSY_TIMEOUT_MS);
		for (;;) {
			if ((rreg(REG_STAR) & STAR_CARD_BUSY) == 0)
				break;
			if (rd_cntpct() - start > cap)
				return -2;   /* card never signaled program-done */
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* sd_bio_write_multi() — CMD25 multi-block write, auto CMD12.          */
/*                                                                      */
/* WHY: one CMD24 per sector pays the card's whole program latency per  */
/* 512 bytes; measured 343 KB/s sustained on the guest's /opt and /var  */
/* (2026-09-26), with each vtbd1 write stalling its vCPU's trap for     */
/* ~1.5 ms a sector. One CMD25 for a run of sectors pays it once.       */
/*                                                                      */
/* The controller sends the CMD12 itself (CMDR bit 12, the same         */
/* AUTO_STOP aw_mmc(4) uses on this IP for mmc_da's multi-block I/O)    */
/* and raises RINT AUTO_STOP_DONE. Any error: stop the card by hand,    */
/* reset the FIFO, wait out busy, and return non-zero -- the caller     */
/* then rewrites the same run one CMD24 at a time, so a failed CMD25    */
/* can cost speed but never data.                                       */
/* ------------------------------------------------------------------ */
#define CMDR_AUTO_STOP        0x00001000u
#define CMDR_STOP_ABORT       0x00004000u
#define RINT_AUTO_STOP_DONE   0x00004000u
/* RESP_ERR|RESP_CRC|DATA_CRC|RESP_TO|DATA_TO|FIFO_RUN|HW_LOCK|START|END,
 * aw_mmc's AW_MMC_INT_ERR_BIT plus DATA_TIMEOUT */
#define RINT_ERR_MASK         0x0000BBC2u
#define CMD25_WRITE_CMDR      (0x80002759u | CMDR_AUTO_STOP)
#define CMD12_STOP_CMDR       (CMDR_LOAD | CMDR_STOP_ABORT | CMDR_CHK_CRC | \
                               CMDR_RESP_EXP | 12u)

static void sd_multi_abort(void)
{
	uint64_t start, cap = ms_to_ticks(SD_WRITE_BUSY_TIMEOUT_MS);

	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, 0);
	wreg(REG_CMDR, CMD12_STOP_CMDR);
	(void)poll_rint(RINT_CMD_DONE, RINT_CMD_DONE);
	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();
	start = rd_cntpct();
	while ((rreg(REG_STAR) & STAR_CARD_BUSY) && rd_cntpct() - start < cap)
		;
	wreg(REG_RINT, RINT_ALL);
}

/* sd_bio_read_multi() — CMD18 multi-block read, auto CMD12. Same error
 * contract as sd_bio_write_multi(): stop the card, reset the FIFO, return
 * non-zero; the caller re-reads the run one CMD17 at a time (the buffer's
 * partial contents are simply overwritten). */
#define CMD18_READ_CMDR       (0x80002352u | CMDR_AUTO_STOP)

int sd_bio_read_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	uint32_t nwords = 0, total = nblk * 128u, ri = 0;
	uint64_t start, cap;

	if (!g_sd_inited)
		return -100;
	if (nblk < 2 || nblk > SD_MULTI_MAX_BLOCKS)
		return -101;

	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, nblk * 512u);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD18_READ_CMDR);

	start = rd_cntpct();
	cap = ms_to_ticks(SD_WRITE_DATA_TIMEOUT_MS);
	while (nwords < total) {
		if ((rreg(REG_STAR) & STAR_FIFO_EMPTY) == 0) {
			buf[nwords++] = rreg(REG_FIFO);
			continue;
		}
		ri = rreg(REG_RINT);
		if ((ri & RINT_ERR_MASK) || rd_cntpct() - start > cap)
			goto fail;
	}

	start = rd_cntpct();
	for (;;) {
		ri = rreg(REG_RINT);
		if (ri & RINT_ERR_MASK)
			goto fail;
		if ((ri & (RINT_DATA_OVER | RINT_AUTO_STOP_DONE)) ==
		    (RINT_DATA_OVER | RINT_AUTO_STOP_DONE))
			break;
		if (rd_cntpct() - start > cap)
			goto fail;
	}
	start = rd_cntpct();
	while (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (rd_cntpct() - start > cap) {
			ri = STAR_CARD_BUSY;
			goto fail;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail:
	sd_multi_abort();
	return (int)(0x40000000u | ((nwords & 0x3fffu) << 16) | (ri & 0xffffu));
}

int sd_bio_write_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	volatile uint32_t *buf = (volatile uint32_t *)(unsigned long)buf_pa;
	uint32_t nwords = 0, total = nblk * 128u, ri = 0;
	uint64_t start, cap;

	if (!g_sd_inited)
		return -100;
	if (nblk < 2 || nblk > SD_MULTI_MAX_BLOCKS)
		return -101;

	__asm__ volatile("dsb sy" ::: "memory");

	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, nblk * 512u);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD25_WRITE_CMDR);

	start = rd_cntpct();
	cap = ms_to_ticks(SD_WRITE_DATA_TIMEOUT_MS);
	while (nwords < total) {
		if ((rreg(REG_STAR) & STAR_FIFO_FULL) == 0) {
			wreg(REG_FIFO, buf[nwords++]);
			continue;
		}
		ri = rreg(REG_RINT);
		if ((ri & RINT_ERR_MASK) || rd_cntpct() - start > cap)
			goto fail;
	}

	/* DATA_OVER and the controller's own CMD12 */
	start = rd_cntpct();
	for (;;) {
		ri = rreg(REG_RINT);
		if (ri & RINT_ERR_MASK)
			goto fail;
		if ((ri & (RINT_DATA_OVER | RINT_AUTO_STOP_DONE)) ==
		    (RINT_DATA_OVER | RINT_AUTO_STOP_DONE))
			break;
		if (rd_cntpct() - start > cap)
			goto fail;
	}

	/* CMD12 is R1b: the card holds DAT0 low while it programs */
	start = rd_cntpct();
	cap = ms_to_ticks(SD_WRITE_BUSY_TIMEOUT_MS);
	while (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (rd_cntpct() - start > cap) {
			ri = STAR_CARD_BUSY;   /* busy never cleared */
			goto fail;
		}
	}
	return 0;

fail:
	/* RINT in the low bits, the words that made it into the FIFO above;
	 * the caller keeps the record (sd_bio's own window is full) */
	sd_multi_abort();
	return (int)(0x40000000u | ((nwords & 0x3fffu) << 16) | (ri & 0xffffu));
}

/* ------------------------------------------------------------------ */
/* sd_bio_read_dma() / sd_bio_write_dma() -- IDMAC descriptor-chain DMA */
/* ------------------------------------------------------------------ */
/* Same controller IP as emmc_bio.c's SMHC2/eMMC (confirmed: byte-identical
 * CMD17/CMD24 CMDR encoding across both files), just a different MMIO base
 * (SD_BASE, SMHC0) -- so the IDMAC register map, descriptor format and the
 * enable/reset/wait sequence are the SAME ones emmc_bio_read_dma()/
 * write_dma() already hardware-validated, transcribed here against SD_BASE
 * instead. See that file's own header comment for the full derivation
 * (verbatim from FreeBSD's aw_mmc.c, this project's standing reference for
 * anything not already covered by a hardware-verified Python sequence). */
#define REG_FWLR   0x40u
#define REG_DMAC   0x80u
#define REG_DLBA   0x84u
#define REG_IDST   0x88u
#define REG_IDIE   0x8Cu

#define GCTL_FIFO_AC_MOD   0x80000000u
#define GCTL_DMA_ENB       0x00000020u

#define DMAC_IDMAC_SOFT_RST   0x00000001u
#define DMAC_IDMAC_FIX_BURST  0x00000002u
#define DMAC_IDMAC_IDMA_ON    0x00000080u

#define IDST_TX_INT        0x00000001u
#define IDST_RX_INT        0x00000002u
#define IDST_FATAL_BERR    0x00000004u
#define IDST_DES_UNAVL     0x00000010u
#define IDST_ERR_FLAG_SUM  0x00000020u
#define IDST_ABN_INT_SUM   0x00000200u
#define IDST_ERROR   (IDST_FATAL_BERR | IDST_ERR_FLAG_SUM | \
                      IDST_DES_UNAVL | IDST_ABN_INT_SUM)

#define DESC_DIC   0x00000002u
#define DESC_LD    0x00000004u
#define DESC_FD    0x00000008u
#define DESC_CH    0x00000010u
#define DESC_ER    0x00000020u
#define DESC_OWN   0x80000000u

#define SD_DMA_FTRGLEVEL  0x20070008u

#define SD_DMA_SEG_BYTES    0x2000u
#define SD_DMA_DESC_COUNT   ((SD_MULTI_MAX_BLOCKS * 512u + \
                              SD_DMA_SEG_BYTES - 1u) / SD_DMA_SEG_BYTES)

struct sd_dma_desc {
	uint32_t config;
	uint32_t buf_size;
	uint32_t buf_addr;
	uint32_t next;
};
static struct sd_dma_desc g_sd_dma_desc[SD_DMA_DESC_COUNT]
	__attribute__((aligned(64)));
#define SD_DMA_DESC_PA  ((uint64_t)(uintptr_t)&g_sd_dma_desc[0])

/* Clean+invalidate [pa, pa+len) to PoC -- see emmc_bio.c's emmc_dma_cmo()
 * for the full rationale (same one applies here verbatim: the IDMAC is a
 * real bus-master, not this CPU, so its reads/writes need explicit cache
 * maintenance that a same-core PIO load/store never did). */
static void sd_dma_cmo(uint64_t pa, uint32_t len)
{
	uint64_t p = pa & ~63ULL;
	uint64_t end = pa + len;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t sd_dma_build_desc(uint64_t buf_pa, uint32_t total_bytes)
{
	uint32_t ndesc = 0, done = 0;

	while (done < total_bytes) {
		uint32_t seg = total_bytes - done;
		if (seg > SD_DMA_SEG_BYTES)
			seg = SD_DMA_SEG_BYTES;
		g_sd_dma_desc[ndesc].buf_size = seg;
		g_sd_dma_desc[ndesc].buf_addr = (uint32_t)(buf_pa + done);
		g_sd_dma_desc[ndesc].config = DESC_CH | DESC_OWN | DESC_DIC;
		g_sd_dma_desc[ndesc].next = (uint32_t)(SD_DMA_DESC_PA +
			(uint64_t)(ndesc + 1) * sizeof(struct sd_dma_desc));
		done += seg;
		ndesc++;
	}
	g_sd_dma_desc[0].config |= DESC_FD;
	g_sd_dma_desc[ndesc - 1].config |= DESC_LD | DESC_ER;
	g_sd_dma_desc[ndesc - 1].config &= ~(uint32_t)DESC_DIC;
	g_sd_dma_desc[ndesc - 1].next = 0;

	sd_dma_cmo(SD_DMA_DESC_PA, ndesc * (uint32_t)sizeof(struct sd_dma_desc));
	return ndesc;
}

static void sd_dma_arm(void)
{
	uint32_t gctl = rreg(REG_GCTL);

	wreg(REG_GCTL, gctl | GCTL_FIFO_RST | GCTL_DMA_RST);
	small_delay();

	gctl = rreg(REG_GCTL);
	gctl &= ~GCTL_FIFO_AC_MOD;
	gctl |= GCTL_DMA_ENB;
	wreg(REG_GCTL, gctl);

	wreg(REG_DMAC, DMAC_IDMAC_SOFT_RST);
	small_delay();
	wreg(REG_DMAC, DMAC_IDMAC_IDMA_ON | DMAC_IDMAC_FIX_BURST);
	wreg(REG_IDIE, rreg(REG_IDIE) | IDST_RX_INT | IDST_TX_INT);
	wreg(REG_DLBA, (uint32_t)SD_DMA_DESC_PA);
	wreg(REG_FWLR, SD_DMA_FTRGLEVEL);
	wreg(REG_IDST, 0xFFFFFFFFu);
}

/* Restore plain AHB/PIO FIFO-access mode so every other function in this
 * file (sd_bio_read/write, *_multi, sd_set_hs50's own test read) keeps
 * working unmodified -- none of them expect GCTL_DMA_ENB set. */
static void sd_dma_disarm(void)
{
	uint32_t gctl = rreg(REG_GCTL);
	gctl |= GCTL_FIFO_AC_MOD;
	gctl &= ~GCTL_DMA_ENB;
	wreg(REG_GCTL, gctl);
	wreg(REG_DMAC, 0);
}

static uint32_t g_sd_dma_irqs;   /* sdbc[1]: real IDMAC completion IRQs
                                  * observed (gic_timer.c's SD_DMA_IRQ_INTID
                                  * arm) -- diagnostic only, see
                                  * emmc_bio.c's g_dma_irqs comment */

/* Completion owner (vblk_sd.c) -- same contract as emmc_bio.c's hook. */
static void (*volatile g_sd_dma_done_hook)(uint32_t spin_us);

void sd_bio_set_dma_done_hook(void (*fn)(uint32_t spin_us))
{
	g_sd_dma_done_hook = fn;
}

void sd_bio_dma_irq_note(void)
{
	void (*fn)(uint32_t) = g_sd_dma_done_hook;

	sdbc(1, ++g_sd_dma_irqs);
	if (fn)
		fn(SD_DMA_IRQ_SPIN_US);
}

void sd_bio_dma_tick(void)
{
	void (*fn)(uint32_t) = g_sd_dma_done_hook;

	if (fn)
		fn(0);
}

/* WFI between iterations, same rationale and same unchanged correctness
 * logic as emmc_dma_wait_complete() -- see its 2026-09-30 comment. */
static int sd_dma_wait_complete(uint32_t is_read, uint32_t *ri_out)
{
	uint64_t start = rd_cntpct();
	uint64_t cap = ms_to_ticks(SD_WRITE_DATA_TIMEOUT_MS);
	uint32_t want = is_read ? IDST_RX_INT : IDST_TX_INT;

	for (;;) {
		uint32_t idst = rreg(REG_IDST);
		uint32_t ri = rreg(REG_RINT);

		if ((idst & IDST_ERROR) || (ri & RINT_ERR_MASK)) {
			*ri_out = ri;
			return -1;
		}
		if ((idst & want) && (ri & RINT_DATA_OVER)) {
			wreg(REG_IDST, idst);
			*ri_out = ri;
			return 0;
		}
		if (rd_cntpct() - start > cap) {
			*ri_out = ri;
			return -1;
		}
		__asm__ volatile("wfi" ::: "memory");
	}
}

int sd_bio_read_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	uint32_t total, ndesc, ri = 0;
	uint64_t start, cap;

	if (!g_sd_inited)
		return -100;
	if (nblk < 2 || nblk > SD_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;
	/* Whole cache lines only -- see emmc_bio_read_dma(): a partial edge
	 * line shared with data another core dirties mid-transfer gets written
	 * back over the DMA'd bytes. The caller falls back to PIO. */
	if ((buf_pa | total) & 63u)
		return -102;

	ndesc = sd_dma_build_desc(buf_pa, total);
	sd_dma_cmo(buf_pa, total);   /* IDMAC is about to WRITE buf_pa */
	sd_dma_arm();
	(void)ndesc;

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD18_READ_CMDR);

	if (sd_dma_wait_complete(1, &ri) != 0)
		goto fail;

	start = rd_cntpct();
	cap = ms_to_ticks(SD_WRITE_BUSY_TIMEOUT_MS);
	while (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (rd_cntpct() - start > cap) {
			ri = STAR_CARD_BUSY;
			goto fail;
		}
	}

	sd_dma_disarm();
	sd_dma_cmo(buf_pa, total);   /* publish the IDMAC's write */
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;

fail:
	sd_dma_disarm();
	sd_multi_abort();
	return (int)(0x40000000u | (ri & 0xffffu));
}

int sd_bio_write_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk)
{
	uint32_t total, ndesc, ri = 0;
	uint64_t start, cap;

	if (!g_sd_inited)
		return -100;
	if (nblk < 2 || nblk > SD_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;

	__asm__ volatile("dsb sy" ::: "memory");

	ndesc = sd_dma_build_desc(buf_pa, total);
	sd_dma_cmo(buf_pa, total);   /* IDMAC is about to READ buf_pa */
	sd_dma_arm();
	(void)ndesc;

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));
	wreg(REG_CMDR, CMD25_WRITE_CMDR);

	if (sd_dma_wait_complete(0, &ri) != 0)
		goto fail;

	start = rd_cntpct();
	cap = ms_to_ticks(SD_WRITE_BUSY_TIMEOUT_MS);
	while (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (rd_cntpct() - start > cap) {
			ri = STAR_CARD_BUSY;
			goto fail;
		}
	}

	sd_dma_disarm();
	return 0;

fail:
	sd_dma_disarm();
	sd_multi_abort();
	return (int)(0x40000000u | (ri & 0xffffu));
}

/* Split-phase form of sd_bio_read_dma()/write_dma() -- the SD mirror of
 * emmc_bio_dma_start()/poll() (see emmc_bio.c for the design). Same
 * sequence, caps and failure handling (sd_multi_abort, 0x4000xxxx code) as
 * the synchronous pair above, cut at the CMDR write; the only addition is
 * emmc's two-poll confirmation of a timeout, since a poll here is one
 * sample, not a loop. Caller holds the SD lock from start until poll()
 * returns != SD_DMA_RUNNING. */
#define SADMA_DATA 0u
#define SADMA_BUSY 1u
static struct {
	volatile uint32_t active;
	uint32_t is_read, phase, total, over;
	uint64_t buf_pa, t0;
} g_sadma;

int sd_bio_dma_start(uint32_t is_read, uint32_t lba, uint64_t buf_pa,
                     uint32_t nblk)
{
	uint32_t total;

	if (!g_sd_inited)
		return -100;
	if (g_sadma.active)
		return -103;
	if (nblk < 2 || nblk > SD_MULTI_MAX_BLOCKS)
		return -101;
	total = nblk * 512u;
	if (is_read && ((buf_pa | total) & 63u))
		return -102;

	__asm__ volatile("dsb sy" ::: "memory");
	(void)sd_dma_build_desc(buf_pa, total);
	sd_dma_cmo(buf_pa, total);
	sd_dma_arm();

	wreg(REG_BKSR, 512);
	wreg(REG_BYCR, total);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, sd_addr(lba));

	g_sadma.is_read = is_read ? 1u : 0u;
	g_sadma.phase = SADMA_DATA;
	g_sadma.total = total;
	g_sadma.over = 0;
	g_sadma.buf_pa = buf_pa;
	g_sadma.t0 = rd_cntpct();
	g_sadma.active = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	wreg(REG_CMDR, is_read ? CMD18_READ_CMDR : CMD25_WRITE_CMDR);
	return 0;
}

static int sadma_timed_out(uint32_t ms)
{
	uint64_t now = rd_cntpct();
	uint64_t el = (now >= g_sadma.t0) ? (now - g_sadma.t0) : 0ull;

	if (el <= ms_to_ticks(ms)) {
		g_sadma.over = 0;
		return 0;
	}
	return ++g_sadma.over >= 2u;
}

static int sadma_step(void)
{
	uint32_t ri = 0;

	if (g_sadma.phase == SADMA_DATA) {
		uint32_t want = g_sadma.is_read ? IDST_RX_INT : IDST_TX_INT;
		uint32_t idst = rreg(REG_IDST);

		ri = rreg(REG_RINT);
		if ((idst & IDST_ERROR) || (ri & RINT_ERR_MASK))
			goto fail;
		if (!((idst & want) && (ri & RINT_DATA_OVER))) {
			if (sadma_timed_out(SD_WRITE_DATA_TIMEOUT_MS))
				goto fail;
			return 1;
		}
		wreg(REG_IDST, idst);   /* W1C: drops the level line */
		g_sadma.phase = SADMA_BUSY;
		g_sadma.over = 0;
		g_sadma.t0 = rd_cntpct();
	}

	if (rreg(REG_STAR) & STAR_CARD_BUSY) {
		if (sadma_timed_out(SD_WRITE_BUSY_TIMEOUT_MS)) {
			ri = STAR_CARD_BUSY;
			goto fail;
		}
		return 1;
	}

	sd_dma_disarm();
	if (g_sadma.is_read)
		sd_dma_cmo(g_sadma.buf_pa, g_sadma.total);
	__asm__ volatile("dsb sy" ::: "memory");
	g_sadma.active = 0;
	return 0;

fail:
	sd_dma_disarm();
	sd_multi_abort();
	g_sadma.active = 0;
	return (int)(0x40000000u | (ri & 0xffffu));
}

int sd_bio_dma_poll(uint32_t spin_us)
{
	uint64_t start = rd_cntpct();
	uint64_t cap = ms_to_ticks(1) * spin_us / 1000ull;

	if (!g_sadma.active)
		return -103;
	for (;;) {
		int rc = sadma_step();
		uint64_t now;

		if (rc != 1)
			return rc;
		now = rd_cntpct();
		if (now < start || now - start >= cap)
			return 1;
	}
}

/* ------------------------------------------------------------------ */
/* sd_set_hs50() — SD High Speed (50 MHz), FAIL-SAFE.                   */
/*                                                                      */
/* WHY: at 25 MHz x 4 bits the bus tops out at 12.5 MB/s and reads      */
/* already measured 11.25 MB/s (2026-09-26). High Speed doubles the     */
/* bus; it needs a CMD6 SWITCH_FUNC (mode 1, group 1 -> 1) the card     */
/* must accept (status bits 379:376 == 1), then the host clock raised.  */
/* UHS modes need 1.8 V signalling this slot does not have.             */
/*                                                                      */
/* Proof before keeping it, BOTH directions: LBA 0 must read back       */
/* identical to its 25 MHz copy, and the black-box sector (SDBOX_LBA,   */
/* 64) is rewritten with its own contents at 50 MHz and read back.      */
/* Any failure: back to 25 MHz (an HS-switched card still runs Default  */
/* Speed clocks) and LBA 64 rewritten there, so the box is never left   */
/* with a bad 50 MHz write. SD window [1]: 50 = active, 0x20|step =     */
/* stayed at 25 MHz.                                                    */
/* ------------------------------------------------------------------ */
#define CCU_MMC0_CLK_HS50   0x8100000Bu   /* PLL6(600MHz) | N=0 | M=11 -> 50 MHz */
#define CMD6_SWITCH_CMDR    0x80002346u   /* LOAD|WAIT_PRE|DATA|CRC|RESP|6 */
#define SD_BC_HS50          1
#define SD_HS50_LBA_BOX     64u

static int sd_hs50_fallback(uint32_t step, const uint32_t *box)
{
	(void)sd_reclock(CCU_MMC0_CLK_HS25);
	if (box != NULL)
		(void)sd_bio_write(SD_HS50_LBA_BOX, (uint64_t)(uintptr_t)box);
	sdbc(SD_BC_HS50, 0x20u | step);
	return -1;
}

static int sd_set_hs50(void)
{
	static uint32_t ref0[128], box[128], chk[128];
	uint32_t st[16], nwords = 0, grp1;
	int i;

	if (sd_bio_read(0, (uint64_t)(uintptr_t)ref0) != 0 ||
	    sd_bio_read(SD_HS50_LBA_BOX, (uint64_t)(uintptr_t)box) != 0)
		return sd_hs50_fallback(1, NULL);

	/* CMD6 mode 1 (set), group 1 = 1 (High Speed), others unchanged */
	wreg(REG_GCTL, rreg(REG_GCTL) | GCTL_FIFO_RST);
	small_delay();
	wreg(REG_BKSR, 64);
	wreg(REG_BYCR, 64);
	wreg(REG_RINT, RINT_ALL);
	wreg(REG_CAGR, 0x80FFFFF1u);
	wreg(REG_CMDR, CMD6_SWITCH_CMDR);
	for (i = 0; i < SD_POLL_CAP && nwords < 16; i++) {
		if (rreg(REG_STAR) & STAR_FIFO_EMPTY) {
			if (rreg(REG_RINT) & RINT_ERR_MASK)
				break;
			continue;
		}
		st[nwords++] = rreg(REG_FIFO);
	}
	if (nwords < 16 || poll_rint(RINT_DATA_OVER, RINT_DATA_OVER) != 0)
		return sd_hs50_fallback(2, NULL);
	/* 512-bit status, MSB first on the wire; the FIFO hands it over as
	 * little-endian words, so byte 16 of the block (bits 379:376 in its
	 * low nibble) is the low byte of st[4]. */
	grp1 = st[4] & 0x0Fu;
	if (grp1 != 1u)
		return sd_hs50_fallback(0x10u | grp1, NULL);

	/* the card switches within 8 clocks of the status block's end */
	small_delay();
	if (sd_reclock(CCU_MMC0_CLK_HS50) != 0)
		return sd_hs50_fallback(3, NULL);

	if (sd_bio_read(0, (uint64_t)(uintptr_t)chk) != 0)
		return sd_hs50_fallback(4, NULL);
	for (i = 0; i < 128; i++)
		if (chk[i] != ref0[i])
			return sd_hs50_fallback(5, NULL);
	if (sd_bio_write(SD_HS50_LBA_BOX, (uint64_t)(uintptr_t)box) != 0)
		return sd_hs50_fallback(6, box);
	for (i = 0; i < 128; i++)
		chk[i] = 0;
	if (sd_bio_read(SD_HS50_LBA_BOX, (uint64_t)(uintptr_t)chk) != 0)
		return sd_hs50_fallback(7, box);
	for (i = 0; i < 128; i++)
		if (chk[i] != box[i])
			return sd_hs50_fallback(8, box);

	sdbc(SD_BC_HS50, 50);
	return 0;
}
