/* emac.h — sun8i-emac (Allwinner A64) raw-Ethernet console API for the bzdOS
 * microkernel. Contract shared by main_net.c (caller) and emac.c (impl).
 *
 * This is a SECOND, INDEPENDENT bring-up channel alongside the MUSB USB
 * console (musb.h/musb.c): the A64's EMAC is a separate hardware block, so
 * nothing here touches or depends on the USB gadget. Mirrors the shape of
 * musb.h so main_net.c reads like main.c.
 *
 * Wire protocol (v0 — no IP/ARP/UDP, sniff with tcpdump, inject with a raw
 * AF_PACKET socket):
 *   - Every log line is emitted as ONE raw Ethernet frame:
 *       dst       = ff:ff:ff:ff:ff:ff (broadcast)
 *       src       = 02:bd:05:00:00:01 (our locally-administered "bzdOS" MAC)
 *       ethertype = 0x88B5            (IEEE 802 local-experimental EtherType 1)
 *       payload   = ASCII text, zero-padded to the 46-byte minimum
 *   - Host->device: frames with ethertype 0x88B5 addressed to our MAC or to
 *     broadcast have their payload bytes fed to emac_getc().
 *
 * A SECOND ethertype, 0x88B6, carries the "netcon" reliable-datagram layer
 * (see netcon.h) built on top of this same raw-Ethernet TX/RX plumbing:
 *   - emac_send_frame() below queues ONE frame on any ethertype (netcon
 *     uses 0x88B6; the console path above still uses 0x88B5 internally).
 *   - emac_poll()'s RX demux hands 0x88B6 frames to netcon_rx_frame()
 *     (declared in netcon.h) INSTEAD OF the console byte ring — 0x88B5
 *     traffic is completely unaffected.
 */
#ifndef BZDOS_EMAC_H
#define BZDOS_EMAC_H
#include <stdint.h>

/* Bring up the EMAC in the fixed sequence (CCU gate/reset -> SYS_CON PHY
 * select -> PD pinmux -> EMAC soft reset -> MDIO/PHY autoneg -> MAC addr ->
 * DMA rings -> enable RX/TX). Assumes U-Boot left the MMU on with a flat
 * device mapping (same contract as musb.c) and that the PHY power rail
 * (phy-supply reg_dc1sw) is already enabled. All waits are bounded — never
 * spins forever. Returns 0 on success (link up), negative on a bounded
 * timeout (e.g. no link); the breadcrumb records how far it got either way. */
int emac_init(void);

/* Service the EMAC: drain the RX ring into the getc byte buffer and reap TX
 * completions. MUST be called as often as possible from a tight loop (like
 * musb_poll()). Cheap and non-blocking. */
void emac_poll(void);

/* Current, LIVE link state — refreshed periodically inside emac_poll() (not
 * a one-shot latch from emac_init()). A brief autoneg re-negotiation blip is
 * debounced and will NOT flip this to 0; only a link that stays down across
 * several consecutive checks is reported down. Safe to poll every loop. */
int emac_link_up(void);

/* Queue one byte for TX. Buffers into the current line; flushes the line as
 * one Ethernet frame on '\n' or when the line buffer fills. */
void emac_putc(int c);

/* emac_putc over a NUL-terminated string. */
void emac_puts(const char *s);

/* Force any buffered (un-flushed) TX bytes out as a frame now. */
void emac_flush(void);

/* Return one received payload byte (0..255), or -1 if none available. */
int emac_getc(void);

/* Send ONE raw Ethernet frame on the given ethertype: dst = broadcast,
 * src = our MAC, payload zero-padded to the 60-byte minimum frame size.
 * Reuses the same bounded TX-descriptor path as the console (emac_putc/
 * emac_flush) — non-blocking beyond that same bounded wait. Returns 1 if
 * the frame was queued to TX DMA, 0 if it was dropped (link down, or the
 * TX ring stayed busy past the bounded wait — caller/protocol above this
 * decides whether to retry). Intended for netcon.c (ethertype 0x88B6) but
 * usable for any ethertype that isn't 0x88B5 (the console owns that one). */
int emac_send_frame(uint16_t ethertype, const uint8_t *payload, uint16_t len);

/* CPU1 debug-loop link watchdog: call every loop iteration (cheap — rate-
 * limited internally to one real check per ~8 s). If EMAC has NEVER accepted
 * a single RX frame, re-runs the bounded PHY bring-up + rings/DMA/MAC enable
 * (self-heal for a PHY that trained after emac_init()'s bounded wait). After
 * LINK_WD_MAX_ATTEMPTS it gives up (breadcrumb stage 98) and returns 1
 * exactly once so the caller may escalate (e.g. opt-in self-reboot);
 * returns 0 on every other call. */
int emac_link_watchdog(void);

#endif /* BZDOS_EMAC_H */
