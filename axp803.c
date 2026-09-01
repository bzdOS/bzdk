/* SPDX-License-Identifier: BSD-2-Clause */

/* axp803.c — AXP803 PMIC battery telemetry over RSB. Implements axp803.h.
 * See axp803.h for the full citation/confidence breakdown per field — do not
 * strip those comments when editing, they are the record of what is/isn't
 * datasheet-verified for the next person who touches this.
 *
 * Freestanding, no libc, bounded (every rsb_* call is itself iteration-capped
 * in rsb.c; nothing here adds an unbounded loop). Breadcrumb "AXP1" latches
 * axp803_init()'s outcome so a host can see WHY the chip isn't reporting
 * even mid-wedge, same convention as sd_bio.c's SDBC / bmc.c's BMC1.
 */
#include <stdint.h>
#include "rsb.h"
#include "axp803.h"

/* U-Boot include/axp_pmic.h: the fixed RSB hardware address / assigned
 * runtime address for the "primary position" PMIC on every sun8i/A64 board.
 * See axp803.h's header comment for the datasheet's differing §9.9 note and
 * why we trust the U-Boot value instead. */
#define AXP803_HWADDR      0x3a3u
#define AXP803_RUNTIME     0x2du

/* AXP803 datasheet Rev 1.0 register addresses (Table 10-1 Register List). */
#define AXP803_REG_PWR_STATUS   0x00u   /* "Power source status" (R)         */
#define AXP803_REG_CHG_STATUS   0x01u   /* "Power mode and Charger status" (R)*/
#define AXP803_REG_IC_TYPE      0x03u   /* "IC type no." (R)                 */
#define AXP803_REG_TS_H         0x58u   /* TS pin ADC, highest 8 bit          */
#define AXP803_REG_TS_L         0x59u   /* TS pin ADC, lowest 4 bit           */
#define AXP803_REG_VBAT_H       0x78u   /* Average Battery voltage bit[11:4]  */
#define AXP803_REG_VBAT_L       0x79u   /* Average Battery voltage bit[3:0]   */
#define AXP803_REG_ICHG_H       0x7Au   /* Average Battery charge I bit[11:4] */
#define AXP803_REG_ICHG_L       0x7Bu   /* Average Battery charge I bit[3:0]  */
#define AXP803_REG_IDISCHG_H    0x7Cu   /* Average Battery discharge I [11:4] */
#define AXP803_REG_IDISCHG_L    0x7Du   /* Average Battery discharge I [3:0]  */

/* REG03H "IC type no.": bits[7:6] and bits[3:0] concatenate to a 6-bit code
 * 0b010001 for AXP803; bits[5:4] are documented "reserved (uncertain)" so we
 * mask them out of the comparison. Expected byte (masked) = 0b01000001. */
#define AXP803_ICTYPE_MASK      0xCFu
#define AXP803_ICTYPE_EXPECT    0x41u

static int g_axp803_ok;   /* 1 once rsb_init + set_device_address + REG03H check all pass */

/* Breadcrumb: last axp803_init() outcome, for `r 0x50006200 4` diagnosis
 * even if the console text path is unavailable. Clear of BMC1's 64-word
 * window (0x50006000-0x500060ff) and PROF's window (0x50006800-...). */
#define AXP1_BC_BASE   0x50006200UL
#define AXP1_MAGIC     0x41585031u   /* "AXP1" */

/* OWNER MARKER — linker-level mutual exclusion for the PMIC/AXP803 block
 * (same pattern as vcpu3.c/zguest_cpu3.c's bzdos_cpu3_owner). EL2 owns
 * the AXP803 per SPEC_chimp_hal §2 — this marker catches any future
 * alternative PMIC driver linked alongside.
 * Makefile exclusion: axp803.o is in repl/dbg/dual/zephyr targets but
 * never alongside an alternative PMIC implementation. */
const char *const bzdos_pmic_owner = "axp803";

