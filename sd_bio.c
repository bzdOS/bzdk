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

/* ------------------------------------------------------------------ */
/* Physical bases (SD-specific)                                        */
/* ------------------------------------------------------------------ */
#define SD_BASE          0x01c0f000UL   /* SMHC0/aw_mmc0 (SD card) controller */
#define PIO_PF_CFG0      0x01C208B4UL   /* Port F config reg 0 (PF0..PF7)     */
#define CCU_MMC0_CLK     0x01c20088UL   /* CCU MMC0_CLK gate/divider          */

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

static void sdbc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(SDBC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Card type detected by sd_bio_init(); 1 = SDHC/SDXC (block addressing). */
static int g_sd_block_addressed;
static int g_sd_inited;

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

	/* CMD9 SEND_CSD (R2, long), rca<<16. */
	if (sd_cmd_done(9, rca << 16, CMDR_LONG_RESP | CMDR_RESP_EXP, NULL) != 0) {
		sdbc(0, (uint32_t)-8); return -8;
	}

	/* CMD7 SELECT_CARD (R1b), rca<<16. */
	if (sd_cmd_done(7, rca << 16, CMDR_RESP_EXP, NULL) != 0) { sdbc(0, (uint32_t)-9); return -9; }

	/* CMD16 SET_BLOCKLEN 512 (harmless/ignored for SDHC block length). */
	if (sd_cmd_done(16, 512, CMDR_RESP_EXP, NULL) != 0) { sdbc(0, (uint32_t)-10); return -10; }

	g_sd_inited = 1;
	sdbc(0, 0);
	return 0;
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
/* Structurally correct; verify with a read-back compare.              */
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
		uint32_t i2, ri;
		for (i2 = 0; i2 < 30000000u; i2++) {
			ri = rreg(REG_RINT);
			if (ri & RINT_DATA_OVER)
				break;
			if (ri & 0x0180u)   /* DATA_CRC / DATA_TIMEOUT */
				return (int)(0x40000000u | (ri & 0x3fffu));
		}
		if (i2 >= 30000000u)
			return (int)(0x20000000u | (rreg(REG_RINT) & 0x3fffu));
	}
	/* Mirror of emmc_bio_write()'s CARD_BUSY fix: the post-write flash
	 * program time is a genuinely longer latency than the read-side polls,
	 * so use the same generous cap as the DATA_OVER wait above, and return a
	 * real timeout error instead of unconditionally claiming success (which
	 * would let the next command hit a still-programming card). */
	{
		uint32_t i2;
		for (i2 = 0; i2 < 30000000u; i2++)
			if ((rreg(REG_STAR) & STAR_CARD_BUSY) == 0)
				break;
		if (i2 >= 30000000u)
			return -2;   /* card never signaled program-done */
	}
	return 0;
}
