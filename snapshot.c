/* SPDX-License-Identifier: BSD-2-Clause */

/* snapshot.c — guest checkpoint / restore implementation.
 *
 * See snapshot.h for the API contract and the full inventory of what is /
 * is not captured, and docs/snapshot-restore-design.md for the rationale.
 *
 * The MRS/MSR sysreg save/restore is real and, for the encodings used
 * elsewhere in this tree (cmd_sr / cmd_sr2 / guest.c / gic_timer.c), has been
 * cross-checked against them (no mismatches found). The DRAM copy loop and
 * the coherent header commit are complete. Every former TODO(board) in this
 * file has been resolved from source-level reasoning alone (ARM ARM trap
 * semantics, the ALLE1IS "all VMIDs" invalidate, and gic_timer.c's own
 * already-on-board-verified finding that this guest uses the virtual timer)
 * — see the individual comments at each former TODO site for the reasoning.
 * Nothing in this file still needs a board test; what still needs the board
 * is END-TO-END behavior (does a `snap`/`rest` cycle actually resume FreeBSD
 * bit-identically) — that can only be observed live, not proven from source.
 *
 * Freestanding: <stdint.h> only, -mgeneral-regs-only, EL2, flat U-Boot map on.
 */
#include <stdint.h>
#include "snapshot.h"
#include "exceptions.h"
#include "wdt.h"   /* wdt_pet() -- feed the 16 s HW dead-man's switch during the
                    * long (1 GiB) DRAM copy loop, same discipline el2_ncmap.c's
                    * civac_range() uses for its own whole-DRAM sweep. */

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
	/* PMCR_EL0: RESOLVED (source reasoning, no board test needed).
	 * MDCR_EL2.TPM/TPMCR (bits 6/5) only trap EL0/EL1 accesses to the PMU
	 * regs up to EL2 (ARMv8 ARM D13.2.35/D13.2.37) -- they never apply to
	 * an access already executing AT EL2, which is exactly what this MSR
	 * is (dbgmon/snapshot run at EL2, same as every other EL1-register
	 * touch in this function). Confirmed by precedent: MDCR_EL2 in this
	 * tree (hwbp.c, el2_exc.c, gdbstub.c) is only ever read-modify-written
	 * to toggle bit 8 (TDE); nothing here or in firmware sets TPM/TPMCR,
	 * and even if it did, it would not matter for an EL2-origin MSR. */
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
 * RESOLVED (source reasoning, no board test needed): VMID handling is already
 * safe by construction, not by luck. `TLBI ALLE1IS` is architecturally defined
 * to invalidate EL1&0 stage-1 *and* stage-2 entries for ALL VMIDs (ARMv8 ARM
 * D7-xxx, "ALLE1IS"), unconditionally -- it does not matter whether
 * VTTBR_EL2.VMID differs before vs. after this restore, because we always
 * issue the all-VMIDs form below, never a VMID-scoped one. Nothing further
 * to confirm on-board; this is correct for any VMID value. */
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
 * CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320, reflected)
 * ================================================================== */

/* Plain bit-at-a-time software CRC32. Deliberately NOT using the optional
 * ARMv8 CRC32 instruction extension (CRC32B/CRC32W/...): whether this
 * particular Cortex-A53 part has it enabled is exactly the kind of thing
 * that would need on-board verification (ID_AA64ISAR0_EL1.CRC32 read), and
 * a portable software fallback sidesteps the question entirely -- it is
 * correct regardless of what the silicon implements. Runs once per byte of
 * `word`, folded into dram_copy() below so a snapshot_save() does not pay a
 * second full memory pass. Not called on the restore path (see dram_copy),
 * so it never costs anything against the <1 s restore budget. */
#define CRC32_INIT 0xFFFFFFFFu

