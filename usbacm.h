/* SPDX-License-Identifier: BSD-2-Clause */

/* usbacm.h — USB-OTG CDC-ACM interactive console bridge for the bzdOS EL2
 * hypervisor. Glues the already-working MUSB gadget driver (musb.c) to the
 * virtual UART0 console (vconsole.c) that traps the FreeBSD/arm64 EL1
 * guest's console I/O, so a host plugged into the board's OTG port gets a
 * real interactive /dev/ttyACM* serial console into the guest -- replacing
 * the fragile EMAC RX-injection scheme.
 *
 * Data flow (see vconsole.h's "INTERACTIVE CONSOLE" section for the ring
 * layout this reads/writes):
 *
 *   host keystroke -> USB bulk-OUT -> musb_getc() -> vconsole_rx_push()
 *       -> guest's UART0 LSR.DR/RBR emulation (vconsole_handle_fault)
 *
 *   guest THR write -> vconsole_capture_byte() tees a byte
 *       -> vconsole_tx_tee_getc() -> musb_putc()/musb_flush() -> USB bulk-IN
 *       -> host's ttyACM read()
 *
 * WHY THIS IS ITS OWN MODULE (not folded into vconsole.c or musb.c): musb.c
 * owns the MUSB hardware and its own TX/RX buffering; vconsole.c owns the
 * guest-facing UART0 trap emulation and knows nothing about USB. This file
 * is purely the poll-driven pump between the two, with no hardware or
 * guest-trap knowledge of its own -- keeping each of the other two
 * independently testable/reusable (musb.c is also used stand-alone by
 * main_repl.c/main_net.c for other purposes).
 *
 * THREADING MODEL (read before calling from anywhere but the CPU1 debug
 * loop): usbacm_poll() is the ONLY code in this tree, besides musb_init()
 * itself, that is expected to touch MUSB's MMIO registers once the
 * hypervisor is up. It is designed to be called EITHER in a tight loop from
 * the CPU1 SMP debug core (smp.c smp_secondary_main(), alongside
 * dbgmon_service()) -- the default -- OR, under the CPU1-as-vCPU1 design
 * (vcpu1.c, EXPERIMENTAL, dbg_vcpu1), from CPU1's own EL2 IRQ context, via
 * gic_timer_irq()'s MUSB_IRQ_INTID arm (gic_timer.c), the instant the real
 * MUSB "mc" SPI (musb.h) fires -- never both at once, since arming vcpu1
 * stops smp.c's tight loop from running at all (see vcpu1.h). Either way,
 * NOT from CPU0's guest-fault path, and NOT re-entrantly from more than one
 * core. Splitting the RX/TX rings so CPU0
 * only ever pushes/pops its own end (vconsole_rx_push is producer-only from
 * usbacm.c's perspective... wait, no: vconsole_rx_push is called BY this
 * module, i.e. usbacm.c/CPU1 is the producer of guest RX and the consumer of
 * guest TX; vconsole_capture_byte/CPU0 is the producer of guest TX tee and
 * the consumer of guest RX) keeps CPU0's guest-trap handling O(1) and lets
 * CPU1 own every MUSB register access, avoiding any cross-core race on the
 * MUSB/PHY/CCU hardware.
 *
 * KNOWN OPEN RISK -- guest/hypervisor MUSB hardware ownership: stage-2 is an
 * identity pass-through for all MMIO except the one UART0 page (see
 * stage2.h/stage2.c), so if the FreeBSD guest's device tree exposes the
 * OTG/MUSB node (0x01c19000, "allwinner,sun8i-a33-musb" or similar
 * compatible) as enabled, FreeBSD's own musbotg/awusbdrd driver could probe
 * and drive the SAME registers this module's usbacm_poll() (CPU1) is
 * driving, from the guest on CPU0 -- a real hardware race, not just a
 * software one. Before relying on this in day-to-day use, confirm (or have
 * the board-holding agent confirm) that the guest DTB either does not
 * enable that node, or mark it status="disabled" there. This module does
 * NOT attempt to stage-2-trap the MUSB MMIO range the way vconsole.c traps
 * UART0 -- doing that safely (extending stage2.c's per-page tables) is a
 * reasonable follow-up but is intentionally left out of this patch to avoid
 * touching stage2.c's live-tested table-building code in the same change
 * that adds a brand new, unverified-on-hardware peripheral bridge.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only, matching
 * every other module in this tree.
 */
#ifndef BZDOS_USBACM_H
#define BZDOS_USBACM_H

/* Bring up the MUSB gadget (currently a thin wrapper over musb_init()).
 * Call once, on CPU0, BEFORE smp_init() brings up the CPU1 debug core that
 * will call usbacm_poll() -- musb_init() lays down musb.c's static gadget
 * state, which the SMPEN-coherent D-cache then makes visible to CPU1
 * without any extra synchronization (same precedent as every other
 * CPU0-initializes/CPU1-consumes structure in this tree, e.g.
 * g_last_guest_frame). Safe to call unconditionally; idempotent like
 * musb_init() itself. */
void usbacm_init(void);

/* Service the gadget + pump both bridge rings. Call in a tight loop (this
 * is designed to sit right next to dbgmon_service() in smp.c's CPU1 debug
 * loop): drives musb_poll() (EP0 enumeration + EP1-OUT drain into
 * musb.c's RX ring), forwards every byte musb_getc() has into
 * vconsole_rx_push() (host -> guest), then drains up to one bounded batch
 * of vconsole_tx_tee_getc() bytes (guest -> host) into musb_putc(),
 * flushing at the end. Non-blocking and bounded per call -- never spins
 * waiting for more of either direction. */
void usbacm_poll(void);

/* 1 once the host has completed enumeration (SET_CONFIGURATION seen) and
 * /dev/ttyACM* is up on the host side. Thin wrapper over musb_ready(). */
int usbacm_ready(void);

/* Force the break-glass reset path programmatically (same effect as
 * receiving bg_seq {0x00,'~','B','Z','R','S','T',0x00} over USB:
 * sets wdt_debug_hold so the HW WDOG fires within ~16 s). Used by
 * smp.c supervision loop to wire the EMAC-dark detector (internal-note/
 * internal-note second half) to an autonomous reset without needing the
 * host to type the sequence. Board-free, testable via hosted build. */
void usbacm_force_breakglass(void);

#endif /* BZDOS_USBACM_H */