static void axp1_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(AXP1_BC_BASE + (unsigned long)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

int axp803_init(void)
{
	uint8_t ic = 0;
	int rc;

	g_axp803_ok = 0;
	axp1_bc(0, AXP1_MAGIC);

	rc = rsb_init();
	if (rc != 0) { axp1_bc(1, (uint32_t)-1); axp1_bc(2, (uint32_t)rc); return -1; }

	rc = rsb_set_device_address((uint16_t)AXP803_HWADDR, (uint8_t)AXP803_RUNTIME);
	if (rc != 0) { axp1_bc(1, (uint32_t)-2); axp1_bc(2, (uint32_t)rc); return -2; }

	rc = rsb_read((uint8_t)AXP803_RUNTIME, AXP803_REG_IC_TYPE, &ic);
	if (rc != 0) { axp1_bc(1, (uint32_t)-3); axp1_bc(2, (uint32_t)rc); return -3; }

	axp1_bc(3, (uint32_t)ic);
	if ((ic & AXP803_ICTYPE_MASK) != AXP803_ICTYPE_EXPECT) {
		axp1_bc(1, (uint32_t)-4);
		return -4;   /* RSB talked to *something*, but not a REG03H-matching AXP803 */
	}

	g_axp803_ok = 1;
	axp1_bc(1, 0);
	return 0;
}

/* Read one averaged 12-bit ADC register pair: hi = bits[11:4] (full byte),
 * lo = bits[3:0] (low nibble of the low register) — the packing every
 * register pair in Table 9-30/10-1 uses (BATSENSE/charge-I/discharge-I/TS).
 * Returns 0 and fills *out12 on success; leaves *out12 at 0 and returns
 * negative on an RSB read failure (bounded — rsb_read() is itself capped). */
static int axp803_read_adc12(uint8_t reg_hi, uint8_t reg_lo, uint32_t *out12)
{
	uint8_t hi = 0, lo = 0;
	int rc;

	*out12 = 0;
	rc = rsb_read((uint8_t)AXP803_RUNTIME, reg_hi, &hi);
	if (rc != 0) return rc;
	rc = rsb_read((uint8_t)AXP803_RUNTIME, reg_lo, &lo);
	if (rc != 0) return rc;

	*out12 = ((uint32_t)hi << 4) | ((uint32_t)lo & 0xFu);
	return 0;
}

void axp803_read_health(struct axp803_health *out)
{
	uint32_t raw;
	uint8_t st0 = 0, st1 = 0;
	uint32_t status = 0;

	out->vbat_mv = 0;
	out->ichg_ma = 0;
	out->idischg_ma = 0;
	out->ts_mv = 0;
	out->status = 0;
	out->chip_ok = (uint32_t)g_axp803_ok;

	if (!g_axp803_ok)
		return;

	/* BATSENSE: 1.1 mV/step (Table 9-30). Integer mV, truncated (raw*11/10
	 * loses at most 0.5 mV vs the exact 1.1x — same "approx" spirit as
	 * bmc.c's existing SoC-temp affine conversion). */
	if (axp803_read_adc12(AXP803_REG_VBAT_H, AXP803_REG_VBAT_L, &raw) == 0)
		out->vbat_mv = (raw * 11u) / 10u;

	/* Charge / discharge current: 1 mA/step exactly (Table 9-30). */
	if (axp803_read_adc12(AXP803_REG_ICHG_H, AXP803_REG_ICHG_L, &raw) == 0)
		out->ichg_ma = raw;
	if (axp803_read_adc12(AXP803_REG_IDISCHG_H, AXP803_REG_IDISCHG_L, &raw) == 0)
		out->idischg_ma = raw;

	/* TS pin: 0.8 mV/step (Table 9-30). Raw millivolts only — see axp803.h
	 * on why this is NOT converted to a temperature. */
	if (axp803_read_adc12(AXP803_REG_TS_H, AXP803_REG_TS_L, &raw) == 0)
		out->ts_mv = (raw * 8u) / 10u;

	/* Status bitmap from REG00H/REG01H (see axp803.h for the AXP803_ST0/ST1
	 * bit names mapped to BMC_BATT_ flags below). A read failure on either
	 * leaves those bits clear rather than guessing. */
	if (rsb_read((uint8_t)AXP803_RUNTIME, AXP803_REG_PWR_STATUS, &st0) == 0) {
		if (st0 & AXP803_ST0_VBUS_PRESENT) status |= BMC_BATT_VBUS;
	}
	if (rsb_read((uint8_t)AXP803_RUNTIME, AXP803_REG_CHG_STATUS, &st1) == 0) {
		if (st1 & AXP803_ST1_BATT_PRESENT) status |= BMC_BATT_PRESENT;
		if (st1 & AXP803_ST1_CHARGING)     status |= BMC_BATT_CHARGING;
		if (st1 & AXP803_ST1_DIE_OVERTEMP) status |= BMC_BATT_DIE_HOT;
	}
	status |= BMC_BATT_CHIP_OK;
	out->status = status;

	axp1_bc(4, out->vbat_mv);
	axp1_bc(5, out->ichg_ma);
	axp1_bc(6, out->idischg_ma);
	axp1_bc(7, out->status);
}

/* ------------------------------------------------------------------ */
/* DC1SW ("vcc-phy") control — see axp803.h for the full citation and    */
/* safety contract. ONLY bit 7 of OUTPUT_CTRL2 (0x12) is ever written,   */
/* read-modify-write, with a readback verify after every write.          */
/* ------------------------------------------------------------------ */
#define AXP803_REG_OUTPUT_CTRL2  0x12u
#define AXP803_OUT_CTRL2_SW_EN   0x80u

int axp803_dc1sw(int on, uint8_t *prev)
{
	uint8_t ctl = 0;

	/* Defensive: re-assign the runtime address mapping (idempotent) so the
	 * verb works even if axp803_init() never ran on this boot. */
	if (rsb_set_device_address((uint16_t)AXP803_HWADDR,
	                           (uint8_t)AXP803_RUNTIME) != 0)
		return -1;
	if (rsb_read((uint8_t)AXP803_RUNTIME, AXP803_REG_OUTPUT_CTRL2, &ctl) != 0)
		return -2;
	if (prev)
		*prev = ctl;

	{
		uint8_t want = (uint8_t)(on ? (ctl | AXP803_OUT_CTRL2_SW_EN)
		                            : (ctl & ~AXP803_OUT_CTRL2_SW_EN));
		if (want == ctl)
			return 0;                       /* already in the asked state */
		if (rsb_write((uint8_t)AXP803_RUNTIME, AXP803_REG_OUTPUT_CTRL2,
		              want) != 0)
			return -3;
		ctl = 0;
		if (rsb_read((uint8_t)AXP803_RUNTIME, AXP803_REG_OUTPUT_CTRL2,
		             &ctl) != 0)
			return -4;
		if (ctl != want)
			return -5;                      /* readback mismatch */
	}
	return 0;
}
