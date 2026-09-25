/* SPDX-License-Identifier: BSD-2-Clause */

/* reboot.c — see reboot.h. Directly pokes the MUSB + watchdog MMIO (no
 * dependency on musb.c), so any image can link it. */
#include <stdint.h>
#include "reboot.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

#define MUSB_BASE   SOC_A64_MUSB_BASE
#define REG_ISCR    0x0400u          /* Allwinner USB iface status/control */
#define REG_POWER   0x0040u          /* sunxi MUSB POWER (8-bit)            */
#define ISCR_DPDM_PULLUP_EN (1u << 16)
#define ISCR_CHANGE_DETECT  ((1u << 4) | (1u << 5) | (1u << 6)) /* w1c */
#define POWER_SOFTCONN      0x40u

#include "wdt.h"     /* wdt_debug_hold: the guest-progress pet on CPU0 must stop */
#define WDOG_CTRL   SOC_A64_WDOG_CTRL
#define WDOG_CFG    SOC_A64_WDOG_CFG
#define WDOG_MODE   SOC_A64_WDOG_MODE

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

void usb_gadget_reconnect(void)
{
	volatile uint32_t *iscr  = (volatile uint32_t *)(MUSB_BASE + REG_ISCR);
	volatile uint8_t  *power = (volatile uint8_t  *)(MUSB_BASE + REG_POWER);
	uint32_t v;

	v = *iscr & ~ISCR_CHANGE_DETECT;      /* same w1c care as disconnect */
	*iscr = v | ISCR_DPDM_PULLUP_EN;
	*power = (uint8_t)(*power | POWER_SOFTCONN);
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t rd_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t rd_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static void arm_watchdog(void)
{
	/* ~2s watchdog (MODE interval field 2), reset whole system, load it. */
	*(volatile uint32_t *)WDOG_CFG  = 1u;
	*(volatile uint32_t *)WDOG_MODE = 0x21u;
	*(volatile uint32_t *)WDOG_CTRL = 0x14AFu;
}

void reboot_clean(void)
{
	uint64_t hz, t0;
	unsigned long guard;

	/* STOP EVERY PET PATH FIRST, HERE, NOT IN THE CALLER.
	 *
	 * This runs on whichever core asked for the reset (CPU1 for `bmc reset`
	 * and the EMAC-dark escalation, CPU0 for the PSCI paths). CPU0 keeps
	 * running the guest meanwhile, and el2_exc.c's wdt_pet() restarts the
	 * WDOG counter on EVERY EL2 exception while the guest's last "progress"
	 * is under 180 s old -- and vblk_emmc.c counts eMMC traffic as progress.
	 * So a 2 s watchdog armed below, underneath a live guest that touched
	 * the eMMC in the last three minutes, NEVER FIRES: CPU0 reloads it
	 * hundreds of times a second. The gadget is already dropped, this core
	 * is about to park, and the board goes dark on every channel with the
	 * guest still running behind it -- recoverable only by cutting power.
	 *
	 * That is what `bmc reset` did on 2026-09-24 01:46 (11 s after an ssh
	 * session wrote uboot.env) and 2026-09-25 11:33 (right after ssh
	 * commands read the root disk); it worked at 01:39 only because the
	 * guest had been idle for nine minutes. Two of the three reboot_clean()
	 * callers set wdt_debug_hold themselves; bmc_reset() and repl.c did
	 * not. The hold belongs here, so no caller can forget it again. It is
	 * a fixed-address word (hv_addrmap.h) that wdt_arm() clears on the next
	 * generation, so it cannot outlive the reset it asks for. */
	wdt_debug_hold = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	usb_gadget_disconnect();
	arm_watchdog();

	/* This used to be `for (;;) { }` -- arm the watchdog, spin, and trust it
	 * to land. It normally does, in ~2 s, and the host gets a clean
	 * disconnect before U-Boot re-enumerates.
	 *
	 * When it does NOT land, that spin is the worst possible place to be.
	 * The pull-up is already down, so USB is gone; this path is reached from
	 * the EMAC-dark escalation, so the wire is gone too. The board is then
	 * executing, healthy, and completely invisible -- no console, no debug
	 * channel, not even a BROM to talk to, because no reset ever happened.
	 * Nothing software can reach it; it costs a physical power-cycle.
	 *
	 * Observed 2026-09-23: the hypervisor's gadget disconnected 101 s into a
	 * boot and the board was never seen again on any channel for hours. That
	 * is consistent with exactly this -- a reset that was asked for and not
	 * delivered -- though the cause of the watchdog not firing was never
	 * established, since by then there was nothing left to ask.
	 *
	 * So: wait a bounded time for the reset, and if it has not arrived, put
	 * the gadget back and keep trying. The reset has failed either way; the
	 * only thing in our gift is whether the board stays reachable while it
	 * fails. Reconnecting cannot make a successful reset worse, because a
	 * successful reset happens long before this deadline. */
	hz = rd_cntfrq();
	if (hz == 0u)
		hz = 24000000u;                /* A64 arch timer, if CNTFRQ is unset */
	t0 = rd_cntpct();
	/* Belt and braces on the counter itself: it has been seen to run
	 * backwards on this SoC, which would make the deadline unreachable and
	 * put us right back in an unbounded spin. */
	for (guard = 0; guard < 200000000ul; guard++) {
		uint64_t now = rd_cntpct();
		if (now < t0 || now - t0 > 5u * hz)
			break;
	}

	/* Reconnect and then STOP TOUCHING THE WATCHDOG.
	 *
	 * The first version of this loop called arm_watchdog() repeatedly to
	 * "keep asking". That was backwards: arm_watchdog() writes
	 * WDOG_CTRL = 0x14AF, and that is the RESTART key -- it reloads the
	 * countdown. Calling it in a tight loop is petting the watchdog, so a
	 * watchdog that was merely slow would have been prevented from ever
	 * firing, by the very code waiting for it. The timer is already armed
	 * above; leaving it alone is what lets it land.
	 *
	 * What is left here is the honest fallback: the reset did not arrive in
	 * time, so put the gadget back and sit still. If the watchdog was slow it
	 * fires on its own. If it is genuinely not running, at least the board is
	 * reachable again instead of silent. Reconnecting is best-effort -- the
	 * host re-enumerates when D+ goes high, but MUSB may want more than the
	 * pull-up bit to come back cleanly, and there is no way to check from
	 * here. */
	usb_gadget_reconnect();
	for (;;)
		__asm__ volatile("wfe" ::: "memory");
}
