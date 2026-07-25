/* SPDX-License-Identifier: BSD-2-Clause */

/* rsb.c — Allwinner A64 RSB (Reduced Serial Bus) controller driver.
 * Implements rsb.h. See rsb.h for the full citation list (ported verbatim
 * from U-Boot's drivers/i2c/sun8i_rsb.c + arch-sunxi rsb.h/cpu_sun4i.h/
 * prcm_sun6i.h/sunxi_gpio.h) and the honesty notes on what is/isn't verified
 * without the board.
 *
 * Freestanding bare-metal AArch64, direct volatile MMIO on the flat
 * identity-mapped Device-nGnRE range U-Boot leaves the MMU with — same
 * contract as emmc_bio.c/sd_bio.c/bmc.c's THS read. All polls are
 * iteration-capped (RSB_POLL_CAP) so a bug here can never hang the CPU1
 * debug core; a bounded timeout returns a negative code instead of spinning.
 */
#include <stdint.h>
#include "rsb.h"

/* ------------------------------------------------------------------ */
/* Physical bases (A64 == cpu_sun4i.h branch of U-Boot's cpu.h: not sun9i,  */
/* not H6, not ncat2). Citations in rsb.h.                                  */
/* ------------------------------------------------------------------ */
#define RSB_BASE        0x01f03400UL   /* SUNXI_RSB_BASE  (cpu_sun4i.h)     */
#define PRCM_BASE       0x01f01400UL   /* SUNXI_PRCM_BASE (cpu_sun4i.h)     */
#define R_PIO_BASE      0x01f02c00UL   /* SUNXI_R_PIO_BASE (sunxi_gpio.h)   */

/* RSB controller register offsets — struct sunxi_rsb_reg (rsb.h in U-Boot). */
#define RSB_CTRL        0x00u
#define RSB_CCR         0x04u
#define RSB_INTE        0x08u
#define RSB_STAT        0x0Cu
#define RSB_ADDR        0x10u
#define RSB_DATA        0x1Cu
#define RSB_LCR         0x24u
#define RSB_DMCR        0x28u
#define RSB_CMD         0x2Cu
#define RSB_DEVADDR     0x30u

#define RSB_CTRL_SOFT_RST       (1u << 0)
#define RSB_CTRL_START_TRANS    (1u << 7)

#define RSB_STAT_TOVER_INT      (1u << 0)   /* transfer over (success)      */
#define RSB_STAT_TERR_INT       (1u << 1)   /* transfer error               */
#define RSB_STAT_LBSY_INT       (1u << 2)   /* bus busy                     */

#define RSB_DMCR_DEVICE_MODE_DATA   0x7c3e00u
#define RSB_DMCR_DEVICE_MODE_START  (1u << 31)

#define RSB_CMD_BYTE_WRITE      0x4Eu
#define RSB_CMD_BYTE_READ       0x8Bu
#define RSB_CMD_SET_RTSADDR     0xE8u

#define RSB_DEVADDR_RUNTIME(x)  ((uint32_t)(x) << 16)
#define RSB_DEVADDR_DEVICE(x)   ((uint32_t)(x) << 0)

/* PRCM APB0 clock-gate/reset register offsets + bit assignments
 * (prcm_sun6i.h: apb0_gate @ +0x28, apb0_reset @ +0xb0). */
#define PRCM_APB0_GATE          0x28u
#define PRCM_APB0_RESET         0xB0u
#define PRCM_APB0_GATE_PIO      (1u << 0)
#define PRCM_APB0_GATE_RSB      (1u << 3)

/* R_PIO bank-L (GPL0/GPL1) register layout — legacy (non-NEW_PINCTRL)
 * geometry A64 uses: 0x24 bytes/bank, cfg0 @+0x00, drv0 @+0x14, pull0 @+0x1c.
 * Bank L is the FIRST bank in the R_PIO domain on A64 (single-bank R_PIO),
 * so its bank base IS R_PIO_BASE (bank index 0 within this domain — see
 * BANK_TO_GPIO() in U-Boot's sunxi_gpio.c: bank -= SUNXI_GPIO_L). */
#define RPIO_L_CFG0     (R_PIO_BASE + 0x00u)
#define RPIO_L_DRV0     (R_PIO_BASE + 0x14u)
#define RPIO_L_PULL0    (R_PIO_BASE + 0x1Cu)

#define GPL_R_RSB       2u   /* SUN8I_GPL_R_RSB: GPL0/GPL1 pinmux function   */
#define GPIO_PULL_UP    1u

#define RSB_POLL_CAP    200000   /* bounded iteration cap, every poll below  */

/* ------------------------------------------------------------------ */
/* Raw MMIO helpers (sd_bio.c/emmc_bio.c style).                       */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(unsigned long pa) { return *(volatile uint32_t *)pa; }
static inline void wr32(unsigned long pa, uint32_t v) { *(volatile uint32_t *)pa = v; }

static inline uint32_t rreg(uint32_t off) { return rd32(RSB_BASE + off); }
static inline void wreg(uint32_t off, uint32_t v) { wr32(RSB_BASE + off, v); }

static int g_rsb_ready;

