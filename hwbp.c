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
 * 0x50002400, SST1 0x50002800, HDMI 0x50003000). 21 words -> ..0x50000654,
 * still far below the next instrument (dbg 0x50000e00).
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
 *   [12] hit x1 (low 32)              [13] hit x1 high 32
 *   [14] 64-bit word read from the guest at (x1 + HWBP_ARG1_PROBE_OFF),
 *        or 0xDEADBEEF if the guest VA could not be translated
 *                                       [15] that word's high 32
 *   [16] re-arms performed by hwbp_reassert() -- see it for why this exists
 *   [17] DBGBCR0_EL1 as actually read back from the hardware, so the [10]
 *        bitmap (which is only this file's own bookkeeping) can be checked
 *        against the PE instead of believed
 *   [18] MDSCR_EL1  [19] MDCR_EL2  [20] OSLSR_EL1 -- the keep-alive's live
 *        precondition readings, via hwbp_publish_preconditions().
 *
 * Slots [18..20] are here because el2_exc.c used to write those three words to a
 * HARDCODED 0x50000638, which is slots [14],[15],[16] of THIS window. So the
 * "first argument" probe never worked: it always read back MDSCR/MDCR, and on
 * 2026-08-18 that produced a beautifully plausible sgl->dma_map value of
 * 0x0000010600008000 -- which is just (MDCR_EL2 0x106 << 32) | MDSCR_EL1 0x8000.
 * The re-arm counter added the same day landed on OSLSR_EL1 and read "8", which
 * is OSLSR, not a count. Exactly the failure this tree already has a rule
 * against: never derive a window address from a neighbour. Owning the slots
 * instead of squatting them is the fix.
 *
 * Slots [12..15] were reserved; they now carry the FIRST ARGUMENT of the
 * breakpointed function and one word dereferenced from it. Recording ESR/PC/FAR
 * alone is enough to prove a breakpoint fired but not to say anything about WHY
 * -- and the case this was added for needs an argument's contents, not the fact
 * of the call: a guest kernel panic in linux_dma_unmap_sg_attrs() whose faulting
 * dereference is `sgl->dma_map`, with sgl arriving in x1 and dma_map at offset
 * 24 of struct scatterlist.
 *
 * The read is done HERE, on the core that took the hit, deliberately. Guest
 * kernel VAs cannot be translated from CPU1 over the debug channel -- AT S1E1R
 * there uses CPU1's TTBR_EL1, which is not the guest's, and that has produced
 * confidently wrong answers in this project before. On the hit core the guest's
 * translation regime is live, so AT S12E1R resolves the whole stage-1+stage-2
 * walk to a PA that EL2 can then read directly.
 */
#define HWBP_BC_BASE 0x50000600UL
#define HWBP_MAGIC   0x48574250u   /* "HWBP" */

/* Byte offset dereferenced from the breakpointed function's first argument and
 * published in [14]/[15]. 24 = offsetof(struct scatterlist, dma_map) in
 * linuxkpi's scatterlist.h: page_link(8) + offset(4) + length(4) +
 * dma_address(8). Change this when breakpointing something else -- it is
 * deliberately a compile-time constant so the recorded value always has one
 * documented meaning rather than depending on who read it. */
#define HWBP_ARG1_PROBE_OFF 24u

/* Read one 64-bit word from a GUEST virtual address, on the core that is
 * currently executing the guest's translation regime. Returns 0 and sets *ok=0
 * if the address does not translate, rather than faulting EL2 -- a diagnostic
 * must never be able to bring down the thing it is diagnosing. */
static uint64_t hwbp_guest_read64(uint64_t gva, int *ok)
{
	uint64_t par;

	__asm__ volatile("at s12e1r, %0\n\tisb" :: "r"(gva) : "memory");
	__asm__ volatile("mrs %0, par_el1" : "=r"(par));

	if (par & 1u) {              /* PAR_EL1.F: translation failed */
		*ok = 0;
		return 0;
	}
	*ok = 1;
	return *(volatile uint64_t *)((par & 0x0000FFFFFFFFF000ULL) |
	                             (gva & 0xFFFULL));
}

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
/* The control value ACTUALLY programmed, so hwbp_reassert() can restore a slot
 * byte-for-byte instead of guessing a constant. Not cosmetic: a watchpoint armed
 * by hwbp_set_wp_el2() uses WCR_ARM_EL2 (PMC=0b10, matches EL2) while a guest
 * watchpoint uses WCR_ARM (PMC=0b11) -- re-arming an EL2 self-watch with the
 * guest constant would silently retarget it at the wrong EL and quietly stop
 * answering the question it was armed to answer. */
static uint32_t bp_bcr[HWBP_MAX_BP];
static uint32_t wp_wcr[HWBP_MAX_WP];

static int hwbp_inited;

/* ------------------------------------------------------------------ *
 * Debug-register access. The register NAME must be a literal, so switch-arm
 * every implemented index. Only DBG{B,W}{V,C}R<0..max>_EL1 are touched. */

#define WR(reg, v) __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)) : "memory")
#define RD(reg)    ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })

static uint64_t rd_bcr(int n)
{
	switch (n) {
	case 0:  return RD("dbgbcr0_el1");
	case 1:  return RD("dbgbcr1_el1");
	case 2:  return RD("dbgbcr2_el1");
	case 3:  return RD("dbgbcr3_el1");
	case 4:  return RD("dbgbcr4_el1");
	case 5:  return RD("dbgbcr5_el1");
	default: return 0;
	}
}

static uint64_t rd_wcr(int n)
{
	switch (n) {
	case 0:  return RD("dbgwcr0_el1");
	case 1:  return RD("dbgwcr1_el1");
	case 2:  return RD("dbgwcr2_el1");
	case 3:  return RD("dbgwcr3_el1");
	default: return 0;
	}
}

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

/* Re-arm every slot this file believes is armed, and publish what the hardware
 * actually says.
 *
 * THE FIFTH BARRIER. The four documented ones (wrong core, MDSCR_EL1.MDE,
 * MDCR_EL2.TDE, OSLSR_EL1.OSLK) are all necessary and none of them is this:
 * FreeBSD's own dbg_monitor_init() zeroes DBGBCR/DBGBVR for every breakpoint
 * and watchpoint slot as it brings each CPU up. It clears MDSCR_EL1.MDE too,
 * which is why the keep-alive in el2_exc.c already re-asserts that -- but the
 * SLOT REGISTERS were never re-asserted, so the guest quietly disarmed us while
 * bc[10] went on reporting "armed" from this file's own bookkeeping.
 *
 * Measured 2026-08-18: a breakpoint on linux_dma_unmap_sg_attrs, address
 * verified against the deployed kernel's symbol table, MDE=1 / TDE=1 / OSLK=0
 * all confirmed live, bitmap 0x1 -- and ZERO hits across a run that
 * demonstrably executed that function (the guest panicked at
 * linux_dma_unmap_sg_attrs+0xac).
 *
 * bc[17] exists because of that: "armed" must be readable from the PE, not
 * inferred from a variable that cannot know the guest overwrote the register.
 * Runs on the guest's core only -- DBGB*_EL1 are banked, so doing this anywhere
 * else would write somebody else's bank and prove nothing. */
void hwbp_reassert(void)
{
	static uint32_t reasserts;
	int did = 0;
	int i;

	/* Bound by BOTH: n_bp is derived at runtime from ID_AA64DFR0_EL1 and the
	 * compiler cannot prove hwbp_probe() only ever lowers it, so indexing the
	 * shadow arrays on n_bp alone is an out-of-bounds warning that is right to
	 * fire -- a future probe change really could raise it. */
	for (i = 0; i < n_bp && i < HWBP_MAX_BP; i++) {
		if (!bp_en[i])
			continue;
		if ((rd_bcr(i) & 1u) == 0u) {      /* E bit gone: guest cleared it */
			wr_bvr(i, bp_va[i]);
			wr_bcr(i, bp_bcr[i]);
			did = 1;
		}
	}
	for (i = 0; i < n_wp && i < HWBP_MAX_WP; i++) {
		if (!wp_en[i])
			continue;
		if ((rd_wcr(i) & 1u) == 0u) {
			/* DBGWVR is doubleword-aligned and wp_va[] keeps the
			 * caller's UNALIGNED va (bpl prints it), so re-apply the
			 * same mask the arming path used. Writing the raw value
			 * back would be CONSTRAINED UNPREDICTABLE. */
			wr_wvr(i, wp_va[i] & ~7ull);
			wr_wcr(i, wp_wcr[i]);
			did = 1;
		}
	}
	if (did) {
		__asm__ volatile("isb" ::: "memory");
		bc_wr(16, ++reasserts);
	}
	bc_wr(17, (uint32_t)rd_bcr(0));
}

/* Publish the keep-alive's precondition readings as OWNED slots. Replaces
 * el2_exc.c writing them to a hardcoded address that turned out to be the middle
 * of this window -- see the [18..20] note above for what that cost. */
void hwbp_publish_preconditions(uint64_t mdscr, uint64_t mdcr, uint64_t oslsr)
{
	bc_wr(18, (uint32_t)mdscr);
	bc_wr(19, (uint32_t)mdcr);
	bc_wr(20, (uint32_t)oslsr);
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
	/* Publish EVERY hit-record slot as a real zero, not just the hit count.
	 *
	 * Learned the hard way on this very instrument: slots [12..15] were added
	 * for the argument capture and left unwritten until a hit. This window
	 * survives a warm reset, so a fresh boot with ZERO hits read back a PC, an
	 * x1 and a dereferenced word left over from an experiment fifteen days
	 * earlier -- values that look exactly like data and mean nothing. Only
	 * slot [1] was being cleared, which made the hit COUNT trustworthy while
	 * everything it described stayed stale.
	 *
	 * Same trap the vblk and EBIO lanes were fixed for: a reader cannot tell
	 * "never happened" from "this build has no such field". A diagnostic that
	 * can be misread as evidence is worse than no diagnostic. */
	for (int z = 1; z <= 20; z++)
		bc_wr(z, 0);
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

/* SELF-WATCH (EL2) variant — "which of OUR OWN instructions wrote this
 * address?". The guest-facing builder above can never answer that: PAC=0b11 +
 * HMC=0 matches Non-secure EL1&EL0 only, so an EL2 store sails straight past
 * it. Per the ARM ARM's (SSC, HMC, PAC) selection table, EL2 needs
 * HMC=1(bit13) + PAC=0b10(bits[2:1]) + SSC=0b00(bits[15:14]).
 *
 * Written for a concrete hunt (2026-08-01): four breadcrumb windows kept being
 * refilled with guest console text after being zeroed, and a tree-wide grep
 * found no writer — nothing outside vconsole.c even computes that address. At
 * that point guessing had already produced three wrong answers, so the honest
 * move was to stop reasoning about who COULD write it and let the hardware name
 * the instruction. A hit lands as a current-EL sync exception, which el2_trap()
 * already funnels into flightrec (FLTR_K_FAULT, a0=ESR a1=ELR) and advances
 * ELR past — so the ELR in that record IS the culprit, resolvable with
 * `aarch64-linux-gnu-addr2line -f -e microkernel-dbg.elf <elr>`. */
#define WCR_ARM_EL2(lsc) ((1u << 0) | (0x2u << 1) | ((uint32_t)(lsc) << 3) | \
                          (0xffu << 5) | (1u << 13))

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
		wp_wcr[idx] = WCR_ARM(WCR_LSC_STORE);
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
		bp_bcr[idx] = BCR_ARM;
		bp_en[idx] = 1;
	}

	set_mde(1);
	set_tde(1);
	bc_bitmaps();
	return 0;
}

