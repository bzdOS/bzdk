/* vconsole.h — trap-and-emulate virtual UART0 console for the FreeBSD/
 * arm64 EL1 guest running under our from-scratch EL2 hypervisor on the
 * Allwinner A64 (Banana Pi M64).
 *
 * WHY THIS EXISTS: FreeBSD's console is physical UART0 (0x01C28000, see
 * stage2.h's UART0_BASE / the DTB's chosen/stdout-path), but our host link
 * is the USB gadget, not the UART pins — we are not wired to see real
 * UART0 output. stage2.c now leaves the UART0_BASE page UNMAPPED in the
 * stage-2 identity map (see stage2.h's table-topology comment), so every
 * guest access to it takes a stage-2 translation fault into EL2 instead of
 * reaching hardware. This file is the EL2-side handler for that fault: it
 * emulates just enough of an 8250/16550 UART for FreeBSD's low-level
 * console driver to work (never blocks on transmit-ready, ignores
 * everything but THR/LSR/USR), and captures every transmitted byte into a
 * DRAM ring buffer that survives past a WDT reset so the byte stream can be
 * read externally after the fact via `md`/`bc`-style physical memory
 * inspection (no live UART needed).
 *
 * Wiring contract (owned by the el2_trap integrator, not this file):
 *   On a lower-EL sync exception (frame->kind >> 2 == 2, i.e. a trap from
 *   the EL1 guest, kind bit pattern per exceptions.h) with
 *   ESR_EL2.EC == 0x24 (data abort from a lower EL), call
 *   vconsole_handle_fault(frame) FIRST, before any generic fault-record
 *   path:
 *
 *       if ((frame->kind >> 2) == 2 &&
 *           ((frame->esr >> 26) & 0x3F) == 0x24) {
 *               if (vconsole_handle_fault(frame))
 *                       return;         // handled: emulated, ELR advanced
 *               // else: not ours (or undecodable) -- fall through to the
 *               // existing generic fault-record path
 *       }
 *
 *   vconsole_init() must be called once, before stage2_enable() (or at
 *   least before the guest can possibly fault on UART0), to lay down the
 * capture ring's magic + zero its counters. Calling it more than once is
 *   safe (idempotent re-init) but not required.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only. All ring
 * writes are cache-coherent (dc civac + dsb sy per store), the same
 * pattern as every other breadcrumb window in this tree, so the ring is
 * readable via physical memory dump even across the ~6s WDT reset that
 * main_fbsd.c arms before entering the guest.
 */
#ifndef BZDOS_VCONSOLE_H
#define BZDOS_VCONSOLE_H

#include <stdint.h>
#include "exceptions.h"

/* ------------------------------------------------------------------ *
 * Capture ring, fixed at physical/virtual (identity-mapped, MMU-off guest)
 * DRAM address 0x50000f00 — distinct from every other breadcrumb window in
 * this tree (MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300, EL2
 * exceptions 0x50000400, jitter/TIMR 0x50000500, ring 0x50000600, alloc
 * 0x50000700, GIC timer 0x50000800, sched 0x50000a00, guest 0x50000b00,
 * stage2 0x50000c00, main_fbsd 0x50000e00).
 *
 * Layout (all header words are 32-bit, little-endian, at word granularity
 * from VCONSOLE_RING_BASE):
 *   word[0] (0x50000f00) magic         VCONSOLE_MAGIC ("UART" ASCII-packed
 *                                       big-endian-looking 0x55415254, i.e.
 *                                       bytes 'U' 'A' 'R' 'T' as loaded by
 *                                       a 32-bit `md.l` read)
 *   word[1] (0x50000f04) total_bytes   running count of bytes captured
 *                                       (transmit writes seen), NOT
 *                                       clamped to the buffer size -- use
 *                                       it mod VCONSOLE_BUF_SIZE to find
 *                                       the logical write offset if it
 *                                       ever wraps
 *   word[2] (0x50000f08) fault_count   running count of stage-2 faults
 *                                       this module was asked to handle
 *                                       (whether emulated or given up on)
 *   word[3] (0x50000f0c) reserved      always written 0 by vconsole_init()
 *                                       (padding so the byte buffer below
 *                                       starts on a clean word[4] boundary,
 *                                       matching the brief's "word[4]
 *                                       onward")
 *   bytes starting at word[4], i.e. VCONSOLE_RING_BASE + 0x10
 *   (= 0x50000f10) up to VCONSOLE_RING_BASE + 0x10 + VCONSOLE_BUF_SIZE
 *   (= 0x50001b10 for the default 3 KiB buffer): the captured console
 *   byte stream, oldest-to-newest within any given wrap, written 1 byte
 *   at a time and wrapping (overwriting from the start) once
 *   VCONSOLE_BUF_SIZE bytes have been captured. For early boot console
 *   output (a few KiB at most before the WDT fires), this will not wrap
 *   in practice.
 *
 * Reading it back: `md.l 0x50000f00 4` gives magic/total_bytes/fault_count
 * /reserved; `md 0x50000f10 <n>` (byte-granularity memory dump) or
 * equivalent shows the captured ASCII console text directly.
 *
 * ------------------------------------------------------------------------
 * INTERACTIVE CONSOLE (USB-OTG CDC-ACM bridge, usbacm.c) -- RX injection +
 * TX tee, both consumed/produced by usbacm.c on the CPU1 debug core:
 *
 *   Host -> guest (RX): usbacm.c calls vconsole_rx_push() with each byte it
 *   reads off the gadget's bulk-OUT endpoint (i.e. what the user typed into
 *   /dev/ttyACM*). vconsole_handle_fault()'s read path (vconsole.c) drains
 *   this same ring: an LSR read ORs in UART_LSR_DR (bit0) whenever a byte is
 *   waiting, and an RBR read (THR's byte offset, 0x00, on a read) dequeues
 *   the next one -- exactly what FreeBSD's ns8250 RX poll loop expects, so
 *   the guest's console becomes interactive (mountroot>, login, a shell)
 *   instead of output-only. Ring: 0x50000e40 head / 0x50000e44 tail /
 *   0x50000e48 buf[128] (see vconsole.c).
 *
 *   Guest -> host (TX tee): every byte the guest writes to THR is, as
 *   before, appended to the 64 KiB postmortem capture ring above, AND tee'd
 *   into a small separate 256-byte ring at 0x50004000 (head)/0x50004004
 *   (tail)/0x50004008 (buf) that usbacm.c drains every poll and pushes out
 *   the gadget's bulk-IN endpoint. Two separate rings on purpose: the 64 KiB
 *   one is write-only (for a memory dump after the fact) and must never be
 *   drained; the TX tee is actively consumed and must stay small/bounded.
 * ------------------------------------------------------------------------ */
