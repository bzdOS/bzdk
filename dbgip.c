/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgip.c -- debug channel over UDP/IPv4. Design and limits: dbgip.h.
 *
 * Runs where emac.c's RX drain runs (CPU1's tick/IRQ, or CPU0's hold loop,
 * never both at once). dbgip_tx_route() is called from any core that
 * transmits an HV frame; it only reads the peer record, which is published
 * under a sequence counter. Freestanding: <stdint.h> and our headers only.
 */
#include <stdint.h>
#include "dbgip.h"
#include "emac.h"

/* emac.c: hand one inner debug Ethernet frame to the debug plane.
 * Returns 2 = authenticated and dispatched, 1 = consumed (handshake,
 * NAK, rejected), 0 = not a debug frame for us. */
extern int emac_dbg_rx_inner(const uint8_t *frame, uint32_t len);

#define ET_ARP   0x0806u
#define ET_IPV4  0x0800u

struct dpeer {
	uint32_t ip;
	uint32_t port;
	uint8_t  mac[6];        /* where its datagrams came from (host/router) */
	uint8_t  inner[6];      /* inner source MAC: frames TO it are wrapped  */
};

static uint8_t  g_mac[6];
static volatile uint32_t g_ip, g_mask, g_gw, g_host, g_port = DBGIP_PORT;
static volatile uint32_t g_src;

/* Authenticated peer, published with a sequence counter (odd = writing). */
static struct dpeer g_peer;
static volatile uint32_t g_peer_seq;
static volatile uint32_t g_peer_valid;

/* The datagram being processed right now (HELLO/NAK replies go here). */
static struct dpeer g_ctx;
static volatile uint32_t g_ctx_on;

/* Next hop towards serverip (serverip itself on-link, else the gateway). */
static volatile uint32_t g_nh_ip;
static uint8_t  g_nh_mac[6];
static volatile uint32_t g_nh_ok;
static uint64_t g_nh_next;        /* CNTPCT of the next ARP request   */
static uint32_t g_nh_tries;

static volatile uint32_t g_st[DBGIP_ST_N];
static uint16_t g_ipid;
static uint8_t  g_buf[1514];      /* ARP/ICMP replies (RX context only) */

static inline uint64_t now_ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t freq_hz(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

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

static void mac_cp(uint8_t *d, const uint8_t *s)
{
	for (int i = 0; i < 6; i++)
		d[i] = s[i];
}

static int mac_bcast(const uint8_t *a)
{
	for (int i = 0; i < 6; i++)
		if (a[i] != 0xff)
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

static int on_link(uint32_t ip)
{
	return ((ip ^ g_ip) & g_mask) == 0;
}

/* ---- configuration ------------------------------------------------------ */

static void nh_reset(void)
{
	uint32_t nh = 0;

	if (g_host)
		nh = on_link(g_host) ? g_host : g_gw;
	g_nh_ok = 0;
	g_nh_ip = nh;
	g_nh_tries = 0;
	g_nh_next = 0;            /* ARP at the next tick */
	g_st[DBGIP_ST_NH_OK] = 0;
}

void dbgip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t host,
	       uint32_t port)
{
	emac_hv_mac(g_mac);
	g_ip = 0;                 /* off while the fields change */
	__asm__ volatile("dmb ish" ::: "memory");
	g_peer_valid = 0;
	g_mask = mask ? mask : 0xffffff00u;
	g_gw = gw;
	g_host = host;
	g_port = (port && port < 65536u) ? port : DBGIP_PORT;
	__asm__ volatile("dmb ish" ::: "memory");
	g_ip = ip;
	nh_reset();
	g_st[DBGIP_ST_IP] = g_ip;
	g_st[DBGIP_ST_MASK] = g_mask;
	g_st[DBGIP_ST_GW] = g_gw;
	g_st[DBGIP_ST_HOST] = g_host;
	g_st[DBGIP_ST_PORT] = g_port;
	g_src = ip ? 2u : 0u;
	g_st[DBGIP_ST_SRC] = g_src;
}

/* "a.b.c.d" at s (stops at the first char that is not a digit or '.'). */
static int parse_ip(const char *s, uint32_t *out)
{
	uint32_t v = 0, part = 0, dots = 0, digits = 0;

	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10u + (uint32_t)(*s - '0');
			if (part > 255u || ++digits > 3u)
				return 0;
		} else if (*s == '.') {
			if (!digits || ++dots > 3u)
				return 0;
			v = (v << 8) | part;
			part = 0;
			digits = 0;
		} else {
			break;
		}
	}
	if (dots != 3u || !digits)
		return 0;
	*out = (v << 8) | part;
	return 1;
}

