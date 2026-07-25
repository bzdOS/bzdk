/* SPDX-License-Identifier: BSD-2-Clause */

/* reboot.c — see reboot.h. Directly pokes the MUSB + watchdog MMIO (no
 * dependency on musb.c), so any image can link it. */
#include <stdint.h>
#include "reboot.h"

#define MUSB_BASE   0x01c19000UL
#define REG_ISCR    0x0400u          /* Allwinner USB iface status/control */
#define REG_POWER   0x0040u          /* sunxi MUSB POWER (8-bit)            */
#define ISCR_DPDM_PULLUP_EN (1u << 16)
#define ISCR_CHANGE_DETECT  ((1u << 4) | (1u << 5) | (1u << 6)) /* w1c */
#define POWER_SOFTCONN      0x40u

#define WDOG_CTRL   0x01c20cb0UL
#define WDOG_CFG    0x01c20cb4UL
#define WDOG_MODE   0x01c20cb8UL

void usb_gadget_disconnect(void)
{
	volatile uint32_t *iscr  = (volatile uint32_t *)(MUSB_BASE + REG_ISCR);
	volatile uint8_t  *power = (volatile uint8_t  *)(MUSB_BASE + REG_POWER);
	uint32_t v;

	/* Read-modify-write ISCR, keeping the write-1-to-clear change-detect
	 * bits at 0 (so we don't accidentally ack pending events), and clearing
	 * the D+/D- pull-up enable -> the host sees the device leave the bus. */
	v = *iscr & ~ISCR_CHANGE_DETECT;
	*iscr = v & ~ISCR_DPDM_PULLUP_EN;
	/* Also drop SOFTCONN at the MUSB core. */
	*power = (uint8_t)(*power & ~POWER_SOFTCONN);
	__asm__ volatile("dsb sy" ::: "memory");
}

void reboot_clean(void)
{
	usb_gadget_disconnect();
	/* ~2s watchdog (MODE interval field 2), reset whole system, load it,
	 * then spin without petting -> the host gets ~2s of clean disconnect
	 * before U-Boot re-enumerates. */
	*(volatile uint32_t *)WDOG_CFG  = 1u;
	*(volatile uint32_t *)WDOG_MODE = 0x21u;
	*(volatile uint32_t *)WDOG_CTRL = 0x14AFu;
	for (;;) { }
}
