/* SPDX-License-Identifier: BSD-2-Clause */

/* wdt.h — Allwinner A64 hardware watchdog for the bzdOS microkernel.
 * The A64 WDOG does a full SoC reset in hardware, bypassing U-Boot's
 * (unsupported) software sysreset. Armed at microkernel entry and petted in
 * the poll loop: if any bring-up code (MUSB/EMAC) hangs or faults, the board
 * resets within the interval and lands back at U-Boot's '=>' (bootdelay=-1),
 * so a broken payload NEVER bricks the session — the core of the
 * "always-recoverable" design. */
#ifndef BZDOS_WDT_H
#define BZDOS_WDT_H

#include <stdint.h>
#include "hv_addrmap.h"   /* HVMAP_WDT_DEBUG_HOLD: fixed address for wdt_debug_hold */

/* Arm the watchdog for a full-system reset after ~16 s (verified working on
 * this board). Starts the countdown immediately. */
void wdt_arm(void);

/* Reload the countdown ("pet"). Called from el2_trap() on every EL2 exception,
 * but it only re-arms while the guest has made progress within WDT_TIMEOUT_S
 * (see wdt.c) — so a busy-but-stuck guest still reboots. */
void wdt_pet(void);

/* The guest made observable forward progress (emitted a console byte). Refreshes
 * the software minutes-window so the watchdog stays fed. Call from vconsole on a
 * THR write. */
void wdt_note_progress(void);

/* Disable the watchdog (e.g. right before a clean return to U-Boot, so we
 * don't reset U-Boot out from under the prompt). */
void wdt_disarm(void);

/* SMP debug core (CPU1) watchdog owner: unconditionally re-arm the HW WDOG
 * unless wdt_debug_hold is set. Called every debug-poll iteration so the board
 * stays resident/inspectable while CPU1 lives. Set wdt_debug_hold != 0 over
 * EMAC to release the pet and let the HW WDOG reset the board. See wdt.c. */
void wdt_debug_kick(void);

/* Someone reached the board (emac.c: an RX frame). Every pet path above also
 * requires a reach within WDT_UNREACH_S -- see wdt.c's reachability gate. */
void wdt_note_reachable(void);
extern volatile uint32_t wdt_unreach_test;   /* !=0: ignore all reach sources */

/* Fixed-address flag, NOT a linked symbol: a host tool sets this with a
 * single `w <addr> <val>` MMIO poke (HVMAP_WDT_DEBUG_HOLD, hv_addrmap.h) --
 * no `nm`-resolved symbol address, and therefore no build-vs-running-image
 * skew risk (see hv_addrmap.h's HVMAP_WDT_DEBUG_HOLD comment for the full
 * rationale, which mirrors HVMAP_DBGTOOLS_HOLD's existing precedent). The
 * macro name matches the old plain-global identifier exactly so every
 * existing call site (`wdt_debug_hold = 1;` / `if (wdt_debug_hold)`) keeps
 * compiling completely unchanged; no BSS storage backs the name anymore. */
#define wdt_debug_hold (*(volatile uint32_t *)HVMAP_WDT_DEBUG_HOLD)

#endif /* BZDOS_WDT_H */
