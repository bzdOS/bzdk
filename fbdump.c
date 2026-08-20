/* SPDX-License-Identifier: BSD-2-Clause */

/* fbdump.c — see fbdump.h for the transport rationale and framing. */
#include <stdint.h>
#include "fbdump.h"

extern int  emac_send_frame(uint16_t ethertype, const uint8_t *payload,
                            uint16_t len);
extern void emac_poll(void);
extern void wdt_debug_kick(void);

/* The DRAM window we are willing to read. Same bounds coredump.c uses, and for
 * the same reason: reading outside it is what faults, and a screenshot tool
 * must never be able to take the board down. */
#define DRAM_LO 0x40000000u
#define DRAM_HI 0x80000000u

struct fbdump_hdr {
	uint32_t magic;
	uint32_t offset;
	uint16_t len;
	uint16_t flags;
};

int
fbdump_send(uint32_t pa, uint32_t len)
{
	uint8_t frame[sizeof(struct fbdump_hdr) + FBDUMP_DATA_MAX];
	struct fbdump_hdr *h = (struct fbdump_hdr *)frame;
	uint8_t *data = frame + sizeof(*h);
	uint32_t off, frames = 0;

	if (len == 0u)
		return (-3);
	if (len > FBDUMP_MAX_LEN)
		return (-2);
	/* The addition cannot wrap past the check because len is already capped
	 * well below 4 GiB, but check the end explicitly anyway -- an
	 * off-by-one here is a fault in EL2, not a bad pixel. */
	if (pa < DRAM_LO || pa >= DRAM_HI || (pa + len) > DRAM_HI)
		return (-1);

	for (off = 0u; off < len; off += FBDUMP_DATA_MAX) {
		uint32_t n = len - off;
		uint32_t i;

		if (n > FBDUMP_DATA_MAX)
			n = FBDUMP_DATA_MAX;

		h->magic = FBDUMP_MAGIC;
		h->offset = off;
		h->len = (uint16_t)n;
		h->flags = ((off + n) >= len) ? FBDUMP_FLAG_LAST : 0u;

		/* Byte-wise on purpose: the region is arbitrary and may not be
		 * 4-byte aligned at either end, and a wide load off the end of
		 * the window would fault in EL2 to save nothing measurable
		 * against a 1 Gbit wire. */
		for (i = 0u; i < n; i++)
			data[i] = *(volatile uint8_t *)(uintptr_t)(pa + off + i);

		(void)emac_send_frame(FBDUMP_ETHERTYPE, frame,
		    (uint16_t)(sizeof(*h) + n));
		emac_poll();
		frames++;

		/* See fbdump.h: this core owns the watchdog and we are not
		 * returning to its poll loop for several seconds. */
		if ((frames % FBDUMP_KICK_FRAMES) == 0u)
			wdt_debug_kick();
	}
	wdt_debug_kick();
	return ((int)frames);
}