/* Arm a WRITE watchpoint that matches EL2 (our own) stores. See WCR_ARM_EL2.
 * Deliberately a separate entry point rather than a flag on hwbp_set(): every
 * existing caller means "watch the guest", and silently changing what they
 * match would be the kind of quiet behaviour swap this tree keeps getting
 * bitten by. Returns 0 on success, -1 on a bad index. */
int hwbp_set_wp_el2(int idx, uint64_t va)
{
	uint64_t base;

	if (!hwbp_inited)
		hwbp_init();
	if (idx < 0 || idx >= n_wp)
		return -1;

	base = va & ~7ull;                  /* DBGWVR is doubleword-aligned */
	wr_wcr(idx, 0);
	__asm__ volatile("isb" ::: "memory");
	wr_wvr(idx, base);
	wr_wcr(idx, WCR_ARM_EL2(WCR_LSC_STORE));
	wp_wcr[idx] = WCR_ARM_EL2(WCR_LSC_STORE);
	__asm__ volatile("isb" ::: "memory");
	wp_va[idx] = va;
	wp_en[idx] = 1;

	/* MDE enables breakpoints/watchpoints at all. TDE is about routing EL1/EL0
	 * debug exceptions to EL2 and is irrelevant for an EL2-taken one, but
	 * hwbp_set() sets it and leaving the module's state consistent avoids a
	 * surprise for whoever arms a guest watchpoint next. */
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

	/* First argument and one word dereferenced from it -- see the map above. */
	bc_wr(12, (uint32_t)f->x[1]);
	bc_wr(13, (uint32_t)(f->x[1] >> 32));
	{
		int ok = 0;
		uint64_t w = hwbp_guest_read64(f->x[1] + HWBP_ARG1_PROBE_OFF, &ok);
		bc_wr(14, ok ? (uint32_t)w : 0xDEADBEEFu);
		bc_wr(15, ok ? (uint32_t)(w >> 32) : 0xDEADBEEFu);
	}

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
		if (slot >= 0) {
#if defined(GUEST_BP_ADDR) && (GUEST_BP_ADDR)
			/* SAMPLING mode, for -DGUEST_BP_ADDR investigations only.
			 *
			 * A one-shot breakpoint answers "was this function ever
			 * reached". It cannot answer "what were the arguments of the
			 * call that CRASHED", because the crash is generally not the
			 * first call: on 2026-08-18 the breakpoint on
			 * linux_dma_unmap_sg_attrs caught call #1, which had a
			 * perfectly valid sgl->dma_map, while the panic happens on a
			 * later call with far=0x1.
			 *
			 * So clear only the HARDWARE enable -- letting the guest eret
			 * past the instruction instead of re-faulting forever -- and
			 * leave bp_en set, so hwbp_reassert() puts the slot back on
			 * the next guest trap. record_hit() overwrites, so the window
			 * ends up holding the MOST RECENT call, which is the one that
			 * matters when the guest then dies.
			 *
			 * HONEST LIMITATION: this is sampling, not tracing. Re-arming
			 * is deferred to the next lower-EL sync trap (it MUST be --
			 * re-arming inside this handler would re-fault on the same
			 * instruction immediately), so calls that happen before the
			 * guest next traps are missed entirely. The hit count is
			 * therefore a lower bound, never a call count. It is enough
			 * for "what did the last call look like", which is the
			 * question, and it is not enough for anything quantitative.
			 *
			 * Safe against the re-fault loop only because el2_exc.c runs
			 * the keep-alive BEFORE hwbp_handle(): by the time the slot is
			 * put back, this trap's clear has already happened. */
			wr_bcr(slot, 0);
			__asm__ volatile("isb" ::: "memory");
			bc_wr(17, (uint32_t)rd_bcr(0));
#else
			hwbp_clear(slot, 0);
#endif
		}
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
