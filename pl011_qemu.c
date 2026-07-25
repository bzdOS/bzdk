/* SPDX-License-Identifier: BSD-2-Clause */

/* pl011_qemu.c — minimal ARM PL011 UART driver for the QEMU `virt` CI
 * target. See pl011_qemu.h for the "why not vconsole.c" rationale.
 *
 * ------------------------------------------------------------------
 * PL011 MMIO base — cited, not guessed: QEMU's hw/arm/virt.c
 * `a15memmap[VIRT_UART]` is { 0x09000000, 0x00001000 }, unchanged across
 * QEMU releases (it is part of the machine's stable ABI — Linux/U-Boot/EDK2
 * all hardcode it for `-M virt`). This is also the address QEMU's own
 * `-serial` chardev is wired to for this one UART instance.
 * ------------------------------------------------------------------
 */
#include <stdint.h>
#include "pl011_qemu.h"

#define UART_BASE 0x09000000UL

#define UARTDR    (*(volatile uint32_t *)(UART_BASE + 0x000))
#define UARTFR    (*(volatile uint32_t *)(UART_BASE + 0x018))
#define UARTIBRD  (*(volatile uint32_t *)(UART_BASE + 0x024))
#define UARTFBRD  (*(volatile uint32_t *)(UART_BASE + 0x028))
#define UARTLCR_H (*(volatile uint32_t *)(UART_BASE + 0x02c))
#define UARTCR    (*(volatile uint32_t *)(UART_BASE + 0x030))
#define UARTICR   (*(volatile uint32_t *)(UART_BASE + 0x044))

#define UARTFR_TXFF (1u << 5)   /* transmit FIFO full */

#define UARTLCR_H_FEN   (1u << 4)          /* enable FIFOs */
#define UARTLCR_H_WLEN8 (3u << 5)          /* 8 data bits */

#define UARTCR_UARTEN (1u << 0)
#define UARTCR_TXE    (1u << 8)
#define UARTCR_RXE    (1u << 9)

void
pl011_init(void)
{
	UARTCR    = 0;                 /* disable while reconfiguring */
	UARTICR   = 0x7ffu;             /* clear any pending interrupt state */
	UARTIBRD  = 1;                  /* baud divisor is meaningless to QEMU's
	                                 * chardev backend, but real PL011 HW
	                                 * requires SOME value be programmed
	                                 * before UARTEN — keep this realistic. */
	UARTFBRD  = 0;
	UARTLCR_H = UARTLCR_H_WLEN8 | UARTLCR_H_FEN;
	UARTCR    = UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE;
}

void
pl011_putc(char c)
{
	while (UARTFR & UARTFR_TXFF)
		;
	UARTDR = (uint32_t)(uint8_t)c;
}

void
pl011_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			pl011_putc('\r');
		pl011_putc(*s);
		s++;
	}
}

void
pl011_put_hex32(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 7; i >= 0; i--)
		pl011_putc(hex[(v >> (i * 4)) & 0xfu]);
}

void
pl011_put_hex64(uint64_t v)
{
	pl011_put_hex32((uint32_t)(v >> 32));
	pl011_put_hex32((uint32_t)v);
}

void
pl011_put_udec(uint32_t v)
{
	char buf[10];   /* max uint32 = 4294967295 -> 10 digits */
	int  i = 0;

	if (v == 0) {
		pl011_putc('0');
		return;
	}
	while (v > 0) {
		buf[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i > 0)
		pl011_putc(buf[--i]);
}
