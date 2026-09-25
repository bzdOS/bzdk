/* SPDX-License-Identifier: BSD-2-Clause
 * sdbox.c -- the black box: evidence that outlives the board.
 *
 * Every breadcrumb this hypervisor keeps lives in DRAM. That survives a warm
 * reset and is exactly what a post-mortem reads -- until the board goes
 * dark on every channel and the only way back is cutting the power, which
 * wipes DRAM. Then the evidence dies with the board. 2026-09-23, 09-24 and
 * 09-25 16:41 were all of that shape: a reboot_clean() that was asked for,
 * and nothing after it to say why.
 *
 * So: on the way into reboot_clean(), write one raw sector to the SD card,
 * LBA 64 -- inside the 1004 KiB gap before p1, below nothing that matters
 * (the GPT array ends at LBA 33, p1 starts at 2048). The card keeps it
 * through any reset or power-cycle; the guest reads it back with dd
 * (/dev/vtbd1), sdbox_read.py decodes it.
 *
 * Layout (128 words):
 *   [0] magic "BZBX"  [1] WDEP boots  [2] reason  [3..4] CNTPCT lo/hi
 *   [5] MPIDR & 0xff (which core)  [6] WDEP[4] last BMSR  [7] WDEP[16]
 *   [8..15]   vblk breadcrumb words 56..63 (slot 62 = boot-guard refusals)
 *   [16..23]  flight recorder header (magic, total, head, capacity, stride)
 *   [24..123] the 20 most recent flight-recorder slots, oldest first
 *   [124..127] zero
 *
 * Written with PIO by sd_bio_write(): no DMA, no cache question. Bounded:
 * if the SD lock cannot be taken in ~200 ms, or the SD stack never came up,
 * the record is skipped -- a reset must never wait on a black box. */
#include <stdint.h>
#include "sd_bio.h"
#include "vblk_sd.h"
#include "sdbox.h"

#define SDBOX_LBA        64u
#define SDBOX_MAGIC      0x58425A42u          /* "BZBX" little-endian */
#define SDBOX_BUF_PA     0x50023000UL         /* first free hv-scratch page */
#define WDEP_BASE        0x50022000UL
#define VBLK_BC_BASE     0x50020000UL
#define FLTR_HDR         0x50012000UL
#define FLTR_TAIL_SLOTS  20u

static inline uint32_t rd32(unsigned long pa)
{
	return *(volatile uint32_t *)pa;
}

static inline uint64_t rd_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

void sdbox_record(uint32_t reason)
{
	volatile uint32_t *b = (volatile uint32_t *)SDBOX_BUF_PA;
	uint64_t t = rd_cntpct(), mpidr;
	uint32_t i, head, cap, stride, total;
	unsigned spin;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	for (i = 0; i < 128; i++)
		b[i] = 0;
	b[0] = SDBOX_MAGIC;
	b[1] = rd32(WDEP_BASE + 4);
	b[2] = reason;
	b[3] = (uint32_t)t;
	b[4] = (uint32_t)(t >> 32);
	b[5] = (uint32_t)(mpidr & 0xffu);
	b[6] = rd32(WDEP_BASE + 4 * 4);
	b[7] = rd32(WDEP_BASE + 16 * 4);
	for (i = 0; i < 8; i++)
		b[8 + i] = rd32(VBLK_BC_BASE + (56 + i) * 4);
	for (i = 0; i < 8; i++)
		b[16 + i] = rd32(FLTR_HDR + i * 4);
	total = b[17]; head = b[18]; cap = b[19]; stride = b[20];
	if (b[16] == 0x464C5452u && cap && cap <= 4096u && stride == 5u) {
		uint32_t n = total < FLTR_TAIL_SLOTS ? total : FLTR_TAIL_SLOTS;
		uint32_t first = (head + cap - n) % cap;
		for (i = 0; i < n; i++) {
			uint32_t slot = (first + i) % cap, w;
			unsigned long pa = FLTR_HDR + (8u + slot * stride) * 4u;
			for (w = 0; w < 5; w++)
				b[24 + i * 5 + w] = rd32(pa + w * 4);
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");

	for (spin = 0; spin < 2000; spin++) {          /* ~200 ms at ~100 us/try */
		if (vblk_sd_trylock())
			break;
		for (volatile int d = 0; d < 2000; d++) ;
	}
	if (spin == 2000)
		return;                                   /* guest holds the card: skip */
	(void)sd_bio_write(SDBOX_LBA, SDBOX_BUF_PA);
	vblk_sd_unlock();
}
