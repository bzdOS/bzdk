/* SPDX-License-Identifier: BSD-2-Clause */

/* firstfault.c — latch the FreeBSD guest's ORIGINAL first EL1 fault.
 *
 * The guest dies in a recursive-exception storm so early that the original
 * fault's banked EL1 registers (ELR_EL1/ESR_EL1/FAR_EL1) are overwritten by
 * each re-entry into its own EL1 vector table before anything can read them.
 * stage2_unmap_guest_vector() leaves the guest's EL1 vector PAGE
 * (GUEST_VECTOR_IPA = phys 0x46927000) invalid at stage-2, so the guest's
 * FIRST vector fetch — which happens with the original fault still latched in
 * ELR_EL1/ESR_EL1/FAR_EL1/SPSR_EL1 — takes a stage-2 INSTRUCTION ABORT to EL2
 * (ESR_EL2 EC=0x20) instead of executing. el2_trap routes that here.
 *
 * We read the guest's still-pristine banked EL1 fault registers (an `mrs`
 * from EL2 reads them directly, no trap) plus all 31 GPRs from the saved EL2
 * frame — this IS the original fault — into the FF1V breadcrumb.
 *
 * ------------------------------------------------------------------ *
 * Breadcrumb window: 0x50005800 ("FF1V"). Cache-coherent (dc civac + dsb sy)
 * like every other lane so it survives a WDT reset and is network-readable.
 * Decoded by the dbgmon `ffv` command.
 *
 *   [0]      magic     0x46463156 ("FF1V")
 *   [1]      count     number of vector-page faults seen (>=1 => latched)
 *   [2]      vecoff    ELR_EL2 & 0x780 — which of the 16 vector entries the
 *                      guest was branching to (0x000 sync-SP0, 0x200 IRQ-SP0,
 *                      ... 0x400 sync-SPx, 0x600 sync-lower-64, etc.)
 *   [3]      el2_ec    EC of the EL2 abort itself (0x20 iabort-lower expected;
 *                      0x21/0x24/0x25 if a data access hit the page instead)
 *   [4]  esr_el1(lo)  [5]  esr_el1(hi)   ORIGINAL guest fault cause
 *   [6]  elr_el1(lo)  [7]  elr_el1(hi)   ORIGINAL guest faulting PC
 *   [8]  far_el1(lo)  [9]  far_el1(hi)   ORIGINAL guest faulting address
 *   [10] spsr_el1(lo) [11] spsr_el1(hi)
 *   [12] sp_el1(lo)   [13] sp_el1(hi)
 *   [14 + i*2] = x[i](lo), [15 + i*2] = x[i](hi),  for i = 0..30
 * Total 14 + 31*2 = 76 words (0x130 bytes): 0x50005800..0x50005930.
 *
 * Re-entry policy (option (a) from the brief): latch ONCE (only on the first
 * hit, guarded by ff_count==0), then re-map the vector page and eret so the
 * guest continues into its own EL1 handler — we have captured everything we
 * need, and re-mapping prevents this stage-2 abort from looping in EL2.
 * ff_count keeps counting any further hits (there should be none once the
 * page is mapped) without disturbing the latched record.
 * ------------------------------------------------------------------ */
#include <stdint.h>
#include "exceptions.h"
#include "stage2.h"
#include "firstfault.h"

#define FF1V_BASE   0x50005800UL
#define FF1V_MAGIC  0x46463156u   /* "FF1V" */

static uint32_t ff_count;

static inline void ff_wr(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(FF1V_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void ff_wr64(unsigned i, uint64_t v)
{
	ff_wr(i, (uint32_t)v);
	ff_wr(i + 1u, (uint32_t)(v >> 32));
}

#define FF_RDSYS(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

int firstfault_handle(struct el2_frame *frame)
{
	uint64_t hpfar, ipa;

	/* HPFAR_EL2.FIPA (bits[39:4]) holds the faulting IPA[47:12] on a
	 * stage-2 abort — the page the guest could not translate. Compare its
	 * page base against the guest vector page. */
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;
	if ((ipa & ~(uint64_t)(GUEST_VECTOR_SIZE - 1u)) != GUEST_VECTOR_IPA)
		return 0;   /* not the vector page — let el2_trap handle it */

	/* Latch ONCE: the first hit carries the ORIGINAL, unmasked EL1 fault. */
	if (ff_count == 0u) {
		unsigned i;
		uint32_t ec = (uint32_t)((frame->esr >> 26) & 0x3fu);

		ff_wr(0, FF1V_MAGIC);
		ff_wr(2, (uint32_t)(frame->elr & 0x780ull)); /* vector entry offset */
		ff_wr(3, ec);                                /* EL2 abort EC */
		ff_wr64(4,  FF_RDSYS(esr_el1));
		ff_wr64(6,  FF_RDSYS(elr_el1));
		ff_wr64(8,  FF_RDSYS(far_el1));
		ff_wr64(10, FF_RDSYS(spsr_el1));
		ff_wr64(12, FF_RDSYS(sp_el1));
		for (i = 0; i < 31u; i++)
			ff_wr64(14u + i * 2u, frame->x[i]);
	}

	ff_count++;
	ff_wr(1, ff_count);

	/* Re-map the vector page and eret: the guest resumes into its own EL1
	 * handler (the storm continues, but we have captured its origin) and no
	 * further stage-2 abort recurs into an EL2 loop. */
	stage2_map_guest_vector();
	return 1;
}
