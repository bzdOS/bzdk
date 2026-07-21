/* snapshot.c — guest checkpoint / restore implementation (SKELETON).
 *
 * See snapshot.h for the API contract and the full inventory of what is /
 * is not captured, and docs/snapshot-restore-design.md for the rationale.
 *
 * This is a compilable-looking skeleton: the MRS/MSR sysreg save/restore is
 * real and, for the encodings used elsewhere in this tree (cmd_sr / cmd_sr2 /
 * guest.c), can be trusted; anything that needs on-board verification of a bit
 * layout or a sequencing subtlety is marked TODO(board). The DRAM copy loop
 * and the coherent header commit are complete.
 *
 * Freestanding: <stdint.h> only, -mgeneral-regs-only, EL2, flat U-Boot map on.
 */
#include <stdint.h>
#include "snapshot.h"
#include "exceptions.h"

/* ------------------------------------------------------------------ *
 * MRS/MSR helpers — same idiom as dbgmon.c's RDSYSREG / cmd_sw and guest.c.
 * A read is pure; a write is followed by ISB only where the change must take
 * effect before the next dependent instruction (MMU/vector/timer control).
 * ------------------------------------------------------------------ */
#define RD(reg)      ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })
#define WR(reg, v)   __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)) : "memory")
#define WR_ISB(reg, v) \
	__asm__ volatile("msr " reg ", %0\n\tisb" :: "r"((uint64_t)(v)) : "memory")

/* Coherent 32-bit store into the store header, matching every breadcrumb
 * writer in this tree: dc civac + dsb sy so the record is visible to a host
 * physical-memory dump and survives a warm WDT reset with the D-cache on. */
