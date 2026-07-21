/* wdt.h — Allwinner A64 hardware watchdog for the bzdOS microkernel.
 * The A64 WDOG does a full SoC reset in hardware, bypassing U-Boot's
 * (unsupported) software sysreset. Armed at microkernel entry and petted in
 * the poll loop: if any bring-up code (MUSB/EMAC) hangs or faults, the board
 * resets within the interval and lands back at U-Boot's '=>' (bootdelay=-1),
 * so a broken payload NEVER bricks the session — the core of the
 * "always-recoverable" design. */
#ifndef BZDOS_WDT_H
#define BZDOS_WDT_H

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
extern volatile uint32_t wdt_debug_hold;

#endif /* BZDOS_WDT_H */
