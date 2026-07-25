/* SPDX-License-Identifier: BSD-2-Clause */

/* backtrace.c — AArch64 frame-pointer chain walk. See backtrace.h.
 *
 * Freestanding: <stdint.h> only. No libc, no allocation, no recursion — safe
 * to call from EL2 trap/tick context. The single job is to turn a live
 * (pc,fp,lr) into a bounded list of return addresses without ever faulting the
 * walker itself.
 *
 * ROADMAP B3 adds backtrace_symbolize() (bottom of this file): a SEPARATE,
 * additive pass that resolves the first few frames of an already-computed
 * backtrace to "symbol+0xoffset" via ksym.c and mirrors them into a NEW
 * breadcrumb window, "BTS1". It is deliberately a SEPARATE function/window
 * rather than folded into backtrace_walk()'s own BTR1 mirror, for two
 * reasons: (1) backtrace_walk() is called unconditionally on every single
 * recorded fault (see el2_exc.c's B3 block) and by dbgmon's on-demand `bt`,
 * both of which must stay cheap and side-effect-free w.r.t. symbol lookup —
 * symbolization is a bounded-but-real linear scan (see ksym.c), better kept
 * an explicit, opt-in extra step than silently taxing every walk; and (2) an
 * audit of every _BC_BASE in this tree while adding this found that BTR1's
 * own base address (0x50000700) already ALIASES alloc.c's ALLC_BC_BASE — a
 * pre-existing collision, not introduced here and not this change's to fix,
 * but reason enough not to grow BTR1's word layout further into contested
 * space. BTS1 instead uses 0x50000e00, the last free slot before the
 * vconsole ring at 0x50000f00 per hv_addrmap.h's documented range boundary.
 */
#include <stdint.h>
#include "backtrace.h"
#include "ksym.h"

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

/* ------------------------------------------------------------------ *
 * ROADMAP B3: on-board symbolization of the first few backtrace_walk()
 * frames. Breadcrumb window @ 0x50000e00, magic "BTS1" — a NEW, separate
 * window from BTR1 (see the file header comment for why). Layout:
 *   [0] magic 0x42545331 ("BTS1")
 *   [1] nresolved in the last call (0..BTS_SLOTS, counts EVERY frame
 *       attempted, resolved or not — see [name empty] below for how to tell
 *       an individual slot apart)
 *   [2] total calls
 *   [3..7] reserved
 *   per slot i (0..BTS_SLOTS-1), at BTS_HDR + i*BTS_SLOT_WORDS:
 *     [0..4] name, packed 4 ASCII bytes/word little-endian, NUL-padded;
 *            all-zero means ksym_resolve() found no match for this frame
 *     [5,6]  offset into the symbol, low/high 32 bits (0 if unresolved)
 * BTS_SLOTS=6, BTS_NAME_WORDS=5 (20-byte name incl. NUL) ->
 * 8 + 6*7 = 50 words = 200 bytes -> 0x50000e00..0x50000ec8, comfortably
 * inside the free 0x50000e00-0x50000eff slot ahead of the vconsole ring
 * @0x50000f00 (see hv_addrmap.h's documented range boundary).
 * ------------------------------------------------------------------ */
#define BTS_BC_BASE       0x50000e00UL
#define BTS_MAGIC         0x42545331u   /* "BTS1" */
#define BTS_SLOTS         6u
#define BTS_NAME_WORDS    5u            /* 20 bytes, incl. NUL */
#define BTS_NAME_MAX      20
#define BTS_SLOT_WORDS    (BTS_NAME_WORDS + 2u)   /* + offset lo/hi */
#define BTS_HDR           8u

static uint32_t bts_total;

static inline void bts_wr(uint32_t widx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BTS_BC_BASE + widx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Pack up to BTS_NAME_MAX-1 bytes of `name` (NUL-terminated, already bounded
 * by ksym_resolve()'s own name_max) into BTS_NAME_WORDS breadcrumb words,
 * little-endian (byte 0 in bits[7:0] of word 0). Stops packing real bytes at
 * the first NUL (or name==NULL, or BTS_NAME_MAX) and zero-fills everything
 * from there on — so any uninitialized stack garbage the caller's buffer
 * happens to hold PAST its own NUL is never copied out, only a clean,
 * zero-padded string. */
static void bts_wr_name(uint32_t base, const char *name)
{
	uint32_t w;
	int stopped = 0;

	for (w = 0; w < BTS_NAME_WORDS; w++) {
		uint32_t k, word = 0;

		for (k = 0; k < 4u; k++) {
			uint32_t idx = w * 4u + k;
			uint8_t  byte = 0;

			if (!stopped && name && idx < (uint32_t)BTS_NAME_MAX && name[idx] != 0)
				byte = (uint8_t)name[idx];
			else
				stopped = 1;
			word |= (uint32_t)byte << (k * 8u);
		}
		bts_wr(base + w, word);
	}
}

void backtrace_symbolize(const uint64_t *frames, int n)
{
	int i, m;

	if (!frames)
		n = 0;
	m = n;
	if (m > (int)BTS_SLOTS)
		m = (int)BTS_SLOTS;
	if (m < 0)
		m = 0;

	bts_wr(0, BTS_MAGIC);
	bts_wr(1, (uint32_t)m);
	bts_wr(2, ++bts_total);

	for (i = 0; i < m; i++) {
		char name[BTS_NAME_MAX];
		uint64_t off = 0;
		uint32_t base = BTS_HDR + (uint32_t)i * BTS_SLOT_WORDS;
		int ok;

		ok = ksym_resolve(frames[i], name, BTS_NAME_MAX, &off);
		bts_wr_name(base, ok ? name : (const char *)0);
		bts_wr(base + BTS_NAME_WORDS,      ok ? (uint32_t)off : 0u);
		bts_wr(base + BTS_NAME_WORDS + 1u, ok ? (uint32_t)(off >> 32) : 0u);
	}
	/* Slots beyond m (if a previous call left them populated with a longer
	 * backtrace than this one) are left as-is -- BTS1's [1]=m tells a reader
	 * exactly how many of the BTS_SLOTS entries are current this call. */
}