#define VCONSOLE_RING_BASE   0x50000f00UL
#define VCONSOLE_MAGIC       0x55415254u   /* "UART" */

#define VCONSOLE_HDR_WORDS   4u                       /* magic/total/faults/reserved */
#define VCONSOLE_HDR_SIZE    (VCONSOLE_HDR_WORDS * 4u) /* 16 bytes -> buffer @ +0x10 */
#define VCONSOLE_BUF_BASE    (VCONSOLE_RING_BASE + VCONSOLE_HDR_SIZE) /* 0x50000f10 */
/* 64 KiB capture window (was 3 KiB): a full FreeBSD verbose boot is ~40 KiB, so
 * the ENTIRE boot log fits with no wrap — the mountroot failure + "List of GEOM
 * managed disk devices" + aw_mmc/mmcsd attach lines survive even the cngrab
 * panic-loop (each panic dump is ~600 B, 64 KiB >> that). Buffer spans
 * 0x50000f10..0x50010f10; this overlaps the (currently unused-in-this-boot)
 * gtrace/firstfault/hdmi/... breadcrumb windows, which is fine while chasing
 * the guest root-mount. Guest RAM is 0x40000000-0x80000000 but the guest does
 * not clobber this low breadcrumb region during early boot (same reason the
 * 3 KiB ring survived). */
#define VCONSOLE_BUF_SIZE    0x10000u                  /* 64 KiB capture window */

/* Lay down the ring's magic + zero its counters/buffer. Call once before
 * the guest can fault on UART0 (i.e. before stage2_enable() in
 * main_fbsd.c). Idempotent. */
void vconsole_init(void);

/* Called from el2_trap (see the wiring contract in the file banner above)
 * on a guest (lower-EL) synchronous data abort. Inspects frame->esr/far
 * and either fully emulates the UART register access (THR write capture,
 * LSR/USR read synthesis, ignoring init-time writes to IER/LCR/FCR/MCR),
 * advancing frame->elr past the faulting instruction, or determines the
 * fault isn't a UART0 access at all.
 *
 * Returns 1 if the fault was handled here (el2_trap should return
 * immediately, without recording a generic fault) or 0 if it was not ours
 * -- either the FAR is outside [UART0_BASE, UART0_BASE + UART0_SIZE), or
 * ESR.EC wasn't a data abort, in which case el2_trap should fall through
 * to its existing generic fault-record path. (An ISV=0 syndrome that IS a
 * UART0-range data abort we can't decode is still counted and its ELR
 * advanced -- see vconsole.c -- and this still returns 1, since giving up
 * and re-recording a "real" fault for an address we know is UART0 is not
 * useful and would just spin the same fault forever.) */
int vconsole_handle_fault(struct el2_frame *frame);

/* ------------------------------------------------------------------ *
 * USB-ACM bridge API (see the "INTERACTIVE CONSOLE" section above).
 * Intended caller: usbacm.c, polled on the CPU1 debug core. Safe to call
 * from any core (bounded ring ops, no locks needed -- SMPEN gives cache
 * coherency, and each ring has exactly one producer core and one consumer
 * core by convention), but calling from anywhere else defeats the
 * single-owner-of-MUSB-hardware discipline usbacm.c documents.
 * ------------------------------------------------------------------ */

/* Push one host-typed byte (read off the gadget's bulk-OUT endpoint) into
 * the guest's virtual UART0 RX ring. Drops the byte if the ring is full
 * (guest not draining fast enough) rather than overwriting or blocking. */
void vconsole_rx_push(uint8_t c);

/* Dequeue one byte the guest wrote to THR, tee'd for transmission out the
 * gadget's bulk-IN endpoint. Returns 1 and stores it into *out, or 0 if
 * nothing is currently pending. */
int vconsole_tx_tee_getc(uint8_t *out);

#endif /* BZDOS_VCONSOLE_H */
