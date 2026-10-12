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
#include "dbgdhcp.h"
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

/* Address mode (dbgip.h "DHCP"). The static values from U-Boot's env (or
 * the last `ip set`) are kept as the fallback for DHCP mode. */
static volatile uint32_t g_mode;               /* DBGIP_MODE_*            */
static uint32_t g_fb_ip, g_fb_mask, g_fb_gw;   /* static / fallback       */
static uint64_t g_fb_at;                       /* CNTPCT: fallback due    */
static struct dhcpc g_dh;
static uint8_t  g_gmac[6];                     /* the guest's MAC         */
static volatile uint32_t g_guest_ip;           /* last seen in guest TX   */
static uint64_t g_ann_next;                    /* CNTPCT: next announce   */
static uint8_t  g_tbuf[128];                   /* announce (tick context) */

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

/* The address itself (mode and fallback untouched). Every change drops
 * the UDP peer: its next hop may be another router now. An unchanged
 * address (a renewed lease) changes nothing. */
static void addr_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t src)
{
	mask = mask ? mask : 0xffffff00u;
	if (ip == g_ip && mask == g_mask && gw == g_gw) {
		g_src = src;
		g_st[DBGIP_ST_SRC] = src;
		return;
	}
	emac_hv_mac(g_mac);
	g_ip = 0;                 /* off while the fields change */
	__asm__ volatile("dmb ish" ::: "memory");
	g_peer_valid = 0;
	g_mask = mask;
	g_gw = gw;
	__asm__ volatile("dmb ish" ::: "memory");
	g_ip = ip;
	nh_reset();
	g_ann_next = 0;           /* announce the new address at once */
	g_st[DBGIP_ST_IP] = g_ip;
	g_st[DBGIP_ST_MASK] = g_mask;
	g_st[DBGIP_ST_GW] = g_gw;
	g_src = ip ? src : 0u;
	g_st[DBGIP_ST_SRC] = g_src;
}

static void host_set(uint32_t host, uint32_t port)
{
	g_host = host;
	g_port = (port && port < 65536u) ? port : DBGIP_PORT;
	g_st[DBGIP_ST_HOST] = g_host;
	g_st[DBGIP_ST_PORT] = g_port;
	nh_reset();
}

static uint64_t now_ms(void)
{
	uint64_t f = freq_hz();
	return f ? now_ticks() / (f / 1000u ? f / 1000u : 1u) : 0;
}

void dbgip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t host,
	       uint32_t port)
{
	/* dbgmon `ip set`/`ip off`: static, and the new fallback */
	if (g_mode == DBGIP_MODE_DHCP)
		dhcpc_stop(&g_dh, 1);
	g_mode = DBGIP_MODE_STATIC;
	g_fb_ip = ip;
	g_fb_mask = mask;
	g_fb_gw = gw;
	g_fb_at = 0;
	g_st[DBGIP_ST_MODE] = g_mode;
	addr_set(0, 0, 0, 0);     /* forces nh/peer reset even if unchanged */
	host_set(host, port);
	addr_set(ip, mask, gw, 2u);
}

/* dbgmon `ip dhcp` / `ip static`. DHCP keeps whatever address is in use
 * until a lease arrives; static goes back to the fallback address. */
void dbgip_mode(uint32_t mode)
{
	emac_hv_mac(g_mac);
	if (mode == DBGIP_MODE_DHCP) {
		g_mode = DBGIP_MODE_DHCP;
		g_fb_at = now_ticks();    /* no waiting: fallback applies now */
		dhcpc_start(&g_dh, g_mac, (uint32_t)now_ticks(), now_ms());
	} else {
		if (g_mode == DBGIP_MODE_DHCP)
			dhcpc_stop(&g_dh, 1);
		g_mode = DBGIP_MODE_STATIC;
		addr_set(g_fb_ip, g_fb_mask, g_fb_gw, g_fb_ip ? 1u : 0u);
	}
	g_st[DBGIP_ST_MODE] = g_mode;
}

/* dbgdhcp.h hooks. */
int dhcpc_xmit(const uint8_t dst[6], const uint8_t *ipv4, uint32_t len)
{
	return emac_send_frame_to(dst, ET_IPV4, ipv4, (uint16_t)len);
}

void dhcpc_bound(const struct dhcpc_lease *l)
{
	if (g_mode != DBGIP_MODE_DHCP)
		return;
	addr_set(l->ip, l->mask, l->router, 3u);
}