/* ------------------------------------------------------------------ */
/* GPIO pinmux — GPL0/GPL1 -> RSB function, pull-up, drive strength 2. */
/* Direct register poke (no shared gpio helper exists in this tree yet;    */
/* mirrors sd_bio.c's inline PIO_PF_CFG0 read-modify-write convention).    */
/* ------------------------------------------------------------------ */
static void rsb_pinmux(void)
{
	uint32_t v;

	/* cfg0: GPL0 -> bits[3:0], GPL1 -> bits[7:4], function 2 (RSB). */
	v = rd32(RPIO_L_CFG0);
	v = (v & ~0xFFu) | ((GPL_R_RSB) | (GPL_R_RSB << 4));
	wr32(RPIO_L_CFG0, v);

	/* pull0: GPL0 -> bits[1:0], GPL1 -> bits[3:2], pull-up (1). */
	v = rd32(RPIO_L_PULL0);
	v = (v & ~0xFu) | (GPIO_PULL_UP | (GPIO_PULL_UP << 2));
	wr32(RPIO_L_PULL0, v);

	/* drv0: GPL0 -> bits[1:0], GPL1 -> bits[3:2], drive level 2. */
	v = rd32(RPIO_L_DRV0);
	v = (v & ~0xFu) | (2u | (2u << 2));
	wr32(RPIO_L_DRV0, v);
}

/* Bounded wait for a transaction to finish; mirrors sun8i_rsb_await_trans().
 * Returns 0 on TOVER (success), -1 on TERR/LBSY, -2 on iteration timeout.
 * Clears the status bits on the way out either way (write-1-to-clear). */
static int rsb_await(void)
{
	int i;
	uint32_t stat = 0;
	int ret = -2;

	for (i = 0; i < RSB_POLL_CAP; i++) {
		stat = rreg(RSB_STAT);
		if (stat & RSB_STAT_LBSY_INT) { ret = -1; break; }
		if (stat & RSB_STAT_TERR_INT) { ret = -1; break; }
		if (stat & RSB_STAT_TOVER_INT) { ret = 0; break; }
	}
	wreg(RSB_STAT, stat);   /* write-1-to-clear whatever we saw */
	return ret;
}

static int rsb_do_trans(void)
{
	wreg(RSB_CTRL, rreg(RSB_CTRL) | RSB_CTRL_START_TRANS);
	return rsb_await();
}

/* ------------------------------------------------------------------ */
/* rsb_init() — controller + pin + clock bring-up + device-mode switch.*/
/* ------------------------------------------------------------------ */
int rsb_init(void)
{
	uint32_t div, cd_odly;
	int i;

	g_rsb_ready = 0;

	/* PRCM: enable + de-assert reset for the PIO and RSB APB0 gates. */
	wr32(PRCM_BASE + PRCM_APB0_GATE,
	     rd32(PRCM_BASE + PRCM_APB0_GATE) | PRCM_APB0_GATE_PIO | PRCM_APB0_GATE_RSB);
	wr32(PRCM_BASE + PRCM_APB0_RESET,
	     rd32(PRCM_BASE + PRCM_APB0_RESET) | PRCM_APB0_GATE_PIO | PRCM_APB0_GATE_RSB);

	rsb_pinmux();

	/* Soft reset the controller. */
	wreg(RSB_CTRL, RSB_CTRL_SOFT_RST);

	/* Clock: Hosc24M source, target 3 MHz (sun8i_rsb_set_clk()). */
	div = 24000000u / 3000000u / 2u - 1u;
	cd_odly = div >> 1;
	if (!cd_odly) cd_odly = 1u;
	wreg(RSB_CCR, (cd_odly << 8) | div);

	/* Device-mode switch: broadcast all RSB devices on the bus from I2C
	 * mode to RSB mode. Bounded wait for DMCR_START to self-clear, then
	 * the same await-completion poll as any other transaction. */
	wreg(RSB_DMCR, RSB_DMCR_DEVICE_MODE_START | RSB_DMCR_DEVICE_MODE_DATA);
	for (i = 0; i < RSB_POLL_CAP; i++)
		if ((rreg(RSB_DMCR) & RSB_DMCR_DEVICE_MODE_START) == 0)
			break;
	if (i >= RSB_POLL_CAP)
		return -1;

	if (rsb_await() != 0)
		return -2;

	g_rsb_ready = 1;
	return 0;
}

int rsb_set_device_address(uint16_t hw_addr, uint8_t runtime_addr)
{
	if (!g_rsb_ready)
		return -100;

	wreg(RSB_DEVADDR, RSB_DEVADDR_RUNTIME(runtime_addr) | RSB_DEVADDR_DEVICE(hw_addr));
	wreg(RSB_CMD, RSB_CMD_SET_RTSADDR);
	return rsb_do_trans();
}

int rsb_read(uint8_t runtime_addr, uint8_t reg, uint8_t *out)
{
	int ret;

	if (!g_rsb_ready)
		return -100;

	wreg(RSB_DEVADDR, RSB_DEVADDR_RUNTIME(runtime_addr));
	wreg(RSB_ADDR, reg);
	wreg(RSB_CMD, RSB_CMD_BYTE_READ);
	ret = rsb_do_trans();
	if (ret != 0)
		return ret;
	if (out)
		*out = (uint8_t)(rreg(RSB_DATA) & 0xFFu);
	return 0;
}

int rsb_write(uint8_t runtime_addr, uint8_t reg, uint8_t val)
{
	if (!g_rsb_ready)
		return -100;

	wreg(RSB_DEVADDR, RSB_DEVADDR_RUNTIME(runtime_addr));
	wreg(RSB_ADDR, reg);
	wreg(RSB_DATA, val);
	wreg(RSB_CMD, RSB_CMD_BYTE_WRITE);
	return rsb_do_trans();
}
