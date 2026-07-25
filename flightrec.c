/* SPDX-License-Identifier: BSD-2-Clause */

/* flightrec.c — flight recorder ring buffer. See flightrec.h for the
 * breadcrumb window layout, address choice, and cost rationale.
 *
 * Freestanding: <stdint.h> only. No libc, no allocation, no recursion. */
#include <stdint.h>
#include "flightrec.h"

static uint32_t fltr_total;  /* mirrors header word [1]; kept in .bss so we
                               * don't need to read DRAM back before every
                               * increment (the DRAM copy is still updated
                               * every call so a cold/warm-reset reader always
                               * sees the true count). */
static uint32_t fltr_head;   /* mirrors header word [2], same reasoning. */

/* Plain store + dc civac, NO barrier here — flightrec_log() issues exactly
 * one `dsb sy` after every word of a given call is written, see below. This
 * is the "cheap" half of the design: a per-trap/per-byte call site can't
 * afford a dsb per word the way the rare, one-shot breadcrumbs (EXC1/BTR1)
 * can. */
static inline void fltr_wr(uint32_t widx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(FLTR_BC_BASE + widx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
}

void flightrec_log(uint32_t kind, uint64_t a0, uint64_t a1)
{
	uint32_t slot = fltr_head;
	uint32_t base = FLTR_HDR + slot * FLTR_SLOT_WORDS;

	fltr_wr(base + 0u, kind);
	fltr_wr(base + 1u, (uint32_t)a0);
	fltr_wr(base + 2u, (uint32_t)(a0 >> 32));
	fltr_wr(base + 3u, (uint32_t)a1);
	fltr_wr(base + 4u, (uint32_t)(a1 >> 32));

	fltr_total++;
	fltr_head = (slot + 1u) % FLTR_SLOTS;

	fltr_wr(0, FLTR_MAGIC);
	fltr_wr(1, fltr_total);
	fltr_wr(2, fltr_head);
	fltr_wr(3, FLTR_SLOTS);
	fltr_wr(4, FLTR_SLOT_WORDS);

	/* One barrier for the whole call — every dc civac above is now
	 * guaranteed complete and the record is visible to a physical memory
	 * dump even if a WDT reset lands one instruction after we return. */
	__asm__ volatile("dsb sy" ::: "memory");
}
