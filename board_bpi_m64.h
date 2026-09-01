/* SPDX-License-Identifier: BSD-2-Clause */

/* board_bpi_m64.h — Banana Pi BPI-M64 board properties for bzdOS.
 *
 * Addresses, DRAM geometry, SPI assignments and HDMI assumptions that belong
 * to THIS BOARD, not to the A64 die. See soc_a64.h for the SoC-level split:
 * addresses are a property of the die and live there; which DRAM the board
 * has, how much of it, where the image loads, what MAC address the PHY uses,
 * and how many HDMI sinks are connected are properties of the BOARD and live
 * here. The PinePhone shares soc_a64.h almost entirely and shares very little
 * of this file.
 *
 * CRITERION: the compiled binary must be byte-for-byte identical before and
 * after this extraction — this is a pure refactoring of the board/SoC
 * boundary, not new logic. Every constant below is either (a) already a
 * literal in a .c file, or (b) already #defined with a literal in another
 * header, and this header simply names it at the board level.
 *
 * This file does NOT replace the per-driver SPI/IRQ definitions in musb.h,
 * hdmi.h, or board-config.xml's virtio-mmio entries — those live with the
 * drivers that cite them (see soc_a64.h's PORTING NOTE). This header
 * documents the WHOLE board's SPI map in one place for porting reference,
 * but consumers keep their own local defines.
 *
 * Cited against: link.ld, stage2.h, musb.h, hdmi.h, hv_addrmap.h, rsb.c,
 * axp803.c, board-config.xml, and the flashed DTB.
 */
#ifndef BZDOS_BOARD_BPI_M64_H
#define BZDOS_BOARD_BPI_M64_H

#include <stdint.h>
#include "soc_a64.h"   /* SoC addresses this board shares with PinePhone */

/* ---- Load address and DRAM geometry ------------------------------------ *
 * The image is loaded by U-Boot at physical 0x42000000 as a raw binary
 * (link.ld: ENTRY(_start), . = 0x42000000). The stage-2 "A1" boundary
 * protects exactly ONE 2 MiB L2 block at this address (stage2.c
 * HVIMG_L2_IDX) — anything the image places above 0x42200000 stays
 * identity-mapped RW to the guest. */
#define BOARD_BPI_M64_LOAD_ADDR      0x42000000UL

/* The BPI-M64 has 2 GiB of DRAM at [0x40000000, 0xC0000000). By default
 * the guest only gets the low 1 GiB because snapshot.c claims the high GiB
 * as its verbatim mirror (SNAP_DRAM_STORE, exactly 1 GiB, hv_addrmap.h).
 * With GUEST_DRAM_2G the guest gets the full 2 GiB and snapshot/restore is
 * not linked — a measured trade (see stage2.h GUEST_DRAM_2G comment).
 * The DTB's /memory node is told to stop at 0xB8000000 to avoid U-Boot's
 * no-overwrite region at the top of DRAM. */
#define BOARD_BPI_M64_DRAM_BASE      0x40000000UL
#define BOARD_BPI_M64_DRAM_TOTAL     0x80000000UL   /* 2 GiB total */
#define BOARD_BPI_M64_DRAM_DTB_MAX   0x78000000UL   /* safe guest ceiling */

/* Stage-2 protected window at the load address: exactly 2 MiB (one L2 block). */
#define BOARD_BPI_M64_HVIMG_SIZE     0x200000UL     /* 2 MiB */

/* ---- RSB / AXP803 PMIC (board-level addresses) ------------------------- *
 * The AXP803 is the "primary position" PMIC on every sun8i/A64 board. These
 * are the physical register bases the RSB controller and its companion
 * peripherals occupy — the SAME silicon the PinePhone has, different rails
 * and regulator names (see soc_a64.h PORTING NOTE). Listed here because
 * they are referenced from rsb.c/axp803.c and porting to a different A64
 * board may change the PMIC's runtime address or the PRCM/R_PIO bases. */
#define BOARD_BPI_M64_RSB_BASE       0x01F03400UL   /* SUNXI_RSB_BASE       */
#define BOARD_BPI_M64_PRCM_BASE      0x01F01400UL   /* SUNXI_PRCM_BASE      */
#define BOARD_BPI_M64_R_PIO_BASE     0x01F02C00UL   /* SUNXI_R_PIO_BASE     */

/* AXP803 RSB addresses (U-Boot axp_pmic.h): fixed HW address 0x3A3,
 * runtime address 0x2D. The datasheet §9.9 documents a different value,
 * but every A64 board in U-Boot uses these — see axp803.h. */
#define BOARD_BPI_M64_AXP803_HWADDR  0x3A3u
#define BOARD_BPI_M64_AXP803_RUNTIME 0x2Du

/* ---- GIC SPI map (this board's DTB, confirmed by dtc decompile) -------- *
 * These are SoC facts that happen to live with their drivers because the
 * drivers are the ones that cite them. Listed here for porting reference;
 * consumers keep their own local defines. */
/* MUSB CDC-ACM: SPI 71 → INTID 103 (musb.h) */
/* HDMI TCON1:  SPI 87 → INTID 119 (hdmi.h) */
/* Virtio MMIO: SPI 105 (vblk_emmc), 106 (vnet_emac), 107 (vinput),
 *              108 (vblk_sd) — scanout has no SPI (no dtb-node, hdmi.h) */

/* ---- HDMI / display assumptions ---------------------------------------- *
 * The BPI-M64 connects to an HDMI sink via TCON1 → DE2 → DW HDMI PHY.
 * This is a board property: the PinePhone drives a MIPI-DSI panel off TCON0
 * instead, so the HDMI assumption does not apply to it. The guest window
 * geometry and framebuffer base are DRAM-layout-dependent board facts. */
#define BOARD_BPI_M64_HDMI_FB_BASE    0x4D000000UL
#define BOARD_BPI_M64_HDMI_FB_SIZE    0x00800000UL   /* 8 MiB (two buffers) */
#define BOARD_BPI_M64_HDMI_GUESTWIN  0x4B000000UL   /* guest composited fb  */
#define BOARD_BPI_M64_HDMI_GUESTWIN_SIZE 0x00200000UL /* 2 MiB reservation  */

/* ---- Virtio MMIO devices (trapped block at 0x0A000000) ------------------ *
 * Every virtio-mmio device in this build lives in a stage-2-trapped 2 MiB
 * block (vblk_emmc.h's "MMIO window"). Base addresses and SPI assignments
 * are board-level facts from board-config.xml; device IDs are SoC/guest-
 * side facts. Listed here for the porting overview. */

#endif /* BZDOS_BOARD_BPI_M64_H */
