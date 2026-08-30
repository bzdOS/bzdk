/* SPDX-License-Identifier: BSD-2-Clause */

/* axp803.h — X-Powers AXP803 PMIC telemetry (battery voltage/current/status)
 * over the Allwinner RSB bus (rsb.c/rsb.h), for the bzdOS EL2 hypervisor.
 * ROADMAP milestone B1 ("software-BMC ... server with a real UPS").
 *
 * CITATION / CONFIDENCE (read before trusting a field on real hardware):
 *
 *   RSB transport + addressing  -- HIGH confidence. AXP_PMIC_PRI_DEVICE_ADDR
 *   (0x3a3) / AXP_PMIC_PRI_RUNTIME_ADDR (0x2d) are ported from U-Boot's
 *   include/axp_pmic.h — the exact constant every sun8i/A64 board's U-Boot
 *   SPL uses to address its "primary position" PMIC over RSB before this
 *   hypervisor ever runs (if wrong, DRAM regulators would never program and
 *   the board would not boot at all, so this pairing is about as proven as
 *   an undocumented protocol constant can be without a datasheet for RSB
 *   itself). NOTE: the AXP803 datasheet's own §9.9 TWSI/RSB note states
 *   "the slave address is 0x01D1 or 0x0273" — a DIFFERENT pair of numbers
 *   than U-Boot's 0x3a3/0x2d. This driver trusts U-Boot's shipped value (RSB
 *   hardware addresses are a bus-position convention shared across the
 *   sun8i/A64 PMIC family generically, not a per-chip-model constant, per
 *   the Linux sun8i_rsb.c comment "the mapping ... is fixed, and shared
 *   among all RSB drivers"), and cross-checks it by reading REG03H (IC type)
 *   after assigning the runtime address — see axp803_init(). Still, this
 *   address-scheme discrepancy between two nominally-authoritative sources
 *   is exactly the kind of thing that should be confirmed on the real board
 *   before fully trusting it.
 *
 *   Register map (REG00H/01H status bits, REG78H-7DH battery ADC data,
 *   REG58H/59H TS-pin ADC, REG82H ADC-enable bits, the 12-bit
 *   high-byte/low-nibble packing, the 1.1 mV/1 mA/0.8 mV ADC step sizes) --
 *   HIGH confidence, taken directly from the primary vendor source: the
 *   X-Powers "AXP803 Datasheet" Rev 1.0 (2015-04-27), fetched from the exact
 *   URL cited in U-Boot's own arch/arm/dts/axp803.dtsi header comment
 *   (http://files.pine64.org/doc/datasheet/pine64/AXP803_Datasheet_V1.0.pdf)
 *   and read directly (Table 9-30 "ADC input signal and data", Table 10-1
 *   "Register List", and the REG00H/REG01H/REG78H-7DH/REG82H per-register
 *   bit tables). This is a real, if leaked/pre-release-marked ("Confidential"
 *   watermark), copy of the vendor datasheet — not reconstructed from memory.
 *
 *   Battery TEMPERATURE -- NOT exposed as a calibrated °C reading here. The
 *   datasheet's ADC table has an "Internal temperature" row (PMIC DIE temp,
 *   formula -267.7 + 0.10625*raw °C) but no host-readable data register for
 *   it appears in the Register List (bmc.c's existing A64 THS read already
 *   covers "how hot" for the whole board). The TS pin IS wired as a battery-
 *   NTC-thermistor input by default (REG84H bit2 default 0 = "TS pin is
 *   battery temperature sensor input"), but converting its raw millivolts
 *   (REG58H/59H, 0.8 mV/step) to degrees needs the NTC's resistance-vs-
 *   temperature curve, which is board/battery-specific and NOT in this
 *   datasheet — so we report the raw TS-pin millivolts (0 if the ADC
 *   channel reads all-zero, the same "0 = n/a" convention bmc.c's THS read
 *   already uses) rather than fabricate a °C conversion.
 *
 * UNVERIFIABLE WITHOUT THE BOARD (flag before trusting): every field above
 * is a correct DECODE of whatever the AXP803 puts in these registers per its
 * datasheet — but whether a battery is even connected to this Banana Pi M64
 * unit, whether the RSB electrical bring-up behaves as U-Boot's sequence
 * predicts when re-run from EL2 after U-Boot has already done it once, and
 * the real-world accuracy of the ADC (the datasheet documents *the register
 * encoding*, not calibration tolerance) are all things only a live board run
 * can confirm. `axp803_init()`'s REG03H IC-type check is the one built-in
 * sanity gate: if it doesn't match, `bmc battery`/the health record report
 * "no AXP803" rather than print numbers decoded from a device that was never
 * actually confirmed to be there.
 */
#ifndef BZDOS_AXP803_H
#define BZDOS_AXP803_H
#include <stdint.h>

