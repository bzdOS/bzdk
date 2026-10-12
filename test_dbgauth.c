/* SPDX-License-Identifier: BSD-2-Clause */

/* test_dbgauth.c -- hosted unit test for dbgauth.c (the HV debug-channel
 * envelope) and hmac_sha256.c. Built and run by `make test`.
 *
 * Covers: HMAC-SHA256 against RFC 4231 test case 2; the DATA tag layout
 * against a vector produced by hvauth.py (the host side -- hvauth.py's own
 * selftest checks the same hex); HELLO/CHAL; acceptance per channel; replay,
 * stale counter, forged tag, unknown session (NAK), truncation, legacy
 * frames, LRU eviction, and a per-boot random session base.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DBG_AUTH 1
#define DBGAUTH_TEST_KEY 1
#include "hmac_sha256.c"
#include "dbgauth.c"

static uint64_t fake_t = 1000;
uint64_t cntpct(void) { fake_t += 1000000u; return fake_t; }

static uint8_t last[256];
static int last_len;
int emac_send_frame_to(const uint8_t dst[6], uint16_t et, const uint8_t *p,
		       uint16_t len)
{
	(void)dst; (void)et;
	memcpy(last, p, len);
	last_len = len;
	return 1;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

static void unhex(const char *h, uint8_t *o)
{
	unsigned v;
	while (h[0] && h[1]) { sscanf(h, "%2x", &v); *o++ = (uint8_t)v; h += 2; }
}

static void be32w(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* frame = 14-byte Ethernet header + payload, padded like the wire */
static int mkframe(uint8_t *f, const uint8_t *pl, int n)
{
	memset(f, 0xff, 6);
	memset(f + 6, 0x22, 6);
	f[12] = 0x88; f[13] = 0xb5;
	memcpy(f + 14, pl, n);
	if (n < 46) { memset(f + 14 + n, 0, 46 - n); n = 46; }
	return 14 + n + 4;          /* + FCS bytes, as the DMA delivers */
}

static int hello(uint32_t *sess, uint8_t ks[32])
{
	uint8_t pl[22] = { 0, 'B', 'Z', 'K', 1, 0 }, f[128], msg[45];
	uint8_t chk[32];
	uint8_t ch; const uint8_t *in; uint32_t il;
	int i, r;

	for (i = 0; i < 16; i++) pl[6 + i] = (uint8_t)(0x40 + i);
	last_len = 0;
	r = dbgauth_rx(f, (uint32_t)mkframe(f, pl, 22), &ch, &in, &il);
	if (r != DBGAUTH_CONSUMED || last_len != 58 || last[4] != 2)
		return 0;
	*sess = ((uint32_t)last[6] << 24) | (last[7] << 16) | (last[8] << 8) |
		last[9];
	/* host side: proof and session key */
	memcpy(msg, "BZK1/chal", 9); memcpy(msg + 9, last + 10, 16);
	memcpy(msg + 25, last + 6, 4); memcpy(msg + 29, pl + 6, 16);
	hmac_sha256(dbgauth_key, DBGAUTH_KEY_LEN, msg, 45, chk);
	if (memcmp(chk, last + 42, 16))
		return 0;
	memcpy(msg, "BZK1/sess", 9);
	hmac_sha256(dbgauth_key, DBGAUTH_KEY_LEN, msg, 45, ks);
	return 1;
}

static int data(uint8_t *pl, const uint8_t ks[32], uint32_t sess,
		uint64_t ctr, uint8_t chan, const char *inner)
{
	uint32_t n = (uint32_t)strlen(inner);
	uint8_t tag[32];

	pl[0] = 0; pl[1] = 'B'; pl[2] = 'Z'; pl[3] = 'K'; pl[4] = 3; pl[5] = chan;
	be32w(pl + 6, sess);
	be32w(pl + 10, (uint32_t)(ctr >> 32)); be32w(pl + 14, (uint32_t)ctr);
	pl[18] = (uint8_t)(n >> 8); pl[19] = (uint8_t)n;
	memcpy(pl + 36, inner, n);
	hmac_sha256_2(ks, 32, pl, 20, pl + 36, n, tag);
	memcpy(pl + 20, tag, 16);
	return 36 + (int)n;
}

