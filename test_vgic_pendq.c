/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vgic_pendq.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests
 * for the parts of vgic.c that the QEMU vGIC gate CANNOT reach: the
 * LR-exhaustion pending-injection queue, the maintenance-driven drain, the
 * List-Register word layout, vgic_inject_cntv()'s duplicate-vINTID gate, and
 * — added for Phase 2 step P1 (docs/phase2-all-cores.md §1.6) — that two
 * cores' worth of this state never cross-talk now that vgic.c keeps it in
 * struct vg_percpu[SMP_MAX_CPUS] instead of file-scope statics.
 *
 * ============================================================================
 * READ THIS FIRST: THE DIVISION OF LABOUR WITH vgic-qemu-ci.sh
 * ============================================================================
 * As of 2026-08-05 there are TWO board-free checks over vgic.c, and they cover
 * disjoint things. Do not treat either as covering the other:
 *
 *   vgic-qemu-ci.sh — runs the REAL vgic.c (not this mirror) against QEMU
 *     virt's real GICv2 virtualization extensions, and proves DELIVERY: a
 *     List Register written by vgic_inject_cntv() actually reaches an EL1
 *     guest through GICV, the guest's virtual IAR returns the injected vINTID,
 *     and its virtual EOIR frees the LR. That is the strong test, because
 *     hardware — not an assertion — judges whether the LR word was valid.
 *     What it cannot reach: with one vINTID in flight at a time, 4 List
 *     Registers are never exhausted, so vgic_inject_hw()'s pending queue is
 *     never pushed, GICH_HCR.UIE is never set, INTID 25 never fires, and
 *     vgic_maintenance()'s drain loop never executes. Provoking that under
 *     QEMU would need HW=1 List Registers tied to physical INTIDs that were
 *     never activated — deactivate-on-guest-EOI of an inactive physical
 *     interrupt — which is exactly the kind of "does the emulator do what
 *     silicon does here?" territory where a green gate would prove nothing.
 *     It also runs with QEMU virt's single CPU (index 0 always), so it can
 *     never exercise the per-core separation this file's two-core tests do.
 *
 *   THIS FILE — covers that unreached logic as pure arithmetic, host-side and
 *     deterministically. It cannot prove delivery, and it does not claim to.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vgic.c"` WITH STUBS
 * ============================================================================
 * vgic.c was read in full before writing this file (and again, in full,
 * before the P1 per-core update below). The queue itself
 * (vgic_pendq_push/vgic_pendq_pop, vgic.c:293-330-ish) and the LR word
 * builder (vgic_lr_write_hw) are pure integer/array logic — but they are not
 * separable in the source, because vgic.c threads raw AArch64 mnemonics
 * through every path that reaches them:
 *   - vg_bc()                  "dc civac, %0\n\tdsb sy"
 *   - write_cntvoff_el2()      "msr cntvoff_el2, %0\n\tisb"
 *   - the GICH() macro         absolute-address volatile MMIO at
 *                              VGIC_GICH_BASE — push() itself does
 *                              `GICH(GICH_HCR) |= GICH_HCR_UIE`
 *   - vgic_st_vectors          a whole __asm__ EL1 vector table + IRQ
 *                              trampoline
 *   - read_currentel()/write_vbar_el1()/"msr daifclr, #2"
 * gcc on x86_64 rejects every one of those, and making them overridable would
 * mean editing vgic.c to weaken the very register plumbing the board depends
 * on. So this file mirrors the pure logic verbatim with an exact vgic.c
 * line-number citation on each function, per the house style established by
 * test_vblk_ring.c / test_vconsole_uart.c / test_stage2_tables.c.
 *
 * MIRROR DRIFT is the known weakness of this pattern (REVIEW-2026-07-24.md
 * recorded a case where a real fix never reached its mirror). Two mitigations
 * here: every mirrored function carries its source line range, and the GICH
 * register block is modelled as a real array (g_gich[]) rather than stubbed
 * away, so the mirror keeps the same read-modify-write structure as the source
 * instead of quietly dropping the UIE toggle.
 *
 * ============================================================================
 * PHASE 2 STEP P1 ADDITION (2026-08-10): per-core parameterization
 * ============================================================================
 * The real vgic.c used to hold this state (vg_nr_lr, the injection counters,
 * vg_active, the CNTV counters, and the whole pending queue) in file-scope
 * statics — ONE instance, shared by whichever core happened to call in.
 * docs/phase2-all-cores.md §1.6 names this the first real blocker for any
 * second core taking EL2 interrupts: GICH is per-PE-banked hardware, so the
 * software queue that feeds it must be per-core too, or two cores injecting
 * concurrently interleave push/pop on one queue and can hand core A's
 * pending interrupt to core B's List Register.
 *
 * vgic.c's fix is `struct vg_percpu g_vg[SMP_MAX_CPUS]`, indexed by
 * `vg_self()` (smp_cpu_id(), clamped). This file mirrors that shape exactly:
 * every mirrored function now takes an explicit `unsigned core` argument
 * (the mirror equivalent of "the pointer vg_self() would have returned"),
 * both the register bank (g_gich[NUM_TEST_CORES][...], modelling that GICH
 * really is one independent bank per PE) and the software state
 * (g_vg[NUM_TEST_CORES]) are now per-core arrays, and a new test
 * (test_two_cores_independent_pendq_and_lrs) drives core 0 and core 1
 * through interleaved operations and asserts neither ever observes the
 * other's state.
 *
 * The existing 9 tests are UNCHANGED in what they assert — they were already
 * testing "the" queue's logic correctly, just against a single implicit
 * instance. They now pass an explicit `TEST_CORE_A` (0) everywhere a bare
 * call used to work; behaviour is identical because there was only ever one
 * instance before, and TEST_CORE_A's slot is that same instance now.
 *
 * NEGATIVE CONTROL (do it, don't just claim it — see the task that produced
 * this comment). `core_bank()` is the ONE choke point every register/state
 * access goes through to turn a requested core into an array index. Build
 * normally and the new two-core test passes (core 0 and core 1 map to
 * distinct slots). Rebuild with `-DMIRROR_FORCE_SHARED=1` and `core_bank()`
 * collapses BOTH cores onto slot 0 — exactly what vgic.c looked like before
 * this pass — and the new test FAILS, because core "1"'s pushes and LR
 * writes land in core "0"'s queue and GICH bank. That is the cross-core
 * corruption docs/phase2-all-cores.md §1.6 describes, reproduced on purpose
 * and caught by an assertion instead of by a live guest's wedged timebase.
 * The other 9 tests are unaffected by the flag (they only ever address
 * core 0, which maps to slot 0 either way) — see the report this test run
 * produced for the actual pass/fail counts in both configurations.
 *
 * Build: gcc -o test_vgic_pendq test_vgic_pendq.c && ./test_vgic_pendq
 * Negative control: gcc -DMIRROR_FORCE_SHARED=1 -o test_vgic_pendq_negctl \
 *                       test_vgic_pendq.c && ./test_vgic_pendq_negctl
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * How many independent cores this mirror models. 2 is enough to prove "no
 * cross-talk" (a 3rd core would only repeat the same argument) and keeps the
 * new tests readable. The real vgic.c uses SMP_MAX_CPUS==4 (smp.h:42); this
 * file does not need to match that number, only to model "more than one".
 * ------------------------------------------------------------------ */
