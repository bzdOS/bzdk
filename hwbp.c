/* SPDX-License-Identifier: BSD-2-Clause */

/* hwbp.c — EL2-controlled hardware breakpoints + watchpoints. See hwbp.h.
 *
 * Freestanding: <stdint.h> + exceptions.h only, no libc. All state is a few
 * static arrays plus a DRAM breadcrumb; the debug-register writes are plain
 * MSR/MRS to the guest's banked EL1 debug registers, issued from EL2.
 *
 * ---- TDE coordination with the single-step lane (el2_exc.c) ----------------
 * Both this module and el2_ss_toggle() drive MDCR_EL2.TDE (bit8), the single
 * bit that routes EL1/EL0 debug exceptions to EL2. They are meant to be used
 * one at a time (you either single-step OR set breakpoints for a given run).
 * To avoid the classic refcount fight, hwbp SETS TDE when it arms a slot and
 * NEVER clears it on its own — a stray TDE=1 with no slot armed and MDE clear
 * generates no exceptions, so leaving it on is harmless. (The single-step lane
 * still clears TDE when you toggle `ss` off; if you interleave `ss` and `bp`
 * you should re-arm the breakpoints afterwards. Documented, not defended.)
 * MDSCR_EL1.MDE (the actual breakpoint/watchpoint enable) is owned solely by
 * hwbp and is independent of MDSCR_EL1.SS used by single-step. */

#include <stdint.h>
#include "exceptions.h"
#include "hwbp.h"

/* ------------------------------------------------------------------ *
 * Breadcrumb window @ 0x50000600, magic "HWBP". Distinct from every other
 * instrument (MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300, EXC
 * 0x50000400, dbg 0x50000e00, vconsole 0x50000f00, gtrace 0x50002000, FFL1
 * 0x50002400, SST1 0x50002800, HDMI 0x50003000). 16 words -> ..0x50000640.
 *   [0]  magic 0x48574250 ("HWBP")
 *   [1]  hit count (total bp+wp hits seen)
 *   [2]  last kind: 0 = breakpoint, 1 = watchpoint
 *   [3]  last slot index that hit
 *   [4]  last ESR_EL2 (low 32)
 *   [5]  last EC (ESR>>26 & 0x3f)
 *   [6]  hit PC  (ELR_EL2 low 32)     [7] hit PC high 32
 *   [8]  hit FAR (FAR_EL2 low 32)     [9] hit FAR high 32
 *   [10] armed-bp bitmap (bit i = bp slot i enabled)
 *   [11] armed-wp bitmap (bit i = wp slot i enabled)
 *   [12..15] reserved
 */
#define HWBP_BC_BASE 0x50000600UL
#define HWBP_MAGIC   0x48574250u   /* "HWBP" */

