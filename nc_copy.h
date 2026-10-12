/* SPDX-License-Identifier: BSD-2-Clause */

/* nc_copy.h — memcpy for EL2's Normal Non-cacheable mappings (guest RAM and
 * the EMAC DMA scratch, see el2_ncmap.c).
 *
 * Every access to Non-cacheable memory is its own bus transaction, so the
 * width of each access is what a copy costs: the old byte loops in
 * vnet_emac.c/emac.c paid ~1500 transactions per full-size frame and side.
 * This copies 8 bytes per access once the destination is 8-aligned; a
 * source at a different alignment is read as aligned 8-byte words and
 * shifted into place, so neither side ever takes an unaligned access
 * (alignment checking may be on at EL2, and must be for Device memory).
 *
 * Reads of the source are rounded out to the enclosing aligned 8-byte
 * words. Those few extra bytes never leave the 8-byte granule of a byte
 * that is copied, so they can never cross a page or a guest_gpa_ok()
 * window boundary; the extra bytes are read, never written.
 *
 * All accesses are volatile and of fixed width, so the compiler can neither
 * split, merge nor elide them. Cache maintenance stays the caller's job
 * (one range operation per copy, not one per access). Little-endian only. */
#ifndef BZDOS_NC_COPY_H
#define BZDOS_NC_COPY_H

#include <stdint.h>

static inline void nc_copy_small(uintptr_t d, uintptr_t s, uint32_t n)
{
	while (n) {
		if (n >= 4 && !(d & 3) && !(s & 3)) {
			*(volatile uint32_t *)d = *(const volatile uint32_t *)s;
			d += 4; s += 4; n -= 4;
		} else if (n >= 2 && !(d & 1) && !(s & 1)) {
			*(volatile uint16_t *)d = *(const volatile uint16_t *)s;
			d += 2; s += 2; n -= 2;
		} else {
			*(volatile uint8_t *)d = *(const volatile uint8_t *)s;
			d += 1; s += 1; n -= 1;
		}
	}
}

static inline void nc_copy(void *dst, const void *src, uint32_t n)
{
	uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;

	if (n < 16) {
		nc_copy_small(d, s, n);
		return;
	}
	/* Head: bring the destination to 8-byte alignment. */
	if (d & 7) {
		uint32_t h = 8u - (uint32_t)(d & 7);
		nc_copy_small(d, s, h);
		d += h; s += h; n -= h;
	}
	if (!(s & 7)) {
		for (; n >= 8; n -= 8, d += 8, s += 8)
			*(volatile uint64_t *)d = *(const volatile uint64_t *)s;
	} else {
		/* Source misaligned by k bytes: merge two aligned words. */
		uint32_t sh = (uint32_t)(s & 7) * 8u;
		const volatile uint64_t *sw = (const volatile uint64_t *)(s & ~(uintptr_t)7);
		uint64_t w0 = *sw++;
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			uint64_t w1 = *sw++;
			*(volatile uint64_t *)d = (w0 >> sh) | (w1 << (64u - sh));
			w0 = w1;
		}
	}
	nc_copy_small(d, s, n);
}

/* Zero n bytes at dst with the widest aligned stores. */
static inline void nc_zero(void *dst, uint32_t n)
{
	uintptr_t d = (uintptr_t)dst;
	while (n && (d & 7)) {
		*(volatile uint8_t *)d = 0; d++; n--;
	}
	for (; n >= 8; n -= 8, d += 8)
		*(volatile uint64_t *)d = 0;
	while (n) {
		*(volatile uint8_t *)d = 0; d++; n--;
	}
}

#endif /* BZDOS_NC_COPY_H */