void dhcpc_lost(void)
{
	if (g_mode != DBGIP_MODE_DHCP)
		return;
	/* back to the static address if there is one, else off */
	addr_set(g_fb_ip, g_fb_mask, g_fb_gw, g_fb_ip ? 4u : 0u);
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
	uint32_t n, ip = 0, mask = 0, gw = 0, host = 0, port = 0, dhcp = 0;

	emac_hv_mac(g_mac);
	emac_guest_mac(g_gmac);
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
		else if (key_is(l, "bzdk_net"))
			dhcp = (l[9] == 'd' && l[10] == 'h' && l[11] == 'c' &&
				l[12] == 'p');
		l = *e ? e + 1 : e;
	}
	/* consumed: a later boot that does not export must not see this */
	for (n = 0; n < 64; n++)
		blk[n] = 0;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(DBGIP_CFG_PA) : "memory");

	g_fb_ip = ip;
	g_fb_mask = mask;
	g_fb_gw = gw;
	host_set(host, port);
	if (dhcp) {
		/* DHCP; U-Boot's static address only after DBGIP_FALLBACK_S
		 * without a lease (dbgip_tick), DHCP carries on after that. */
		g_mode = DBGIP_MODE_DHCP;
		g_st[DBGIP_ST_MODE] = g_mode;
		g_fb_at = now_ticks() + (uint64_t)DBGIP_FALLBACK_S * freq_hz();
		dhcpc_start(&g_dh, g_mac, (uint32_t)now_ticks(), now_ms());
		return;
	}
	if (!ip)
		return;
	addr_set(ip, mask, gw, 1u);
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

/* Unauthenticated "here I am" to serverip:port (dbgip.h ANNOUNCE): one
 * inner 0x88B5 frame, envelope type 5, nothing in it a host may trust --
 * the host proves the address with a handshake before using it. */
static void announce(void)
{
	uint8_t *ip = g_tbuf, *u = g_tbuf + 20, *e = g_tbuf + 28, *a;
	uint32_t ulen = 8u + 14u + DBGIP_ANN_LEN;

	for (uint32_t i = 0; i < sizeof(g_tbuf); i++)
		g_tbuf[i] = 0;
	for (int i = 0; i < 6; i++) {
		e[i] = 0xff;
		e[6 + i] = g_mac[i];
	}
	put16(e + 12, 0x88B5u);
	a = e + 14;
	a[1] = 'B'; a[2] = 'Z'; a[3] = 'K';
	a[4] = DBGIP_ANN_TYPE;
	a[5] = 1;                           /* version */
	put32(a + 6, g_ip);
	put32(a + 10, g_mask);
	put32(a + 14, g_gw);
	a[18] = (uint8_t)g_src;
	a[19] = (uint8_t)g_mode;
	put32(a + 20, g_guest_ip);
	for (int i = 0; i < 6; i++)
		a[24 + i] = g_gmac[i];
	ip[0] = 0x45;
	put16(ip + 2, 20u + ulen);
	put16(ip + 4, g_ipid++);
	put16(ip + 6, 0x4000);
	ip[8] = 64;
	ip[9] = 17;
	put32(ip + 12, g_ip);
	put32(ip + 16, g_host);
	put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
	put16(u, g_port);
	put16(u + 2, g_port);
	put16(u + 4, ulen);
	(void)emac_send_frame_to(g_nh_mac, ET_IPV4, g_tbuf, (uint16_t)(20u + ulen));
	g_st[DBGIP_ST_ANNOUNCE]++;
}