#define NUM_TEST_CORES 2u

/* NEGATIVE-CONTROL TOGGLE. 0 (default): core_bank() is the identity map, so
 * core 0 and core 1 get genuinely separate register banks and state slots —
 * this is what vgic.c does today, after P1. 1 (via -DMIRROR_FORCE_SHARED=1):
 * core_bank() collapses every core onto slot 0 — this is what vgic.c did
 * BEFORE P1 (one shared instance, whichever core happened to call in). See
 * the file header's "NEGATIVE CONTROL" section above. */
#ifndef MIRROR_FORCE_SHARED
#define MIRROR_FORCE_SHARED 0
#endif

static inline unsigned core_bank(unsigned core)
{
#if MIRROR_FORCE_SHARED
	(void)core;
	return 0u;
#else
	return core % NUM_TEST_CORES;
#endif
}

/* ------------------------------------------------------------------ *
 * GICH register block, modelled as real memory so the mirror keeps the
 * source's read-modify-write shape. Offsets mirror vgic.c:90-97. Now one
 * bank PER CORE (g_gich[core_bank(core)][...]) — modelling the real
 * architectural fact that GICH is per-PE-banked hardware (gic_timer.c's own
 * comment: "GICH_* and HCR_EL2 are banked per PE"), which is WHY the
 * software queue that feeds it has to be per-core too (see the file header).
 * ------------------------------------------------------------------ */
#define GICH_HCR    0x000u
#define GICH_VTR    0x004u
#define GICH_VMCR   0x008u
#define GICH_MISR   0x010u
#define GICH_EISR0  0x020u
#define GICH_ELRSR0 0x030u
#define GICH_LR(n)  (0x100u + 4u * (n))

static uint32_t g_gich[NUM_TEST_CORES][0x200 / 4];

/* GICH_ELRSR0 is DERIVED, not stored — modelled exactly the way the hardware
 * defines it: "bit n == 1 means List Register n is empty (invalid)", i.e. its
 * State field is 00. Getting this wrong is the single easiest way to write a
 * mirror test that passes against logic hardware would reject, so it is
 * recomputed on every read rather than maintained by hand in the tests:
 *   - writing an LR to pending/active makes it busy IMMEDIATELY, with no
 *     bookkeeping call in vgic.c (hardware does it) — which is precisely what
 *     makes vgic_maintenance()'s once-per-call ELRSR snapshot safe: the free
 *     set can only shrink while it drains, never grow;
 *   - a guest EOI invalidating an LR frees it again.
 * The mirrored functions below therefore keep vgic.c's exact text — they only
 * ever READ this register — while the model stays faithful. Body is below the
 * LR field-layout defines it needs. */
static uint32_t *gich_ptr(unsigned core, unsigned reg);

#define GICH(core, reg)   (*gich_ptr((core), (reg)))

/* Mirrors vgic.c:101-104 GICH_HCR bits. */
#define GICH_HCR_EN      (1u << 0)
#define GICH_HCR_UIE     (1u << 1)

/* Mirrors vgic.c:120-126 GICH_LR<n> field layout. */
#define GICH_LR_VID_MASK       0x3ffu
#define GICH_LR_PRIO_SHIFT     23
#define GICH_LR_STATE_PENDING  (1u << 28)
#define GICH_LR_STATE_ACTIVE   (1u << 29)
#define GICH_LR_GRP1           (1u << 30)
#define GICH_LR_HW             (1u << 31)
#define GICH_LR_EOI            (1u << 19)

static uint32_t *
gich_ptr(unsigned core, unsigned reg)
{
	unsigned bank = core_bank(core);

	if (reg == GICH_ELRSR0) {
		uint32_t elrsr = 0;

		for (unsigned n = 0; n < 16u; n++) {
			uint32_t lr = g_gich[bank][(0x100u + 4u * n) / 4u];

			if ((lr & (GICH_LR_STATE_PENDING | GICH_LR_STATE_ACTIVE)) == 0u)
				elrsr |= (1u << n);
		}
		g_gich[bank][GICH_ELRSR0 / 4u] = elrsr;
	}
	return &g_gich[bank][reg / 4u];
}

