/* SPDX-License-Identifier: BSD-2-Clause */

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
 *       src       = the HV MAC (per board, from the SoC SID: emac_hv_mac())
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

/* As emac_send_frame(), but to a caller-supplied Ethernet destination instead
 * of broadcast. Needed by the bridged virtio-net TX path: emac_send_frame()
 * discards the guest's own destination MAC and broadcasts, which ARP survives
 * (a peer learns the sender's HA from the ARP payload, not the frame header)
 * but TCP does not -- Linux drops a unicast-IP packet delivered to the
 * broadcast MAC, so the guest's SYN-ACK was seen by tcpdump and ignored by the
 * host's stack, and every connection hung retransmitting SYN (measured
 * 2026-07-30). Source MAC is still the EMAC's own, which is correct here: the
 * guest used to be assigned the same MAC; since 2026-10-12 it has its own
 * (emac_guest_mac()) and vnet uses emac_send_frame_from() with it.
 * emac_send_frame() itself is left byte-identical -- the debug console depends
 * on it, and this channel is the only way back in. */
int emac_send_frame_to(const uint8_t dst[6], uint16_t ethertype,
                       const uint8_t *payload, uint16_t len);

/* emac_send_frame_to() with the guest's own Ethernet source address. A guest
 * that bridges another interface (WiFi AP + if_bridge) forwards frames whose
 * source is a client's MAC; rewriting it to the HV MAC hid the client from the
 * LAN, so DHCP replies and the switch's MAC learning never reached it
 * (measured 2026-10-12: a phone on the guest's AP sent DHCPDISCOVER that left
 * the board as 02:bd:05:00:00:01 and got no offer). */
int emac_send_frame_from(const uint8_t dst[6], const uint8_t src[6],
                         uint16_t ethertype, const uint8_t *payload,
                         uint16_t len);

/* CPU1 debug-loop link watchdog: call every loop iteration (cheap — rate-
 * limited internally to one real check per ~8 s). If EMAC has NEVER accepted
 * a single RX frame, re-runs the bounded PHY bring-up + rings/DMA/MAC enable
 * (self-heal for a PHY that trained after emac_init()'s bounded wait). After
 * LINK_WD_MAX_ATTEMPTS it gives up (breadcrumb stage 98) and returns 1
 * exactly once so the caller may escalate (e.g. opt-in self-reboot);
 * returns 0 on every other call. */
int emac_link_watchdog(void);

/* Watchdog-ladder test mode ("PHY refuses to train" in software): while on,
 * (a) emac_link_watchdog()'s healthy short-circuit is bypassed and (b)
 * phy_startup() fails immediately WITHOUT touching MDIO — so the re-kick
 * ladder attempts 1..7 -> BC_STAGE_WD_GAVEUP -> one-shot return 1 ->
 * dbg_emac_watchdog_reboot -> wdt_debug_hold -> WDOG reboot runs its full
 * course with the EMAC console perfectly alive throughout (the link is
 * never actually down; the console can cancel with wdtest off at any
 * point). This is the give-up/escalation half of the cold-boot gate,
 * testable with zero channel risk. Breadcrumb [26] latches the state.
 *
 * NOTE on the PHYSICAL repro (actually cutting the PHY's power): do it
 * FROM THE GUEST (/tmp/phycut.c — I2CRDWR over the guest's own iichb1
 * adapter), NEVER from EL2: the guest owns the RSB bus in the standard
 * build, and any EL2 RSB access races its driver (measured 2026-08-30:
 * one EL2 poke wedged the guest's interrupt path, killed vtnet and froze
 * cpu3). See HANDOFF.md §1. */
void emac_wd_test_mode(int on);

/* 1 while the link watchdog's PHY re-kick state machine is running (it
 * owns the PHY then; emac_poll() skips its own link_recheck()). */
int emac_rekick_active(void);

/* dbgmon `nettest` (audit item 4/5 test levers, CPU1 only):
 *  - emac_nettest_quiet(s): for s seconds the link watchdog ignores every
 *    received frame and every non-probe TX completion -- a quiet wire as
 *    the watchdog sees it, without silencing the LAN. 0 cancels.
 *  - emac_nettest_phydown(): power the PHY down (BMCR bit 11) on the next
 *    watchdog call: a REAL link loss that only the watchdog's re-kick (BMCR
 *    reset) recovers. The debug channel is down until it does (~20 s).
 *  - emac_nettest_stats(): [0] link up [1] re-kick state [2] re-kicks
 *    [3] link-down events [4] probes sent [5] TX completions seen
 *    [6] TX-stuck verdicts [7] last verdict bits (0 link bad, 1 tx stuck,
 *    2 rx ok, 3 tx ok, 4 deaf, 5 probe sent) [8] s since last RX
 *    [9] s since last TX completion [10] quiet s left [11] deaf give-ups
 *    [12] attempts [13] TX drops. */
#define EMAC_NT_NSTATS 14
void emac_nettest_quiet(uint32_t seconds);
void emac_nettest_phydown(void);
void emac_nettest_stats(uint32_t out[EMAC_NT_NSTATS]);

/* MAC addresses (audit item 7, emac.c): the HV's own (EMAC_ADDR0, source of
 * every HV frame) is derived from the A64 SID like U-Boot's eth0 ethaddr; the
 * guest's (VIRTIO_NET_F_MAC) is the same with 0x10 ORed into octet 0.
 * emac_mac_source(): 1 = SID, 2 = legacy 02:bd:05:00:00:01 fallback (SID 0).
 * emac_rx_to_hv_dropped(): non-debug frames addressed to the HV MAC, which
 * no longer reach the guest. */
void emac_hv_mac(uint8_t out[6]);
void emac_guest_mac(uint8_t out[6]);
int emac_mac_is_hv(const uint8_t m[6]);
uint32_t emac_mac_source(void);
uint32_t emac_rx_to_hv_dropped(void);

#endif /* BZDOS_EMAC_H */
