/* SPDX-License-Identifier: BSD-2-Clause */

/* test_dbgdhcp.c -- host test of dbgdhcp.c, the HV's DHCP client.
 *
 * Compiles the REAL dbgdhcp.c (it has no asm and no hardware access: the
 * clock is a parameter, the two output hooks are defined here). A fake
 * server builds OFFER/ACK/NAK frames; the checks cover the whole lease
 * cycle (DISCOVER, OFFER, REQUEST, ACK, RENEW at T1 unicast, REBIND at T2
 * broadcast, expiry, NAK, RELEASE), retransmission backoff, and above all
 * that a reply for the guest (chaddr = guest MAC, broadcast or not, even
 * with our xid) is never taken.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dbgdhcp.c"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t HV[6]    = { 0x02, 0xba, 0xfb, 0x3b, 0xf5, 0x8c };
static const uint8_t GUEST[6] = { 0x12, 0xba, 0xfb, 0x3b, 0xf5, 0x8c };
static const uint8_t SRV[6]   = { 0x48, 0xa9, 0x8a, 0x00, 0x00, 0x01 };
static const uint8_t BC[6]    = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define SRV_IP IP(10, 0, 0, 1)

/* ---- hooks ---- */
static uint8_t  tx_dst[6], tx_pkt[1500];
static uint32_t tx_len, tx_n;
static struct dhcpc_lease bound_l;
static int bound_n, lost_n;

int dhcpc_xmit(const uint8_t dst[6], const uint8_t *ipv4, uint32_t len)
{
	memcpy(tx_dst, dst, 6);
	memcpy(tx_pkt, ipv4, len);
	tx_len = len;
	tx_n++;
	return 1;
}
void dhcpc_bound(const struct dhcpc_lease *l) { bound_l = *l; bound_n++; }
void dhcpc_lost(void) { lost_n++; }

/* ---- what the client sent ---- */
struct sent {
	uint32_t type, xid, ciaddr, src, dst, req_ip, srv_id, flags;
	int has_cid, has_host, csum_ok;
	uint8_t chaddr[6];
};

static struct sent parse_tx(void)
{
	struct sent s;
	const uint8_t *ip = tx_pkt, *u = tx_pkt + 20, *b = tx_pkt + 28, *o;
	uint32_t ulen = be16(u + 4), sum;

	memset(&s, 0, sizeof(s));
	s.csum_ok = csum_fold(csum_add(0, ip, 20)) == 0;
	sum = csum_add(0, ip + 12, 8) + 17u + ulen;
	s.csum_ok &= csum_fold(csum_add(sum, u, ulen)) == 0;
	s.csum_ok &= be16(ip + 2) == tx_len && be16(u) == 68 && be16(u + 2) == 67;
	s.csum_ok &= ulen - 8 >= DHCP_MIN_LEN;
	s.src = be32(ip + 12);
	s.dst = be32(ip + 16);
	s.xid = be32(b + 4);
	s.flags = be16(b + 10);
	s.ciaddr = be32(b + 12);
	memcpy(s.chaddr, b + 28, 6);
	CHECK(be32(b + BOOTP_LEN) == 0x63825363u && b[0] == 1);
	o = b + BOOTP_LEN + 4;
	while (*o != 255) {
		const uint8_t *v = o + 2;
		switch (o[0]) {
		case 53: s.type = v[0]; break;
		case 50: s.req_ip = be32(v); break;
		case 54: s.srv_id = be32(v); break;
		case 61: s.has_cid = o[1] == 7 && v[0] == 1 && !memcmp(v + 1, HV, 6); break;
		case 12: s.has_host = o[1] == 4 && !memcmp(v, "bzdk", 4); break;
		}
		o += 2 + o[1];
	}
	return s;
}

/* ---- the fake server ---- */
struct reply {
	const uint8_t *eth_dst, *chaddr;
	uint32_t xid, type, yiaddr, ip_dst;
	uint32_t lease, t1, t2;      /* 0 = option absent */
	int no_server_id, bad_udp_csum, zero_udp_csum, truncate;
	uint32_t server;
};

