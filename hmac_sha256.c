/* hmac_sha256.c -- compact freestanding SHA-256 / HMAC-SHA256, no libc.
 * See hmac_sha256.h for the why (ROADMAP T5 / docs/security-notes.md,
 * -DDBG_AUTH). This is a textbook FIPS 180-4 SHA-256 core plus an RFC 2104
 * HMAC wrapper; nothing here is board-specific.
 */
#include "hmac_sha256.h"

/* This whole translation unit is a no-op unless -DDBG_AUTH is set: emac.c
 * only ever calls into it from behind its own matching #if defined(DBG_AUTH)
 * gate (see emac.c's dbg_auth_check()), and the Makefile's DBG_OBJS/GDB_OBJS
 * lists link hmac_sha256.o UNCONDITIONALLY (append-only, no per-flag object
 * selection). Without this guard, the default (no -D) `make dbg`/`make gdb`
 * builds would silently grow by this file's compiled code -- dead, unreached
 * code, but still bytes added to .text -- which breaks the "default build is
 * byte-for-byte unchanged" requirement (ROADMAP T5). Guarding the entire body
 * keeps the object file empty (no symbols) in every build that doesn't ask
 * for DBG_AUTH, so linking it in is a true no-op. */
#if defined(DBG_AUTH)

/* ------------------------------------------------------------------ *
 * SHA-256 core.
 * ------------------------------------------------------------------ */

static const uint32_t K[64] = {
	0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
	0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
	0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
	0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
	0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
	0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
	0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
	0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
	0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
	0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
	0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
	0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
	0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
	0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
	0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
	0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

struct sha256_ctx {
	uint32_t h[8];
	uint64_t total_len;      /* bytes processed so far */
	uint8_t  buf[SHA256_BLOCK_LEN];
	uint32_t buf_len;        /* bytes currently held in buf[] */
};

static inline uint32_t rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32 - n));
}

/* Process exactly one 64-byte block. */
static void sha256_block(struct sha256_ctx *c, const uint8_t block[SHA256_BLOCK_LEN])
{
	uint32_t w[64];
	uint32_t a, b, cc, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8)  |
		       ((uint32_t)block[i * 4 + 3]);
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}

	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}

	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c)
{
	c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
	c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
	c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
	c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
	c->total_len = 0;
	c->buf_len = 0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *data, uint32_t len)
{
	c->total_len += len;

	if (c->buf_len > 0) {
		uint32_t need = SHA256_BLOCK_LEN - c->buf_len;
		uint32_t take = (len < need) ? len : need;
		uint32_t i;
		for (i = 0; i < take; i++)
			c->buf[c->buf_len + i] = data[i];
		c->buf_len += take;
		data += take;
		len -= take;
		if (c->buf_len == SHA256_BLOCK_LEN) {
			sha256_block(c, c->buf);
			c->buf_len = 0;
		}
	}

	while (len >= SHA256_BLOCK_LEN) {
		sha256_block(c, data);
		data += SHA256_BLOCK_LEN;
		len -= SHA256_BLOCK_LEN;
	}

	if (len > 0) {
		uint32_t i;
		for (i = 0; i < len; i++)
			c->buf[c->buf_len + i] = data[i];
		c->buf_len += len;
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN])
{
	uint64_t bit_len = c->total_len * 8;
	uint8_t pad = 0x80;
	int i;

	sha256_update(c, &pad, 1);

	/* Zero-pad until buf_len == 56 (mod 64), leaving 8 bytes for the
	 * bit-length. sha256_update() above already advanced total_len by 1
	 * for the 0x80 byte, so recompute the target fill from buf_len alone
	 * rather than touching total_len again. */
	{
		uint8_t zero = 0x00;
		while (c->buf_len != 56)
			sha256_update(c, &zero, 1);
	}

	/* total_len has been perturbed by the padding update() calls above;
	 * it must NOT be used for bit_len (captured before padding started). */
	for (i = 0; i < 8; i++) {
		uint8_t b = (uint8_t)(bit_len >> (56 - 8 * i));
		/* Append directly to buf (guaranteed buf_len==56+i here) rather
		 * than through sha256_update(), which would re-touch total_len
		 * pointlessly -- still correct either way, but this is clearer. */
		c->buf[56 + i] = b;
	}
	c->buf_len = 64;
	sha256_block(c, c->buf);
	c->buf_len = 0;

	for (i = 0; i < 8; i++) {
		out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(c->h[i]);
	}
}

/* ------------------------------------------------------------------ *
 * HMAC-SHA256 (RFC 2104), built on the core above.
 * ------------------------------------------------------------------ */

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *msg, uint32_t msg_len,
                  uint8_t out[SHA256_DIGEST_LEN])
{
	uint8_t k0[SHA256_BLOCK_LEN];
	uint8_t ipad[SHA256_BLOCK_LEN];
	uint8_t opad[SHA256_BLOCK_LEN];
	uint8_t inner_hash[SHA256_DIGEST_LEN];
	struct sha256_ctx ctx;
	uint32_t i;

	/* Step 1: normalize key to exactly one block (k0). Keys longer than
	 * the block size are hashed down first; shorter keys are zero-padded. */
	for (i = 0; i < SHA256_BLOCK_LEN; i++)
		k0[i] = 0;

	if (key_len > SHA256_BLOCK_LEN) {
		sha256_init(&ctx);
		sha256_update(&ctx, key, key_len);
		sha256_final(&ctx, k0);   /* first 32 bytes of k0 = H(key), rest 0 */
	} else {
		for (i = 0; i < key_len; i++)
			k0[i] = key[i];
	}

	for (i = 0; i < SHA256_BLOCK_LEN; i++) {
		ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
		opad[i] = (uint8_t)(k0[i] ^ 0x5cu);
	}

	/* inner = H(ipad || msg) */
	sha256_init(&ctx);
	sha256_update(&ctx, ipad, SHA256_BLOCK_LEN);
	sha256_update(&ctx, msg, msg_len);
	sha256_final(&ctx, inner_hash);

	/* outer = H(opad || inner) */
	sha256_init(&ctx);
	sha256_update(&ctx, opad, SHA256_BLOCK_LEN);
	sha256_update(&ctx, inner_hash, SHA256_DIGEST_LEN);
	sha256_final(&ctx, out);
}

int hmac_sha256_equal(const uint8_t a[SHA256_DIGEST_LEN],
                       const uint8_t b[SHA256_DIGEST_LEN])
{
	uint8_t diff = 0;
	int i;
	for (i = 0; i < SHA256_DIGEST_LEN; i++)
		diff |= (uint8_t)(a[i] ^ b[i]);
	return diff == 0;
}

#endif /* DBG_AUTH */
