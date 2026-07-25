/* SPDX-License-Identifier: BSD-2-Clause */

/* pl011_qemu.h — minimal ARM PL011 UART driver for the QEMU `virt` CI
 * target.
 *
 * WHY THIS EXISTS (vs reusing vconsole.c): on the real A64 board, the HV's
 * host link is the USB gadget, not the UART pins, so vconsole.c
 * trap-and-emulates a fake 16550 register set purely in software and
 * captures bytes to a DRAM ring for later inspection over EMAC/USB. Under
 * QEMU there is no such constraint — `-machine virt` provides a REAL
 * (emulated-but-functional) PL011 UART at a fixed MMIO address, wired to
 * QEMU's `-serial` chardev, and stage2.c's existing MMIO identity map
 * (0x00000000..0x40000000, Device-nGnRE, unmodified — see stage2.h) already
 * covers it. So both the HV (EL2) and the guest (EL1, once stage-2 is
 * enabled) can just write to it DIRECTLY and see real output — no
 * trap-and-emulate needed, no ring buffer, no vconsole.c involved at all
 * for this target.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only.
 */
#ifndef BZDOS_PL011_QEMU_H
#define BZDOS_PL011_QEMU_H

#include <stdint.h>

/* Program UARTCR/UARTLCR_H for 8N1 with FIFOs enabled and TX/RX on. Safe to
 * call once at start of day, before anything else in this build prints. */
void pl011_init(void);

/* Blocking single-character write (busy-waits on UARTFR.TXFF). '\n' is NOT
 * auto-translated here — callers wanting CRLF use pl011_puts(), which does
 * translate it, since raw terminals/log-capture want that. */
void pl011_putc(char c);

/* Blocking NUL-terminated string write; each '\n' is preceded by a '\r' so
 * plain terminal captures (and `qemu -serial stdio`/`-serial file:...`)
 * render lines correctly. */
void pl011_puts(const char *s);

/* Minimal formatting helpers — this build has no libc, no printf. Print an
 * unsigned 32-bit value in hex (fixed 8 hex digits, no "0x" prefix — callers
 * that want the prefix print it themselves, matching this tree's existing
 * house style of literal "0x" in surrounding pl011_puts() calls) / decimal
 * (no leading zeros, "0" for zero) / a 64-bit hex value (16 digits). */
void pl011_put_hex32(uint32_t v);
void pl011_put_hex64(uint64_t v);
void pl011_put_udec(uint32_t v);

#endif /* BZDOS_PL011_QEMU_H */