/* Mirrors vgic.h:116/134 the two PPIs vgic.c names. */
#define VGIC_VTIMER_INTID   27u
#define VGIC_MAINT_INTID    25u

/* Mirrors vgic.h:98-100: VGIC_GROUP0 defaults to 1, and vgic.c:146-152's
 * resulting group selection. Injecting Grp0 is what made hardware deliver
 * anything at all (GICV has no security banking) — see vgic.h:93-97. */
#define VGIC_GROUP0 1
#if VGIC_GROUP0
#define VGIC_LR_GRP     0u
#else
#define VGIC_LR_GRP     GICH_LR_GRP1
#endif

/* ------------------------------------------------------------------ *
 * Module state, per core (mirrors vgic.c's struct vg_percpu / g_vg[] — the
 * P1 change this file exists to pin). Field names match vgic.c's post-P1
 * struct field names (nr_lr, inject_count, ... pendq_overflow) rather than
 * the old vg_-prefixed statics, so a reader who has just read vgic.c
 * recognises the shape immediately.
 * ------------------------------------------------------------------ */
#define VGIC_PENDQ_SIZE 32u          /* vgic.c:272 (mechanical constant) */

struct vgic_pend {                   /* vgic.c:274-278 (now vg_percpu.pendq[]) */
	uint32_t vintid;
	uint32_t pintid;
	int      priority;
};

struct vg_mirror {
	uint32_t nr_lr;
	uint32_t inject_count;
	uint32_t inject_ok;
	uint32_t inject_drop;
	uint32_t cntv_inject;
	uint32_t cntv_gate;
	uint32_t cntv_drop;
	struct vgic_pend pendq[VGIC_PENDQ_SIZE];
	uint32_t pendq_head;
	uint32_t pendq_tail;
	uint32_t pendq_count;
	uint32_t pendq_hwm;
	uint32_t pendq_overflow;
};

static struct vg_mirror g_vg[NUM_TEST_CORES];

static inline struct vg_mirror *vg_of(unsigned core)
{
	return &g_vg[core_bank(core)];
}

/* Mirrors vgic.c's vgic_pendq_push() verbatim (minus the vg_bc() breadcrumb
 * stores — pure "dc civac" telemetry with no effect on the queue), now
 * operating on `core`'s own slot via vg_of(). */
static int
vgic_pendq_push(unsigned core, uint32_t vintid, uint32_t pintid, int priority)
{
	struct vg_mirror *vg = vg_of(core);

	if (vg->pendq_count >= VGIC_PENDQ_SIZE) {
		vg->pendq_overflow++;
		return 0;
	}

	if (vg->pendq_count == 0)
		GICH(core, GICH_HCR) |= GICH_HCR_UIE;   /* start driving drains */

	vg->pendq[vg->pendq_head].vintid   = vintid;
	vg->pendq[vg->pendq_head].pintid   = pintid;
	vg->pendq[vg->pendq_head].priority = priority;
	vg->pendq_head = (vg->pendq_head + 1u) % VGIC_PENDQ_SIZE;
	vg->pendq_count++;
	if (vg->pendq_count > vg->pendq_hwm)
		vg->pendq_hwm = vg->pendq_count;

	return 1;
}

/* Mirrors vgic.c's vgic_pendq_pop() verbatim. */
static int
vgic_pendq_pop(unsigned core, uint32_t *vintid, uint32_t *pintid, int *priority)
{
	struct vg_mirror *vg = vg_of(core);

	if (vg->pendq_count == 0)
		return 0;

	*vintid   = vg->pendq[vg->pendq_tail].vintid;
	*pintid   = vg->pendq[vg->pendq_tail].pintid;
	*priority = vg->pendq[vg->pendq_tail].priority;
	vg->pendq_tail = (vg->pendq_tail + 1u) % VGIC_PENDQ_SIZE;
	vg->pendq_count--;
	return 1;
}

/* Mirrors vgic.c's vgic_lr_write_hw() verbatim, minus vg_bc()/
 * flightrec_log() telemetry. */
static void
vgic_lr_write_hw(unsigned core, uint32_t n, uint32_t vintid, uint32_t pintid,
                  int priority)
{
	struct vg_mirror *vg = vg_of(core);
	uint32_t prio5 = ((uint32_t)priority >> 3) & 0x1fu;
	uint32_t lr = (vintid & GICH_LR_VID_MASK) |
	              ((pintid & 0x3ffu) << 10) |
	              VGIC_LR_GRP |
	              GICH_LR_HW |
	              GICH_LR_STATE_PENDING |
	              (prio5 << GICH_LR_PRIO_SHIFT);

	GICH(core, GICH_LR(n)) = lr;

	vg->inject_ok++;
}

/* Mirrors vgic.c's vgic_inject_hw() verbatim, minus vg_bc(). */
static void
vgic_inject_hw(unsigned core, uint32_t vintid, uint32_t pintid, int priority)
{
	struct vg_mirror *vg = vg_of(core);
	uint32_t elrsr, n;

	vg->inject_count++;

	if (vg->pendq_count == 0) {
		elrsr = GICH(core, GICH_ELRSR0);
		for (n = 0; n < vg->nr_lr; n++) {
			if (elrsr & (1u << n)) {
				vgic_lr_write_hw(core, n, vintid, pintid, priority);
				return;
			}
		}
	}

	if (!vgic_pendq_push(core, vintid, pintid, priority))
		vg->inject_drop++;   /* true loss: even the pending queue was full */
}

