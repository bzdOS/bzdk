/* SPDX-License-Identifier: BSD-2-Clause */

/* vetrap.h — passive register trace of the Allwinner A64 video engine.
 *
 * With HV_VETRAP the 4 KiB page at VETRAP_PAGE_BASE (the VE register window,
 * video-codec@1c0e000) is invalid in stage 2.  Every guest access faults to
 * vetrap_handle_fault(), which performs the same access on the real device
 * and appends (offset, direction, width, value) to a ring.  The guest sees an
 * ordinary device; nothing is altered.  Purpose: hardware observation of the
 * order of register writes a working driver makes (clean-room fact source).
 *
 * Ring at HVMAP_VETRAP (hv_addrmap.h):
 *   header (0x40 bytes): [0] magic "VETR" [1] entries written [2] ring size
 *     [3] ISV==0 accesses [4] bytes not traced
 *   entries (8 bytes): w0 = off | page<<12 | wnr<<16 | sas<<17 | cpu<<19 | rep<<24
 *     (rep = extra identical consecutive reads folded into this entry),
 *     w1 = value read or written. */
#ifndef BZDOS_VETRAP_H
#define BZDOS_VETRAP_H

#include "exceptions.h"

#define VETRAP_PAGE_BASE  0x01C0E000UL
#define VETRAP_PAGE_SIZE  0x1000UL

/* HV_VETRAP=2 also traces the pages a driver touches around the engine:
 * system control (SRAM mapping), clock/reset unit (offsets below 0x400
 * only are logged; the rest of that page is the pin controller and timers),
 * and the DRAM controller.  The page index goes in bits 12..15 of the ring's
 * offset field: 0 VE, 1 system control, 2 CCU, 3 DRAM controller. */
#if HV_VETRAP >= 2
#define VETRAP_NPAGES 4
#else
#define VETRAP_NPAGES 1
#endif
extern const unsigned long vetrap_pages[VETRAP_NPAGES];
static inline int vetrap_pa_traced(unsigned long pa)
{
	for (int i = 0; i < VETRAP_NPAGES; i++)
		if ((pa & ~0xFFFUL) == vetrap_pages[i])
			return 1;
	return 0;
}

int vetrap_handle_fault(struct el2_frame *frame);

#endif