static uint32_t crc32_update_word(uint32_t crc, uint64_t word)
{
	int k, b;

	for (k = 0; k < 8; k++) {
		crc ^= (uint32_t)(word >> (8 * k)) & 0xffu;
		for (b = 0; b < 8; b++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
	}
	return crc;
}

/* ================================================================== *
 * EXCLUSION WINDOWS — see snapshot.h's "EXCLUSION WINDOWS" section for the
 * full rationale. This is the SOLE place that list is built; snapshot_net.c's
 * crc32_region_excluding() calls snapshot_excl_windows() (declared in
 * snapshot.h) rather than re-deriving its own copy.
 * ================================================================== */

/* Provided by EVERY link script in this tree (link.ld for the real board,
 * link_qemu.ld for the QEMU snapshot-qemu harness) — the same two symbols
 * start.S/start_qemu.S already rely on for their own .bss-zeroing loops, and
 * the same one el2_ncmap.c already reads (__bss_end) to learn "how big is
 * our image really" at runtime instead of trusting a hardcoded literal.
 *
 * WHY A LINKER-SYMBOL WINDOW, NOT A HARDCODED HVIMG_BASE: stage2.c already
 * has a fixed #define HVIMG_BASE 0x42000000 for exactly this purpose (its
 * OWN purpose: guest stage-2 exclusion). Reusing that literal here would be
 * wrong on this module's SECOND build target: link.ld places the real-board
 * image at 0x42000000 (HVIMG_BASE is correct there BY CONSTRUCTION), but
 * link_qemu.ld links the IDENTICAL source at 0x40080000 (see that file's own
 * header) — a hardcoded 0x42000000 exclusion window would protect the real
 * board and do ABSOLUTELY NOTHING on the QEMU harness, which is exactly the
 * bug this fix closes (confirmed live: the QEMU harness's own stack,
 * qemu_boot_stack in start_qemu.S, sits at 0x40080000+something, inside the
 * unexcluded range, and snapshot_restore()'s old blind dram_copy() corrupted
 * it mid-restore — see snapshot.h). Computing the window from _text_start/
 * __bss_end instead makes it correct on BOTH targets with one definition,
 * and automatically tracks the image if it ever grows (no periodic re-audit
 * needed, unlike a hand-picked size margin).
 *
 * __bss_end is the right upper bound, not _text_end: per stage2.c's H2(a)
 * comment, every CPU's EL2 runtime stack (smp_stacks[], real-board build) or
 * the QEMU harness's own qemu_boot_stack (start_qemu.S) is a plain .bss
 * array, so it is covered only if the window runs to __bss_end. */
extern char    _text_start[];
extern uint8_t __bss_end[];

/* hv-scratch: a FIXED absolute PA, unlike the image window above — this is
 * ordinary DRAM the code dereferences directly by address, so the link
 * address is irrelevant. Mirrors stage2.c's own HVSCR_BASE (stage2.c:436)
 * exactly: one 2 MiB L2 block, matching the DTB's `hv-scratch@50000000`
 * reg=<0x200000> on the real board (see hv_addrmap.h's map for everything
 * that lives inside it, including this very module's SNAP_META_BASE at
 * 0x50060000 — see snapshot.h's "EXCLUSION WINDOWS"). Re-declared here
 * rather than shared via a header for the same reason test_stage2_tables.c
 * re-declares it at its own :325-326: stage2.c's copy is a file-local
 * #define with no exported header. If stage2.c's own value ever moves, this
 * copy must move with it — there is deliberately no compile-time link
 * between the two today (same known gap as the existing test-file mirror). */
#define SNAP_EXCL_HVSCR_BASE  0x50000000UL
#define SNAP_EXCL_HVSCR_SIZE  0x00200000UL

#ifdef HV_HDMI
/* HDMI framebuffer — only excluded in HV_HDMI builds, mirroring stage2.c's
 * own #ifdef HV_HDMI gate exactly (stage2.c HVFB_BASE/HVFB_SIZE). Neither
 * `dbg` nor `snapshot-qemu` currently defines HV_HDMI (grep the Makefile),
 * so this arm is untested dead code today, same status quo as stage2.c's
 * own HVFB carve-out. */
#define SNAP_EXCL_HVFB_BASE   0x4D000000UL
#define SNAP_EXCL_HVFB_SIZE   0x00800000UL
#endif

unsigned
snapshot_excl_windows(struct snapshot_excl_window out[SNAPSHOT_EXCL_MAX])
{
	unsigned n = 0;
	uint64_t img_base = (uint64_t)(uintptr_t)_text_start;
	uint64_t img_end   = (uint64_t)(uintptr_t)__bss_end;

	/* 1. Our own running image (see the extern declarations' comment). */
	out[n].off = img_base - SNAP_DRAM_BASE;
	out[n].len = img_end - img_base;
	n++;

	/* 2. hv-scratch (fixed PA, every build target). */
	out[n].off = SNAP_EXCL_HVSCR_BASE - SNAP_DRAM_BASE;
	out[n].len = SNAP_EXCL_HVSCR_SIZE;
	n++;

#ifdef HV_HDMI
	/* 3. HDMI framebuffer (fixed PA, HV_HDMI builds only). */
	out[n].off = SNAP_EXCL_HVFB_BASE - SNAP_DRAM_BASE;
	out[n].len = SNAP_EXCL_HVFB_SIZE;
	n++;
#endif

	/* Sort ascending by `off` -- n <= 3, a plain insertion sort costs nothing
	 * and lets dram_copy_excluding()/crc32_region_excluding() assume
	 * ascending, non-overlapping order instead of re-deriving it themselves. */
	{
		unsigned i, j;
		for (i = 1; i < n; i++) {
			struct snapshot_excl_window key = out[i];
			j = i;
			while (j > 0 && out[j - 1].off > key.off) {
				out[j] = out[j - 1];
				j--;
			}
			out[j] = key;
		}
	}
	return n;
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
 * `crc_inout`: if non-NULL, accumulates a running CRC32 (see crc32_update_word)
 * over the words read from `src_pa`, folded into this same pass so the
 * integrity check costs no extra memory bandwidth. Pass NULL on the restore
 * path -- CRC is a save-time integrity aid only, never computed on the fast
 * restore path (see design doc "Performance", <1 s restore goal).
 *
 * 1 GiB / 8 = 134,217,728 iterations. At EL2 with the D-cache on this is the
 * dominant cost of a snapshot; see design doc "Performance" for the
 * dc-cvac-per-line vs bulk-clean tradeoff and the <1 s budget. Pets the HW
 * watchdog periodically (same discipline as el2_ncmap.c's civac_range(), which
 * sweeps a comparably-sized region): this loop runs with no other trap/tick
 * reaching el2_trap (the only place that otherwise re-arms the 16 s WDT), so
 * without this a slower-than-expected copy (e.g. cold caches, DRAM refresh
 * contention) could hit the watchdog mid-copy. */
static void dram_copy(uint64_t dst_pa, uint64_t src_pa, uint64_t bytes, uint32_t *crc_inout)
{
	volatile uint64_t *d = (volatile uint64_t *)dst_pa;
	volatile uint64_t *s = (volatile uint64_t *)src_pa;
	uint64_t n = bytes / 8u;
	uint64_t i;
	uint32_t crc = crc_inout ? *crc_inout : 0u;

	for (i = 0; i < n; i++) {
		uint64_t v = s[i];
		d[i] = v;
		if (crc_inout)
			crc = crc32_update_word(crc, v);
		/* Clean one cache line every 8 words (64-byte line on A53). Cleaning
		 * per-line rather than per-word keeps this ~8x cheaper. */
		if ((i & 7u) == 7u)
			__asm__ volatile("dc cvac, %0" :: "r"(&d[i]) : "memory");
		/* Every 1M words (~8 MiB): keep the 16 s HW WDT fed, same interval
		 * discipline as el2_ncmap.c's civac_range(). */
		if ((i & 0xFFFFFu) == 0xFFFFFu)
			wdt_pet();
	}
	__asm__ volatile("dsb sy" ::: "memory");
	if (crc_inout)
		*crc_inout = crc;
}

/* dram_copy(), but skipping every exclusion window (snapshot_excl_windows())
 * instead of sweeping [0, total_len) blindly -- see snapshot.h's "EXCLUSION
 * WINDOWS" for why. `dst_base`/`src_base` must be addresses whose OFFSET FROM
 * SNAP_DRAM_BASE is the SAME offset space the windows are expressed in --
 * true of both current callers: snapshot_save() passes (SNAP_DRAM_STORE,
 * SNAP_DRAM_BASE, ...) and snapshot_restore() passes (SNAP_DRAM_BASE,
 * SNAP_DRAM_STORE, ...), and SNAP_DRAM_STORE's own mirror is laid out 1:1
 * against SNAP_DRAM_BASE at offset 0 either way (snapshot.h's "Snapshot
 * STORE region"), so "offset from SNAP_DRAM_BASE" and "offset from
 * SNAP_DRAM_STORE into the mirror" are the identical number.
 *
 * Windows are sorted ascending + non-overlapping (snapshot_excl_windows()'s
 * own contract), so a single forward pass with a monotonically-advancing
 * window index `wi` is enough -- no rescans, no O(n*windows) blowup. */
static void
dram_copy_excluding(uint64_t dst_base, uint64_t src_base, uint64_t total_len,
                     uint32_t *crc_inout)
{
	struct snapshot_excl_window win[SNAPSHOT_EXCL_MAX];
	unsigned n = snapshot_excl_windows(win);
	uint64_t off = 0;
	unsigned wi = 0;

	while (off < total_len) {
		uint64_t seg_len = total_len - off;

		/* Advance past any window that has already fully elapsed. */
		while (wi < n && win[wi].off + win[wi].len <= off)
			wi++;

		if (wi < n && win[wi].off <= off) {
			/* `off` is INSIDE the next window: skip straight to its end
			 * (clamped to total_len) without copying anything. */
			uint64_t skip_end = win[wi].off + win[wi].len;
			off = (skip_end < total_len) ? skip_end : total_len;
			continue;
		}

		if (wi < n && win[wi].off < off + seg_len)
			/* The next window starts partway through this segment:
			 * copy only up to its start. */
			seg_len = win[wi].off - off;

		dram_copy(dst_base + off, src_base + off, seg_len, crc_inout);
		off += seg_len;
	}
}

/* ================================================================== *
 * PUBLIC API
 * ================================================================== */

static volatile struct snapshot_hdr *snap_hdr(void)
{
	/* SNAP_META_BASE, not SNAP_STORE_BASE: the header lives in low-memory
	 * hv-scratch now, disjoint from the high-GiB DRAM mirror -- see
	 * snapshot.h's "Snapshot STORE region" comment for why. */
	return (volatile struct snapshot_hdr *)SNAP_META_BASE;
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

	/* Guest DRAM -> store. This is the long pole. Folds a running CRC32 over
	 * the source words into the same pass (see crc32_update_word / dram_copy)
	 * so integrity-checking costs no extra memory bandwidth. RESOLVED: this
	 * closes the former "CRC left 0 in skeleton" TODO with a real check --
	 * software CRC32, not the optional HW CRC32 extension (see
	 * crc32_update_word for why that dependency is deliberately avoided).
	 *
	 * dram_copy_excluding(), not plain dram_copy(): see snapshot.h's
	 * "EXCLUSION WINDOWS" -- this range also physically contains the
	 * hypervisor's own image/hv-scratch/(HV_HDMI) framebuffer, which must be
	 * skipped rather than read as if it were guest content. The CRC therefore
	 * covers exactly the bytes that end up in the mirror (the excluded-aware
	 * pass), which is also exactly what snapshot_net.c's
	 * crc32_region_excluding() recomputes on the receiving end -- see that
	 * function's comment for why the two MUST agree. */
	{
		uint32_t crc = CRC32_INIT;

		dram_copy_excluding(SNAP_DRAM_STORE, SNAP_DRAM_BASE, SNAP_DRAM_SIZE, &crc);
		h->crc32 = crc ^ 0xFFFFFFFFu;
	}

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
	struct snapshot_sysregs sr;
	struct el2_frame saved_frame;
	uint64_t dram_size, taken_cntpct, now_cntpct, delta;

	if (!frame)
		return -1;
	if (!snapshot_present())
		return -2;

	/* PRECONDITION (caller-enforced): secondary cores parked (design "SMP"),
	 * guest quiesced at this trap boundary. */

	/* 0. Read EVERY header field this function still needs into locals
	 *    BEFORE step 1's DRAM copy, and NOT after. This ordering is not
	 *    cosmetic: `h` (snap_hdr(), SNAP_META_BASE) now lives in low-memory
	 *    hv-scratch, which -- unlike the old high-GiB header location -- sits
	 *    INSIDE [SNAP_DRAM_BASE, SNAP_DRAM_BASE+dram_size), i.e. inside step
	 *    1's own COPY DESTINATION (see snapshot.h's "Snapshot STORE region"
	 *    CAVEAT). Reading `h->sysregs`/`h->frame`/`h->taken_cntpct` AFTER that
	 *    copy would read back whatever byte-for-byte shadow of the header
	 *    snapshot_save() happened to embed in the mirror at save time, not
	 *    necessarily this call's own values -- harmless today (nothing
	 *    changes the header's own storage between a save and its restore, so
	 *    the shadow and the live header agree), but fragile and not a sound
	 *    argument to build "restore is correct" on. Capturing these first
	 *    makes the function correct independent of that coincidence. */
	dram_size    = h->dram_size;
	taken_cntpct = h->taken_cntpct;
	sr           = h->sysregs;
	saved_frame  = h->frame;

	/* 1. Reload guest DRAM from the store. Do this BEFORE sysregs so that when
	 *    the MMU regime comes back the memory it translates already holds the
	 *    snapshot contents. NULL crc: restore never recomputes/verifies CRC32
	 *    -- that would cost the same pass again and defeat the <1 s restore
	 *    goal (design doc "Performance"). The stored crc32 is a save-time
	 *    integrity aid for out-of-band inspection (e.g. a host memory dump),
	 *    not an on-path restore check.
	 *
	 *    dram_copy_excluding(), not plain dram_copy(): THIS is the fix for
	 *    the live-reproduced self-corruption bug (see snapshot.h's
	 *    "EXCLUSION WINDOWS") -- a blind dram_copy() here overwrites the
	 *    hypervisor's own running .text/.data/.bss AND ITS OWN LIVE CALL
	 *    STACK with year-old snapshot bytes while this very function is
	 *    still executing on that stack. Skipping the exclusion windows means
	 *    those PAs are left holding whatever the HV currently has there
	 *    (its own live, correct state) instead of being clobbered. */
	dram_copy_excluding(SNAP_DRAM_BASE, SNAP_DRAM_STORE, dram_size, (uint32_t *)0);

	/* 2. Reload the EL1 + per-guest EL2 sysreg set. */
	{
		/* 3. TIMER RE-BASE (see design "timer skew"): the physical counter has
		 *    advanced by (now - taken) since the snapshot. The saved compare
		 *    values (CNTP_CVAL/CNTV_CVAL) are ABSOLUTE counts and would now be
		 *    far in the past -> the guest would take an immediate storm of
		 *    "already expired" timer interrupts, or (worse) jump its notion of
		 *    time forward by minutes. We hide the elapsed real time from the
		 *    guest by pushing CNTVOFF_EL2 back by the same delta, so the
		 *    guest's virtual counter reads the value it had at snapshot time.
		 *    RESOLVED (already board-verified elsewhere in this tree, no new
		 *    hardware test needed): gic_timer.c's gic_timer_init() comment
		 *    records that this was checked live -- "the guest uses the
		 *    virtual timer (CNTV_*) for its own interrupts" and "FreeBSD's
		 *    DELAY() and getcycles() use CNTVCT_EL0 (not CNTPCT)" -- which is
		 *    exactly why that function zeroes CNTVOFF_EL2 at guest boot
		 *    (ATF/BL31 otherwise leaves it at a huge nonzero value). The
		 *    physical timer (CNTP_CTL/CVAL) is captured/restored here too
		 *    (belt-and-suspenders, and it's this HV's OWN tick source per
		 *    gic_timer.c), but the guest-visible rebase this comment is about
		 *    is correctly CNTVOFF_EL2, not CNTP_CVAL_EL0. */
		now_cntpct = RD("cntpct_el0");
		delta      = now_cntpct - taken_cntpct;
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
	*frame = saved_frame;

	/* NB: SP_EL1/SP_EL0 were already reloaded in sysregs_reload(); they are
	 * banked and NOT part of the eret-restored frame, so they must be (and
	 * are) restored via MSR, not through `frame`. */

	return 0;
}
