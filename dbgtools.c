/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgtools.c — see dbgtools.h for the full design rationale. Freestanding:
 * <stdint.h> + our own headers only, no libc.
 */
#include <stdint.h>
#include "dbgtools.h"
#include "emac.h"

/* Ethertype for the raw peek — mirrors emac.c's own ETHERTYPE_* constants
 * (0x88B5 console, 0x88B6 netcon, 0x88B8 snapnet; 0x88B7 confirmed unused by
 * a full-tree grep before picking it). Kept here too, same reasoning as
 * netcon.c's own NETCON_ETHERTYPE copy: emac.h intentionally exposes only
 * emac_send_frame()'s ethertype PARAMETER, not the individual constants. */
#define DBGTOOLS_ETHERTYPE 0x88B7u

/* -DBZDOS_BUILD_ID is supplied by the Makefile (`git rev-parse --short=12
 * HEAD`, +'+' if the tree is dirty, else a UTC date-time stamp — see
 * Makefile's BUILD_ID comment). This fallback only fires if someone compiles
 * dbgtools.c directly without going through the Makefile. */
#ifndef BZDOS_BUILD_ID
#define BZDOS_BUILD_ID "unknown-build"
#endif
static const char g_build_id_str[] = BZDOS_BUILD_ID;

/* Cache-coherent word store — identical dc-civac + dsb idiom used throughout
 * the tree (backtrace.c, vconsole.c, el2_exc.c, smp.c, ...): reaches physical
 * DRAM (not just this core's cache) before returning, so the value survives
 * a WARM WDOG reset even if it happens moments later. */
static inline void bc_wr32(uint32_t off, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_DBGTOOLS_BASE + off);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t bc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(HVMAP_DBGTOOLS_BASE + off);
}

void dbgtools_init(void)
{
	uint32_t warm = (bc_rd32(0x00) == HVMAP_DBGTOOLS_MAGIC);

	bc_wr32(0x00, HVMAP_DBGTOOLS_MAGIC);
	bc_wr32(0x04, 0);        /* HEARTBEAT: always restart at 0, cold or warm */

	if (!warm) {
		/* True cold boot (fresh TFTP load, or the very first boot ever):
		 * force the hold gate OFF. See dbgtools.h's header comment —
		 * this is the property that keeps every ordinary automated
		 * chimpd boot cycle byte-for-byte identical to today's
		 * behavior (boot straight through) unless a host tool
		 * explicitly armed the hold across a WARM reset. */
		bc_wr32(0x08, 0);    /* HOLD */
		bc_wr32(0x0c, 0);    /* RELEASE */
	}
	/* else: warm reset — our own magic survived, so DRAM here is exactly
	 * what a host tool last wrote. Leave HOLD/RELEASE untouched. */

	/* Build-id: always refreshed (reflects whatever's ACTUALLY running
	 * right now — no reason to preserve a stale string across a
	 * rebuild-and-reflash cycle, warm reset or not). */
	{
		volatile uint8_t *dst =
			(volatile uint8_t *)(HVMAP_DBGTOOLS_BUILDID);
		uint32_t i;
		for (i = 0; i < HVMAP_DBGTOOLS_BUILDID_SIZE; i++) {
			dst[i] = (i < sizeof(g_build_id_str))
			         ? (uint8_t)g_build_id_str[i] : 0;
		}
		__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(&dst[0])
		                  : "memory");
		/* One cache line covers 32 bytes on the A53 (64B lines), but
		 * civac on the first byte's line only guarantees that line —
		 * flush the tail too since BUILDID_SIZE may grow. */
		__asm__ volatile("dc civac, %0\n\tdsb sy" ::
		                  "r"(&dst[HVMAP_DBGTOOLS_BUILDID_SIZE - 1])
		                  : "memory");
	}
}

void dbgtools_hold_set(void)
{
	bc_wr32(0x08, 1);   /* HOLD   */
	bc_wr32(0x0c, 0);   /* RELEASE */
}

void dbgtools_release_set(void)
{
	bc_wr32(0x0c, 1);   /* RELEASE */
}

void dbgtools_rx_frame(const uint8_t *payload, uint16_t len)
{
	uint8_t reply[4 + 4 + 4 + 4 + HVMAP_DBGTOOLS_BUILDID_SIZE];
	uint32_t i;

	(void)payload; (void)len;   /* request carries no fields (yet) */

	reply[0] = 'D'; reply[1] = 'B'; reply[2] = 'G'; reply[3] = 'T';

	{
		uint32_t hb = bc_rd32(0x04), hold = bc_rd32(0x08),
		         rel = bc_rd32(0x0c);
		reply[4]  = (uint8_t)(hb  & 0xff); reply[5]  = (uint8_t)((hb  >> 8)  & 0xff);
		reply[6]  = (uint8_t)((hb >> 16) & 0xff); reply[7]  = (uint8_t)((hb >> 24) & 0xff);
		reply[8]  = (uint8_t)(hold & 0xff); reply[9]  = (uint8_t)((hold >> 8) & 0xff);
		reply[10] = (uint8_t)((hold >> 16) & 0xff); reply[11] = (uint8_t)((hold >> 24) & 0xff);
		reply[12] = (uint8_t)(rel & 0xff); reply[13] = (uint8_t)((rel >> 8) & 0xff);
		reply[14] = (uint8_t)((rel >> 16) & 0xff); reply[15] = (uint8_t)((rel >> 24) & 0xff);
	}

	{
		const volatile uint8_t *src =
			(const volatile uint8_t *)(HVMAP_DBGTOOLS_BUILDID);
		for (i = 0; i < HVMAP_DBGTOOLS_BUILDID_SIZE; i++)
			reply[16 + i] = src[i];
	}

	(void)emac_send_frame(DBGTOOLS_ETHERTYPE, reply, (uint16_t)sizeof(reply));
}
