/* SPDX-License-Identifier: BSD-2-Clause */

/* reboot.h — clean USB-gadget disconnect + watchdog reboot, to stop the
 * recurring U-Boot download-gadget "zombie" after our code runs.
 *
 * Root cause: when we take over from U-Boot (bootelf), U-Boot stops servicing
 * its USB gadget but the MUSB hardware keeps D+ pulled up, so the host sees a
 * device that "stopped responding" rather than a clean disconnect. A fast WDT
 * reset then re-enumerates faster than the host's xHCI cleanly resets the
 * port -> intermittent unresponsive gadget. Dropping the D+ pull-up as soon as
 * we take over (and giving a ~2s window on reboot) makes the host see a proper
 * disconnect, so U-Boot re-enumerates cleanly. */
#ifndef BZDOS_REBOOT_H
#define BZDOS_REBOOT_H

/* Drop the MUSB D+/D- pull-up + SOFTCONN so the USB host cleanly disconnects
 * the (now unserviced) U-Boot gadget. Safe to call once we own the machine. */
void usb_gadget_disconnect(void);

/* Clean reboot to U-Boot: disconnect the gadget, then arm a ~2s watchdog and
 * stop feeding it. Never returns. The 2s window lets the host register the
 * disconnect before U-Boot re-enumerates, avoiding the zombie. */
void reboot_clean(void) __attribute__((noreturn));

#endif