/* REG00H "Power source status" bits we decode (R). */
#define AXP803_ST0_VBUS_PRESENT   (1u << 5)  /* VBUS>4.1V                    */
#define AXP803_ST0_VBAT_GT_3V5    (1u << 3)  /* VBAT>3.5V                    */
#define AXP803_ST0_CHARGING_DIR   (1u << 2)  /* 1=battery charging, 0=discharge */

/* REG01H "Power mode and Charger status" bits we decode (R). */
#define AXP803_ST1_DIE_OVERTEMP   (1u << 7)  /* PMIC die over-temperature    */
#define AXP803_ST1_CHARGING       (1u << 6)  /* 1=actively charging          */
#define AXP803_ST1_BATT_PRESENT   (1u << 5)  /* 1=battery connected          */

/* Compact status bitmap `bmc battery`/the health record expose (our own
 * numbering, NOT AXP803 register bits — see axp803.c for the mapping). */
#define BMC_BATT_PRESENT     (1u << 0)
#define BMC_BATT_CHARGING    (1u << 1)
#define BMC_BATT_VBUS        (1u << 2)
#define BMC_BATT_DIE_HOT     (1u << 3)
#define BMC_BATT_CHIP_OK     (1u << 4)   /* AXP803 detected + REG03H verified */

struct axp803_health {
	uint32_t vbat_mv;      /* battery voltage, mV (0 if chip absent/unread) */
	uint32_t ichg_ma;      /* charge current, mA (0 if not charging)        */
	uint32_t idischg_ma;   /* discharge current, mA (0 if not discharging)  */
	uint32_t ts_mv;        /* raw TS-pin millivolts (NOT calibrated to °C)  */
	uint32_t status;       /* BMC_BATT_* bitmap                             */
	uint32_t chip_ok;      /* 1 = RSB init + REG03H IC-type check passed    */
};

/* One-time (idempotent) bring-up: rsb_init(), assign the AXP803 its runtime
 * RSB address, then read+mask-check REG03H against the AXP803 IC-type
 * pattern. Safe to call repeatedly (cheap on failure). Returns 0 if the chip
 * is confirmed present and addressable, negative step-specific codes
 * otherwise (see axp803.c) — a negative return means axp803_read_health()
 * will report chip_ok=0 / all-zero telemetry, never stale/guessed data. */
int axp803_init(void);

/* Fill *out with a fresh telemetry snapshot. Always succeeds (bounded); if
 * the chip was never confirmed present (axp803_init() didn't return 0) or
 * any individual RSB register read times out, the corresponding field(s)
 * stay 0 / chip_ok stays 0 rather than reporting a stale or garbage value. */
void axp803_read_health(struct axp803_health *out);

/* Drive the DC1SW output ("vcc-phy" — the RTL8211E Ethernet PHY's supply on
 * this board, per the DTB's emac node `phy-supply = <&reg_dc1sw>` with
 * regulator-name "vcc-phy"). This is the only lever in the system that
 * power-cycles the PHY without touching board power, i.e. the software
 * reproduction of the cold-boot PHY-lottery condition (see HANDOFF.md §1).
 *
 * *** WARNING — MEASURED 2026-08-30 ***: in the standard build the GUEST
 * owns the RSB bus (iichb1 + axp8xx_pmu0 in FreeBSD). Calling this from
 * EL2 while the guest is up races the guest's driver and can wedge its
 * interrupt path (measured: vtnet dead, cpu3 frozen). The DC1SW cut must
 * therefore be done GUEST-SIDE (/tmp/phycut.c over /dev/iic1); this
 * function remains for builds where no guest PMIC driver exists, and as
 * the register-map documentation.
 *
 *   on = 0  -> clear OUTPUT_CTRL2.SW_EN (rail off, PHY loses power entirely)
 *   on != 0 -> set OUTPUT_CTRL2.SW_EN  (rail back on, PHY runs its own POR)
 *
 * The register/bit citation is the SAME confidence class as the register map
 * note above: AXP803 is register-compatible with AXP818 here, and U-Boot's
 * include/axp818.h pins it down exactly:
 *     AXP818_OUTPUT_CTRL2        0x12
 *     AXP818_OUTPUT_CTRL2_SW_EN  (1 << 7)
 * The function only ever read-modify-writes BIT 7 of register 0x12 — no
 * voltage register is touched, and DC1SW's input rail (DCDC1, "vcc-3v3",
 * always-on in the DTB) is left alone. Every write is verified by readback.
 *
 * Returns 0 on success (state written AND read back as requested), negative
 * on an RSB read/write failure or a readback mismatch (state NOT trusted to
 * have changed — caller must treat the rail state as unknown). *prev, when
 * non-NULL, receives the register value seen before any write. */
int axp803_dc1sw(int on, uint8_t *prev);

#endif /* BZDOS_AXP803_H */