static uint32_t build(uint8_t *f, const struct reply *r)
{
	uint8_t *ip = f + 14, *u = ip + 20, *b = u + 8, *o;
	uint32_t blen, ulen, sum;

	memset(f, 0, 1514);
	memcpy(f, r->eth_dst, 6);
	memcpy(f + 6, SRV, 6);
	put16(f + 12, 0x0800);
	b[0] = 2; b[1] = 1; b[2] = 6;
	put32(b + 4, r->xid);
	put32(b + 16, r->yiaddr);
	put32(b + 20, SRV_IP);
	memcpy(b + 28, r->chaddr, 6);
	o = b + BOOTP_LEN;
	put32(o, 0x63825363u); o += 4;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)r->type;
	if (!r->no_server_id) {
		*o++ = 54; *o++ = 4; put32(o, r->server ? r->server : SRV_IP); o += 4;
	}
	if (r->type != NAK) {
		*o++ = 1; *o++ = 4; put32(o, IP(255, 255, 255, 0)); o += 4;
		*o++ = 3; *o++ = 4; put32(o, IP(10, 0, 0, 1)); o += 4;
		*o++ = 6; *o++ = 4; put32(o, IP(10, 0, 0, 1)); o += 4;
		if (r->lease) { *o++ = 51; *o++ = 4; put32(o, r->lease); o += 4; }
		if (r->t1)    { *o++ = 58; *o++ = 4; put32(o, r->t1); o += 4; }
		if (r->t2)    { *o++ = 59; *o++ = 4; put32(o, r->t2); o += 4; }
	}
	if (r->truncate) {
		*o++ = 51; *o++ = 40;      /* runs past the end */
	} else {
		*o++ = 255;
	}
	blen = (uint32_t)(o - b);
	if (blen < 300) blen = 300;
	ulen = 8 + blen;
	put16(u, 67); put16(u + 2, 68); put16(u + 4, ulen);
	ip[0] = 0x45; put16(ip + 2, 20 + ulen); ip[8] = 64; ip[9] = 17;
	put32(ip + 12, SRV_IP);
	put32(ip + 16, r->ip_dst ? r->ip_dst : 0xffffffffu);
	put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
	if (!r->zero_udp_csum) {
		sum = csum_add(0, ip + 12, 8) + 17u + ulen;
		sum = csum_fold(csum_add(sum, u, ulen));
		put16(u + 6, (sum ? sum : 0xffff) ^ (r->bad_udp_csum ? 0x5a5a : 0));
	}
	return 14 + 20 + ulen;
}

static int deliver(struct dhcpc *c, struct reply r, uint64_t now)
{
	uint8_t f[1514];
	uint32_t n = build(f, &r);
	return dhcpc_rx(c, f, n, now);
}

static struct reply mk(const uint8_t *dst, const uint8_t *ch, uint32_t xid,
		       uint32_t type, uint32_t yi)
{
	struct reply r;
	memset(&r, 0, sizeof(r));
	r.eth_dst = dst; r.chaddr = ch; r.xid = xid; r.type = type;
	r.yiaddr = yi; r.lease = 600;
	return r;
}

/* Run a client to BOUND at time `t` with the given lease; returns xid. */
static void to_bound(struct dhcpc *c, uint64_t t, uint32_t lease)
{
	struct sent s;
	struct reply r;

	dhcpc_start(c, HV, 1234, t);
	dhcpc_tick(c, t);
	s = parse_tx();
	r = mk(HV, HV, s.xid, OFFER, IP(10, 0, 0, 23));
	CHECK(deliver(c, r, t + 10) == DHCPC_TAKEN);
	r = mk(HV, HV, s.xid, ACK, IP(10, 0, 0, 23));
	r.lease = lease;
	CHECK(deliver(c, r, t + 20) == DHCPC_TAKEN);
	CHECK(c->state == DHCPC_BOUND);
}

