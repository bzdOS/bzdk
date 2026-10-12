/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgdhcp.h -- a small DHCPv4 client (RFC 2131) for the HV's own address.
 *
 * The HV owns one IPv4 address for its UDP debug transport (dbgip.h). With
 * U-Boot's `bzdk_net=dhcp` (or dbgmon `ip dhcp`) that address comes from a
 * DHCP server instead of U-Boot's static `ipaddr`. This file is only the
 * protocol: no hardware, no clock, no globals -- the caller passes a
 * millisecond clock in and supplies two hooks, so the same code runs on the
 * board (dbgip.c) and natively in test_dbgdhcp.c.
 *
 * Client identity is the HV's MAC (chaddr, and option 61 = 01|MAC). A reply
 * is OURS only if it is a BOOTP reply (UDP 67 -> 68) whose chaddr is our MAC;
 * everything else -- in particular the guest's own DHCP traffic, whose
 * chaddr is the guest MAC (12:..), even when the server broadcasts it --
 * returns DHCPC_NOT_OURS and the caller passes the frame on to the guest
 * unchanged. A reply with our chaddr but a stale xid is consumed and dropped.
 *
 * States: INIT -> SELECTING (DISCOVER, backoff 2,4,..,64 s) -> REQUESTING
 * (REQUEST to the first OFFER, 4 tries) -> BOUND -> RENEWING at T1 (unicast
 * to the server) -> REBINDING at T2 (broadcast) -> lease expiry: LOST, INIT.
 * NAK anywhere: LOST (if bound), INIT after a backoff. No ARP conflict probe
 * and no DECLINE: the server is trusted to hand out free addresses.
 *
 * Not thread-safe: one caller context (dbgip.c: emac.c's RX drain + tick,
 * which never run concurrently).
 */
#ifndef DBGDHCP_H
#define DBGDHCP_H

#include <stdint.h>

enum {
	DHCPC_OFF = 0,          /* stopped                               */
	DHCPC_INIT,             /* DISCOVER due at `next`                 */
	DHCPC_SELECTING,        /* DISCOVER sent, waiting for an OFFER    */
	DHCPC_REQUESTING,       /* REQUEST sent for an OFFER              */
	DHCPC_BOUND,
	DHCPC_RENEWING,         /* after T1: unicast REQUEST to server    */
	DHCPC_REBINDING,        /* after T2: broadcast REQUEST            */
};

#define DHCPC_NOT_OURS   0
#define DHCPC_TAKEN      1

#define DHCPC_INFINITE   0xffffffffu

struct dhcpc_lease {
	uint32_t ip, mask, router, server, dns;
	uint32_t lease_s, t1_s, t2_s;    /* as granted (seconds)            */
};

struct dhcpc {
	uint8_t  mac[6];
	uint8_t  srv_mac[6];             /* Ethernet source of the last ACK */
	uint32_t state;
	uint32_t xid;
	uint32_t rng;
	uint32_t tries;                  /* sends in the current state      */
	uint32_t naks;                   /* consecutive NAKs (backoff)      */
	uint32_t offer_ip, offer_srv;    /* the OFFER being requested       */
	uint64_t next;                   /* ms: next (re)send               */
	uint64_t t_req;                  /* ms: last REQUEST sent (lease start) */
	uint64_t t1, t2, t_exp;          /* ms, absolute; 0 = never         */
	struct dhcpc_lease lease;        /* valid while bound/renew/rebind  */
	uint16_t ipid;
	/* counters */
	uint32_t n_tx, n_offer, n_ack, n_nak, n_stale, n_bad, n_lost;
};

/* Provided by the caller. dhcpc_xmit: send `ipv4` (IPv4 header onwards,
 * `len` bytes) in an Ethernet frame to `dst` from the client's MAC. */
int  dhcpc_xmit(const uint8_t dst[6], const uint8_t *ipv4, uint32_t len);
/* dhcpc_bound: a lease was granted or renewed (l = the lease).
 * dhcpc_lost:  the lease ended (expiry, NAK, stop). */
void dhcpc_bound(const struct dhcpc_lease *l);
void dhcpc_lost(void);

/* Start (or restart) from INIT; the first DISCOVER goes out at the next
 * tick. `seed` feeds the xid/jitter generator (timer + MAC is fine). */
void dhcpc_start(struct dhcpc *c, const uint8_t mac[6], uint32_t seed,
		 uint64_t now_ms);
/* Stop; sends a RELEASE first when `release` and a lease is held. */
void dhcpc_stop(struct dhcpc *c, int release);
/* Timers. Call often (every few ms .. 1 s). */
void dhcpc_tick(struct dhcpc *c, uint64_t now_ms);
/* One received Ethernet frame (no FCS). DHCPC_TAKEN = a reply for this
 * client (handled or dropped); DHCPC_NOT_OURS = pass it on. */
int  dhcpc_rx(struct dhcpc *c, const uint8_t *f, uint32_t len,
	      uint64_t now_ms);

static inline int dhcpc_has_lease(const struct dhcpc *c)
{
	return c->state >= DHCPC_BOUND;
}
const char *dhcpc_state_name(uint32_t st);

#endif /* DBGDHCP_H */
