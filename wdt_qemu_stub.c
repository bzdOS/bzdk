/* SPDX-License-Identifier: BSD-2-Clause */

/* wdt_qemu_stub.c — no-op wdt_pet() for QEMU `virt` targets that link
 * snapshot.c/.o.
 *
 * WHY THIS EXISTS: snapshot.c's dram_copy() calls wdt_pet() periodically
 * (see snapshot.c's own comment: "feed the 16 s HW dead-man's switch during
 * the long (1 GiB) DRAM copy loop") because on the real board that loop runs
 * long enough to risk the A64 hardware watchdog firing mid-copy. wdt.c's
 * real implementation pokes fixed Allwinner A64 MMIO addresses
 * (WDOG_CTRL/CFG/MODE @ 0x01C20CBx — see wdt.c's own header) that do not
 * exist on QEMU's `virt` machine at all; linking the real wdt.o into a QEMU
 * target would mean the very first wdt_pet() call writes to unbacked
 * physical memory, which is exactly the kind of confounding variable this
 * file avoids so the QEMU snapshot exercise (main_snapshot_qemu.c /
 * el2_exc_snapshot_qemu.c) tests snapshot.c's OWN logic, not "what does an
 * unassigned MMIO write do under qemu-system-aarch64 -M virt".
 *
 * This mirrors the zephyr-qemu target's own comment about wdt.o (Makefile:
 * "wdt_pet()/wdt_init(), the parts that poke the A64 WDOG ... are never
 * called from this target") — same fact, different resolution: that target
 * avoids calling wdt_pet() at all; this one links a real, but inert,
 * implementation so snapshot.c's unmodified source (which DOES call
 * wdt_pet()) still links and runs.
 *
 * Nothing else in this build needs any other wdt.h symbol — snapshot.c is
 * the only caller wired into the QEMU snapshot target, so wdt_pet() is the
 * only symbol this file needs to provide.
 *
 * Freestanding: <stdint.h> only (not even used directly, but kept for
 * consistency with every other file in this tree), no libc.
 */
#include <stdint.h>
#include "wdt.h"

void
wdt_pet(void)
{
	/* No hardware watchdog exists on QEMU's `virt` machine; nothing to do. */
}