static void test_full_cycle(void)
{
	struct dhcpc c;
	struct sent s;
	struct reply r;
	uint8_t f[1514];
	uint32_t xid, n0;
	uint64_t t = 1000;

	memset(&c, 0, sizeof(c));
	bound_n = lost_n = 0;
	dhcpc_start(&c, HV, 42, t);
	CHECK(c.state == DHCPC_INIT);
	dhcpc_tick(&c, t);
	CHECK(c.state == DHCPC_SELECTING && tx_n > 0);
	s = parse_tx();
	CHECK(s.csum_ok);
	CHECK(s.type == DISCOVER && s.src == 0 && s.dst == 0xffffffffu);
	CHECK(!memcmp(tx_dst, BC, 6) && !memcmp(s.chaddr, HV, 6));
	CHECK(s.has_cid && s.has_host && s.flags == 0);
	xid = s.xid;

	/* the guest's OFFER, broadcast, even with our xid: not ours */
	n0 = tx_n;
	r = mk(BC, GUEST, xid, OFFER, IP(10, 0, 0, 50));
	CHECK(deliver(&c, r, t + 5) == DHCPC_NOT_OURS);
	r = mk(GUEST, GUEST, xid, OFFER, IP(10, 0, 0, 50));
	CHECK(deliver(&c, r, t + 5) == DHCPC_NOT_OURS);
	CHECK(c.state == DHCPC_SELECTING && tx_n == n0);

	/* not DHCP at all: a broadcast UDP 67->68 frame without BOOTP size,
	 * and an ARP frame */
	memset(f, 0, sizeof(f));
	memcpy(f, BC, 6);
	put16(f + 12, 0x0806);
	CHECK(dhcpc_rx(&c, f, 60, t) == DHCPC_NOT_OURS);
	r = mk(BC, HV, xid, OFFER, IP(10, 0, 0, 23));
	n0 = build(f, &r);
	CHECK(dhcpc_rx(&c, f, 200, t) == DHCPC_NOT_OURS);   /* truncated frame */
	/* addressed to someone else's MAC */
	r = mk(SRV, HV, xid, OFFER, IP(10, 0, 0, 23));
	CHECK(deliver(&c, r, t) == DHCPC_NOT_OURS);

	/* ours, but a stale xid: taken and dropped */
	n0 = tx_n;
	r = mk(HV, HV, xid ^ 1, OFFER, IP(10, 0, 0, 23));
	CHECK(deliver(&c, r, t + 5) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_SELECTING && tx_n == n0 && c.n_stale == 1);

	/* ours, bad UDP checksum / truncated options: taken, no effect */
	r = mk(BC, HV, xid, OFFER, IP(10, 0, 0, 23));
	r.bad_udp_csum = 1;
	CHECK(deliver(&c, r, t + 5) == DHCPC_TAKEN);
	r.bad_udp_csum = 0;
	r.truncate = 1;
	CHECK(deliver(&c, r, t + 5) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_SELECTING && c.n_bad == 2);

	/* an OFFER without a server id is not usable */
	r = mk(BC, HV, xid, OFFER, IP(10, 0, 0, 23));
	r.no_server_id = 1;
	CHECK(deliver(&c, r, t + 5) == DHCPC_TAKEN && c.state == DHCPC_SELECTING);

	/* the real OFFER, broadcast, zero UDP checksum: REQUEST */
	r = mk(BC, HV, xid, OFFER, IP(10, 0, 0, 23));
	r.zero_udp_csum = 1;
	CHECK(deliver(&c, r, t + 10) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_REQUESTING);
	s = parse_tx();
	CHECK(s.csum_ok && s.type == REQUEST && s.xid == xid);
	CHECK(s.req_ip == IP(10, 0, 0, 23) && s.srv_id == SRV_IP);
	CHECK(s.ciaddr == 0 && s.src == 0 && !memcmp(tx_dst, BC, 6));

	/* the guest's ACK, broadcast, our xid: still not ours */
	r = mk(BC, GUEST, xid, ACK, IP(10, 0, 0, 50));
	CHECK(deliver(&c, r, t + 15) == DHCPC_NOT_OURS && bound_n == 0);
	/* an ACK from a server we did not ask */
	r = mk(HV, HV, xid, ACK, IP(10, 0, 0, 23));
	r.server = IP(10, 0, 0, 99);
	CHECK(deliver(&c, r, t + 15) == DHCPC_TAKEN && bound_n == 0);

	/* our ACK, unicast to our MAC and the offered IP */
	r = mk(HV, HV, xid, ACK, IP(10, 0, 0, 23));
	r.ip_dst = IP(10, 0, 0, 23);
	r.lease = 600;
	CHECK(deliver(&c, r, t + 20) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_BOUND && bound_n == 1);
	CHECK(bound_l.ip == IP(10, 0, 0, 23) && bound_l.mask == IP(255, 255, 255, 0));
	CHECK(bound_l.router == IP(10, 0, 0, 1) && bound_l.server == SRV_IP);
	CHECK(bound_l.lease_s == 600 && bound_l.t1_s == 300 && bound_l.t2_s == 525);
	CHECK(!memcmp(c.srv_mac, SRV, 6));
	/* duplicate ACK while bound: dropped, not re-bound */
	CHECK(deliver(&c, r, t + 21) == DHCPC_TAKEN && bound_n == 1);

	/* nothing until T1 (lease counts from the REQUEST at t+10) */
	n0 = tx_n;
	dhcpc_tick(&c, t + 10 + 299999);
	CHECK(tx_n == n0 && c.state == DHCPC_BOUND);
	dhcpc_tick(&c, t + 10 + 300000);
	CHECK(c.state == DHCPC_RENEWING && tx_n == n0 + 1);
	s = parse_tx();
	CHECK(s.csum_ok && s.type == REQUEST && s.ciaddr == IP(10, 0, 0, 23));
	CHECK(s.src == IP(10, 0, 0, 23) && s.dst == SRV_IP);
	CHECK(!memcmp(tx_dst, SRV, 6) && s.req_ip == 0 && s.srv_id == 0);
	CHECK(s.xid != xid);
	xid = s.xid;

	/* renewed */
	t = t + 10 + 300000;
	r = mk(HV, HV, xid, ACK, IP(10, 0, 0, 23));
	r.ip_dst = IP(10, 0, 0, 23);
	r.lease = 600;
	CHECK(deliver(&c, r, t + 3) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_BOUND && bound_n == 2 && lost_n == 0);
	CHECK(c.t1 == t + 300000);

	/* T1 again, the server stays silent: retransmissions, then T2 */
	dhcpc_tick(&c, t + 300000);
	CHECK(c.state == DHCPC_RENEWING);
	n0 = tx_n;
	dhcpc_tick(&c, t + 300000 + 1000);
	CHECK(tx_n == n0);                         /* not yet */
	dhcpc_tick(&c, t + 300000 + 112500);       /* half of T2-T1 */
	CHECK(tx_n == n0 + 1);
	dhcpc_tick(&c, t + 525000);
	CHECK(c.state == DHCPC_REBINDING);
	s = parse_tx();
	CHECK(s.type == REQUEST && s.ciaddr == IP(10, 0, 0, 23) &&
	      s.src == IP(10, 0, 0, 23) && s.dst == 0xffffffffu &&
	      !memcmp(tx_dst, BC, 6));
	/* expiry */
	dhcpc_tick(&c, t + 599999);
	CHECK(lost_n == 0);
	dhcpc_tick(&c, t + 600000);
	CHECK(lost_n == 1 && c.state == DHCPC_INIT);
	n0 = tx_n;
	dhcpc_tick(&c, t + 600000);
	CHECK(c.state == DHCPC_SELECTING && tx_n == n0 + 1 &&
	      parse_tx().type == DISCOVER);
}

