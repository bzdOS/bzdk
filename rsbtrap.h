/* SPDX-License-Identifier: BSD-2-Clause */

/* rsbtrap.h — the guest's RSB controller, emulated (0x01F03000 page).
 *
 * FreeBSD's aw_rsb + axp8xx_pmu drive the AXP803 PMIC through the same RSB
 * controller that EL2 uses (hdmi.c relock, emac.c phy_rail_ensure, axp803.c
 * health). Unarbitrated, one side's register setup could be overwritten by
 * the other's between its writes and its START, so a write could land in a
 * register nobody asked for -- REG 0x10 switches the CPU and DRAM rails, and
 * a board without them is off: no watchdog, no USB, only the power switch.
 *
 * With HV_RSBTRAP the page is invalid in stage 2 (stage2.c). Every guest
 * access faults here: the controller registers are shadowed, and a START
 * executes the guest's whole transaction through rsb_guest_transfer() under
 * rsb.c's bus lock. Guest SOFT_RESET, clock and device-mode writes stay in
 * the shadow: the bus is EL2's, configured once. The rest of the page
 * (R_PWM at +0x800, disabled in the guest DTB) passes through.
 *
 * Policy on guest writes to the AXP803 (runtime address 0x2d):
 *   REG 0x10  DCDC1/2/5/6 enable forced on (3V3, CPU, DRAM, SYS: all
 *             regulator-always-on in the DTB; no legitimate guest clears them)
 *   REG 0x12  DC1SW forced on (vcc-phy: EL2 owns the EMAC); DLDO1 forced on
 *             in HV_HDMI builds (vcc-hdmi: EL2 owns the display PHY)
 *   REG 0x32  bit 7 (PMIC power-off) refused: the guest's shutdown is PSCI
 *             SYSTEM_OFF, handled in EL2; a PMIC off is a board only the
 *             power switch brings back
 * Every guest PMIC write (register, value asked, value sent) goes into a
 * ring at HVMAP_RSBTRAP_LOG, so who changes which rail is observable.
 *
 * Breadcrumbs, HVMAP_RSBTRAP_BC (u32 words):
 *   [0] magic "RSBT"  [1] faults  [2] reads  [3] writes  [4] transactions
 *   [5] transaction errors  [6] guest PMIC writes altered by policy
 *   [7] ISV==0 accesses  [8] last CPU  [9] last offset  [10] soft resets
 *   [11] log ring head (entries written)
 * Log ring, HVMAP_RSBTRAP_LOG: RSBTRAP_LOG_N entries of 2 words,
 *   w0 = reg | asked<<8 | sent<<16 | cpu<<24, w1 = CNTPCT low 32 bits. */
#ifndef BZDOS_RSBTRAP_H
#define BZDOS_RSBTRAP_H

#include "exceptions.h"

#define RSBTRAP_PAGE_BASE  0x01F03000UL
#define RSBTRAP_PAGE_SIZE  0x1000UL
#define RSBTRAP_RSB_OFF    0x400u     /* rsb@1f03400 */
#define RSBTRAP_RSB_END    0x800u
#define RSBTRAP_LOG_N      16u

/* 1 if this data abort was a guest access to the RSB page (handled, ELR
 * advanced), 0 if it is not ours. */
int rsbtrap_handle_fault(struct el2_frame *frame);

#endif /* BZDOS_RSBTRAP_H */
