/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgdhcp.c -- DHCPv4 client for the HV's own address. Design: dbgdhcp.h.
 *
 * Freestanding: <stdint.h> and our header only, no clock or hardware access,
 * so test_dbgdhcp.c compiles this very file natively.
 */
#include <stdint.h>
#include "dbgdhcp.h"

#define ET_IPV4         0x0800u
#define BOOTP_LEN       236u            /* fixed BOOTP part, up to the cookie */
#define DHCP_MIN_LEN    300u            /* BOOTP minimum (pad options)        */
#define PKT_MAX         (20u + 8u + 548u)

/* message types (option 53) */
#define DISCOVER 1u
#define OFFER    2u
#define REQUEST  3u
#define DECLINE  4u
#define ACK      5u
#define NAK      6u
#define RELEASE  7u

static const uint8_t g_bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const char g_hostname[] = "bzdk";

static inline uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 6; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static uint32_t csum_add(uint32_t sum, const uint8_t *p, uint32_t n)
{
	while (n > 1) {
		sum += ((uint32_t)p[0] << 8) | p[1];
		p += 2;
		n -= 2;
	}
	if (n)
		sum += (uint32_t)p[0] << 8;
	return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

static uint32_t rnd(struct dhcpc *c)
{
	uint32_t x = c->rng ? c->rng : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	c->rng = x;
	return x;
}

const char *dhcpc_state_name(uint32_t st)
{
	static const char *const n[] = { "off", "init", "selecting",
		"requesting", "bound", "renewing", "rebinding" };
	return st < sizeof(n) / sizeof(n[0]) ? n[st] : "?";
}

/* ---- TX ------------------------------------------------------------------ */

/* Build and send one message. ciaddr/src_ip: 0 or the leased address;
 * dst_ip/dst_mac: broadcast or the server. req_ip/srv_id: options 50/54
 * (0 = omitted). */
static void send_msg(struct dhcpc *c, uint32_t type, uint32_t ciaddr,
		     uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
		     uint32_t req_ip, uint32_t srv_id)
{
	uint8_t p[PKT_MAX];
	uint8_t *ip = p, *u = p + 20, *b = p + 28, *o;
	uint32_t blen, ulen, sum;

	for (uint32_t i = 0; i < sizeof(p); i++)
		p[i] = 0;
	b[0] = 1;                         /* BOOTREQUEST */
	b[1] = 1;                         /* Ethernet */
	b[2] = 6;
	put32(b + 4, c->xid);
	put32(b + 12, ciaddr);
	for (int i = 0; i < 6; i++)
		b[28 + i] = c->mac[i];
	o = b + BOOTP_LEN;
	*o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
	*o++ = 61; *o++ = 7; *o++ = 1;    /* client id: htype + MAC */
	for (int i = 0; i < 6; i++)
		*o++ = c->mac[i];
	if (req_ip) {
		*o++ = 50; *o++ = 4; put32(o, req_ip); o += 4;
	}
	if (srv_id) {
		*o++ = 54; *o++ = 4; put32(o, srv_id); o += 4;
	}
	if (type != RELEASE) {
		*o++ = 12; *o++ = (uint8_t)(sizeof(g_hostname) - 1);
		for (uint32_t i = 0; i < sizeof(g_hostname) - 1; i++)
			*o++ = (uint8_t)g_hostname[i];
		*o++ = 57; *o++ = 2; put16(o, 576); o += 2;
		*o++ = 55; *o++ = 6;          /* parameter request list */
		*o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51; *o++ = 58; *o++ = 59;
	}
	*o++ = 255;
	blen = (uint32_t)(o - b);
	if (blen < DHCP_MIN_LEN)
		blen = DHCP_MIN_LEN;          /* zero padding after END */

	ulen = 8u + blen;
	put16(u, 68);
	put16(u + 2, 67);
	put16(u + 4, ulen);
	ip[0] = 0x45;
	put16(ip + 2, 20u + ulen);
	put16(ip + 4, c->ipid++);
	ip[8] = (type == RELEASE || src_ip) ? 64 : 16;
	ip[9] = 17;
	put32(ip + 12, src_ip);
	put32(ip + 16, dst_ip);
	put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
	sum = csum_add(0, ip + 12, 8);
	sum += 17u + ulen;
	sum = csum_add(sum, u, ulen);
	sum = csum_fold(sum);
	put16(u + 6, sum ? sum : 0xffffu);

	(void)dhcpc_xmit(dst_mac, p, 20u + ulen);
	c->n_tx++;
}

/* 2, 4, 8, .. 64 s (+ up to 1 s of jitter) for the n-th retransmission. */
static uint64_t backoff_ms(struct dhcpc *c, uint32_t n)
{
	uint64_t s = n >= 5 ? 64u : (2u << n);
	return s * 1000u + (rnd(c) & 1023u);
}

static void new_xid(struct dhcpc *c)
{
	c->xid = rnd(c);
}

static void send_discover(struct dhcpc *c, uint64_t now)
{
	send_msg(c, DISCOVER, 0, 0, 0xffffffffu, g_bcast_mac, 0, 0);
	c->next = now + backoff_ms(c, c->tries);
	c->tries++;
}

static void send_select_request(struct dhcpc *c, uint64_t now)
{
	send_msg(c, REQUEST, 0, 0, 0xffffffffu, g_bcast_mac, c->offer_ip,
		 c->offer_srv);
	c->t_req = now;
	c->next = now + backoff_ms(c, c->tries);
	c->tries++;
}

/* RENEWING/REBINDING retransmission: half the time left to `until`, at
 * least 4 s (RFC 2131 4.4.5 says 60 s; short test leases need less). */
static uint64_t half_left(uint64_t now, uint64_t until)
{
	uint64_t h = until > now ? (until - now) / 2u : 0;
	if (h < 4000u)
		h = 4000u;
	return now + h;
}

static void send_renew(struct dhcpc *c, uint64_t now)
{
	send_msg(c, REQUEST, c->lease.ip, c->lease.ip, c->lease.server,
		 c->srv_mac, 0, 0);
	c->t_req = now;
	c->next = half_left(now, c->t2);
	c->tries++;
}

static void send_rebind(struct dhcpc *c, uint64_t now)
{
	send_msg(c, REQUEST, c->lease.ip, c->lease.ip, 0xffffffffu,
		 g_bcast_mac, 0, 0);
	c->t_req = now;
	c->next = half_left(now, c->t_exp);
	c->tries++;
}

/* ---- state changes -------------------------------------------------------- */

static void to_init(struct dhcpc *c, uint64_t now, uint64_t delay)
{
	int had = dhcpc_has_lease(c);

	c->state = DHCPC_INIT;
	c->tries = 0;
	c->next = now + delay;
	c->t1 = c->t2 = c->t_exp = 0;
	if (had) {
		c->n_lost++;
		dhcpc_lost();
	}
}

void dhcpc_start(struct dhcpc *c, const uint8_t mac[6], uint32_t seed,
		 uint64_t now_ms)
{
	uint8_t *z = (uint8_t *)c;

	if (dhcpc_has_lease(c))
		dhcpc_lost();
	for (uint32_t i = 0; i < sizeof(*c); i++)
		z[i] = 0;
	for (int i = 0; i < 6; i++)
		c->mac[i] = mac[i];
	c->rng = seed ^ be32(mac + 2) ^ 0x6a09e667u;
	(void)rnd(c);
	c->ipid = (uint16_t)rnd(c);
	c->state = DHCPC_INIT;
	c->next = now_ms;
}

void dhcpc_stop(struct dhcpc *c, int release)
{
	if (dhcpc_has_lease(c)) {
		if (release && c->lease.server)
			send_msg(c, RELEASE, c->lease.ip, c->lease.ip,
				 c->lease.server, c->srv_mac, 0, c->lease.server);
		c->n_lost++;
		c->state = DHCPC_OFF;
		dhcpc_lost();
	}
	c->state = DHCPC_OFF;
}

void dhcpc_tick(struct dhcpc *c, uint64_t now)
{
	switch (c->state) {
	case DHCPC_INIT:
		if (now >= c->next) {
			new_xid(c);
			c->tries = 0;
			c->state = DHCPC_SELECTING;
			send_discover(c, now);
		}
		break;
	case DHCPC_SELECTING:
		if (now >= c->next)
			send_discover(c, now);
		break;
	case DHCPC_REQUESTING:
		if (now >= c->next) {
			if (c->tries >= 4)
				to_init(c, now, 0);
			else
				send_select_request(c, now);
		}
		break;
	case DHCPC_BOUND:
		if (c->t1 && now >= c->t1) {
			c->state = DHCPC_RENEWING;
			c->tries = 0;
			new_xid(c);
			send_renew(c, now);
		}
		break;
	case DHCPC_RENEWING:
		if (c->t2 && now >= c->t2) {
			c->state = DHCPC_REBINDING;
			c->tries = 0;
			send_rebind(c, now);
		} else if (now >= c->next) {
			send_renew(c, now);
		}
		break;
	case DHCPC_REBINDING:
		if (c->t_exp && now >= c->t_exp)
			to_init(c, now, 0);
		else if (now >= c->next)
			send_rebind(c, now);
		break;
	default:
		break;
	}
}

/* ---- RX ------------------------------------------------------------------- */

struct opts {
	uint32_t type, mask, router, server, dns, lease, t1, t2;
	uint32_t have;              /* bit per option seen */
};
#define HAVE_MASK   1u
#define HAVE_ROUTER 2u
#define HAVE_SERVER 4u
#define HAVE_LEASE  8u
#define HAVE_T1     16u
#define HAVE_T2     32u
#define HAVE_DNS    64u

static int parse_opts(const uint8_t *o, uint32_t n, struct opts *r)
{
	uint32_t i = 0;

	for (uint32_t k = 0; k < sizeof(*r) / 4; k++)
		((uint32_t *)r)[k] = 0;
	while (i < n) {
		uint32_t code = o[i], len;
		const uint8_t *v;

		if (code == 0) {
			i++;
			continue;
		}
		if (code == 255)
			return 1;
		if (i + 2 > n)
			return 0;
		len = o[i + 1];
		v = o + i + 2;
		if (i + 2 + len > n)
			return 0;
		switch (code) {
		case 53: if (len == 1) r->type = v[0]; break;
		case 1:  if (len == 4) { r->mask = be32(v); r->have |= HAVE_MASK; } break;
		case 3:  if (len >= 4) { r->router = be32(v); r->have |= HAVE_ROUTER; } break;
		case 6:  if (len >= 4) { r->dns = be32(v); r->have |= HAVE_DNS; } break;
		case 54: if (len == 4) { r->server = be32(v); r->have |= HAVE_SERVER; } break;
		case 51: if (len == 4) { r->lease = be32(v); r->have |= HAVE_LEASE; } break;
		case 58: if (len == 4) { r->t1 = be32(v); r->have |= HAVE_T1; } break;
		case 59: if (len == 4) { r->t2 = be32(v); r->have |= HAVE_T2; } break;
		default: break;
		}
		i += 2 + len;
	}
	return 1;                   /* no END: take what was there */
}

static uint64_t at(uint64_t base, uint32_t s)
{
	return s == DHCPC_INFINITE ? 0 : base + (uint64_t)s * 1000u;
}

static void take_ack(struct dhcpc *c, const uint8_t *f, uint32_t yiaddr,
		     const struct opts *op, uint64_t now)
{
	struct dhcpc_lease l;
	uint32_t lease = (op->have & HAVE_LEASE) ? op->lease : DHCPC_INFINITE;
	uint32_t t1, t2;

	if (lease != DHCPC_INFINITE && lease < 8u)
		lease = 8u;
	if (lease == DHCPC_INFINITE) {
		t1 = t2 = DHCPC_INFINITE;
	} else {
		t1 = (op->have & HAVE_T1) ? op->t1 : lease / 2u;
		t2 = (op->have & HAVE_T2) ? op->t2 : lease - lease / 8u;
		if (t2 > lease || t2 == 0)
			t2 = lease - lease / 8u;
		if (t1 > t2 || t1 == 0)
			t1 = t2 / 2u ? t2 / 2u : 1u;
	}
	l.ip = yiaddr;
	l.mask = (op->have & HAVE_MASK) ? op->mask : 0xffffff00u;
	l.router = (op->have & HAVE_ROUTER) ? op->router : 0;
	l.server = (op->have & HAVE_SERVER) ? op->server :
		   (c->state == DHCPC_REQUESTING ? c->offer_srv : c->lease.server);
	l.dns = (op->have & HAVE_DNS) ? op->dns : 0;
	l.lease_s = lease;
	l.t1_s = t1;
	l.t2_s = t2;
	c->lease = l;
	for (int i = 0; i < 6; i++)
		c->srv_mac[i] = f[6 + i];
	{
		uint64_t base = c->t_req ? c->t_req : now;
		c->t1 = at(base, t1);
		c->t2 = at(base, t2);
		c->t_exp = at(base, lease);
	}
	c->state = DHCPC_BOUND;
	c->tries = 0;
	c->naks = 0;
	c->n_ack++;
	dhcpc_bound(&c->lease);
}

int dhcpc_rx(struct dhcpc *c, const uint8_t *f, uint32_t len, uint64_t now)
{
	const uint8_t *ip, *u, *b;
	uint32_t ihl, tot, ulen, blen, yiaddr;
	struct opts op;
	int to_us = 1, bc = 1;

	/* Shape first: anything that is not IPv4/UDP 67->68 with our chaddr
	 * is not ours, whatever else is wrong with it. */
	if (len < 14u + 20u + 8u + BOOTP_LEN + 4u || be16(f + 12) != ET_IPV4)
		return DHCPC_NOT_OURS;
	for (int i = 0; i < 6; i++) {
		if (f[i] != c->mac[i]) to_us = 0;
		if (f[i] != 0xff)      bc = 0;
	}
	if (!to_us && !bc)
		return DHCPC_NOT_OURS;
	ip = f + 14;
	if ((ip[0] >> 4) != 4 || ip[9] != 17)
		return DHCPC_NOT_OURS;
	ihl = (uint32_t)(ip[0] & 0xfu) * 4u;
	if (ihl < 20u || 14u + ihl + 8u + BOOTP_LEN + 4u > len)
		return DHCPC_NOT_OURS;
	u = ip + ihl;
	if (be16(u) != 67u || be16(u + 2) != 68u)
		return DHCPC_NOT_OURS;
	b = u + 8;
	if (b[0] != 2 || b[1] != 1 || b[2] != 6 || !mac_eq(b + 28, c->mac))
		return DHCPC_NOT_OURS;

	/* From here on the reply names our MAC: it is ours, used or not. */
	tot = be16(ip + 2);
	ulen = be16(u + 4);
	if (tot < ihl + 8u || tot > len - 14u || (be16(ip + 6) & 0x3fffu) ||
	    csum_fold(csum_add(0, ip, ihl)) != 0 ||
	    ulen < 8u + BOOTP_LEN + 4u || ulen > tot - ihl) {
		c->n_bad++;
		return DHCPC_TAKEN;
	}
	if (be16(u + 6)) {
		uint32_t sum = csum_add(0, ip + 12, 8);
		sum += 17u + ulen;
		sum = csum_add(sum, u, ulen);
		if (csum_fold(sum) != 0) {
			c->n_bad++;
			return DHCPC_TAKEN;
		}
	}
	blen = ulen - 8u;
	if (be32(b + BOOTP_LEN) != 0x63825363u ||
	    !parse_opts(b + BOOTP_LEN + 4u, blen - BOOTP_LEN - 4u, &op)) {
		c->n_bad++;
		return DHCPC_TAKEN;
	}
	if (c->state == DHCPC_OFF || c->state == DHCPC_INIT ||
	    c->state == DHCPC_BOUND || be32(b + 4) != c->xid) {
		c->n_stale++;
		return DHCPC_TAKEN;
	}
	yiaddr = be32(b + 16);

	switch (op.type) {
	case OFFER:
		if (c->state != DHCPC_SELECTING || !yiaddr ||
		    yiaddr == 0xffffffffu || !(op.have & HAVE_SERVER)) {
			c->n_stale++;
			break;
		}
		c->n_offer++;
		c->offer_ip = yiaddr;
		c->offer_srv = op.server;
		c->state = DHCPC_REQUESTING;
		c->tries = 0;
		send_select_request(c, now);
		break;
	case ACK:
		if (c->state == DHCPC_SELECTING || !yiaddr ||
		    yiaddr == 0xffffffffu) {
			c->n_stale++;
			break;
		}
		if (c->state == DHCPC_REQUESTING && op.have & HAVE_SERVER &&
		    op.server != c->offer_srv) {
			c->n_stale++;              /* another server's answer */
			break;
		}
		take_ack(c, f, yiaddr, &op, now);
		break;
	case NAK:
		if (c->state == DHCPC_SELECTING) {
			c->n_stale++;
			break;
		}
		c->n_nak++;
		c->naks++;
		to_init(c, now, backoff_ms(c, c->naks - 1u) / 2u);
		break;
	default:
		c->n_stale++;
		break;
	}
	return DHCPC_TAKEN;
}
