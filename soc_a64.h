/* SPDX-License-Identifier: BSD-2-Clause */

/* soc_a64.h — every Allwinner A64 peripheral address this tree touches, in ONE
 * place. The first step of a port layer, and deliberately only the first step.
 *
 * WHY THIS EXISTS. Before this header, 48 `#define`s across 44 files each
 * carried their own copy of an A64 address, and the same address was spelled
 * differently depending on who needed it. The GIC distributor at 0x01C81000 had
 * EIGHT names — GICD_BASE, GICD_CTLR_ADDR, VGICD_BASE, VGIC_GICD_BASE,
 * VBLK_GICD_BASE, VBLK_SD_GICD_BASE, VINPUT_GICD_BASE, VNET_GICD_BASE — living
 * in 19 different files. MUSB, CCU and SRAMC each had two names; WDOG_CTRL/CFG/
 * MODE were defined twice each, in different letter case.
 *
 * That is not untidiness, it is the exact mechanism by which a port breaks: you
 * change seven of the eight and the eighth silently keeps pointing at the old
 * SoC. This project's stated direction is the rest of the Allwinner Banana Pi
 * family and, notably, the PinePhone — which is the SAME A64 die as the
 * BPI-M64, so most of this file is already correct for it (see the PORTING note
 * at the bottom for what is NOT).
 *
 * WHAT THIS HEADER IS NOT. It is not a HAL and it does not abstract anything.
 * Every value is a literal A64 fact, unchanged, and nothing here is selected at
 * runtime. Consumers keep their own local names and simply point them here, so
 * this change is a pure consolidation with no behavioural component at all —
 * verified the only way that claim can be verified, by checking that the
 * compiled binary is byte-for-byte identical before and after.
 *
 * BOARD vs SoC. Addresses are a property of the die and live here. Which PIN
 * carries what, which PHY address answers on MDIO, what MAC we use, how much
 * DRAM there is and where the image loads are properties of the BOARD and live
 * in board_bpi_m64.h. The split matters because the PinePhone shares this file
 * almost entirely and shares very little of that one.
 *
 * Cited against sun50i-a64.dtsi / the A64 user manual; where a name here
 * differs from the DTSI's node name, the DTSI name is given in the comment so a
 * reader can grep either.
 */
#ifndef BZDOS_SOC_A64_H
#define BZDOS_SOC_A64_H

/* ---- Interrupt controller: GIC-400 (GICv2) ------------------------------ *
 * sun50i-a64.dtsi `interrupt-controller@1c81000`. Four separate windows, all
 * used by this tree: the distributor and the physical CPU interface, plus the
 * hypervisor control interface and the VIRTUAL CPU interface that the vGIC
 * needs (vgic.c drives GICH; the guest reaches GICV because stage2.c redirects
 * its GICC accesses there under HCR_EL2.IMO=1). */
#define SOC_A64_GICD_BASE        0x01C81000UL  /* distributor              */
#define SOC_A64_GICC_BASE        0x01C82000UL  /* physical CPU interface   */
#define SOC_A64_GICH_BASE        0x01C84000UL  /* hypervisor control (EL2) */
#define SOC_A64_GICV_BASE        0x01C86000UL  /* virtual CPU interface    */

/* ---- Clocks, pinmux, timers/watchdog, SRAM controller ------------------- */
#define SOC_A64_CCU_BASE         0x01C20000UL  /* `ccu@1c20000`            */
#define SOC_A64_CCU_MMC0_CLK     0x01C20088UL  /* SD (SMHC0) clock gate    */
#define SOC_A64_CCU_MMC2_CLK     0x01C20090UL  /* eMMC (SMHC2) clock gate  */
#define SOC_A64_PIO_BASE         0x01C20800UL  /* `pio@1c20800`            */
/* Port C config register 0 — nibble 5 is the eMMC clock pin (PC5/MMC2_CLK).
 * Named here because it had been written out as a bare literal INLINE in four
 * separate places (gic_timer.c's tick path, main_dbg.c, main_gdb.c, smp.c's
 * debug loop), all of them enforcing the same thing: FreeBSD's own pinctrl
 * driver muxes the other mmc2 pins but leaves PC5 at gpio, which kills the eMMC
 * clock and takes the guest's root filesystem with it. Four copies of the
 * address that keeps root alive is exactly the duplication this header exists
 * to end. */
#define SOC_A64_PIO_PC_CFG0      (SOC_A64_PIO_BASE + 0x48)
/* The watchdog lives inside the timer block (`timer@1c20c00`). These three
 * registers are the entire reset lever this project relies on — see wdt.c and
 * the uboot-watchdog-soft-reset note: they are how the board reboots itself
 * with no human and no power cycle. */
#define SOC_A64_WDOG_CTRL        0x01C20CB0UL
#define SOC_A64_WDOG_CFG         0x01C20CB4UL
#define SOC_A64_WDOG_MODE        0x01C20CB8UL
/* The watchdog's DTB node (`watchdog@1c20ca0`, `reg = <0x1c20ca0 0x20>` — read
 * from the live tftpboot DTB, `dtc -I dtb -O dts bananapi-min.dtb`) claims a
 * wider span than the 3 registers above: it also covers WDOG_IRQ_EN/IRQ_STA at
 * 0x1C20CA0/0x1C20CA4, which wdt.c never touches but which still arm/observe
 * the same hardware reset lever. That node is deliberately `status =
 * "disabled"` in this project's DTB (see PROGRESS.md's "Watchdog Disable in
 * DTB" entry, 2026-07-15 — FreeBSD's aw_wdog(4) attaches to an enabled node
 * and disables the hardware watchdog on purpose, which would have silently
 * defeated the whole auto-recovery story) precisely so no in-tree FreeBSD
 * driver ever probes here. A DTB `status` is a convention the driver
 * framework honours, not a permission bit stage-2 enforces — a root shell
 * that opens `/dev/mem` (or any custom module) and touches this physical
 * range directly is not stopped by the disabled node at all. This span is
 * the boundary a stage-2 trap would need to deny; see soc_a64.h's own
 * porting note and docs/wdog-ccu-pio-stage2.md for the full design. */