static int key_is(const char *line, const char *key)
{
	while (*key)
		if (*line++ != *key++)
			return 0;
	return *line == '=';
}

static uint32_t parse_dec(const char *s)
{
	uint32_t v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10u + (uint32_t)(*s++ - '0');
	return v;
}

void dbgip_init(void)
{
	volatile char *blk = (volatile char *)DBGIP_CFG_PA;
	char txt[DBGIP_CFG_MAX + 1];
	uint32_t n, ip = 0, mask = 0, gw = 0, host = 0, port = 0;

	emac_hv_mac(g_mac);
	/* U-Boot wrote this with its D-cache on: push any dirty line out
	 * before reading (same as the DTB U-Boot loads next to it). */
	for (uintptr_t a = DBGIP_CFG_PA; a < DBGIP_CFG_PA + DBGIP_CFG_MAX; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");

	for (n = 0; n < DBGIP_CFG_MAX; n++) {
		char c = blk[n];
		if (c == 0)
			break;
		if (c != '\n' && (c < 0x20 || c > 0x7e))
			return;           /* not U-Boot's text: leave UDP off */
		txt[n] = c;
	}
	if (n == 0 || n == DBGIP_CFG_MAX)
		return;
	txt[n] = 0;

	for (char *l = txt; *l; ) {
		char *e = l;
		while (*e && *e != '\n')
			e++;
		if (key_is(l, "ipaddr"))
			(void)parse_ip(l + 7, &ip);
		else if (key_is(l, "netmask"))
			(void)parse_ip(l + 8, &mask);
		else if (key_is(l, "gatewayip"))
			(void)parse_ip(l + 10, &gw);
		else if (key_is(l, "serverip"))
			(void)parse_ip(l + 9, &host);
		else if (key_is(l, "bzdk_dbgport"))
			port = parse_dec(l + 13);
		l = *e ? e + 1 : e;
	}
	/* consumed: a later boot that does not export must not see this */
	for (n = 0; n < 64; n++)
		blk[n] = 0;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(DBGIP_CFG_PA) : "memory");

	if (!ip)
		return;
	dbgip_set(ip, mask, gw, host, port);
	g_src = 1;
	g_st[DBGIP_ST_SRC] = 1;
}

/* ---- peer record ---------------------------------------------------------- */

static volatile uint64_t g_peer_t;     /* CNTPCT of its last accepted frame */
#define PEER_ACTIVE_S 120u

static void peer_commit(const struct dpeer *p)
{
	g_peer_seq++;
	__asm__ volatile("dmb ish" ::: "memory");
	g_peer = *p;
	g_peer_valid = 1;
	__asm__ volatile("dmb ish" ::: "memory");
	g_peer_seq++;
	g_st[DBGIP_ST_PEER_IP] = p->ip;
	g_st[DBGIP_ST_PEER_PORT] = p->port;
}

static int peer_read(struct dpeer *out)
{
	for (int tries = 0; tries < 4; tries++) {
		uint32_t s = g_peer_seq;
		__asm__ volatile("dmb ish" ::: "memory");
		if (s & 1u)
			continue;
		if (!g_peer_valid)
			return 0;
		*out = g_peer;
		__asm__ volatile("dmb ish" ::: "memory");
		if (g_peer_seq == s)
			return 1;
	}
	return 0;
}

/* The UDP peer's inner MAC, if it spoke within PEER_ACTIVE_S (emac.c's
 * tx_reply() copies "to the peer" output to it while an L2 host is the
 * newer peer, and vice versa). */
int dbgip_peer_active(uint8_t inner[6])
{
	struct dpeer t;
	uint64_t last = g_peer_t;

	if (!g_ip || !last || now_ticks() - last >= (uint64_t)PEER_ACTIVE_S * freq_hz())
		return 0;
	if (!peer_read(&t))
		return 0;
	mac_cp(inner, t.inner);
	return 1;
}

/* ---- ARP ------------------------------------------------------------------ */

static void arp_send(uint32_t op, const uint8_t *tha, uint32_t tpa,
		     const uint8_t *dst)
{
	uint8_t p[28];

	put16(p + 0, 1);          /* Ethernet */
	put16(p + 2, ET_IPV4);
	p[4] = 6;
	p[5] = 4;
	put16(p + 6, op);
	mac_cp(p + 8, g_mac);
	put32(p + 14, g_ip);
	for (int i = 0; i < 6; i++)
		p[18 + i] = tha ? tha[i] : 0;
	put32(p + 24, tpa);
	(void)emac_send_frame_to(dst, ET_ARP, p, sizeof(p));
	g_st[DBGIP_ST_ARP_TX]++;
}