static int rx(const uint8_t *pl, int n, uint8_t *ch, char *out)
{
	uint8_t f[2048];
	const uint8_t *in; uint32_t il;
	int r;

	last_len = 0;
	r = dbgauth_rx(f, (uint32_t)mkframe(f, pl, n), ch, &in, &il);
	if (r == DBGAUTH_ACCEPT && out) { memcpy(out, in, il); out[il] = 0; }
	return r;
}

int main(void)
{
	uint8_t out[32], exp[32], pl[600], ks[32], ks2[32], ch;
	uint8_t v[39];
	char s[600];
	uint32_t sess, sess2, i;
	int n;

	/* RFC 4231 test case 2, one-shot and split */
	unhex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", exp);
	hmac_sha256((const uint8_t *)"Jefe", 4,
		    (const uint8_t *)"what do ya want for nothing?", 28, out);
	CHECK(!memcmp(out, exp, 32));
	hmac_sha256_2((const uint8_t *)"Jefe", 4,
		      (const uint8_t *)"what do ya", 10,
		      (const uint8_t *)" want for nothing?", 18, out);
	CHECK(!memcmp(out, exp, 32));

	/* tag layout == hvauth.data_payload(bytes(range(32)), 0x01020304, 5, 2, b"abc") */
	unhex("00425a4b030201020304000000000000000500039ac08c87a00bb8ceacfcfbb170"
	      "50929d616263", v);
	for (i = 0; i < 32; i++) ks[i] = (uint8_t)i;
	n = data(pl, ks, 0x01020304u, 5, 2, "abc");
	CHECK(n == 39 && !memcmp(pl, v, 39));

	CHECK(hello(&sess, ks));
	CHECK(sess != 1);                       /* random per-boot base */
	for (ch = 0; ch < 4; ch++) {
		uint8_t got = 0xff;
		n = data(pl, ks, sess, 1 + ch, ch, "cmd");
		CHECK(rx(pl, n, &got, s) == DBGAUTH_ACCEPT && got == ch &&
		      !strcmp(s, "cmd"));
	}
	n = data(pl, ks, sess, 10, 0, "w 50000170 1\r");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_ACCEPT);
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);          /* replay */
	CHECK(g_stats[DBGAUTH_ST_REPLAY] == 1);
	n = data(pl, ks, sess, 9, 0, "x");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);          /* stale ctr */
	n = data(pl, ks, sess, 11, 0, "abc");
	pl[36] ^= 1;
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);          /* forged */
	CHECK(g_stats[DBGAUTH_ST_BADTAG] == 1 && last_len == 10 && last[4] == 4);
	n = data(pl, ks, sess + 1000, 1, 0, "x");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);          /* no session */
	CHECK(g_stats[DBGAUTH_ST_NOSESS] == 1 && last_len == 10 && last[4] == 4);
	n = data(pl, ks, sess, 12, 0, "0123456789");
	pl[19] = 200;                                          /* len > frame */
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);
	CHECK(g_stats[DBGAUTH_ST_MALFORMED] == 1);
	CHECK(rx((const uint8_t *)"t\r", 2, &ch, s) == DBGAUTH_NOT_ENVELOPE);
	n = data(pl, ks, sess, 13, 0, "");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_ACCEPT && s[0] == 0);
	{                                             /* 1400-byte payload */
		char big[1401];
		memset(big, 'z', 1400); big[1400] = 0;
		uint8_t bp[1500];
		int bn = data(bp, ks, sess, 15, 3, big);
		CHECK(rx(bp, bn, &ch, NULL) == DBGAUTH_ACCEPT);
	}

	/* eviction: 16 newer sessions push the least recently used one out */
	for (i = 0; i < 16; i++)
		CHECK(hello(&sess2, ks2));
	n = data(pl, ks, sess, 20, 0, "x");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_CONSUMED);
	CHECK(g_stats[DBGAUTH_ST_EVICT] >= 1);
	n = data(pl, ks2, sess2, 1, 0, "ok");
	CHECK(rx(pl, n, &ch, s) == DBGAUTH_ACCEPT);

	if (fails) {
		printf("test_dbgauth: %d FAILED\n", fails);
		return 1;
	}
	printf("test_dbgauth: all checks passed\n");
	return 0;
}
