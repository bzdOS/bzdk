/* SPDX-License-Identifier: BSD-2-Clause */

/* repl.h — resident interactive REPL for the bzdOS microkernel (Allwinner A64).
 *
 * A line-oriented command interpreter that turns the board into a live
 * "hardware REPL": peek/poke MMIO, call functions, and re-run driver bring-up
 * AT RUNTIME over a console channel, without reloading or resetting — collapsing
 * debug iteration from a ~90 s reflash cycle to milliseconds.
 *
 * The REPL is I/O-channel agnostic: it never talks to any device directly.
 * Instead it drives three thin console hooks that the entry translation unit
 * (main_repl.c) supplies. In the shipped wiring those hooks are backed by the
 * EMAC raw-Ethernet console (emac.h) — deliberately NOT MUSB, because the USB
 * gadget is the device-under-test the REPL is used to poke at. Keeping the
 * REPL's own I/O on a separate hardware block means we can `mi`/`mp` MUSB into
 * any state (even wedged) without losing our command channel.
 *
 * Freestanding: no libc, only <stdint.h>.
 */
#ifndef BZDOS_REPL_H
#define BZDOS_REPL_H

/* Console hooks the REPL uses for ALL of its own I/O. Defined by the entry
 * translation unit (main_repl.c) as thin wrappers over the chosen channel
 * (EMAC in the shipped wiring). The REPL calls these and nothing else, so the
 * device-under-test (MUSB) is never entangled with the REPL's command channel.
 *
 *   console_getc()  -> one received byte 0..255, or -1 if none (non-blocking)
 *   console_putc(c) -> queue one byte for TX
 *   console_poll()  -> service the channel (drain RX / reap TX); called every
 *                      REPL iteration so the channel never starves
 *   console_flush() -> push any buffered TX out now. Needed because the EMAC
 *                      console only auto-flushes on '\n', but the "mk> " prompt
 *                      has no trailing newline; without this the prompt would
 *                      sit buffered and never reach the host.
 */
extern int  console_getc(void);
extern void console_putc(int c);
extern void console_poll(void);
extern void console_flush(void);
/* Liveness of the control channel. The resident loop pets the watchdog ONLY
 * while this is true, so a sustained channel loss lets the WDT recover us to
 * U-Boot without a button. */
extern int  console_link_up(void);

/* Run the interactive REPL forever. Never returns. Pets the watchdog and polls
 * the console on every iteration so the board stays resident and responsive and
 * the ~16 s WDT never fires while idle at the "mk> " prompt. */
void repl_run(void);

#endif /* BZDOS_REPL_H */