static int arp_rx(const uint8_t *f, uint32_t len)
{
	const uint8_t *a = f + 14;
	uint32_t op, spa, tpa;

	if (len < 14 + 28 || be16(a) != 1 || be16(a + 2) != ET_IPV4 ||
	    a[4] != 6 || a[5] != 4)
		return 0;
	op = be16(a + 6);
	spa = be32(a + 14);
	tpa = be32(a + 24);

	if (spa && spa == g_nh_ip && !mac_bcast(a + 8)) {
		mac_cp(g_nh_mac, a + 8);
		__asm__ volatile("dmb ish" ::: "memory");
		if (!g_nh_ok)
			g_nh_tries = 0;
		g_nh_ok = 1;
		g_st[DBGIP_ST_NH_OK] = 1;
	}
	if (op == 1 && tpa == g_ip) {
		arp_send(2, a + 8, spa, a + 8);
		g_st[DBGIP_ST_ARP_RX]++;
		return 1;
	}
	if (op == 2 && mac_eq(f, g_mac)) {
		g_st[DBGIP_ST_ARP_RX]++;
		return 1;          /* an answer to our own request */
	}
	return 0;
}

void dbgip_tick(void)
{
	uint64_t now, f;

	if (!g_ip || !g_nh_ip)
		return;
	now = now_ticks();
	if (now < g_nh_next)
		return;
	f = freq_hz();
	/* unresolved: 1 s for a minute, then 10 s; resolved: refresh 10 min */
	if (g_nh_ok)
		g_nh_next = now + 600u * f;
	else
		g_nh_next = now + (g_nh_tries++ < 60u ? f : 10u * f);
	{
		static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		arp_send(1, 0, g_nh_ip, bc);
	}
}

/* ---- IPv4 ----------------------------------------------------------------- */

static void icmp_echo(const uint8_t *f, const uint8_t *ip, uint32_t ihl,
		      uint32_t tot)
{
	uint32_t ilen = tot - ihl;
	uint8_t *o = g_buf;

	if (ilen < 8 || tot > 1500u || ip[ihl] != 8 || ip[ihl + 1] != 0)
		return;
	for (uint32_t i = 0; i < 20; i++)
		o[i] = 0;
	o[0] = 0x45;
	put16(o + 2, 20u + ilen);
	put16(o + 4, g_ipid++);
	o[8] = 64;
	o[9] = 1;
	put32(o + 12, g_ip);
	for (int i = 0; i < 4; i++)
		o[16 + i] = ip[12 + i];
	put16(o + 10, csum_fold(csum_add(0, o, 20)));
	for (uint32_t i = 0; i < ilen; i++)
		o[20 + i] = ip[ihl + i];
	o[20] = 0;               /* echo reply */
	o[22] = o[23] = 0;
	put16(o + 22, csum_fold(csum_add(0, o + 20, ilen)));
	(void)emac_send_frame_to(f + 6, ET_IPV4, o, (uint16_t)(20u + ilen));
	g_st[DBGIP_ST_ICMP]++;
}

static int udp_rx(const uint8_t *f, const uint8_t *ip, uint32_t ihl,
		  uint32_t tot)
{
	const uint8_t *u = ip + ihl;
	uint32_t ulen, plen, sum;
	struct dpeer ctx;
	int r;

	if (tot < ihl + 8u)
		return 0;
	if (be16(u + 2) != g_port)
		return 0;                 /* not ours: emac.c drops it */
	g_st[DBGIP_ST_RX_UDP]++;
	ulen = be16(u + 4);
	if (ulen < 8u + 14u || ulen > tot - ihl) {
		g_st[DBGIP_ST_DROP]++;
		return 1;
	}
	if (be16(u + 6)) {
		sum = csum_add(0, ip + 12, 8);      /* src + dst */
		sum += 17u + ulen;
		sum = csum_add(sum, u, ulen);
		if (csum_fold(sum) != 0) {
			g_st[DBGIP_ST_DROP]++;
			return 1;
		}
	}
	plen = ulen - 8u;

	ctx.ip = be32(ip + 12);
	ctx.port = be16(u);
	mac_cp(ctx.mac, f + 6);
	mac_cp(ctx.inner, u + 8 + 6);
	g_ctx = ctx;
	__asm__ volatile("dmb ish" ::: "memory");
	g_ctx_on = 1;
	r = emac_dbg_rx_inner(u + 8, plen);
	g_ctx_on = 0;
	if (r == 2) {
		g_st[DBGIP_ST_RX_ACCEPT]++;
		g_peer_t = now_ticks();
		if (!g_peer_valid || g_peer.ip != ctx.ip ||
		    g_peer.port != ctx.port || !mac_eq(g_peer.mac, ctx.mac) ||
		    !mac_eq(g_peer.inner, ctx.inner))
			peer_commit(&ctx);
	} else if (r == 0) {
		g_st[DBGIP_ST_DROP]++;
	}
	return 1;
}