/* Mirrors the drain half of vgic.c's vgic_maintenance(): the EISR LR-clear
 * loop, the ELRSR-bounded drain, and the UIE-clear. Omits only the vg_bc()
 * breadcrumbs. */
static void
vgic_maintenance(unsigned core)
{
	struct vg_mirror *vg = vg_of(core);
	uint32_t eisr, elrsr, n;

	eisr = GICH(core, GICH_EISR0);

	for (n = 0; n < vg->nr_lr; n++)
		if (eisr & (1u << n))
			GICH(core, GICH_LR(n)) = 0u;

	elrsr = GICH(core, GICH_ELRSR0);
	for (n = 0; n < vg->nr_lr && vg->pendq_count > 0; n++) {
		uint32_t vintid, pintid;
		int priority;

		if (!(elrsr & (1u << n)))
			continue;
		if (!vgic_pendq_pop(core, &vintid, &pintid, &priority))
			break;
		vgic_lr_write_hw(core, n, vintid, pintid, priority);
	}

	if (vg->pendq_count == 0)
		GICH(core, GICH_HCR) &= ~GICH_HCR_UIE;
}

/* Mirrors vgic.c's vgic_inject_cntv(), minus vg_bc()/flightrec_log() and the
 * VGIC_CNTV_HW / VGIC_CNTV_EOI_TRACE #if arms (both default to 0 per
 * vgic.h:90-103, i.e. the pure-virtual software-vtimer path this mirrors).
 * Returns 1 if a vIRQ was (re)injected, 0 if gated or dropped. */
static int
vgic_inject_cntv(unsigned core)
{
	struct vg_mirror *vg = vg_of(core);
	uint32_t elrsr, n, lr, free_lr = 0xffffffffu;

	vg->inject_count++;

	elrsr = GICH(core, GICH_ELRSR0);
	for (n = 0; n < vg->nr_lr; n++) {
		if (elrsr & (1u << n)) {
			if (free_lr == 0xffffffffu)
				free_lr = n;
			continue;
		}
		lr = GICH(core, GICH_LR(n));
		if ((lr & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID) {
			vg->cntv_gate++;
			return 0;
		}
	}

	if (free_lr == 0xffffffffu) {
		vg->cntv_drop++;
		return 0;
	}

	n  = free_lr;
	lr = (VGIC_VTIMER_INTID & GICH_LR_VID_MASK) |
	     VGIC_LR_GRP |
	     GICH_LR_STATE_PENDING;   /* priority field 0 => prio5 == 0 */

	GICH(core, GICH_LR(n)) = lr;

	vg->cntv_inject++;
	vg->inject_ok++;
	return 1;
}

/* ==================================================================== *
 * Test-harness helpers (NOT mirrors — this is the test double for "what
 * the GIC hardware and the guest would do"). All take an explicit `core`.
 * ==================================================================== */

/* Reset core's mirrored state to a just-after-vgic_init() condition: every LR
 * invalid, so GICH_ELRSR0 shows all vg->nr_lr of them empty (vgic.c clears
 * the LRs; ELRSR is hardware-derived from that). */
static void
harness_reset(unsigned core)
{
	struct vg_mirror *vg = vg_of(core);
	unsigned bank = core_bank(core);

	memset(g_gich[bank], 0, sizeof(g_gich[bank]));
	memset(vg, 0, sizeof(*vg));
	vg->nr_lr = 4;
	GICH(core, GICH_HCR) = GICH_HCR_EN;
	/* No ELRSR init needed: it is derived from the (now all-invalid) LRs,
	 * so it already reads "all empty" — see gich_ptr(). */
}

/* Model the guest retiring LR n via a virtual EOI: the LR goes invalid and
 * its ELRSR bit sets. This is what real hardware does on GICV_EOIR for a
 * pending/active LR, and it is the only reason an LR is ever reclaimed. */
static void
harness_guest_eoi(unsigned core, uint32_t n)
{
	GICH(core, GICH_LR(n)) = 0u;   /* ELRSR bit n follows automatically */
}

/* Model the guest ACKing LR n (virtual IAR) without EOIing it yet: state
 * pending -> active. Still occupied, so its derived ELRSR bit stays clear —
 * "active" is emphatically not "free", which is the distinction the CNTV
 * duplicate gate depends on. */
static void
harness_guest_ack(unsigned core, uint32_t n)
{
	uint32_t lr = GICH(core, GICH_LR(n));

	lr &= ~GICH_LR_STATE_PENDING;
	lr |= GICH_LR_STATE_ACTIVE;
	GICH(core, GICH_LR(n)) = lr;
}

static unsigned
lr_occupied_count(unsigned core)
{
	struct vg_mirror *vg = vg_of(core);
	unsigned c = 0;

	for (uint32_t n = 0; n < vg->nr_lr; n++)
		if (!(GICH(core, GICH_ELRSR0) & (1u << n)))
			c++;
	return c;
}

/* ==================================================================== *
 * Tests. TEST_CORE_A is what the original (pre-P1) 9 tests always meant by
 * "the" queue — kept as a named constant rather than a bare 0u so it reads
 * as a deliberate choice next to the new two-core test's CORE_A/CORE_B.
 * ==================================================================== */
#define TEST_CORE_A 0u

/* The LR word vgic_lr_write_hw() builds must place every field exactly where
 * vgic.c:110-126 documents it. Priority in particular is the TOP 5 bits of the
 * 8-bit GIC priority (>>3), which is easy to get backwards and impossible to
 * notice from a "the interrupt arrived" test. */
static void test_lr_word_field_layout(void)
{
	harness_reset(TEST_CORE_A);

	vgic_inject_hw(TEST_CORE_A, /*vintid=*/106, /*pintid=*/106, /*priority=*/0xA0);

	uint32_t lr = GICH(TEST_CORE_A, GICH_LR(0));

	assert((lr & GICH_LR_VID_MASK) == 106u);          /* [9:0]   VirtualID  */
	assert(((lr >> 10) & 0x3ffu) == 106u);            /* [19:10] PhysicalID */
	assert(lr & GICH_LR_HW);                          /* [31]    HW=1       */
	assert(lr & GICH_LR_STATE_PENDING);               /* [29:28] pending    */
	assert(!(lr & GICH_LR_STATE_ACTIVE));
	/* [27:23] Priority = top 5 bits of 0xA0 == 0xA0>>3 == 0x14. */
	assert(((lr >> GICH_LR_PRIO_SHIFT) & 0x1fu) == 0x14u);
	/* VGIC_GROUP0=1 (vgic.h's default) => Grp1 bit CLEAR. This is the bit
	 * whose wrong value made hardware deliver nothing at all (vgic.h:93-97),
	 * and which the vgic-qemu gate independently reproduces as a failure. */
	assert(!(lr & GICH_LR_GRP1));
	assert(!(lr & GICH_LR_EOI));                      /* HW mode: no EOI maint */

	/* A vINTID wider than 10 bits must be masked, not allowed to corrupt the
	 * PhysicalID field above it. */
	harness_reset(TEST_CORE_A);
	vgic_inject_hw(TEST_CORE_A, 0x7FFu, 0x7FFu, 0);
	lr = GICH(TEST_CORE_A, GICH_LR(0));
	assert((lr & GICH_LR_VID_MASK) == 0x3FFu);
	assert(((lr >> 10) & 0x3ffu) == 0x3FFu);
}

/* THE v2 FIX: with all 4 List Registers occupied, an injection must be QUEUED,
 * not dropped. Pre-v2 this lost the interrupt outright (vgic.h:216-225). */
static void test_injection_with_no_free_lr_is_queued_not_dropped(void)
{
	harness_reset(TEST_CORE_A);

	/* Fill all 4 LRs with distinct device INTIDs and have the guest ACK (but
	 * not EOI) each, so every LR is genuinely occupied. */
	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}
	assert(lr_occupied_count(TEST_CORE_A) == 4u);
	assert(vg_of(TEST_CORE_A)->inject_ok == 4u);
	assert(vg_of(TEST_CORE_A)->pendq_count == 0u);
	assert(!(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE));   /* nothing queued yet */

	/* The 5th injection has nowhere to go. */
	vgic_inject_hw(TEST_CORE_A, 50u, 50u, 0);

	assert(vg_of(TEST_CORE_A)->pendq_count == 1u);        /* queued ... */
	assert(vg_of(TEST_CORE_A)->pendq_overflow == 0u);     /* ... and NOT lost */
	assert(vg_of(TEST_CORE_A)->inject_drop == 0u);
	assert(vg_of(TEST_CORE_A)->inject_ok == 4u);          /* no new LR was written */
	/* Empty -> non-empty must arm the underflow maintenance IRQ, which is
	 * the only thing that will ever drive the drain (vgic.c:301-302). */
	assert(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE);
}