void dbgip_tick(void)
{
	uint64_t now, f;
	uint8_t inner[6];

	now = now_ticks();
	f = freq_hz();
	if (g_mode == DBGIP_MODE_DHCP) {
		dhcpc_tick(&g_dh, now_ms());
		if (!dhcpc_has_lease(&g_dh) && !g_ip && g_fb_ip && g_fb_at &&
		    now >= g_fb_at)
			addr_set(g_fb_ip, g_fb_mask, g_fb_gw, 4u);
	}
	if (!g_ip || !g_nh_ip)
		return;
	/* Announce: at once on an address change (g_ann_next = 0), then every
	 * DBGIP_ANN_S while no UDP peer is active. */
	if (g_nh_ok && g_host && now >= g_ann_next) {
		if (!g_ann_next || !dbgip_peer_active(inner))
			announce();
		g_ann_next = now + (uint64_t)DBGIP_ANN_S * f;
	}
	if (now < g_nh_next)
		return;
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

/* The guest's IPv4 address, from what it sends (vnet TX): the sender of
 * its ARP frames, else the source of its IPv4 packets -- only with the
 * guest's own MAC as the Ethernet source, and never 0/127/169.254/class D+
 * (a DHCP DISCOVER's 0.0.0.0, link-local, multicast sources). */
static int guest_ip_ok(uint32_t a)
{
	uint32_t t = a >> 24;
	return a && t != 0 && t != 127u && t < 224u &&
	       (a >> 16) != 0xa9feu;
}

void dbgip_guest_tx(const uint8_t *f, uint32_t len)
{
	uint32_t et, a = 0;

	if (len < 42u || !mac_eq(f + 6, g_gmac))
		return;
	et = be16(f + 12);
	if (et == ET_ARP) {
		if (be16(f + 14) == 1 && be16(f + 16) == ET_IPV4 &&
		    f[18] == 6 && f[19] == 4 && mac_eq(f + 22, g_gmac))
			a = be32(f + 28);
	} else if (et == ET_IPV4 && (f[14] >> 4) == 4) {
		a = be32(f + 26);
	}
	if (guest_ip_ok(a) && a != g_guest_ip) {
		g_guest_ip = a;
		g_st[DBGIP_ST_GUEST_IP] = a;
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

	if (len < 14)
		return 0;
	et = be16(f + 12);
	/* DHCP replies naming the HV's MAC; the guest's (chaddr 12:..) and
	 * everything else go on (dbgdhcp.h). */
	if (et == ET_IPV4 && g_mode == DBGIP_MODE_DHCP &&
	    dhcpc_rx(&g_dh, f, len, now_ms()) == DHCPC_TAKEN) {
		g_st[DBGIP_ST_DHCP_RX]++;
		return 1;
	}
	if (!g_ip)
		return 0;
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
	uint64_t now = now_ms();

	g_st[DBGIP_ST_MODE] = g_mode;
	g_st[DBGIP_ST_DHCP_STATE] = g_mode == DBGIP_MODE_DHCP ? g_dh.state : 0;
	g_st[DBGIP_ST_DHCP_SERVER] = dhcpc_has_lease(&g_dh) ? g_dh.lease.server : 0;
	g_st[DBGIP_ST_LEASE_LEFT] = !dhcpc_has_lease(&g_dh) ? 0 :
		!g_dh.t_exp ? DHCPC_INFINITE :
		g_dh.t_exp > now ? (uint32_t)((g_dh.t_exp - now) / 1000u) : 0;
	for (int i = 0; i < DBGIP_ST_N; i++)
		out[i] = g_st[i];
}

uint32_t dbgip_guest_ip(void)
{
	return g_guest_ip;
}

/* ---- the human-readable `ip` line ----------------------------------------- */

struct fmt {
	char *p;
	uint32_t left;
};

static void fs(struct fmt *o, const char *s)
{
	while (*s && o->left > 1) {
		*o->p++ = *s++;
		o->left--;
	}
	*o->p = 0;
}

static void fd(struct fmt *o, uint32_t v)
{
	char t[11];
	int n = 0;

	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	while (n && o->left > 1) {
		*o->p++ = t[--n];
		o->left--;
	}
	*o->p = 0;
}

static void fip(struct fmt *o, uint32_t a)
{
	for (int i = 3; i >= 0; i--) {
		fd(o, (a >> (8 * i)) & 0xffu);
		if (i)
			fs(o, ".");
	}
}

static uint32_t mask_len(uint32_t m)
{
	uint32_t n = 0;
	while (m & 0x80000000u) {
		n++;
		m <<= 1;
	}
	return n;
}

void dbgip_format(char *buf, uint32_t n)
{
	static const char *const src[] = { "off", "env", "dbgmon", "dhcp",
					   "env-fallback" };
	struct fmt o = { buf, n };
	uint32_t st[DBGIP_ST_N];

	if (!n)
		return;
	buf[0] = 0;
	dbgip_stats(st);
	fs(&o, "net mode=");
	fs(&o, g_mode == DBGIP_MODE_DHCP ? "dhcp" : "static");
	fs(&o, " addr=");
	fip(&o, g_ip);
	fs(&o, "/");
	fd(&o, mask_len(g_mask));
	fs(&o, " gw=");
	fip(&o, g_gw);
	fs(&o, " src=");
	fs(&o, g_src < 5u ? src[g_src] : "?");
	fs(&o, " host=");
	fip(&o, g_host);
	fs(&o, ":");
	fd(&o, g_port);
	if (g_mode == DBGIP_MODE_DHCP) {
		fs(&o, " dhcp=");
		fs(&o, dhcpc_state_name(g_dh.state));
		if (dhcpc_has_lease(&g_dh)) {
			fs(&o, " lease=");
			fip(&o, g_dh.lease.ip);
			fs(&o, " server=");
			fip(&o, g_dh.lease.server);
			fs(&o, " left=");
			if (st[DBGIP_ST_LEASE_LEFT] == DHCPC_INFINITE)
				fs(&o, "inf");
			else {
				fd(&o, st[DBGIP_ST_LEASE_LEFT]);
				fs(&o, "s");
			}
			fs(&o, "/");
			fd(&o, g_dh.lease.lease_s);
			fs(&o, "s");
		}
		fs(&o, " fallback=");
		fip(&o, g_fb_ip);
		fs(&o, " tx=");
		fd(&o, g_dh.n_tx);
		fs(&o, " offer=");
		fd(&o, g_dh.n_offer);
		fs(&o, " ack=");
		fd(&o, g_dh.n_ack);
		fs(&o, " nak=");
		fd(&o, g_dh.n_nak);
		fs(&o, " stale=");
		fd(&o, g_dh.n_stale);
		fs(&o, " lost=");
		fd(&o, g_dh.n_lost);
	}
	fs(&o, " guest=");
	fip(&o, g_guest_ip);
}