static inline void bc_wr(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HWBP_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Shadow state (so `bpl` can list without re-reading the debug regs and so we
 * know the configured VA to match a breakpoint hit against). */
static uint64_t bp_va[HWBP_MAX_BP];
static uint8_t  bp_en[HWBP_MAX_BP];
static uint64_t wp_va[HWBP_MAX_WP];
static uint8_t  wp_en[HWBP_MAX_WP];

static int hwbp_inited;

/* ------------------------------------------------------------------ *
 * Debug-register access. The register NAME must be a literal, so switch-arm
 * every implemented index. Only DBG{B,W}{V,C}R<0..max>_EL1 are touched. */

#define WR(reg, v) __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)) : "memory")
#define RD(reg)    ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })

static void wr_bvr(int n, uint64_t v)
{
	switch (n) {
	case 0: WR("dbgbvr0_el1", v); break;
	case 1: WR("dbgbvr1_el1", v); break;
	case 2: WR("dbgbvr2_el1", v); break;
	case 3: WR("dbgbvr3_el1", v); break;
	case 4: WR("dbgbvr4_el1", v); break;
	case 5: WR("dbgbvr5_el1", v); break;
	}
}

static void wr_bcr(int n, uint64_t v)
{
	switch (n) {
	case 0: WR("dbgbcr0_el1", v); break;
	case 1: WR("dbgbcr1_el1", v); break;
	case 2: WR("dbgbcr2_el1", v); break;
	case 3: WR("dbgbcr3_el1", v); break;
	case 4: WR("dbgbcr4_el1", v); break;
	case 5: WR("dbgbcr5_el1", v); break;
	}
}

static void wr_wvr(int n, uint64_t v)
{
	switch (n) {
	case 0: WR("dbgwvr0_el1", v); break;
	case 1: WR("dbgwvr1_el1", v); break;
	case 2: WR("dbgwvr2_el1", v); break;
	case 3: WR("dbgwvr3_el1", v); break;
	}
}

static void wr_wcr(int n, uint64_t v)
{
	switch (n) {
	case 0: WR("dbgwcr0_el1", v); break;
	case 1: WR("dbgwcr1_el1", v); break;
	case 2: WR("dbgwcr2_el1", v); break;
	case 3: WR("dbgwcr3_el1", v); break;
	}
}

/* ------------------------------------------------------------------ *
 * MDE / TDE control. */
static void set_mde(int on)
{
	uint64_t v = RD("mdscr_el1");
	if (on)
		v |= (1ull << 15);   /* MDSCR_EL1.MDE: monitor debug enable */
	else
		v &= ~(1ull << 15);
	WR("mdscr_el1", v);
	__asm__ volatile("isb" ::: "memory");
}

static void set_tde(int on)
{
	uint64_t v = RD("mdcr_el2");
	if (on)
		v |= (1ull << 8);    /* MDCR_EL2.TDE: EL1/0 debug exceptions -> EL2 */
	else
		v &= ~(1ull << 8);
	WR("mdcr_el2", v);
	__asm__ volatile("isb" ::: "memory");
}

/* Runtime slot counts, clamped from ID_AA64DFR0_EL1 (BRPs bits[15:12],
 * WRPs bits[23:20]; both stored as count-minus-1). */
static int n_bp = HWBP_MAX_BP;
static int n_wp = HWBP_MAX_WP;

static void hwbp_probe(void)
{
	uint64_t dfr = RD("id_aa64dfr0_el1");
	int brps = (int)((dfr >> 12) & 0xf) + 1;
	int wrps = (int)((dfr >> 20) & 0xf) + 1;

	if (brps < n_bp) n_bp = brps;
	if (wrps < n_wp) n_wp = wrps;
}

static void bc_bitmaps(void)
{
	uint32_t bm = 0, wm = 0;
	int i;
	for (i = 0; i < n_bp; i++) if (bp_en[i]) bm |= (1u << i);
	for (i = 0; i < n_wp; i++) if (wp_en[i]) wm |= (1u << i);
	bc_wr(10, bm);
	bc_wr(11, wm);
}

static void hwbp_init(void)
{
	int i;

	hwbp_probe();
	for (i = 0; i < n_bp; i++) { wr_bcr(i, 0); wr_bvr(i, 0); bp_en[i] = 0; }
	for (i = 0; i < n_wp; i++) { wr_wcr(i, 0); wr_wvr(i, 0); wp_en[i] = 0; }
	__asm__ volatile("isb" ::: "memory");

	bc_wr(0, HWBP_MAGIC);
	bc_wr(1, 0);
	bc_bitmaps();
	hwbp_inited = 1;
}

/* ------------------------------------------------------------------ *
 * DBGBCR / DBGWCR field builders.
 *
 * Breakpoint (unlinked instruction address match, guest Non-secure EL1&EL0):
 *   E=1(bit0), PMC=0b11(bits[2:1]) match EL1&EL0, BAS=0xf(bits[8:5]) A64
 *   instruction, HMC=0(bit13), SSC=0b00(bits[15:14]), BT=0b0000(bits[23:20]).
 *
 * Watchpoint (address match, write, guest Non-secure EL1&EL0):
 *   E=1(bit0), PAC=0b11(bits[2:1]) match EL1&EL0, LSC(bits[4:3]) = 0b10 store
 *   / 0b11 both, BAS=0xff(bits[12:5]) all 8 bytes of the doubleword,
 *   HMC=0(bit13), SSC=0b00(bits[15:14]), MASK=0(bits[28:24]). */
#define BCR_ARM  ((1u << 0) | (0x3u << 1) | (0xfu << 5))
#define WCR_ARM(lsc) ((1u << 0) | (0x3u << 1) | ((uint32_t)(lsc) << 3) | (0xffu << 5))
#define WCR_LSC_STORE 0x2u

int hwbp_set(int idx, uint64_t va, int is_write_wp)
{
	if (!hwbp_inited)
		hwbp_init();

	if (is_write_wp) {
		uint64_t base;

		if (idx < 0 || idx >= n_wp)
			return -1;
		/* DBGWVR is doubleword-aligned; BAS selects bytes within it. Match
		 * the whole 8-byte region containing va (BAS=0xff). */
		base = va & ~7ull;
		wr_wcr(idx, 0);                 /* disable while reprogramming */
		__asm__ volatile("isb" ::: "memory");
		wr_wvr(idx, base);
		wr_wcr(idx, WCR_ARM(WCR_LSC_STORE));
		__asm__ volatile("isb" ::: "memory");
		wp_va[idx] = va;
		wp_en[idx] = 1;
	} else {
		if (idx < 0 || idx >= n_bp)
			return -1;
		wr_bcr(idx, 0);                 /* disable while reprogramming */
		__asm__ volatile("isb" ::: "memory");
		wr_bvr(idx, va & ~3ull);        /* instruction address, bits[1:0]=0 */
		wr_bcr(idx, BCR_ARM);
		__asm__ volatile("isb" ::: "memory");
		bp_va[idx] = va & ~3ull;
		bp_en[idx] = 1;
	}

	set_mde(1);
	set_tde(1);
	bc_bitmaps();
	return 0;
}

int hwbp_clear(int idx, int is_write_wp)
{
	if (!hwbp_inited)
		hwbp_init();

	if (is_write_wp) {
		if (idx < 0 || idx >= n_wp)
			return -1;
		wr_wcr(idx, 0);
		wr_wvr(idx, 0);
		wp_en[idx] = 0;
	} else {
		if (idx < 0 || idx >= n_bp)
			return -1;
		wr_bcr(idx, 0);
		wr_bvr(idx, 0);
		bp_en[idx] = 0;
	}
	__asm__ volatile("isb" ::: "memory");
	bc_bitmaps();
	return 0;
}

void hwbp_clear_all(void)
{
	int i;

	if (!hwbp_inited)
		hwbp_init();

	for (i = 0; i < n_bp; i++) { wr_bcr(i, 0); wr_bvr(i, 0); bp_en[i] = 0; }
	for (i = 0; i < n_wp; i++) { wr_wcr(i, 0); wr_wvr(i, 0); wp_en[i] = 0; }
	set_mde(0);
	__asm__ volatile("isb" ::: "memory");
	bc_bitmaps();
	/* Leave TDE as-is (see the TDE coordination note at the top). */
}

/* ------------------------------------------------------------------ *
 * Debug exception dispatch. */
static uint32_t hwbp_hits;

static void record_hit(struct el2_frame *f, uint64_t esr, int kind, int slot)
{
	uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

	bc_wr(0, HWBP_MAGIC);
	bc_wr(1, ++hwbp_hits);
	bc_wr(2, (uint32_t)kind);
	bc_wr(3, (uint32_t)slot);
	bc_wr(4, (uint32_t)esr);
	bc_wr(5, ec);
	bc_wr(6, (uint32_t)f->elr);
	bc_wr(7, (uint32_t)(f->elr >> 32));
	bc_wr(8, (uint32_t)f->far);
	bc_wr(9, (uint32_t)(f->far >> 32));
	bc_bitmaps();
}

int hwbp_handle(struct el2_frame *frame, uint64_t esr)
{
	uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);
	int i, slot;

	if (ec == 0x30u || ec == 0x31u) {
		/* Breakpoint: ELR_EL2 is the matched (about-to-execute) instruction.
		 * Find which armed slot's VA it is, one-shot-disable so the guest can
		 * step past on eret instead of re-faulting forever. */
		uint64_t pc = frame->elr & ~3ull;

		slot = -1;
		for (i = 0; i < n_bp; i++)
			if (bp_en[i] && bp_va[i] == pc) { slot = i; break; }
		record_hit(frame, esr, 0, slot);
		if (slot >= 0)
			hwbp_clear(slot, 0);
		return 1;
	}

	if (ec == 0x34u || ec == 0x35u) {
		/* Watchpoint: FAR_EL2 is the accessed data address. Match against the
		 * armed doubleword and one-shot-disable. */
		uint64_t da = frame->far & ~7ull;

		slot = -1;
		for (i = 0; i < n_wp; i++)
			if (wp_en[i] && (wp_va[i] & ~7ull) == da) { slot = i; break; }
		record_hit(frame, esr, 1, slot);
		if (slot >= 0)
			hwbp_clear(slot, 1);
		return 1;
	}

	return 0;
}

int hwbp_list_bp(uint64_t *va, int *en)
{
	int i;
	if (!hwbp_inited)
		hwbp_init();
	for (i = 0; i < n_bp; i++) { va[i] = bp_va[i]; en[i] = bp_en[i]; }
	return n_bp;
}

int hwbp_list_wp(uint64_t *va, int *en)
{
	int i;
	if (!hwbp_inited)
		hwbp_init();
	for (i = 0; i < n_wp; i++) { va[i] = wp_va[i]; en[i] = wp_en[i]; }
	return n_wp;
}