/* FIFO across queue+LR: once ANYTHING is queued, a later injection must queue
 * behind it even if an LR is free by then — vgic.c:845-861's explicit reason
 * for gating the fast path on vg_pendq_count == 0. */
static void test_fifo_order_across_queue_and_lrs(void)
{
	harness_reset(TEST_CORE_A);

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}

	vgic_inject_hw(TEST_CORE_A, 60u, 60u, 0);         /* queued (position 0) */
	assert(vg_of(TEST_CORE_A)->pendq_count == 1u);

	/* An LR frees up, but no maintenance drain has run yet. */
	harness_guest_eoi(TEST_CORE_A, 1u);
	assert(lr_occupied_count(TEST_CORE_A) == 3u);

	/* This injection COULD take LR 1 — it must not, or it would overtake
	 * vINTID 60 which has been waiting longer. */
	vgic_inject_hw(TEST_CORE_A, 61u, 61u, 0);
	assert(vg_of(TEST_CORE_A)->pendq_count == 2u);
	assert(GICH(TEST_CORE_A, GICH_LR(1)) == 0u);      /* LR 1 still untouched */

	/* Now drain. 60 must land before 61. */
	vgic_maintenance(TEST_CORE_A);
	assert((GICH(TEST_CORE_A, GICH_LR(1)) & GICH_LR_VID_MASK) == 60u);
	assert(vg_of(TEST_CORE_A)->pendq_count == 1u);        /* only one LR was free */
	assert(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE);   /* backlog remains => stay armed */

	harness_guest_eoi(TEST_CORE_A, 2u);
	vgic_maintenance(TEST_CORE_A);
	assert((GICH(TEST_CORE_A, GICH_LR(2)) & GICH_LR_VID_MASK) == 61u);
	assert(vg_of(TEST_CORE_A)->pendq_count == 0u);
	/* Queue empty again => UIE must be cleared, or INTID 25 storms at idle
	 * (vgic.h:126-131). */
	assert(!(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE));
}

/* The drain is bounded by the number of FREE List Registers, never by queue
 * depth, and it must not touch occupied LRs (vgic.c:572-581). */
