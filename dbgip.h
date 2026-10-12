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
 *     env export -t 0x4a0f0000 gatewayip ipaddr netmask serverip
 * before bootm; dbgip_init() parses that text and zeroes it. No block, or
 * no ipaddr in it: the UDP transport is off and nothing changes. dbgmon
 * `ip` shows the state, `ip set ...` changes it at run time.
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

/* Run-time configuration (dbgmon `ip set`); ip == 0 turns UDP off. */
void dbgip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t host,
	       uint32_t port);

enum {
	DBGIP_ST_IP = 0, DBGIP_ST_MASK, DBGIP_ST_GW, DBGIP_ST_HOST, DBGIP_ST_PORT,
	DBGIP_ST_SRC,          /* 0 off, 1 U-Boot env, 2 dbgmon */
	DBGIP_ST_PEER_IP, DBGIP_ST_PEER_PORT,
	DBGIP_ST_RX_UDP,       /* datagrams to our port                */
	DBGIP_ST_RX_ACCEPT,    /* ...whose inner frame dbgauth accepted */
	DBGIP_ST_TX_UDP,       /* frames sent wrapped                  */
	DBGIP_ST_ARP_RX,       /* ARP frames taken                     */
	DBGIP_ST_ARP_TX,       /* ARP replies + requests sent          */
	DBGIP_ST_ICMP,         /* echo requests answered               */
	DBGIP_ST_DROP,         /* bad/oversize/unroutable              */
	DBGIP_ST_NH_OK,        /* next hop to serverip resolved (0/1)  */
	DBGIP_ST_N
};
void dbgip_stats(uint32_t out[DBGIP_ST_N]);

#endif /* DBGIP_H */
