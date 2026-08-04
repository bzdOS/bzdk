/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vgic_pendq.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests
 * for the parts of vgic.c that the QEMU vGIC gate CANNOT reach: the
 * LR-exhaustion pending-injection queue, the maintenance-driven drain, the
 * List-Register word layout, and vgic_inject_cntv()'s duplicate-vINTID gate.
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
 *
 *   THIS FILE — covers that unreached logic as pure arithmetic, host-side and
 *     deterministically. It cannot prove delivery, and it does not claim to.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vgic.c"` WITH STUBS
 * ============================================================================
 * vgic.c was read in full before writing this file. The queue itself
 * (vgic_pendq_push/vgic_pendq_pop, vgic.c:293-330) and the LR word builder
 * (vgic_lr_write_hw, vgic.c:336-355) are pure integer/array logic — but they
 * are not separable in the source, because vgic.c threads raw AArch64
 * mnemonics through every path that reaches them:
 *   - vg_bc()                  "dc civac, %0\n\tdsb sy"          (vgic.c:243)
 *   - write_cntvoff_el2()      "msr cntvoff_el2, %0\n\tisb"      (vgic.c:187)
 *   - the GICH() macro         absolute-address volatile MMIO at
 *                              VGIC_GICH_BASE — push() itself does
 *                              `GICH(GICH_HCR) |= GICH_HCR_UIE` (vgic.c:302)
 *   - vgic_st_vectors          a whole __asm__ EL1 vector table + IRQ
 *                              trampoline (vgic.c:712-762)
 *   - read_currentel()/write_vbar_el1()/"msr daifclr, #2"  (vgic.c:788-815)
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
 * THE BEHAVIOURS THIS FILE PINS, and why each is worth pinning:
 *   1. An injection with no free LR is QUEUED, NOT DROPPED (vgic.c:839-873).
 *      This is the v2 fix itself: vgic.h:216-225 records that pre-v2 the
 *      interrupt was dropped, which is "fine for an idempotent periodic tick,
 *      WRONG for arbitrary device interrupts — an edge-triggered completion
 *      IRQ dropped on the floor never comes back".
 *   2. FIFO order is preserved ACROSS the queue+LR combination: once anything
 *      is queued, even a later injection that could have gone straight into a
 *      free LR queues behind it (vgic.c:845-861's explicit rationale).
 *   3. UIE is set on empty->non-empty and cleared on non-empty->empty, never
 *      left on (vgic.c:301-302, 587-588). vgic.h:126-131 explains why a
 *      permanently-set UIE self-inflicts an INTID 25 storm: "<2 valid LRs" is
 *      the NORMAL idle state of a 4-LR GIC-400.
 *   4. Ring wraparound and true overflow are distinguished: only a full ring
 *      loses an interrupt (vg_pendq_overflow), which is a different counter
 *      from the legacy vg_inject_drop (vgic.c:295-299).
 *   5. The LR word field layout (vgic.c:336-355 against the documented layout
 *      at vgic.c:110-126), including that priority is the TOP 5 bits of the
 *      8-bit GIC priority and that HW mode carries the physical INTID.
 *   6. vgic_inject_cntv()'s one-shot gate (vgic.c:474-545): a second vINTID 27
 *      is never injected while a live one is in any LR, because two LRs with
 *      the same VirtualID is UNPREDICTABLE per the GICv2 spec. The negative
 *      control in vgic-qemu-ci.sh's own history shows what that gate looks
 *      like when delivery is broken (cntv_gate climbing once per tick), so the
 *      gate's exact boundary is worth pinning here.
 *
 * Build: gcc -o test_vgic_pendq test_vgic_pendq.c && ./test_vgic_pendq
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * GICH register block, modelled as real memory so the mirror keeps the
 * source's read-modify-write shape. Offsets mirror vgic.c:90-97.
 * ------------------------------------------------------------------ */
#define GICH_HCR    0x000u
#define GICH_VTR    0x004u
#define GICH_VMCR   0x008u
#define GICH_MISR   0x010u
#define GICH_EISR0  0x020u
#define GICH_ELRSR0 0x030u
#define GICH_LR(n)  (0x100u + 4u * (n))

static uint32_t g_gich[0x200 / 4];

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
static uint32_t *gich_ptr(unsigned reg);

#define GICH(reg)   (*gich_ptr(reg))

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
gich_ptr(unsigned reg)
{
	if (reg == GICH_ELRSR0) {
		uint32_t elrsr = 0;

		for (unsigned n = 0; n < 16u; n++) {
			uint32_t lr = g_gich[(0x100u + 4u * n) / 4u];

			if ((lr & (GICH_LR_STATE_PENDING | GICH_LR_STATE_ACTIVE)) == 0u)
				elrsr |= (1u << n);
		}
		g_gich[GICH_ELRSR0 / 4u] = elrsr;
	}
	return &g_gich[reg / 4u];
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
 * Module state mirrored from vgic.c:247-285.
 * ------------------------------------------------------------------ */
#define VGIC_PENDQ_SIZE 32u          /* vgic.c:272 */

static uint32_t vg_nr_lr = 4;        /* vgic.c:247 — GIC-400 and QEMU virt both give 4 */
static uint32_t vg_inject_count;
static uint32_t vg_inject_ok;
static uint32_t vg_inject_drop;
static uint32_t vg_cntv_inject;
static uint32_t vg_cntv_gate;
static uint32_t vg_cntv_drop;

struct vgic_pend {                   /* vgic.c:274-278 */
	uint32_t vintid;
	uint32_t pintid;
	int      priority;
};

static struct vgic_pend vg_pendq[VGIC_PENDQ_SIZE];
static uint32_t vg_pendq_head;
static uint32_t vg_pendq_tail;
static uint32_t vg_pendq_count;
static uint32_t vg_pendq_hwm;
static uint32_t vg_pendq_overflow;

/* Mirrors vgic.c:293-315 vgic_pendq_push() verbatim, minus the vg_bc()
 * breadcrumb stores (pure "dc civac" telemetry with no effect on the queue). */
static int
vgic_pendq_push(uint32_t vintid, uint32_t pintid, int priority)
{
	if (vg_pendq_count >= VGIC_PENDQ_SIZE) {
		vg_pendq_overflow++;
		return 0;
	}

	if (vg_pendq_count == 0)
		GICH(GICH_HCR) |= GICH_HCR_UIE;   /* start driving drains */

	vg_pendq[vg_pendq_head].vintid   = vintid;
	vg_pendq[vg_pendq_head].pintid   = pintid;
	vg_pendq[vg_pendq_head].priority = priority;
	vg_pendq_head = (vg_pendq_head + 1u) % VGIC_PENDQ_SIZE;
	vg_pendq_count++;
	if (vg_pendq_count > vg_pendq_hwm)
		vg_pendq_hwm = vg_pendq_count;

	return 1;
}

/* Mirrors vgic.c:318-330 vgic_pendq_pop() verbatim. */
static int
vgic_pendq_pop(uint32_t *vintid, uint32_t *pintid, int *priority)
{
	if (vg_pendq_count == 0)
		return 0;

	*vintid   = vg_pendq[vg_pendq_tail].vintid;
	*pintid   = vg_pendq[vg_pendq_tail].pintid;
	*priority = vg_pendq[vg_pendq_tail].priority;
	vg_pendq_tail = (vg_pendq_tail + 1u) % VGIC_PENDQ_SIZE;
	vg_pendq_count--;
	return 1;
}

/* Mirrors vgic.c:336-355 vgic_lr_write_hw() verbatim, minus vg_bc()/
 * flightrec_log() telemetry. */
static void
vgic_lr_write_hw(uint32_t n, uint32_t vintid, uint32_t pintid, int priority)
{
	uint32_t prio5 = ((uint32_t)priority >> 3) & 0x1fu;
	uint32_t lr = (vintid & GICH_LR_VID_MASK) |
	              ((pintid & 0x3ffu) << 10) |
	              VGIC_LR_GRP |
	              GICH_LR_HW |
	              GICH_LR_STATE_PENDING |
	              (prio5 << GICH_LR_PRIO_SHIFT);

	GICH(GICH_LR(n)) = lr;

	vg_inject_ok++;
}

/* Mirrors vgic.c:839-873 vgic_inject_hw() verbatim, minus vg_bc(). */
static void
vgic_inject_hw(uint32_t vintid, uint32_t pintid, int priority)
{
	uint32_t elrsr, n;

	vg_inject_count++;

	if (vg_pendq_count == 0) {
		elrsr = GICH(GICH_ELRSR0);
		for (n = 0; n < vg_nr_lr; n++) {
			if (elrsr & (1u << n)) {
				vgic_lr_write_hw(n, vintid, pintid, priority);
				return;
			}
		}
	}

	if (!vgic_pendq_push(vintid, pintid, priority))
		vg_inject_drop++;   /* true loss: even the pending queue was full */
}

/* Mirrors the drain half of vgic.c:547-595 vgic_maintenance(): the EISR
 * LR-clear loop (:561-563), the ELRSR-bounded drain (:571-581) and the
 * UIE-clear (:587-588). Omits only the vg_bc() breadcrumbs. */
static void
vgic_maintenance(void)
{
	uint32_t eisr, elrsr, n;

	eisr = GICH(GICH_EISR0);

	for (n = 0; n < vg_nr_lr; n++)
		if (eisr & (1u << n))
			GICH(GICH_LR(n)) = 0u;

	elrsr = GICH(GICH_ELRSR0);
	for (n = 0; n < vg_nr_lr && vg_pendq_count > 0; n++) {
		uint32_t vintid, pintid;
		int priority;

		if (!(elrsr & (1u << n)))
			continue;
		if (!vgic_pendq_pop(&vintid, &pintid, &priority))
			break;
		vgic_lr_write_hw(n, vintid, pintid, priority);
	}

	if (vg_pendq_count == 0)
		GICH(GICH_HCR) &= ~GICH_HCR_UIE;
}

/* Mirrors vgic.c:474-545 vgic_inject_cntv(), minus vg_bc()/flightrec_log()
 * and the VGIC_CNTV_HW / VGIC_CNTV_EOI_TRACE #if arms (both default to 0 per
 * vgic.h:90-103, i.e. the pure-virtual software-vtimer path this mirrors).
 * Returns 1 if a vIRQ was (re)injected, 0 if gated or dropped. */
static int
vgic_inject_cntv(void)
{
	uint32_t elrsr, n, lr, free_lr = 0xffffffffu;

	vg_inject_count++;

	elrsr = GICH(GICH_ELRSR0);
	for (n = 0; n < vg_nr_lr; n++) {
		if (elrsr & (1u << n)) {
			if (free_lr == 0xffffffffu)
				free_lr = n;
			continue;
		}
		lr = GICH(GICH_LR(n));
		if ((lr & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID) {
			vg_cntv_gate++;
			return 0;
		}
	}

	if (free_lr == 0xffffffffu) {
		vg_cntv_drop++;
		return 0;
	}

	n  = free_lr;
	lr = (VGIC_VTIMER_INTID & GICH_LR_VID_MASK) |
	     VGIC_LR_GRP |
	     GICH_LR_STATE_PENDING;   /* priority field 0 => prio5 == 0 */

	GICH(GICH_LR(n)) = lr;

	vg_cntv_inject++;
	vg_inject_ok++;
	return 1;
}

/* ==================================================================== *
 * Test-harness helpers (NOT mirrors — this is the test double for "what
 * the GIC hardware and the guest would do").
 * ==================================================================== */

/* Reset all mirrored state to a just-after-vgic_init() condition: every LR
 * invalid, so GICH_ELRSR0 shows all vg_nr_lr of them empty (vgic.c:372-373
 * clears the LRs; ELRSR is hardware-derived from that). */
static void
harness_reset(void)
{
	memset(g_gich, 0, sizeof(g_gich));
	memset(vg_pendq, 0, sizeof(vg_pendq));
	vg_pendq_head = vg_pendq_tail = vg_pendq_count = 0;
	vg_pendq_hwm = vg_pendq_overflow = 0;
	vg_inject_count = vg_inject_ok = vg_inject_drop = 0;
	vg_cntv_inject = vg_cntv_gate = vg_cntv_drop = 0;
	vg_nr_lr = 4;
	GICH(GICH_HCR) = GICH_HCR_EN;
	/* No ELRSR init needed: it is derived from the (now all-invalid) LRs,
	 * so it already reads "all empty" — see gich_ptr(). */
}

/* Model the guest retiring LR n via a virtual EOI: the LR goes invalid and
 * its ELRSR bit sets. This is what real hardware does on GICV_EOIR for a
 * pending/active LR, and it is the only reason an LR is ever reclaimed
 * (vgic.c:433-435 says exactly that). */
static void
harness_guest_eoi(uint32_t n)
{
	GICH(GICH_LR(n)) = 0u;   /* ELRSR bit n follows automatically */
}

/* Model the guest ACKing LR n (virtual IAR) without EOIing it yet: state
 * pending -> active. Still occupied, so its derived ELRSR bit stays clear —
 * "active" is emphatically not "free", which is the distinction the CNTV
 * duplicate gate depends on. */
static void
harness_guest_ack(uint32_t n)
{
	uint32_t lr = GICH(GICH_LR(n));

	lr &= ~GICH_LR_STATE_PENDING;
	lr |= GICH_LR_STATE_ACTIVE;
	GICH(GICH_LR(n)) = lr;
}

static unsigned
lr_occupied_count(void)
{
	unsigned c = 0;

	for (uint32_t n = 0; n < vg_nr_lr; n++)
		if (!(GICH(GICH_ELRSR0) & (1u << n)))
			c++;
	return c;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* The LR word vgic_lr_write_hw() builds must place every field exactly where
 * vgic.c:110-126 documents it. Priority in particular is the TOP 5 bits of the
 * 8-bit GIC priority (>>3), which is easy to get backwards and impossible to
 * notice from a "the interrupt arrived" test. */
static void test_lr_word_field_layout(void)
{
	harness_reset();

	vgic_inject_hw(/*vintid=*/106, /*pintid=*/106, /*priority=*/0xA0);

	uint32_t lr = GICH(GICH_LR(0));

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
	harness_reset();
	vgic_inject_hw(0x7FFu, 0x7FFu, 0);
	lr = GICH(GICH_LR(0));
	assert((lr & GICH_LR_VID_MASK) == 0x3FFu);
	assert(((lr >> 10) & 0x3ffu) == 0x3FFu);
}

/* THE v2 FIX: with all 4 List Registers occupied, an injection must be QUEUED,
 * not dropped. Pre-v2 this lost the interrupt outright (vgic.h:216-225). */
static void test_injection_with_no_free_lr_is_queued_not_dropped(void)
{
	harness_reset();

	/* Fill all 4 LRs with distinct device INTIDs and have the guest ACK (but
	 * not EOI) each, so every LR is genuinely occupied. */
	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}
	assert(lr_occupied_count() == 4u);
	assert(vg_inject_ok == 4u);
	assert(vg_pendq_count == 0u);
	assert(!(GICH(GICH_HCR) & GICH_HCR_UIE));   /* nothing queued yet */

	/* The 5th injection has nowhere to go. */
	vgic_inject_hw(50u, 50u, 0);

	assert(vg_pendq_count == 1u);        /* queued ... */
	assert(vg_pendq_overflow == 0u);     /* ... and NOT lost */
	assert(vg_inject_drop == 0u);
	assert(vg_inject_ok == 4u);          /* no new LR was written */
	/* Empty -> non-empty must arm the underflow maintenance IRQ, which is
	 * the only thing that will ever drive the drain (vgic.c:301-302). */
	assert(GICH(GICH_HCR) & GICH_HCR_UIE);
}

/* FIFO across queue+LR: once ANYTHING is queued, a later injection must queue
 * behind it even if an LR is free by then — vgic.c:845-861's explicit reason
 * for gating the fast path on vg_pendq_count == 0. */
static void test_fifo_order_across_queue_and_lrs(void)
{
	harness_reset();

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}

	vgic_inject_hw(60u, 60u, 0);         /* queued (position 0) */
	assert(vg_pendq_count == 1u);

	/* An LR frees up, but no maintenance drain has run yet. */
	harness_guest_eoi(1u);
	assert(lr_occupied_count() == 3u);

	/* This injection COULD take LR 1 — it must not, or it would overtake
	 * vINTID 60 which has been waiting longer. */
	vgic_inject_hw(61u, 61u, 0);
	assert(vg_pendq_count == 2u);
	assert(GICH(GICH_LR(1)) == 0u);      /* LR 1 still untouched */

	/* Now drain. 60 must land before 61. */
	vgic_maintenance();
	assert((GICH(GICH_LR(1)) & GICH_LR_VID_MASK) == 60u);
	assert(vg_pendq_count == 1u);        /* only one LR was free */
	assert(GICH(GICH_HCR) & GICH_HCR_UIE);   /* backlog remains => stay armed */

	harness_guest_eoi(2u);
	vgic_maintenance();
	assert((GICH(GICH_LR(2)) & GICH_LR_VID_MASK) == 61u);
	assert(vg_pendq_count == 0u);
	/* Queue empty again => UIE must be cleared, or INTID 25 storms at idle
	 * (vgic.h:126-131). */
	assert(!(GICH(GICH_HCR) & GICH_HCR_UIE));
}

/* The drain is bounded by the number of FREE List Registers, never by queue
 * depth, and it must not touch occupied LRs (vgic.c:572-581). */
static void test_maintenance_drain_is_bounded_by_free_lrs(void)
{
	harness_reset();

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}
	for (uint32_t i = 0; i < 10u; i++)
		vgic_inject_hw(70u + i, 70u + i, 0);
	assert(vg_pendq_count == 10u);
	assert(vg_pendq_hwm == 10u);

	/* Free exactly two LRs, then drain: exactly two entries may leave the
	 * queue, and the still-occupied LRs must be unchanged. */
	uint32_t lr0_before = GICH(GICH_LR(0));
	uint32_t lr3_before = GICH(GICH_LR(3));
	harness_guest_eoi(1u);
	harness_guest_eoi(2u);

	vgic_maintenance();

	assert(vg_pendq_count == 8u);
	assert((GICH(GICH_LR(1)) & GICH_LR_VID_MASK) == 70u);
	assert((GICH(GICH_LR(2)) & GICH_LR_VID_MASK) == 71u);
	assert(GICH(GICH_LR(0)) == lr0_before);
	assert(GICH(GICH_LR(3)) == lr3_before);
	assert(GICH(GICH_HCR) & GICH_HCR_UIE);
}

/* Ring wraparound: pushing/popping more than VGIC_PENDQ_SIZE entries over the
 * life of the queue must keep FIFO order and never alias entries. */
static void test_pendq_wraparound_preserves_fifo(void)
{
	harness_reset();

	/* Occupy all LRs so everything queues. */
	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}

	uint32_t next_expect = 100u;
	uint32_t next_push   = 100u;

	/* 5 rounds of (push 20, drain 20) — 100 entries total through a 32-slot
	 * ring, so head/tail wrap several times. */
	for (uint32_t round = 0; round < 5u; round++) {
		for (uint32_t i = 0; i < 20u; i++)
			vgic_inject_hw(next_push++, 0u, 0);
		assert(vg_pendq_count == 20u);
		assert(vg_pendq_overflow == 0u);

		for (uint32_t i = 0; i < 20u; i++) {
			uint32_t v, p;
			int prio;
			assert(vgic_pendq_pop(&v, &p, &prio) == 1);
			assert(v == next_expect);   /* strict FIFO across the wrap */
			next_expect++;
		}
		assert(vg_pendq_count == 0u);
	}
	assert(next_expect == 200u);
	assert(vg_pendq_hwm == 20u);
}

