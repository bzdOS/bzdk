/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgip.h -- the HV debug channel over UDP/IPv4, so it crosses a router.
 *
 * The raw-Ethernet debug plane (0x88B5..0x88B9, emac.c + dbgauth.c) only
 * reaches hosts on the board's own segment. This adds a second transport
 * for the SAME frames: the HV owns one IPv4 address (U-Boot's `ipaddr`, it
 * already shares U-Boot's MAC), answers ARP and ICMP echo for it, and takes
 * UDP datagrams to DBGIP_PORT whose payload is one whole debug Ethernet
 * frame (14-byte header + payload, exactly what the L2 transport puts on
 * the wire). The inner frame goes through dbgauth_rx() like any other: the
 * UDP layer adds no trust of its own.
 *
 * Replies: a frame the HV sends to the inner source MAC of the UDP peer is
 * wrapped Eth/IPv4/UDP and sent back to the Ethernet address the peer's
 * datagram came from (the host on-link, or the router) -- no routing table.
 * The peer is committed only by an authenticated DATA frame; HELLO/NAK
 * replies go to the datagram being processed. Until a peer exists this
 * boot, broadcast debug output (boot log, panic text) is also sent to
 * `serverip`:DBGIP_PORT, through the gateway when serverip is off-link.
 *
 * Configuration (no rebuild): U-Boot's bootcmd runs
 *     env export -t 0x4a0f0000 gatewayip ipaddr netmask serverip bzdk_net
 * before bootm; dbgip_init() parses that text and zeroes it. No block, or
 * no ipaddr in it: the UDP transport is off and nothing changes. dbgmon
 * `ip` shows the state, `ip set ...` changes it at run time.
 *
 * DHCP (bzdk_net=dhcp; anything else, or absent, is the static mode above):
 * the address, mask and router come from a DHCP lease taken with the HV's
 * own MAC (dbgdhcp.c, driven from dbgip_tick()). Only replies whose chaddr
 * is the HV's MAC are taken; the guest's DHCP (chaddr = guest MAC) passes
 * to it untouched, broadcast or not. No lease within DBGIP_FALLBACK_S of
 * boot: U-Boot's static ipaddr/netmask/gatewayip is used (src
 * env-fallback) while DHCP keeps trying; a lease replaces it when it
 * comes, and losing the lease goes back to it. `serverip` is always the
 * env's: it is the one fixed address. dbgmon `ip dhcp` / `ip static`
 * switch at run time.
 *
 * Announce: whenever the address changes, and every DBGIP_ANN_S while no
 * UDP peer is active, the HV sends serverip:port one inner 0x88B5 frame
 * of envelope type DBGIP_ANN_TYPE (after the dbgauth types):
 *     00 'B' 'Z' 'K' | 05 | 01 | ip[4] | mask[4] | gw[4] | src | mode |
 *     guest_ip[4] | guest_mac[6] | zero padding to 46
 * It is NOT authenticated. A host must not trust it: bzd_udplink.py only
 * adopts a new board address after a HELLO/CHAL handshake to it verifies.
 *
 * Guest address: dbgip_guest_tx() sees every frame the guest sends (vnet
 * TX) and records the IPv4 address it uses (ARP sender, else IPv4 source,
 * with the guest's MAC as Ethernet source). dbgmon `ip` and `mac` show it.
 *
 * Everything else -- the guest's own IPv4 traffic, ARP for other
 * addresses, broadcasts -- is not touched and reaches vnet as before.
 */
#ifndef DBGIP_H
#define DBGIP_H

#include <stdint.h>

#define DBGIP_PORT       34997u          /* 0x88B5 */
#define DBGIP_HDR_LEN    42u             /* Ethernet 14 + IPv4 20 + UDP 8 */
#define DBGIP_INNER_MAX  1472u           /* UDP payload at a 1500-byte MTU */
#define DBGIP_CFG_PA     0x4a0f0000UL    /* U-Boot `env export -t` target */
#define DBGIP_CFG_MAX    1024u
#define DBGIP_FALLBACK_S 10u             /* DHCP: static env IP after this */
#define DBGIP_ANN_S      30u             /* announce period without a peer */
#define DBGIP_ANN_TYPE   5u
#define DBGIP_ANN_LEN    46u

#define DBGIP_MODE_STATIC 0u
#define DBGIP_MODE_DHCP   1u

/* TX routing decisions (dbgip_tx_route) */
#define DBGIP_TX_L2      0   /* plain Ethernet, as before          */
#define DBGIP_TX_UDP     1   /* wrapped only                       */
#define DBGIP_TX_BOTH    2   /* L2 as is, plus a wrapped copy      */
#define DBGIP_TX_DROP    3   /* for the UDP peer but too big: drop */

void dbgip_init(void);
/* One received frame (no FCS). 1 = consumed by the HV, 0 = not ours. */
int  dbgip_rx(const uint8_t *f, uint32_t len);
/* Housekeeping from emac_poll(): ARP resolution of the next hop. */
void dbgip_tick(void);
/* For an HV-originated frame (src = HV MAC) of `inner_len` bytes incl. the
 * 14-byte Ethernet header: decide L2/UDP/both; for UDP fills hdr[42]. */
int  dbgip_tx_route(const uint8_t dst[6], uint16_t ethertype,
		    uint32_t inner_len, uint8_t hdr[DBGIP_HDR_LEN]);

/* 1 + the UDP peer's inner MAC if it spoke within the last 120 s. */
int  dbgip_peer_active(uint8_t inner[6]);

/* Run-time configuration (dbgmon `ip set`); ip == 0 turns UDP off.
 * Switches to static mode; the values become the fallback. */
void dbgip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t host,
	       uint32_t port);
