/* SPDX-License-Identifier: BSD-2-Clause */
/* lz4.h — LZ4 block format, freestanding, for vzram's 4 KiB pages.
 *
 * Standard LZ4 *block* format (no frame header), so a page can be checked
 * against the reference `lz4` tool on the host. Inputs are at most 64 KiB
 * (positions fit a uint16_t hash table); vzram only ever passes 4 KiB. */
#ifndef BZDOS_LZ4_H
#define BZDOS_LZ4_H
#include <stdint.h>

/* Worst case for n input bytes: n + n/255 + 16. */
#define LZ4_BOUND(n) ((n) + (n) / 255u + 16u)

/* Compress n (<= 65536) bytes from src into dst (capacity cap). Returns the
 * compressed length, or 0 if it does not fit in cap -- the caller then keeps
 * the input raw. Not reentrant: uses one static hash table (callers hold
 * their device lock). */
uint32_t lz4_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap);

/* Decompress clen bytes into exactly want bytes at dst. Returns 0 on
 * success, -1 on any malformed input: every read and write is bounds-checked,
 * so a corrupted slot can fail but never write outside dst. */
int lz4_decompress(const uint8_t *src, uint32_t clen, uint8_t *dst, uint32_t want);

#endif /* BZDOS_LZ4_H */