static void test_backoff(void)
{
	struct dhcpc c;
	uint64_t t = 0, last;
	uint32_t n0, gaps[7], k = 0;

	memset(&c, 0, sizeof(c));
	dhcpc_start(&c, HV, 7, 0);
	dhcpc_tick(&c, 0);
	last = 0;
	n0 = tx_n;
	for (t = 1; t < 400000 && k < 7; t += 10) {
		dhcpc_tick(&c, t);
		if (tx_n != n0) {
			gaps[k++] = (uint32_t)(t - last);
			last = t;
			n0 = tx_n;
		}
	}
	CHECK(k == 7);
	{
		static const uint32_t want[7] = { 2, 4, 8, 16, 32, 64, 64 };
		for (int i = 0; i < 7; i++)    /* base + up to 1023 ms jitter */
			CHECK(gaps[i] >= want[i] * 1000u &&
			      gaps[i] <= want[i] * 1000u + 1033u);
	}
	CHECK(c.state == DHCPC_SELECTING);
}

static void test_nak_and_request_timeout(void)
{
	struct dhcpc c;
	struct sent s;
	struct reply r;
	uint64_t t = 5000;

	memset(&c, 0, sizeof(c));
	bound_n = lost_n = 0;
	/* NAK in REQUESTING: back to INIT, no lost (never bound) */
	dhcpc_start(&c, HV, 9, t);
	dhcpc_tick(&c, t);
	s = parse_tx();
	r = mk(HV, HV, s.xid, OFFER, IP(10, 0, 0, 23));
	CHECK(deliver(&c, r, t + 1) == DHCPC_TAKEN && c.state == DHCPC_REQUESTING);
	r = mk(BC, HV, s.xid, NAK, 0);
	CHECK(deliver(&c, r, t + 2) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_INIT && lost_n == 0 && c.n_nak == 1);
	/* a NAK for the guest is not ours */
	r = mk(BC, GUEST, s.xid, NAK, 0);
	CHECK(deliver(&c, r, t + 3) == DHCPC_NOT_OURS);

	/* REQUEST unanswered 4 times: INIT */
	dhcpc_start(&c, HV, 10, t);
	dhcpc_tick(&c, t);
	s = parse_tx();
	r = mk(HV, HV, s.xid, OFFER, IP(10, 0, 0, 23));
	deliver(&c, r, t);
	for (uint64_t x = t; x < t + 200000 && c.state == DHCPC_REQUESTING; x += 100)
		dhcpc_tick(&c, x);
	CHECK(c.state == DHCPC_INIT);

	/* NAK while renewing: the lease is lost at once */
	to_bound(&c, t, 100);
	lost_n = 0;
	dhcpc_tick(&c, t + 10 + 50000);
	CHECK(c.state == DHCPC_RENEWING);
	s = parse_tx();
	r = mk(HV, HV, s.xid, NAK, 0);
	CHECK(deliver(&c, r, t + 10 + 50001) == DHCPC_TAKEN);
	CHECK(c.state == DHCPC_INIT && lost_n == 1);
}

