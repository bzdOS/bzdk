/* SPDX-License-Identifier: BSD-2-Clause */

/* main_hdmi.c — bring up HDMI and draw the HUD-skeleton demo, so we can verify
 * the display pipeline on a physical monitor. Standalone (no network/guest yet).
 * hdmi.c writes pipeline progress to breadcrumb 0x50003000 ("HDMI") — so even
 * if the monitor stays blank we learn how far the DE2->TCON->HDMI->PHY chain
 * got. A ~16s watchdog auto-returns to U-Boot so we can read that breadcrumb
 * and reload; if the screen lights up, that's a 16s window to eyeball it.
 * (Once the framebuffer is confirmed on a monitor, the next step is the full
 * HUD: the FreeBSD guest in a window + live register/timing overlays around it,
 * combining hdmi + the live debugger's guest-state reads.) */
#include <stdint.h>
#include "hdmi.h"
#include "reboot.h"

int main(void)
{
	usb_gadget_disconnect();   /* zombie fix: clean-disconnect U-Boot gadget */

	hdmi_init();               /* DE2 + TCON + DWC-HDMI + PHY -> scanout      */
	hdmi_demo();               /* clear, title bar, guest-window box, sample  */

	/* PERSISTENT: no watchdog — keep scanning out the demo forever so the
	 * monitor stays lit for the user to look at whenever. (Physical reset to
	 * exit.) The pipeline breadcrumb at 0x50003000 already recorded scanout. */
	for (;;) { }
}