static void test_maintenance_drain_is_bounded_by_free_lrs(void)
{
	harness_reset(TEST_CORE_A);

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}
	for (uint32_t i = 0; i < 10u; i++)
		vgic_inject_hw(TEST_CORE_A, 70u + i, 70u + i, 0);
	assert(vg_of(TEST_CORE_A)->pendq_count == 10u);
	assert(vg_of(TEST_CORE_A)->pendq_hwm == 10u);

	/* Free exactly two LRs, then drain: exactly two entries may leave the
	 * queue, and the still-occupied LRs must be unchanged. */
	uint32_t lr0_before = GICH(TEST_CORE_A, GICH_LR(0));
	uint32_t lr3_before = GICH(TEST_CORE_A, GICH_LR(3));
	harness_guest_eoi(TEST_CORE_A, 1u);
	harness_guest_eoi(TEST_CORE_A, 2u);

	vgic_maintenance(TEST_CORE_A);

	assert(vg_of(TEST_CORE_A)->pendq_count == 8u);
	assert((GICH(TEST_CORE_A, GICH_LR(1)) & GICH_LR_VID_MASK) == 70u);
	assert((GICH(TEST_CORE_A, GICH_LR(2)) & GICH_LR_VID_MASK) == 71u);
	assert(GICH(TEST_CORE_A, GICH_LR(0)) == lr0_before);
	assert(GICH(TEST_CORE_A, GICH_LR(3)) == lr3_before);
	assert(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE);
}

/* Ring wraparound: pushing/popping more than VGIC_PENDQ_SIZE entries over the
 * life of the queue must keep FIFO order and never alias entries. */
static void test_pendq_wraparound_preserves_fifo(void)
{
	harness_reset(TEST_CORE_A);

	/* Occupy all LRs so everything queues. */
	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}

	uint32_t next_expect = 100u;
	uint32_t next_push   = 100u;

	/* 5 rounds of (push 20, drain 20) — 100 entries total through a 32-slot
	 * ring, so head/tail wrap several times. */
	for (uint32_t round = 0; round < 5u; round++) {
		for (uint32_t i = 0; i < 20u; i++)
			vgic_inject_hw(TEST_CORE_A, next_push++, 0u, 0);
		assert(vg_of(TEST_CORE_A)->pendq_count == 20u);
		assert(vg_of(TEST_CORE_A)->pendq_overflow == 0u);

		for (uint32_t i = 0; i < 20u; i++) {
			uint32_t v, p;
			int prio;
			assert(vgic_pendq_pop(TEST_CORE_A, &v, &p, &prio) == 1);
			assert(v == next_expect);   /* strict FIFO across the wrap */
			next_expect++;
		}
		assert(vg_of(TEST_CORE_A)->pendq_count == 0u);
	}
	assert(next_expect == 200u);
	assert(vg_of(TEST_CORE_A)->pendq_hwm == 20u);
}

/* Only a FULL ring actually loses an interrupt, and that loss is counted in
 * vg_pendq_overflow — a distinct counter from the legacy vg_inject_drop, so a
 * real overrun stays distinguishable from ordinary backlog (vgic.c:295-299,
 * vgic.h:219-225). */
static void test_pendq_overflow_is_the_only_true_loss(void)
{
	harness_reset(TEST_CORE_A);

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}

	/* Exactly VGIC_PENDQ_SIZE injections fit. */
	for (uint32_t i = 0; i < VGIC_PENDQ_SIZE; i++)
		vgic_inject_hw(TEST_CORE_A, 200u + i, 0u, 0);
	assert(vg_of(TEST_CORE_A)->pendq_count == VGIC_PENDQ_SIZE);
	assert(vg_of(TEST_CORE_A)->pendq_hwm == VGIC_PENDQ_SIZE);
	assert(vg_of(TEST_CORE_A)->pendq_overflow == 0u);
	assert(vg_of(TEST_CORE_A)->inject_drop == 0u);

	/* The next one is genuinely lost, and says so. */
	vgic_inject_hw(TEST_CORE_A, 999u, 0u, 0);
	assert(vg_of(TEST_CORE_A)->pendq_overflow == 1u);
	assert(vg_of(TEST_CORE_A)->inject_drop == 1u);
	assert(vg_of(TEST_CORE_A)->pendq_count == VGIC_PENDQ_SIZE);   /* queue not corrupted */

	/* The head of the queue must still be the FIRST thing pushed, i.e. the
	 * overflow rejected the newcomer rather than evicting an older entry. */
	uint32_t v, p;
	int prio;
	assert(vgic_pendq_pop(TEST_CORE_A, &v, &p, &prio) == 1);
	assert(v == 200u);
}

/* vgic_inject_cntv()'s one-shot gate: never a second live vINTID 27, because
 * two LRs with the same VirtualID is UNPREDICTABLE per the GICv2 spec
 * (vgic.c:480-486). Gate must trigger for a PENDING copy and for an ACTIVE
 * one, and must release as soon as the guest EOIs. */