/* Only a FULL ring actually loses an interrupt, and that loss is counted in
 * vg_pendq_overflow — a distinct counter from the legacy vg_inject_drop, so a
 * real overrun stays distinguishable from ordinary backlog (vgic.c:295-299,
 * vgic.h:219-225). */
static void test_pendq_overflow_is_the_only_true_loss(void)
{
	harness_reset();

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}

	/* Exactly VGIC_PENDQ_SIZE injections fit. */
	for (uint32_t i = 0; i < VGIC_PENDQ_SIZE; i++)
		vgic_inject_hw(200u + i, 0u, 0);
	assert(vg_pendq_count == VGIC_PENDQ_SIZE);
	assert(vg_pendq_hwm == VGIC_PENDQ_SIZE);
	assert(vg_pendq_overflow == 0u);
	assert(vg_inject_drop == 0u);

	/* The next one is genuinely lost, and says so. */
	vgic_inject_hw(999u, 0u, 0);
	assert(vg_pendq_overflow == 1u);
	assert(vg_inject_drop == 1u);
	assert(vg_pendq_count == VGIC_PENDQ_SIZE);   /* queue not corrupted */

	/* The head of the queue must still be the FIRST thing pushed, i.e. the
	 * overflow rejected the newcomer rather than evicting an older entry. */
	uint32_t v, p;
	int prio;
	assert(vgic_pendq_pop(&v, &p, &prio) == 1);
	assert(v == 200u);
}