static void test_infinite_and_release(void)
{
	struct dhcpc c;
	struct sent s;
	uint32_t n0;

	memset(&c, 0, sizeof(c));
	to_bound(&c, 0, DHCPC_INFINITE);
	CHECK(c.t1 == 0 && c.t2 == 0 && c.t_exp == 0);
	n0 = tx_n;
	dhcpc_tick(&c, 100ull * 24 * 3600 * 1000);
	CHECK(c.state == DHCPC_BOUND && tx_n == n0);

	/* lease without option 51: treated as infinite too */
	lost_n = 0;
	dhcpc_stop(&c, 1);
	CHECK(c.state == DHCPC_OFF && lost_n == 1 && tx_n == n0 + 1);
	s = parse_tx();
	CHECK(s.type == RELEASE && s.ciaddr == IP(10, 0, 0, 23) &&
	      s.srv_id == SRV_IP && s.dst == SRV_IP && !memcmp(tx_dst, SRV, 6));
	/* stopped: replies with our chaddr are still ours (dropped) */
	{
		struct reply r = mk(HV, HV, s.xid, ACK, IP(10, 0, 0, 23));
		CHECK(deliver(&c, r, 1) == DHCPC_TAKEN && c.state == DHCPC_OFF);
	}
	/* T1/T2 options are honoured, and clamped into the lease */
	to_bound(&c, 0, 1000);
	CHECK(c.lease.t1_s == 500 && c.lease.t2_s == 875);
}

static void test_options_t1t2(void)
{
	struct dhcpc c;
	struct sent s;
	struct reply r;

	memset(&c, 0, sizeof(c));
	dhcpc_start(&c, HV, 77, 0);
	dhcpc_tick(&c, 0);
	s = parse_tx();
	r = mk(HV, HV, s.xid, OFFER, IP(10, 0, 0, 23));
	deliver(&c, r, 0);
	r = mk(HV, HV, s.xid, ACK, IP(10, 0, 0, 23));
	r.lease = 3600; r.t1 = 100; r.t2 = 200;
	deliver(&c, r, 0);
	CHECK(c.lease.t1_s == 100 && c.lease.t2_s == 200);
	r.t1 = 5000; r.t2 = 9000;          /* nonsense: past the lease */
	dhcpc_start(&c, HV, 78, 0);
	dhcpc_tick(&c, 0);
	s = parse_tx();
	r.xid = s.xid; r.type = OFFER;
	deliver(&c, r, 0);
	r.type = ACK;
	deliver(&c, r, 0);
	CHECK(c.state == DHCPC_BOUND);
	CHECK(c.lease.t2_s == 3150 && c.lease.t1_s == 1575);
}

int main(void)
{
	test_full_cycle();
	test_backoff();
	test_nak_and_request_timeout();
	test_infinite_and_release();
	test_options_t1t2();
	printf("test_dbgdhcp: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
