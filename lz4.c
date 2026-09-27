/* SPDX-License-Identifier: BSD-2-Clause */
/* lz4.c — see lz4.h. Greedy single-probe compressor (the "fast" shape of the
 * reference implementation) and a fully bounds-checked decoder.
 *
 * Block format: sequences of
 *   token (hi nibble literal length, lo nibble match length - 4),
 *   [literal length extension bytes], literals,
 *   2-byte little-endian match offset, [match length extension bytes]
 * and the block ends with a literals-only sequence. Rules the decoder of the
 * reference implementation relies on and this encoder keeps: the last match
 * starts at least 12 bytes before the end, and the last 5 bytes are always
 * literals. */
#include "lz4.h"

#define MINMATCH     4u
#define LASTLITERALS 5u
#define MFLIMIT      12u
#define HASH_LOG     12u

static uint16_t g_hash[1u << HASH_LOG];

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t hash4(uint32_t v)
{
	return (v * 2654435761u) >> (32u - HASH_LOG);
}

static uint8_t *put_len(uint8_t *op, uint8_t *oend, uint32_t len)
{
	while (len >= 255u) {
		if (op >= oend)
			return 0;
		*op++ = 255u;
		len -= 255u;
	}
	if (op >= oend)
		return 0;
	*op++ = (uint8_t)len;
	return op;
}

/* Emit one sequence: literals [anchor, anchor+lit) and, if mlen, a match. */
static uint8_t *emit(uint8_t *op, uint8_t *oend, const uint8_t *anchor,
                     uint32_t lit, uint32_t off, uint32_t mlen)
{
	uint8_t *token;
	uint32_t i;

	if (op >= oend)
		return 0;
	token = op++;
	*token = (uint8_t)((lit >= 15u ? 15u : lit) << 4);
	if (lit >= 15u && !(op = put_len(op, oend, lit - 15u)))
		return 0;
	if ((uint32_t)(oend - op) < lit)
		return 0;
	for (i = 0; i < lit; i++)
		op[i] = anchor[i];
	op += lit;
	if (mlen == 0)
		return op;
	if ((uint32_t)(oend - op) < 2u)
		return 0;
	*op++ = (uint8_t)off;
	*op++ = (uint8_t)(off >> 8);
	mlen -= MINMATCH;
	*token |= (uint8_t)(mlen >= 15u ? 15u : mlen);
	if (mlen >= 15u && !(op = put_len(op, oend, mlen - 15u)))
		return 0;
	return op;
}

uint32_t lz4_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap)
{
	const uint8_t *ip = src, *anchor = src;
	const uint8_t *iend = src + n;
	const uint8_t *mflimit = (n > MFLIMIT) ? iend - MFLIMIT : src;
	const uint8_t *matchlimit = iend - LASTLITERALS;
	uint8_t *op = dst, *oend = dst + cap;
	uint32_t i;

	if (n > 65536u)
		return 0;
	for (i = 0; i < (1u << HASH_LOG); i++)
		g_hash[i] = 0;

	if (n > MFLIMIT) {
		ip++;           /* position 0 seeds nothing useful; start at 1 */
		while (ip < mflimit) {
			uint32_t v = rd32(ip), h = hash4(v);
			const uint8_t *ref = src + g_hash[h];
			uint32_t mlen;

			g_hash[h] = (uint16_t)(ip - src);
			if (ref >= ip || rd32(ref) != v) {
				ip++;
				continue;
			}
			/* extend backwards over pending literals, then forwards */
			while (ip > anchor && ref > src && ip[-1] == ref[-1]) {
				ip--;
				ref--;
			}
			mlen = MINMATCH;
			while (ip + mlen < matchlimit && ip[mlen] == ref[mlen])
				mlen++;
			op = emit(op, oend, anchor, (uint32_t)(ip - anchor),
			          (uint32_t)(ip - ref), mlen);
			if (!op)
				return 0;
			ip += mlen;
			anchor = ip;
			if (ip < mflimit)
				g_hash[hash4(rd32(ip - 2))] = (uint16_t)(ip - 2 - src);
		}
	}
	op = emit(op, oend, anchor, (uint32_t)(iend - anchor), 0, 0);
	if (!op)
		return 0;
	return (uint32_t)(op - dst);
}

int lz4_decompress(const uint8_t *src, uint32_t clen, uint8_t *dst, uint32_t want)
{
	const uint8_t *ip = src, *iend = src + clen;
	uint8_t *op = dst, *oend = dst + want;

	for (;;) {
		uint32_t token, lit, mlen, off, i;
		const uint8_t *ref;

		if (ip >= iend)
			return -1;
		token = *ip++;
		lit = token >> 4;
		if (lit == 15u) {
			uint32_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255u);
		}
		if ((uint32_t)(iend - ip) < lit || (uint32_t)(oend - op) < lit)
			return -1;
		for (i = 0; i < lit; i++)
			op[i] = ip[i];
		op += lit;
		ip += lit;
		if (ip == iend)                   /* last sequence: literals only */
			return (op == oend) ? 0 : -1;

		if ((uint32_t)(iend - ip) < 2u)
			return -1;
		off = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
		ip += 2;
		if (off == 0 || off > (uint32_t)(op - dst))
			return -1;
		ref = op - off;
		mlen = (token & 15u);
		if (mlen == 15u) {
			uint32_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				mlen += b;
			} while (b == 255u);
		}
		mlen += MINMATCH;
		if ((uint32_t)(oend - op) < mlen)
			return -1;
		for (i = 0; i < mlen; i++)       /* byte copy: overlap is legal */
			op[i] = ref[i];
		op += mlen;
	}
}