static inline void snap_wr32(volatile uint32_t *p, uint32_t v)
{
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ================================================================== *
 * SYSREG CAPTURE / RELOAD
 * ================================================================== */

/* Read the full EL1 + per-guest EL2 sysreg set into `s`. Runs at EL2, so every
 * EL1 register is a distinct banked register (not the live one) and a plain MRS
 * reads exactly the guest's value — this is the SAME assumption cmd_sr() relies
 * on to dump the guest's live EL1 state. */
static void sysregs_capture(struct snapshot_sysregs *s)
{
	/* MMU / translation */
	s->sctlr_el1     = RD("sctlr_el1");
	s->ttbr0_el1     = RD("ttbr0_el1");
	s->ttbr1_el1     = RD("ttbr1_el1");
	s->tcr_el1       = RD("tcr_el1");
	s->mair_el1      = RD("mair_el1");
	s->amair_el1     = RD("amair_el1");
	s->contextidr_el1= RD("contextidr_el1");

	/* Exception / vector state */
	s->vbar_el1      = RD("vbar_el1");
	s->esr_el1       = RD("esr_el1");
	s->far_el1       = RD("far_el1");
	s->elr_el1       = RD("elr_el1");
	s->spsr_el1      = RD("spsr_el1");
	s->sp_el0        = RD("sp_el0");
	s->sp_el1        = RD("sp_el1");

	/* Thread pointers */
	s->tpidr_el0     = RD("tpidr_el0");
	s->tpidrro_el0   = RD("tpidrro_el0");
	s->tpidr_el1     = RD("tpidr_el1");

	/* EL0 access / feature enables */
	s->cpacr_el1     = RD("cpacr_el1");
	s->mdscr_el1     = RD("mdscr_el1");
	s->pmcr_el0      = RD("pmcr_el0");

	/* Generic timer (skew-sensitive) */
	s->cntkctl_el1   = RD("cntkctl_el1");
	s->cntp_ctl_el0  = RD("cntp_ctl_el0");
	s->cntp_cval_el0 = RD("cntp_cval_el0");
	s->cntv_ctl_el0  = RD("cntv_ctl_el0");
	s->cntv_cval_el0 = RD("cntv_cval_el0");

	/* EL2-side per-guest translation + trap routing */
	s->vttbr_el2     = RD("vttbr_el2");
	s->vtcr_el2      = RD("vtcr_el2");
	s->hcr_el2       = RD("hcr_el2");
	s->cntvoff_el2   = RD("cntvoff_el2");
}

/* Reload the full sysreg set from `s`. Order matters:
 *   1. Program stage-2 (VTTBR/VTCR) + HCR BEFORE we touch guest MMU regs, so
 *      the guest's IPA->PA regime is in place.
 *   2. Reload EL1 translation regs with the MMU effectively frozen (we are at
 *      EL2; the guest MMU only re-engages on eret), then a full TLB + I-cache
 *      invalidate so no stale guest translation or stale instruction survives.
 *   3. Timer regs LAST, after re-basing CNTVOFF (see snapshot_restore()).
 * Each group ends with the barriers the architecture requires. */
static void sysregs_reload(const struct snapshot_sysregs *s)
{
	/* --- EL2 per-guest translation / trap routing FIRST --- */
	WR("vttbr_el2", s->vttbr_el2);
	WR("vtcr_el2",  s->vtcr_el2);
	WR("hcr_el2",   s->hcr_el2);
	WR("cntvoff_el2", s->cntvoff_el2);   /* provisional; re-based below */
	__asm__ volatile("isb" ::: "memory");

	/* --- EL1 MMU / translation --- */
	WR("mair_el1",       s->mair_el1);
	WR("amair_el1",      s->amair_el1);
	WR("tcr_el1",        s->tcr_el1);
	WR("ttbr0_el1",      s->ttbr0_el1);
	WR("ttbr1_el1",      s->ttbr1_el1);
	WR("contextidr_el1", s->contextidr_el1);
	/* SCTLR_EL1 carries M/C/I: write it last of the MMU group so the guest's
	 * MMU-enable bit is only asserted once its tables/attrs are in place. It
	 * does not take effect until eret anyway (we are at EL2), but keep the
	 * dependency order explicit. */
	WR_ISB("sctlr_el1", s->sctlr_el1);

	/* --- Exception / vector state --- */
	WR("vbar_el1",  s->vbar_el1);
	WR("esr_el1",   s->esr_el1);
	WR("far_el1",   s->far_el1);
	WR("elr_el1",   s->elr_el1);
	WR("spsr_el1",  s->spsr_el1);
	WR("sp_el0",    s->sp_el0);
	WR("sp_el1",    s->sp_el1);

	/* --- Thread pointers --- */
	WR("tpidr_el0",   s->tpidr_el0);
	WR("tpidrro_el0", s->tpidrro_el0);
	WR("tpidr_el1",   s->tpidr_el1);

	/* --- EL0 access / feature enables --- */
	WR("cpacr_el1", s->cpacr_el1);
	WR("mdscr_el1", s->mdscr_el1);
	/* PMCR_EL0: writing it may need MDCR_EL2/PMU trap bits clear.
	 * TODO(board): confirm a plain MSR here does not trap under our MDCR_EL2. */
	WR("pmcr_el0",  s->pmcr_el0);

	/* --- Generic timer LAST (compare values re-based by caller) --- */
	WR("cntkctl_el1",   s->cntkctl_el1);
	WR("cntp_cval_el0", s->cntp_cval_el0);
	WR("cntv_cval_el0", s->cntv_cval_el0);
	WR("cntp_ctl_el0",  s->cntp_ctl_el0);
	WR("cntv_ctl_el0",  s->cntv_ctl_el0);

	__asm__ volatile("isb" ::: "memory");
}

/* Post-reload cache/TLB maintenance so nothing from the pre-restore guest
 * survives. We changed guest DRAM contents AND its translation regime, so:
 *   - invalidate the entire guest stage-1 + stage-2 TLB (VMALLE1IS covers the
 *     current VMID's EL1&0 stage-1; the stage-2 base changed too, so also
 *     nuke stage-2 for this VMID),
 *   - invalidate the instruction cache (we rewrote code pages in DRAM),
 *   - the DRAM copy loop already cleaned data lines to PoC as it wrote.
 * TODO(board): confirm VMID handling — if VTTBR_EL2.VMID differs pre/post
 * restore, an ALLE1 (all VMIDs) is the safe hammer. */
static void restore_maintenance(void)
{
	__asm__ volatile(
		"dsb ish\n"
		"tlbi vmalle1is\n"   /* guest EL1&0 stage-1, inner-shareable */
		"tlbi alle1is\n"     /* + stage-2 for EL1 (all VMIDs, safe)  */
		"dsb ish\n"
		"ic ialluis\n"       /* I-cache: we rewrote guest .text      */
		"dsb ish\n"
		"isb\n"
		::: "memory");
}

/* ================================================================== *
 * DRAM COPY
 * ================================================================== */

/* Copy `bytes` from `src_pa` to `dst_pa` as 64-bit words, cleaning each
 * destination line to the Point of Coherency so the copy is visible to a host
 * memory dump and to the guest after eret (guest may run with caches off early,
 * FreeBSD runs with them on — either way PoC-clean is correct). Both addresses
 * are flat-mapped PAs; the loop is straight-line, no allocation, restartable.
 *
 * 1 GiB / 8 = 134,217,728 iterations. At EL2 with the D-cache on this is the
 * dominant cost of a snapshot; see design doc "Performance" for the
 * dc-cvac-per-line vs bulk-clean tradeoff and the <1 s budget. */
static void dram_copy(uint64_t dst_pa, uint64_t src_pa, uint64_t bytes)
{
	volatile uint64_t *d = (volatile uint64_t *)dst_pa;
	volatile uint64_t *s = (volatile uint64_t *)src_pa;
	uint64_t n = bytes / 8u;
	uint64_t i;

	for (i = 0; i < n; i++) {
		d[i] = s[i];
		/* Clean one cache line every 8 words (64-byte line on A53). Cleaning
		 * per-line rather than per-word keeps this ~8x cheaper. */
		if ((i & 7u) == 7u)
			__asm__ volatile("dc cvac, %0" :: "r"(&d[i]) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ================================================================== *
 * PUBLIC API
 * ================================================================== */

static volatile struct snapshot_hdr *snap_hdr(void)
{
	return (volatile struct snapshot_hdr *)SNAP_STORE_BASE;
}

int snapshot_present(void)
{
	volatile struct snapshot_hdr *h = snap_hdr();
	return (h->magic == SNAP_MAGIC && h->valid == 1u &&
	        h->version == SNAPSHOT_VERSION);
}

int snapshot_save(const struct el2_frame *frame)
{
	volatile struct snapshot_hdr *h = snap_hdr();

	if (!frame)
		return -1;

	/* PRECONDITION (caller-enforced, see design): the guest is quiesced at
	 * this trap boundary, secondaries are parked, and no device DMA is in
	 * flight. We cannot assert that from here; it is a wiring contract. */

	/* Mark invalid FIRST so a mid-write crash leaves an obviously-partial
	 * record rather than a plausible-but-torn one. */
	snap_wr32(&h->valid, 0u);
	snap_wr32(&h->magic, SNAP_MAGIC);
	snap_wr32(&h->version, SNAPSHOT_VERSION);
	snap_wr32(&h->store_kind, (uint32_t)SNAP_STORE_DRAM);

	/* Metadata: DRAM geometry + the physical counter at save time (used to
	 * re-base the timer on restore). */
	h->dram_base    = SNAP_DRAM_BASE;
	h->dram_size    = SNAP_DRAM_SIZE;
	h->dram_store   = SNAP_DRAM_STORE;
	h->taken_cntpct = RD("cntpct_el0");

	/* Architectural state: the trap frame verbatim (x0..x30, ELR_EL2 = guest
	 * PC, SPSR_EL2 = guest PSTATE) + the full EL1/EL2 sysreg set. */
	h->frame = *frame;
	{
		struct snapshot_sysregs sr;
		sysregs_capture(&sr);
		h->sysregs = sr;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* Guest DRAM -> store. This is the long pole. */
	dram_copy(SNAP_DRAM_STORE, SNAP_DRAM_BASE, SNAP_DRAM_SIZE);

	/* TODO(board): CRC over the DRAM copy for integrity. Left 0 in skeleton. */
	h->crc32 = 0u;

	/* Commit: publish the whole header (already coherent word-by-word) then
	 * flip valid LAST, with a barrier, so a reader never sees valid=1 over a
	 * torn body. Same discipline as every breadcrumb committer here. */
	__asm__ volatile("dsb sy" ::: "memory");
	snap_wr32(&h->valid, 1u);

	/*
	 * --- ALTERNATE STORE BACK-ENDS (design doc, not wired in skeleton) ---
	 * SNAP_STORE_EMMC: replace dram_copy() with a loop of emmc_bio_write(lba,
	 *   SNAP_DRAM_BASE + i*512) over 1 GiB / 512 = 2,097,152 blocks, header in
	 *   the first blocks. Survives power loss; MUCH slower (see design).
	 * SNAP_STORE_EMAC: stream header + DRAM out the dbgmon EMAC channel to a
	 *   host-side file; restore reverses it via write_word bulk. Needs the
	 *   CPU1 debug core to own EMAC (dbg_core_active).
	 */
	return 0;
}

int snapshot_restore(struct el2_frame *frame)
{
	volatile struct snapshot_hdr *h = snap_hdr();
	uint64_t now_cntpct, delta;

	if (!frame)
		return -1;
	if (!snapshot_present())
		return -2;

	/* PRECONDITION (caller-enforced): secondary cores parked (design "SMP"),
	 * guest quiesced at this trap boundary. */

	/* 1. Reload guest DRAM from the store. Do this BEFORE sysregs so that when
	 *    the MMU regime comes back the memory it translates already holds the
	 *    snapshot contents. */
	dram_copy(SNAP_DRAM_BASE, SNAP_DRAM_STORE, h->dram_size);

	/* 2. Reload the EL1 + per-guest EL2 sysreg set. */
	{
		struct snapshot_sysregs sr = h->sysregs;   /* copy out of volatile store */

		/* 3. TIMER RE-BASE (see design "timer skew"): the physical counter has
		 *    advanced by (now - taken) since the snapshot. The saved compare
		 *    values (CNTP_CVAL/CNTV_CVAL) are ABSOLUTE counts and would now be
		 *    far in the past -> the guest would take an immediate storm of
		 *    "already expired" timer interrupts, or (worse) jump its notion of
		 *    time forward by minutes. We hide the elapsed real time from the
		 *    guest by pushing CNTVOFF_EL2 back by the same delta, so the
		 *    guest's virtual counter reads the value it had at snapshot time.
		 *    TODO(board): FreeBSD arm64 uses the VIRTUAL timer via CNTVOFF; if
		 *    a build uses the physical timer this delta must instead be added
		 *    to the saved CNTP_CVAL. Confirm which on silicon. */
		now_cntpct = RD("cntpct_el0");
		delta      = now_cntpct - h->taken_cntpct;
		sr.cntvoff_el2 = sr.cntvoff_el2 - delta;

		sysregs_reload(&sr);
	}

	/* 4. Cache / TLB maintenance: we changed both DRAM and the translation
	 *    regime; drop all stale translations and stale I-cache. */
	restore_maintenance();

	/* 5. Rewrite the live trap frame in place. When el2_trap returns,
	 *    el2_common restores x0..x30 from this frame and erets using
	 *    frame->elr (-> ELR_EL2) and frame->spsr (-> SPSR_EL2). Overwriting
	 *    them here is exactly how we "jump into" the restored guest: the CPU
	 *    lands at the snapshot PC/PSTATE with the snapshot GP registers. */
	*frame = h->frame;

	/* NB: SP_EL1/SP_EL0 were already reloaded in sysregs_reload(); they are
	 * banked and NOT part of the eret-restored frame, so they must be (and
	 * are) restored via MSR, not through `frame`. */

	return 0;
}
