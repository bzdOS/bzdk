/* SPDX-License-Identifier: BSD-2-Clause */

/* rsb.h — Allwinner A64 RSB (Reduced Serial Bus) controller driver for the
 * bzdOS EL2 hypervisor. Contract for rsb.c.
 *
 * WHAT RSB IS: a 2-wire (SCK/SDATA) Allwinner-proprietary bus, electrically
 * similar to I2C but with its own framing/command set — NOT I2C-compatible.
 * On the A64 it is the ONLY path from software to the AXP803 PMIC (VDD-CPU/
 * VDD-SYS/battery charger), which lives in the always-on "R_" power domain
 * alongside the R_PIO GPIO controller it shares pins with.
 *
 * THIS IS THE FIRST TIME THIS TREE HAS TOUCHED RSB. Register map, base
 * address, GPIO pinmux and the init/read/write sequence below are ported
 * VERBATIM from U-Boot's shipped, hardware-proven A64 bring-up code (the
 * exact code every A64 board's SPL runs to program DRAM voltage regulators —
 * if it were wrong, the board would not boot at all):
 *   - drivers/i2c/sun8i_rsb.c            (sun8i_rsb_init/read/write, the
 *     device-mode-switch + clock-divider sequence, the await-completion poll)
 *   - arch/arm/include/asm/arch-sunxi/rsb.h  (register offsets, CTRL/STAT/
 *     DMCR/CMD bit values — struct sunxi_rsb_reg)
 *   - arch/arm/include/asm/arch-sunxi/cpu_sun4i.h  (SUNXI_RSB_BASE
 *     0x01f03400, SUNXI_PRCM_BASE 0x01f01400 — A64 takes the "cpu_sun4i.h"
 *     branch of cpu.h: it is not sun9i/H6/ncat2)
 *   - arch/arm/include/asm/arch-sunxi/prcm_sun6i.h  (PRCM_APB0_GATE_PIO=bit0,
 *     PRCM_APB0_GATE_RSB=bit3, apb0_gate/apb0_reset register offsets)
 *   - include/sunxi_gpio.h + drivers/gpio/sunxi_gpio.c  (R_PIO base
 *     0x01f02c00, GPIO bank-L pin-config/pull/drive register layout, the
 *     legacy 0x24-byte-per-bank pinctrl geometry A64 uses)
 *   - include/axp_pmic.h  (AXP_PMIC_PRI_DEVICE_ADDR 0x3a3 / RUNTIME_ADDR
 *     0x2d — the fixed hardware->runtime RSB address pair used by every
 *     sun8i/A64 board with a single "primary position" PMIC)
 * This U-Boot source tree was read locally at /opt/bzdos/build/u-boot on the
 * build host during development; it is not vendored into this repo.
 *
 * NOT LOCALLY RE-DERIVED (do not re-guess): the RSB wire protocol itself
 * (hardware-address broadcast -> switch-to-RSB -> assign runtime address ->
 * byte read/write commands) is Allwinner's, undocumented outside vendor/
 * U-Boot/Linux (`drivers/bus/sunxi-rsb.c`) source — there is no public
 * datasheet for the RSB *bus*, only for devices that sit on it (AXP803).
 *
 * WHAT IS UNVERIFIED WITHOUT THE BOARD: the RSB controller's actual
 * electrical/timing behavior on THIS silicon — U-Boot has already run this
 * exact init once (in SPL, before our hypervisor gets control) to bring up
 * DRAM regulators, so pins/clock-gate/device-mode should already be live by
 * the time we run; rsb_init() re-does the whole sequence anyway (matching
 * what U-Boot itself does unconditionally on every boot) rather than assume
 * that. Whether re-running the device-mode-switch broadcast while the AXP803
 * is already in RSB mode is harmless has NOT been confirmed on this board —
 * it is expected to be (U-Boot's own SPL entry always does exactly this),
 * but flag it as the one RSB-level behavior that needs a live board to
 * confirm.
 *
 * DISCIPLINE: same as sd_bio.c/emmc_bio.c — freestanding, no libc, direct
 * volatile MMIO on the flat identity-mapped device range U-Boot leaves the
 * MMU with (0x01c00000-0x01fxxxxx, same range emmc_bio.c/sd_bio.c/bmc.c's
 * THS read already poke), every poll iteration-capped so a bug here can
 * never hang the CPU1 debug core.
 */
#ifndef BZDOS_RSB_H
#define BZDOS_RSB_H
#include <stdint.h>

/* One-time (idempotent) RSB controller bring-up: PRCM APB0 clock-gate +
 * reset for PIO/RSB, R_PIO GPL0/GPL1 pinmux -> RSB function + pull-up +
 * drive strength, RSB clock divider (24MHz -> 3MHz), soft reset, then the
 * device-mode-switch broadcast (all RSB devices on the bus switch from I2C
 * to RSB protocol). Safe to call repeatedly. Returns 0 on success, negative
 * step-specific codes (see rsb.c) on a bounded poll timeout. Must return 0
 * before rsb_set_device_address()/rsb_read()/rsb_write(). */
int rsb_init(void);

/* Assign `runtime_addr` (an 8-bit short address, e.g. 0x2d) to the RSB
 * device whose fixed 12-bit hardware address is `hw_addr` (e.g. 0x3a3 for
 * the "primary position" PMIC on every sun8i/A64 board). Must be called
 * once after rsb_init() and before any rsb_read()/rsb_write() to that
 * device. Returns 0 on success, negative on a bounded poll timeout. */
int rsb_set_device_address(uint16_t hw_addr, uint8_t runtime_addr);

/* Read one 8-bit register `reg` from the RSB device at `runtime_addr` into
 * *out. Returns 0 on success, negative on a bounded poll timeout / bus
 * error (TERR/LBSY status bits). */
int rsb_read(uint8_t runtime_addr, uint8_t reg, uint8_t *out);

/* Write 8-bit `val` to register `reg` on the RSB device at `runtime_addr`.
 * Returns 0 on success, negative on a bounded poll timeout / bus error. */
int rsb_write(uint8_t runtime_addr, uint8_t reg, uint8_t val);

#endif /* BZDOS_RSB_H */