static void test_cntv_duplicate_gate(void)
{
	harness_reset(TEST_CORE_A);

	assert(vgic_inject_cntv(TEST_CORE_A) == 1);
	assert(vg_of(TEST_CORE_A)->cntv_inject == 1u);
	assert((GICH(TEST_CORE_A, GICH_LR(0)) & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID);

	/* Still PENDING (guest hasn't even ACKed): gate. */
	assert(vgic_inject_cntv(TEST_CORE_A) == 0);
	assert(vg_of(TEST_CORE_A)->cntv_gate == 1u);
	assert(vg_of(TEST_CORE_A)->cntv_inject == 1u);

	/* ACKed but not EOIed (state ACTIVE): still a live 27 => still gate.
	 * This is the case that matters — an "active" LR is not "free". */
	harness_guest_ack(TEST_CORE_A, 0u);
	assert(vgic_inject_cntv(TEST_CORE_A) == 0);
	assert(vg_of(TEST_CORE_A)->cntv_gate == 2u);

	/* Guest EOIs: LR reclaimed, next tick injects again. This is exactly the
	 * loop vgic-qemu-ci.sh observes 10 times against real hardware. */
	harness_guest_eoi(TEST_CORE_A, 0u);
	assert(vgic_inject_cntv(TEST_CORE_A) == 1);
	assert(vg_of(TEST_CORE_A)->cntv_inject == 2u);
	assert(vg_of(TEST_CORE_A)->cntv_gate == 2u);
}

/* The gate keys on the vINTID, not on "any occupied LR": with all 4 LRs held
 * by OTHER vINTIDs the tick is DROPPED (counted separately as cntv_drop, since
 * a periodic tick is idempotent), not gated and not queued (vgic.c:505-511). */
static void test_cntv_drop_when_all_lrs_hold_other_vintids(void)
{
	harness_reset(TEST_CORE_A);

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}

	assert(vgic_inject_cntv(TEST_CORE_A) == 0);
	assert(vg_of(TEST_CORE_A)->cntv_drop == 1u);
	assert(vg_of(TEST_CORE_A)->cntv_gate == 0u);      /* no vINTID 27 was present */
	assert(vg_of(TEST_CORE_A)->cntv_inject == 0u);
	assert(vg_of(TEST_CORE_A)->pendq_count == 0u);    /* the CNTV path does not use the queue */

	/* One LR frees: the tick lands there. */
	harness_guest_eoi(TEST_CORE_A, 2u);
	assert(vgic_inject_cntv(TEST_CORE_A) == 1);
	assert((GICH(TEST_CORE_A, GICH_LR(2)) & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID);
}

/* vgic_maintenance()'s EISR loop must invalidate exactly the LRs the guest
 * EOIed via the EOI-maintenance path, and leave the others alone
 * (vgic.c:561-563). Normally a no-op for HW-mode LRs, kept for safety. */
static void test_maintenance_clears_only_eisr_flagged_lrs(void)
{
	harness_reset(TEST_CORE_A);

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(TEST_CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(TEST_CORE_A, i);
	}
	uint32_t lr0 = GICH(TEST_CORE_A, GICH_LR(0));
	uint32_t lr2 = GICH(TEST_CORE_A, GICH_LR(2));

	GICH(TEST_CORE_A, GICH_EISR0) = (1u << 1) | (1u << 3);
	vgic_maintenance(TEST_CORE_A);

	assert(GICH(TEST_CORE_A, GICH_LR(1)) == 0u);
	assert(GICH(TEST_CORE_A, GICH_LR(3)) == 0u);
	assert(GICH(TEST_CORE_A, GICH_LR(0)) == lr0);
	assert(GICH(TEST_CORE_A, GICH_LR(2)) == lr2);
	/* Nothing was queued, so UIE must be (still) clear. */
	assert(!(GICH(TEST_CORE_A, GICH_HCR) & GICH_HCR_UIE));
}

/* ==================================================================== *
 * NEW (Phase 2 step P1): two cores must never cross-talk.
 *
 * This is the test that would have caught the bug docs/phase2-all-cores.md
 * §1.6 describes — two cores injecting concurrently interleaving push/pop on
 * one queue and handing core A's pending interrupt to core B's List
 * Register. It drives CORE_A and CORE_B through DELIBERATELY overlapping
 * INTID ranges and interleaved operations, and asserts at every step that
 * neither core's state ever reflects the other's activity.
 *
 * With MIRROR_FORCE_SHARED=0 (default, models the real post-P1 vgic.c) this
 * passes. With MIRROR_FORCE_SHARED=1 (models the real PRE-P1 vgic.c, one
 * shared instance) core_bank() collapses CORE_A and CORE_B onto the same
 * slot and this test FAILS — see the file header's "NEGATIVE CONTROL"
 * section for exactly how to run that build and what it demonstrates.
 * ==================================================================== */
static void test_two_cores_independent_pendq_and_lrs(void)
{
	const unsigned CORE_A = 0u;
	const unsigned CORE_B = 1u;

	harness_reset(CORE_A);
	harness_reset(CORE_B);

	/* Fill CORE_A's 4 LRs with INTIDs 40-43, and CORE_B's 4 LRs with a
	 * DIFFERENT, deliberately overlapping-looking range (140-143) so any
	 * accidental aliasing between the two banks is immediately visible in
	 * the asserted values below, not just in a count. */
	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(CORE_A, 40u + i, 40u + i, 0);
		harness_guest_ack(CORE_A, i);
		vgic_inject_hw(CORE_B, 140u + i, 140u + i, 0);
		harness_guest_ack(CORE_B, i);
	}
	assert(lr_occupied_count(CORE_A) == 4u);
	assert(lr_occupied_count(CORE_B) == 4u);
	assert(vg_of(CORE_A)->inject_ok == 4u);
	assert(vg_of(CORE_B)->inject_ok == 4u);

	/* CORE_A gets a 5th injection -> queued on CORE_A only. */
	vgic_inject_hw(CORE_A, 50u, 50u, 0);
	assert(vg_of(CORE_A)->pendq_count == 1u);
	assert(vg_of(CORE_B)->pendq_count == 0u);   /* CORE_B must be UNAFFECTED */
	assert(GICH(CORE_A, GICH_HCR) & GICH_HCR_UIE);
	assert(!(GICH(CORE_B, GICH_HCR) & GICH_HCR_UIE));   /* CORE_B's UIE untouched */

	/* CORE_B gets a 5th injection of its own -> queued on CORE_B only.
	 * CORE_A's already-queued entry (50) must still be exactly one deep. */
	vgic_inject_hw(CORE_B, 150u, 150u, 0);
	assert(vg_of(CORE_A)->pendq_count == 1u);   /* unchanged by CORE_B's push */
	assert(vg_of(CORE_B)->pendq_count == 1u);
	assert(GICH(CORE_B, GICH_HCR) & GICH_HCR_UIE);

	/* Drain CORE_A only: free one of ITS LRs and run ITS maintenance. This
	 * must place vINTID 50 into CORE_A's freed LR and must NOT touch any of
	 * CORE_B's LRs or its still-queued vINTID 150. */
	harness_guest_eoi(CORE_A, 1u);
	uint32_t core_b_lr1_before = GICH(CORE_B, GICH_LR(1));
	vgic_maintenance(CORE_A);
	assert((GICH(CORE_A, GICH_LR(1)) & GICH_LR_VID_MASK) == 50u);
	assert(vg_of(CORE_A)->pendq_count == 0u);
	assert(!(GICH(CORE_A, GICH_HCR) & GICH_HCR_UIE));   /* CORE_A drained empty */
	assert(vg_of(CORE_B)->pendq_count == 1u);           /* CORE_B still has 150 queued */
	assert(GICH(CORE_B, GICH_LR(1)) == core_b_lr1_before);  /* CORE_B's LR untouched */
	assert(GICH(CORE_B, GICH_HCR) & GICH_HCR_UIE);          /* CORE_B still armed */

	/* Now drain CORE_B. Its vINTID 150 must land in ITS OWN freed LR — and
	 * it must be exactly 150, not 50 (which would mean the two banks or
	 * queues had aliased). */
	harness_guest_eoi(CORE_B, 2u);
	vgic_maintenance(CORE_B);
	assert((GICH(CORE_B, GICH_LR(2)) & GICH_LR_VID_MASK) == 150u);
	assert(vg_of(CORE_B)->pendq_count == 0u);
	assert(!(GICH(CORE_B, GICH_HCR) & GICH_HCR_UIE));

	/* Final cross-check: every LR CORE_A holds carries a vINTID in [40,50],
	 * every LR CORE_B holds carries one in [140,150]. A shared bank/queue
	 * would have let a CORE_B vINTID (>=140) end up in CORE_A's GICH, or
	 * vice versa. */
	for (uint32_t n = 0; n < 4u; n++) {
		uint32_t lr_a = GICH(CORE_A, GICH_LR(n));
		uint32_t lr_b = GICH(CORE_B, GICH_LR(n));

		if (lr_a != 0u) {
			uint32_t vid = lr_a & GICH_LR_VID_MASK;
			assert(vid >= 40u && vid <= 50u);
		}
		if (lr_b != 0u) {
			uint32_t vid = lr_b & GICH_LR_VID_MASK;
			assert(vid >= 140u && vid <= 150u);
		}
	}

	/* vgic_inject_cntv()'s one-shot gate (VGIC_VTIMER_INTID==27) is also
	 * per-core state (cntv_gate/cntv_inject) — prove a live vINTID 27 on
	 * CORE_A never gates CORE_B's own first injection. */
	harness_reset(CORE_A);
	harness_reset(CORE_B);
	assert(vgic_inject_cntv(CORE_A) == 1);         /* CORE_A: injected        */
	assert(vgic_inject_cntv(CORE_A) == 0);         /* CORE_A: gated (live 27) */
	assert(vg_of(CORE_A)->cntv_gate == 1u);
	assert(vg_of(CORE_B)->cntv_gate == 0u);         /* CORE_B unaffected       */
	assert(vgic_inject_cntv(CORE_B) == 1);         /* CORE_B: its OWN first injection succeeds */
	assert(vg_of(CORE_B)->cntv_inject == 1u);
	assert(vg_of(CORE_A)->cntv_inject == 1u);       /* CORE_A's own count unchanged */
}