#define SOC_A64_WDOG_NODE_BASE   0x01C20CA0UL
#define SOC_A64_WDOG_NODE_SIZE   0x00000020UL
#define SOC_A64_SRAMC_BASE       0x01C00000UL  /* `syscon@1c00000`         */
#define SOC_A64_SYSCON_EMAC      0x01C00030UL  /* EMAC clock/mode register */

/* ---- Serial ------------------------------------------------------------- *
 * UART0 is NOT driven by EL2 as a console here: stage-2 leaves this page
 * unmapped so the guest's own 16550 accesses fault to vconsole.c and are
 * emulated. The address matters precisely because it is the trap target. */
#define SOC_A64_UART0_BASE       0x01C28000UL  /* `serial@1c28000`         */

/* ---- Storage: SMHC (SD/MMC host controllers) ---------------------------- *
 * mmc_no is derived from the base in emmc_bio.c as (base - SMHC0)/0x1000, so
 * these two must keep their real spacing, not just their values. */
#define SOC_A64_SMHC0_BASE       0x01C0F000UL  /* `mmc@1c0f000`, microSD   */
#define SOC_A64_SMHC2_BASE       0x01C11000UL  /* `mmc@1c11000`, eMMC      */
/* SMHC1 (`mmc@1c10000`) carries the AP6212/BCM43430 WiFi module over SDIO.
 * Not referenced by any EL2 C code today -- entirely a guest concern, gated
 * by board-config.xml's guest_wifi_sdio feature (docs/guest-hw-enablement.md)
 * -- listed here for the same reason this file already lists
 * SOC_A64_TCON0_BASE despite hdmi.c only ever driving TCON1: a real A64 fact
 * worth having in one place even before this tree's own C code touches it. */
#define SOC_A64_SMHC1_BASE       0x01C10000UL  /* `mmc@1c10000`, WiFi SDIO */

/* ---- Networking: EMAC (Gigabit, external RGMII PHY) --------------------- *
 * Everything this project's debug plane is built on rides here. Worth knowing
 * for the port: the PinePhone has NO Ethernet at all, so a port to it must move
 * dbgmon/gdbstub/coredump/snapshot onto USB. */
#define SOC_A64_EMAC_BASE        0x01C30000UL  /* `ethernet@1c30000`       */

/* ---- USB: MUSB (OTG) and its PHY --------------------------------------- */
#define SOC_A64_MUSB_BASE        0x01C19000UL  /* `usb@1c19000`            */
#define SOC_A64_MUSB_POWER       0x01C19040UL  /* POWER reg, +0x40         */
#define SOC_A64_USBPHY_CTRL_BASE 0x01C19400UL
#define SOC_A64_USBPHY_PMU0_BASE 0x01C1A800UL  /* DT "pmu0"; +0x10 = PHY_CTL */

/* ---- Display ----------------------------------------------------------- *
 * TCON1 is the TV-facing timing controller that feeds HDMI on this board, and
 * is the block whose TCON_INT0 latching vblank bit EL2 observes (hdmi.c). For
 * the port: the PinePhone drives a MIPI-DSI panel off TCON0 instead, so this is
 * one of the few genuinely different pieces of silicon usage between two boards
 * that share the same die. */
#define SOC_A64_TCON0_BASE       0x01C0C000UL  /* `lcd-controller@1c0c000` */
#define SOC_A64_TCON1_BASE       0x01C0D000UL  /* `lcd-controller@1c0d000` */

/* ---- Thermal ----------------------------------------------------------- */
#define SOC_A64_THS_BASE         0x01C25000UL  /* `thermal-sensor@1c25000` */

/* ------------------------------------------------------------------------ *
 * PORTING NOTE — what this file buys and what it does not.
 *
 * Shares this file almost wholesale: the PinePhone (A64), and any other A64
 * board. Same GIC-400, same CCU/PIO/watchdog, same SMHC, same MUSB and PHY,
 * same DE2 and Mali-400, same AXP803 PMIC over RSB.
 *
 * Needs a sibling file, not an edit of this one: H3 / H2+ / A83T / V40 Banana
 * Pis. Same vendor, different peripheral maps. The Amlogic (M5), MediaTek (R2)
 * and RISC-V (F3) boards are not ports of this at all.
 *
 * Still hardcoded elsewhere, and NOT addressed by this header — the honest
 * remainder of the port surface:
 *   - The load address (0x42000000) and DRAM window, baked into link.ld and
 *     stage2.h. A board with a different DRAM size needs both changed.
 *   - RSB/AXP803 register-level knowledge (rsb.c, axp803.c) — same PMIC on the
 *     PinePhone, different rails and regulator names.
 *   - The GIC SPI numbers (musb.h's 71, hdmi.h's 87, the virtio range in
 *     board-config.xml). These are SoC facts too, but they live with the
 *     drivers that cite them from the DTB, which is where the citation belongs.
 *   - The HDMI/DE2 pipeline in hdmi.c, which assumes an HDMI sink.
 * ------------------------------------------------------------------------ */

#endif /* BZDOS_SOC_A64_H */