static int ip_rx(const uint8_t *f, uint32_t len)
{
	const uint8_t *ip = f + 14;
	uint32_t ihl, tot;

	if (len < 14 + 20 || !mac_eq(f, g_mac))
		return 0;
	if ((ip[0] >> 4) != 4 || be32(ip + 16) != g_ip)
		return 0;
	ihl = (uint32_t)(ip[0] & 0xfu) * 4u;
	tot = be16(ip + 2);
	if (ihl < 20 || tot < ihl || tot > len - 14 ||
	    csum_fold(csum_add(0, ip, ihl)) != 0) {
		g_st[DBGIP_ST_DROP]++;
		return 1;
	}
	if (be16(ip + 6) & 0x3fffu) {         /* fragment: never ours */
		g_st[DBGIP_ST_DROP]++;
		return 1;
	}
	if (ip[9] == 1) {
		icmp_echo(f, ip, ihl, tot);
		return 1;
	}
	if (ip[9] == 17)
		return udp_rx(f, ip, ihl, tot);
	return 0;
}

int dbgip_rx(const uint8_t *f, uint32_t len)
{
	uint32_t et;

	if (!g_ip || len < 14)
		return 0;
	et = be16(f + 12);
	if (et == ET_ARP)
		return arp_rx(f, len);
	if (et == ET_IPV4)
		return ip_rx(f, len);
	return 0;
}

/* ---- TX ------------------------------------------------------------------- */

int dbgip_tx_route(const uint8_t dst[6], uint16_t ethertype,
		   uint32_t inner_len, uint8_t hdr[DBGIP_HDR_LEN])
{
	struct dpeer t;
	int mode;
	uint8_t *ip = hdr + 14, *u = hdr + 34;

	if (!g_ip || ethertype < 0x88B5u || ethertype > 0x88B9u)
		return DBGIP_TX_L2;
	if (g_ctx_on && mac_eq(dst, g_ctx.inner)) {
		t = g_ctx;
		mode = DBGIP_TX_UDP;
	} else if (peer_read(&t) && mac_eq(dst, t.inner)) {
		mode = DBGIP_TX_UDP;
	} else if (mac_bcast(dst)) {
		if (!peer_read(&t)) {
			if (!g_nh_ok || !g_host)
				return DBGIP_TX_L2;
			t.ip = g_host;
			t.port = g_port;
			mac_cp(t.mac, g_nh_mac);
		}
		mode = DBGIP_TX_BOTH;
	} else {
		return DBGIP_TX_L2;
	}
	if (inner_len > DBGIP_INNER_MAX) {
		g_st[DBGIP_ST_DROP]++;
		return mode == DBGIP_TX_BOTH ? DBGIP_TX_L2 : DBGIP_TX_DROP;
	}

	mac_cp(hdr, t.mac);
	mac_cp(hdr + 6, g_mac);
	put16(hdr + 12, ET_IPV4);
	ip[0] = 0x45;
	ip[1] = 0;
	put16(ip + 2, 20u + 8u + inner_len);
	put16(ip + 4, g_ipid++);
	put16(ip + 6, 0x4000);              /* DF */
	ip[8] = 64;
	ip[9] = 17;
	ip[10] = ip[11] = 0;
	put32(ip + 12, g_ip);
	put32(ip + 16, t.ip);
	put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
	put16(u, g_port);
	put16(u + 2, t.port);
	put16(u + 4, 8u + inner_len);
	put16(u + 6, 0);                    /* no checksum (IPv4 allows it) */
	g_st[DBGIP_ST_TX_UDP]++;
	return mode;
}

void dbgip_stats(uint32_t out[DBGIP_ST_N])
{
	for (int i = 0; i < DBGIP_ST_N; i++)
		out[i] = g_st[i];
}
