/* SPDX-License-Identifier: BSD-2-Clause */

/* led.h — GPIO status-LED API for bzdOS microkernel (Allwinner A64, BPI-M64).
 *
 * A third, independent output channel: three on-board LEDs driven directly
 * over the PIO (pin controller) MMIO, no USB, no HDMI framebuffer, no
 * dependency on musb.c/musb.h or fb.c/fb.h. Purpose: blink out progress/
 * error codes so we can see the microkernel is alive and how far it got
 * even with zero other output channels available ("blind" bring-up).
 *
 * Hardware facts (from the board DTS, see led.c for full derivation of the
 * absolute register addresses used):
 *   PIO base = 0x01C20800, per-port block = base + port_index*0x24.
 *   RED   "pwr"  LED = port D, pin 24.
 *   GREEN "user" LED = port E, pin 14.
 *   BLUE  "user" LED = port E, pin 15.
 *
 * Freestanding: only <stdint.h>, no libc, no OS headers. MMU stays ON with
 * the flat device mapping U-Boot left in place — direct MMIO just works,
 * this module never touches the MMU.
 */
#ifndef BZDOS_LED_H
#define BZDOS_LED_H
#include <stdint.h>

enum { LED_RED = 0, LED_GREEN = 1, LED_BLUE = 2 };

/* Configure all three LED pins as GPIO output and drive them OFF. Call once
 * before any other led_* function. */
void led_init(void);

/* on != 0 lights the LED (respecting the per-color active-level #define in
 * led.c); on == 0 turns it off. `led` is one of LED_RED/LED_GREEN/LED_BLUE. */
void led_set(int led, int on);

/* Flip the LED's current output level. */
void led_toggle(int led);

/* Busy-wait blink: turn `led` on, spin `delay_loops` NOPs, turn it off, spin
 * `delay_loops` NOPs again — repeated `count` times. No timers available at
 * this stage, so callers pick delay_loops empirically for a visible blink.
 * Typical use: led_blink(LED_GREEN, stage_number, N) to signal "reached
 * stage_number" by counting flashes. */
void led_blink(int led, int count, uint32_t delay_loops);

#endif /* BZDOS_LED_H */
