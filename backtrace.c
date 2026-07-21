/* backtrace.c — AArch64 frame-pointer chain walk. See backtrace.h.
 *
 * Freestanding: <stdint.h> only. No libc, no allocation, no recursion — safe
 * to call from EL2 trap/tick context. The single job is to turn a live
 * (pc,fp,lr) into a bounded list of return addresses without ever faulting the
 * walker itself.
 */
#include <stdint.h>
#include "backtrace.h"

/* ------------------------------------------------------------------ *
 * Breadcrumb window @ 0x50000700, magic "BTR1". Distinct from every other
 * instrument (see the reserved-address map in the project). 8-word header +
 * up to BT_SLOTS return addresses (2 words each). 8 + 32*2 = 72 words
 * (0x120 bytes) -> 0x50000700..0x50000820, clear of dbg 0x50000e00.
 *   [0] magic 0x42545231 ("BTR1")
 *   [1] nframes in the last walk
 *   [2] total walks
 *   [3] start pc (low 32)   [reserved-ish, quick glance]
 *   [4..7] reserved
 *   [8 + i*2], [8 + i*2 + 1] = out[i] low, high   (i = 0..nframes-1)
 */
#define BT_BC_BASE 0x50000700UL
#define BT_MAGIC   0x42545231u   /* "BTR1" */
#define BT_SLOTS   32u
#define BT_HDR     8u

static uint32_t bt_total;

static inline void bc_wr(uint32_t widx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BT_BC_BASE + widx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Guarded read of a guest/host VA. Returns 1 and *out on success, 0 if the
 * address is not translatable/readable — the walker treats 0 as "stop".
 *
 * Path 1: AT S1E1R runs the guest's own EL1 stage-1 translation (same as
 * dbgmon `gva`); PAR_EL1.F==0 gives the output PA, directly readable under our
 * flat EL2 map. Path 2 (fallback, for MMU-off guest where VA==PA and for our
 * EL2 kernel/stack): if translation faults but the VA sits in the DRAM window
 * we can read it as a physical address directly. Anything else -> fail. */
#define DRAM_LO 0x40000000ull
#define DRAM_HI 0x80000000ull

static int bt_read64(uint64_t va, uint64_t *out)
{
	uint64_t par;

	if (va < 8)                     /* obvious null / tiny — never deref */
		return 0;

	__asm__ volatile("at s1e1r, %0" :: "r"(va) : "memory");
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("mrs %0, par_el1" : "=r"(par));

	if (!(par & 1ull)) {
		uint64_t pa = (par & 0x000ffffffffff000ull) | (va & 0xfffull);
		*out = *(volatile uint64_t *)pa;
		return 1;
	}

	/* Translation faulted — accept only a plausible flat DRAM address. */
	if (va >= DRAM_LO && va < DRAM_HI) {
		*out = *(volatile uint64_t *)va;
		return 1;
	}
	return 0;
}

/* fp must be nonzero, 16-byte aligned, and (except the first hop) strictly
 * increasing — AArch64 stacks grow down, so each caller's frame record sits at
 * a higher address. Rejecting non-monotonic fps kills cycles on corrupt
 * chains. Also keep fp inside the plausible virtual space (reject the top page
 * so [fp+8] can't wrap). */
static int fp_ok(uint64_t fp, uint64_t prev)
{
	if (fp == 0)
		return 0;
	if (fp & 0xfull)
		return 0;
	if (prev != 0 && fp <= prev)
		return 0;
	if (fp > 0xffffffffffff0000ull)
		return 0;
	return 1;
}

int backtrace_walk(uint64_t pc, uint64_t fp, uint64_t lr, uint64_t *out, int max)
{
	int n = 0;
	uint64_t cur = fp, prev = 0;
	int hops;

	if (max <= 0)
		return 0;

	out[n++] = pc;
	if (lr && n < max)
		out[n++] = lr;

	/* Hard hop cap independent of `max` so a pathological chain can't spin
	 * even if the caller passed a huge buffer. */
	for (hops = 0; hops < 64 && n < max; hops++) {
		uint64_t next_fp, saved_lr;

		if (!fp_ok(cur, prev))
			break;
		if (!bt_read64(cur, &next_fp))          /* [fp+0] = caller fp */
			break;
		if (!bt_read64(cur + 8u, &saved_lr))    /* [fp+8] = caller lr */
			break;

		if (saved_lr && n < max)
			out[n++] = saved_lr;

		prev = cur;
		cur = next_fp;
	}

	/* Mirror into the breadcrumb (bounded to BT_SLOTS). */
	{
		int i, m = n;
		if (m > (int)BT_SLOTS)
			m = (int)BT_SLOTS;
		bc_wr(0, BT_MAGIC);
		bc_wr(1, (uint32_t)m);
		bc_wr(2, ++bt_total);
		bc_wr(3, (uint32_t)pc);
		for (i = 0; i < m; i++) {
			bc_wr(BT_HDR + (uint32_t)i * 2u,      (uint32_t)out[i]);
			bc_wr(BT_HDR + (uint32_t)i * 2u + 1u, (uint32_t)(out[i] >> 32));
		}
	}

	return n;
}
