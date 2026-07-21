/* led.c — GPIO status-LED implementation for bzdOS microkernel.
 * See led.h for the API contract and PROJECT.md for the general board/ABI
 * facts. This module is independent of musb.c/fb.c: plain direct MMIO to
 * the Allwinner A64 PIO (pin controller), no libc, no floats.
 *
 * ------------------------------------------------------------------------
 * Register address derivation (from the board DTS, verified facts given by
 * the caller — recomputed here, do not trust without checking the algebra):
 *
 *   PIO base                 = 0x01C20800
 *   port block(port_index)   = PIO_BASE + port_index * 0x24
 *   within a port block:
 *     CFG0 @ +0x00 (pins  0- 7)   CFG1 @ +0x04 (pins  8-15)
 *     CFG2 @ +0x08 (pins 16-23)   CFG3 @ +0x0C (pins 24-31)
 *     DATA @ +0x10
 *   per-pin CFG field: 3 bits (mode), at bit offset pos = (pin % 8) * 4
 *     (each pin gets a 4-bit slot in the CFG word, only the low 3 bits are
 *     used/defined; that's why the multiplier is 4, not 3).
 *     0b001 = GPIO output, 0b000 = input.
 *   per-pin DATA bit: bit number == pin (0..31) within the port.
 *
 * Port indices: A=0 B=1 C=2 D=3 E=4 F=5 G=6 H=7.
 *
 * --- RED "pwr" = port D, pin 24 ---
 *   port D block = 0x01C20800 + 3*0x24 = 0x01C20800 + 0x6C = 0x01C2086C
 *   pin 24 is in CFG3 (pins 24-31): CFG3 = block + 0x0C = 0x01C20878
 *   pin 24 field pos = (24 % 8) * 4 = 0 * 4 = 0        -> bits [2:0]
 *   DATA  = block + 0x10 = 0x01C2087C ; DATA bit = 24
 *
 * --- GREEN "user" = port E, pin 14 ---
 *   port E block = 0x01C20800 + 4*0x24 = 0x01C20800 + 0x90 = 0x01C20890
 *   pin 14 is in CFG1 (pins 8-15): CFG1 = block + 0x04 = 0x01C20894
 *   pin 14 field pos = (14 % 8) * 4 = 6 * 4 = 24       -> bits [26:24]
 *   DATA  = block + 0x10 = 0x01C208A0 ; DATA bit = 14
 *
 * --- BLUE "user" = port E, pin 15 ---
 *   port E block = 0x01C20890 (same block as GREEN)
 *   pin 15 is in CFG1 (pins 8-15): CFG1 = 0x01C20894 (same register as GREEN,
 *     different field)
 *   pin 15 field pos = (15 % 8) * 4 = 7 * 4 = 28       -> bits [30:28]
 *   DATA  = 0x01C208A0 (same register as GREEN, different bit) ; DATA bit = 15
 * ------------------------------------------------------------------------
 *
 * Freestanding: only <stdint.h> and "led.h". No libc. All loops bounded.
 */
#include <stdint.h>
#include "led.h"

/* ---- active-level knobs: flip to 0 for a color if it turns out wired
 * active-low (LED on when the DATA bit is 0) once checked on real HW. ---- */
#ifndef LED_RED_ACTIVE_HIGH
#define LED_RED_ACTIVE_HIGH 1
#endif
#ifndef LED_GREEN_ACTIVE_HIGH
#define LED_GREEN_ACTIVE_HIGH 1
#endif
#ifndef LED_BLUE_ACTIVE_HIGH
#define LED_BLUE_ACTIVE_HIGH 1
#endif

/* ---- recomputed absolute register addresses (see derivation above) ---- */

#define PIO_D_CFG3 0x01C20878u /* port D CFG3 (pins 24-31) */
#define PIO_D_DATA 0x01C2087Cu /* port D DATA */

#define PIO_E_CFG1 0x01C20894u /* port E CFG1 (pins 8-15) */
#define PIO_E_DATA 0x01C208A0u /* port E DATA */

#define MMIO32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* Per-LED wiring table: which CFG/DATA register, which bit position within
 * each, and the active-level knob. cfg_pos is the CFG field's low bit
 * (3-bit field); data_bit is the single DATA bit. */
struct led_pin {
	uint32_t cfg_addr;
	uint32_t cfg_pos;
	uint32_t data_addr;
	uint32_t data_bit;
	int active_high;
};

static const struct led_pin led_pins[3] = {
	[LED_RED]   = { PIO_D_CFG3, 0,  PIO_D_DATA, 24, LED_RED_ACTIVE_HIGH },
	[LED_GREEN] = { PIO_E_CFG1, 24, PIO_E_DATA, 14, LED_GREEN_ACTIVE_HIGH },
	[LED_BLUE]  = { PIO_E_CFG1, 28, PIO_E_DATA, 15, LED_BLUE_ACTIVE_HIGH },
};

/* Set an LED's DATA bit to the given raw electrical level (1 = drive high,
 * 0 = drive low) — active-level translation happens in led_set(). */
static void led_write_bit(const struct led_pin *p, int level)
{
	uint32_t v = MMIO32(p->data_addr);
	if (level)
		v |= (1u << p->data_bit);
	else
		v &= ~(1u << p->data_bit);
	MMIO32(p->data_addr) = v;
}

void led_init(void)
{
	for (int i = 0; i < 3; i++) {
		const struct led_pin *p = &led_pins[i];

		/* Read-modify-write the 3-bit CFG field: clear it, then OR in
		 * 0b001 (GPIO output). */
		uint32_t cfg = MMIO32(p->cfg_addr);
		cfg &= ~(0x7u << p->cfg_pos);
		cfg |= (0x1u << p->cfg_pos);
		MMIO32(p->cfg_addr) = cfg;

		led_set(i, 0); /* start OFF */
	}
}

void led_set(int led, int on)
{
	if (led < 0 || led > 2)
		return;

	const struct led_pin *p = &led_pins[led];
	int level = p->active_high ? (on != 0) : (on == 0);
	led_write_bit(p, level);
}

void led_toggle(int led)
{
	if (led < 0 || led > 2)
		return;

	const struct led_pin *p = &led_pins[led];
	uint32_t v = MMIO32(p->data_addr);
	v ^= (1u << p->data_bit);
	MMIO32(p->data_addr) = v;
}

static void led_delay(uint32_t delay_loops)
{
	for (volatile uint32_t i = 0; i < delay_loops; i++)
		__asm__ volatile("nop");
}

void led_blink(int led, int count, uint32_t delay_loops)
{
	for (int i = 0; i < count; i++) {
		led_set(led, 1);
		led_delay(delay_loops);
		led_set(led, 0);
		led_delay(delay_loops);
	}
}
