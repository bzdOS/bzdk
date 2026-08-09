/* SPDX-License-Identifier: BSD-2-Clause */

/* mmio_absorb.c — implementation. See mmio_absorb.h for the full contract.
 * ESR_EL2.ISS field decode mirrors vconsole.c's vconsole_handle_fault()
 * ISV=0 fallback path (same bit positions, same "we don't know the access
 * shape, but must not spin the guest forever on the same faulting
 * instruction" reasoning) -- duplicated locally rather than shared, same
 * disjoint-files convention as zload2.c/stage2_zephyr.c.
 *
 * Freestanding: <stdint.h> + exceptions.h only, no libc.
 */
#include <stdint.h>
#include "mmio_absorb.h"
#include "hv_addrmap.h"

/* ------------------------------------------------------------------ *
 * Breadcrumb window: HVMAP_MMIOABS_BC (0x50073000, "MABS") -- see
 * hv_addrmap.h.
 *   [0] magic       0x4D414253 ("MABS")
 *   [1] count       running count of faults absorbed
 *   [2] last_far_lo last FAR_EL2 seen, low 32 bits
 *   [3] last_esr_lo last ESR_EL2 seen, low 32 bits
 * ------------------------------------------------------------------ */
#define MABS_MAGIC 0x4D414253u   /* "MABS" */

enum {
	MABS_MAGIC_IDX = 0,
	MABS_COUNT_IDX,
	MABS_LAST_FAR_IDX,
	MABS_LAST_ESR_IDX,
};

static inline void
mabs_bc(int i, uint32_t v)
{
	volatile uint32_t *p =
		(volatile uint32_t *)(HVMAP_MMIOABS_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ESR_EL2.ISS field positions for a data-abort EC (0x24) -- identical
 * layout to vconsole.c's copies (see that file's own comment for the
 * citation), duplicated locally per this file's disjoint convention. */
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)

#define SRT_XZR   31u   /* SRT==31 means the zero register, not x[31] */

int
mmio_absorb_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t isv = esr & ESR_ISV_BIT;
	uint32_t wnr = esr & ESR_WNR_BIT;

	static uint32_t count;
	count++;
	mabs_bc(MABS_MAGIC_IDX, MABS_MAGIC);
	mabs_bc(MABS_COUNT_IDX, count);
	mabs_bc(MABS_LAST_FAR_IDX, (uint32_t)frame->far);
	mabs_bc(MABS_LAST_ESR_IDX, esr);

	if (isv && !wnr) {
		/* Read, decodable: synthesize 0 into the destination GPR. */
		uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
		if (srt != SRT_XZR)
			frame->x[srt] = 0;
	}
	/* Write (any ISV), or undecodable (ISV==0) read: nothing more to do --
	 * a write has no destination to synthesize into, and an undecodable
	 * read has no known SRT to write. Either way, absorb it. */

	frame->elr += 4;
	return 1;
}