/* ==================================================================== *
 * main()
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "lr_word_field_layout",                       test_lr_word_field_layout },
	{ "injection_with_no_free_lr_is_queued_not_dropped",
	                                                test_injection_with_no_free_lr_is_queued_not_dropped },
	{ "fifo_order_across_queue_and_lrs",            test_fifo_order_across_queue_and_lrs },
	{ "maintenance_drain_is_bounded_by_free_lrs",   test_maintenance_drain_is_bounded_by_free_lrs },
	{ "pendq_wraparound_preserves_fifo",            test_pendq_wraparound_preserves_fifo },
	{ "pendq_overflow_is_the_only_true_loss",       test_pendq_overflow_is_the_only_true_loss },
	{ "cntv_duplicate_gate",                        test_cntv_duplicate_gate },
	{ "cntv_drop_when_all_lrs_hold_other_vintids",  test_cntv_drop_when_all_lrs_hold_other_vintids },
	{ "maintenance_clears_only_eisr_flagged_lrs",   test_maintenance_clears_only_eisr_flagged_lrs },
	{ "two_cores_independent_pendq_and_lrs",        test_two_cores_independent_pendq_and_lrs },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

#if MIRROR_FORCE_SHARED
	printf("NOTE: built with MIRROR_FORCE_SHARED=1 -- core_bank() collapses\n"
	       "every core onto slot 0 (models vgic.c BEFORE Phase 2 step P1).\n"
	       "two_cores_independent_pendq_and_lrs is EXPECTED TO FAIL below.\n\n");
#endif

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- vgic pendq/LR tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