/* vgic_inject_cntv()'s one-shot gate: never a second live vINTID 27, because
 * two LRs with the same VirtualID is UNPREDICTABLE per the GICv2 spec
 * (vgic.c:480-486). Gate must trigger for a PENDING copy and for an ACTIVE
 * one, and must release as soon as the guest EOIs. */
static void test_cntv_duplicate_gate(void)
{
	harness_reset();

	assert(vgic_inject_cntv() == 1);
	assert(vg_cntv_inject == 1u);
	assert((GICH(GICH_LR(0)) & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID);

	/* Still PENDING (guest hasn't even ACKed): gate. */
	assert(vgic_inject_cntv() == 0);
	assert(vg_cntv_gate == 1u);
	assert(vg_cntv_inject == 1u);

	/* ACKed but not EOIed (state ACTIVE): still a live 27 => still gate.
	 * This is the case that matters — an "active" LR is not "free". */
	harness_guest_ack(0u);
	assert(vgic_inject_cntv() == 0);
	assert(vg_cntv_gate == 2u);

	/* Guest EOIs: LR reclaimed, next tick injects again. This is exactly the
	 * loop vgic-qemu-ci.sh observes 10 times against real hardware. */
	harness_guest_eoi(0u);
	assert(vgic_inject_cntv() == 1);
	assert(vg_cntv_inject == 2u);
	assert(vg_cntv_gate == 2u);
}

/* The gate keys on the vINTID, not on "any occupied LR": with all 4 LRs held
 * by OTHER vINTIDs the tick is DROPPED (counted separately as cntv_drop, since
 * a periodic tick is idempotent), not gated and not queued (vgic.c:505-511). */
static void test_cntv_drop_when_all_lrs_hold_other_vintids(void)
{
	harness_reset();

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}

	assert(vgic_inject_cntv() == 0);
	assert(vg_cntv_drop == 1u);
	assert(vg_cntv_gate == 0u);      /* no vINTID 27 was present */
	assert(vg_cntv_inject == 0u);
	assert(vg_pendq_count == 0u);    /* the CNTV path does not use the queue */

	/* One LR frees: the tick lands there. */
	harness_guest_eoi(2u);
	assert(vgic_inject_cntv() == 1);
	assert((GICH(GICH_LR(2)) & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID);
}

/* vgic_maintenance()'s EISR loop must invalidate exactly the LRs the guest
 * EOIed via the EOI-maintenance path, and leave the others alone
 * (vgic.c:561-563). Normally a no-op for HW-mode LRs, kept for safety. */
static void test_maintenance_clears_only_eisr_flagged_lrs(void)
{
	harness_reset();

	for (uint32_t i = 0; i < 4u; i++) {
		vgic_inject_hw(40u + i, 40u + i, 0);
		harness_guest_ack(i);
	}
	uint32_t lr0 = GICH(GICH_LR(0));
	uint32_t lr2 = GICH(GICH_LR(2));

	GICH(GICH_EISR0) = (1u << 1) | (1u << 3);
	vgic_maintenance();

	assert(GICH(GICH_LR(1)) == 0u);
	assert(GICH(GICH_LR(3)) == 0u);
	assert(GICH(GICH_LR(0)) == lr0);
	assert(GICH(GICH_LR(2)) == lr2);
	/* Nothing was queued, so UIE must be (still) clear. */
	assert(!(GICH(GICH_HCR) & GICH_HCR_UIE));
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
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- vgic pendq/LR tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