/* dbgmon `ip dhcp` / `ip static` (DBGIP_MODE_*). */
void dbgip_mode(uint32_t mode);

/* vnet TX: one frame the guest sent (at least its first 42 bytes). */
void dbgip_guest_tx(const uint8_t *f, uint32_t len);
/* The guest's IPv4 address as last seen, 0 = none yet. */
uint32_t dbgip_guest_ip(void);
/* One human-readable line (no newline): mode, address, lease, guest. */
void dbgip_format(char *buf, uint32_t n);

enum {
	DBGIP_ST_IP = 0, DBGIP_ST_MASK, DBGIP_ST_GW, DBGIP_ST_HOST, DBGIP_ST_PORT,
	DBGIP_ST_SRC,          /* 0 off, 1 env, 2 dbgmon, 3 dhcp, 4 env fallback */
	DBGIP_ST_PEER_IP, DBGIP_ST_PEER_PORT,
	DBGIP_ST_RX_UDP,       /* datagrams to our port                */
	DBGIP_ST_RX_ACCEPT,    /* ...whose inner frame dbgauth accepted */
	DBGIP_ST_TX_UDP,       /* frames sent wrapped                  */
	DBGIP_ST_ARP_RX,       /* ARP frames taken                     */
	DBGIP_ST_ARP_TX,       /* ARP replies + requests sent          */
	DBGIP_ST_ICMP,         /* echo requests answered               */
	DBGIP_ST_DROP,         /* bad/oversize/unroutable              */
	DBGIP_ST_NH_OK,        /* next hop to serverip resolved (0/1)  */
	DBGIP_ST_MODE,         /* DBGIP_MODE_*                         */
	DBGIP_ST_DHCP_STATE,   /* dbgdhcp.h DHCPC_* (0 in static mode) */
	DBGIP_ST_DHCP_SERVER,  /* server of the lease held             */
	DBGIP_ST_LEASE_LEFT,   /* seconds, ffffffff = infinite         */
	DBGIP_ST_DHCP_RX,      /* DHCP replies taken (chaddr = HV MAC) */
	DBGIP_ST_ANNOUNCE,     /* announces sent                       */
	DBGIP_ST_GUEST_IP,     /* guest's IPv4 (vnet TX), 0 = unseen   */
	DBGIP_ST_N
};
void dbgip_stats(uint32_t out[DBGIP_ST_N]);

#endif /* DBGIP_H */
