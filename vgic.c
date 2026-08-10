/* SPDX-License-Identifier: BSD-2-Clause */

/* vgic.c — GICv2 (GIC-400) virtualization for the bzdOS EL2 hypervisor on the
 * Allwinner A64. See vgic.h for the cited GIC-400 memory map and the API
 * contract; this file is the implementation plus the exact register-bit
 * rationale.
 *
 * ================================================================
 *  What this file drives, and what it deliberately does NOT touch
 * ================================================================
 * DRIVES: GICH (hypervisor control interface, 0x01c84000) — HCR/VTR/VMCR/
 *   MISR/EISR/ELRSR/LR<n> — and CNTVOFF_EL2. This is EL2-only state the guest
 *   can never see.
 * DOES NOT TOUCH: GICV (0x01c86000). The guest reaches GICV through the
 *   stage-2 mapping of its GICC IPA (0x01c82000 -> 0x01c86000, wired in
 *   stage2.c's stage2_build_mmio_tables()); we never program GICV from here —
 *   that would fight the guest. GICD (0x01c81000) is passed through /
 *   identity-mapped to the guest — a DELIBERATE choice, not an oversight:
 *   vgic_gicd_* below are ready-but-INERT trap-emulate hooks for a later
 *   milestone, and they must stay inert here, because vgic_gicd_write()
 *   today only mutates a private SRAM shadow completely disconnected from
 *   the real hardware. Trapping GICD now (with that stub) would let the
 *   guest's own GICD driver "successfully" write ISENABLER/IPRIORITYR/
 *   IGROUPR into the shadow while the REAL distributor — the one
 *   HCR_EL2.IMO=1 depends on to ever hand EL2 a device's physical IRQ in the
 *   first place — never sees those writes, silently breaking every device
 *   interrupt the guest tries to enable. GICD stays pass-through until a
 *   real read-modify-shadow-then-write-through GICD emulation replaces the
 *   current stub. The ONE exception vgic_init() makes to "GICD is the
 *   guest's business" is enabling the maintenance PPI (INTID 25) at the
 *   real distributor — that interrupt is never guest-visible or
 *   guest-programmed, so it doesn't conflict with pass-through GICD at all.
 * DRIVES the physical GICC path only indirectly: gic_timer_irq() (gic_timer.c)
 *   is the one place that reads GICC_IAR/writes GICC_EOIR/DIR — this file
 *   never touches GICC directly. What THIS file adds on top is: (a) the List
 *   Registers that turn "gic_timer_irq() got some physical INTID" into "the
 *   guest sees that INTID as a virtual IRQ, HW-tied so the guest's own
 *   virtual EOI deactivates the real physical source", and (b) the
 *   LR-exhaustion pending queue + maintenance-IRQ-driven drain below, so a
 *   burst bigger than 4 List Registers is queued, never silently dropped.
 *
 * ================================================================
 *  Delivery chain (why a virtual IRQ actually reaches the guest)
 * ================================================================
 *   1. main_dbg.c sets HCR_EL2.IMO=1 (+FMO=1): every physical IRQ/FIQ is now
 *      routed to and taken at EL2, whether it's the guest's own device (an
 *      SPI), its virtual timer's underlying PPI (CNTV, INTID 27), or
 *      anything else — nothing reaches EL1 as a real physical IRQ anymore.
 *   2. vgic_init() sets GICH_HCR.En=1 (virtual interface on), presets VMCR
 *      (Group 1 enabled, virtual PMR wide open), CNTVOFF_EL2=0, and enables
 *      the maintenance PPI (INTID 25) at the real distributor (UIE itself
 *      stays clear until there's an actual backlog — see VGIC_MAINT_INTID).
 *   3. gic_timer_irq() (gic_timer.c) reads the real GICC_IAR for EVERY
 *      physical IRQ EL2 now takes, EOIs it (priority-drop only — see below),
 *      and calls vgic_inject_hw(intid, intid, prio) — this file's job, from
 *      here down.
 *   4. vgic_inject_hw() finds a free List Register (GICH_ELRSR0) and writes
 *      it HW=1 (tied to the physical INTID), Group 1, pending. If no LR is
 *      free, it queues the request instead of dropping it (see the
 *      LR-exhaustion pending queue below) and enables GICH_HCR.UIE so the
 *      underflow maintenance IRQ drives the eventual drain.
 *   5. GICV sees a pending Group-1 vIRQ above the virtual PMR/running-priority
 *      and asserts the virtual IRQ line. The guest at EL1 takes it through
 *      its own VBAR_EL1 (redirected "GICC" IPA -> GICV via stage-2), reads
 *      the virtual IAR, services the device, and writes the virtual EOIR.
 *   6. Because the LR was HW=1, that guest EOI does TWO things atomically in
 *      hardware: it invalidates the LR (freeing it for reuse) AND
 *      deactivates the tied PHYSICAL interrupt — exactly the "the guest's
 *      own driver clears the level source" step that was missing in the two
 *      prior (reverted) attempts, which is why a level-triggered IRQ
 *      (EHCI/INTID 106) stormed at ~145 kHz: EL2 EOI'd *and* DIR'd it itself
 *      without ever letting the guest's driver see/clear it, so the level
 *      source re-asserted forever. gic_timer_irq() here EOIs (priority-drop)
 *      but deliberately NEVER DIRs an HW-mode-injected interrupt — DIR is
 *      hardware's job now, triggered by the guest's virtual EOI.
 * Freestanding, no libc: <stdint.h> only. Cache-coherent breadcrumb stores
 * (dc civac + dsb sy), same convention as every other lane in this tree.
 */
#include <stdint.h>
#include "vgic.h"
#include "guest.h"   /* guest_config(), guest_enter() for the self-test */
#include "flightrec.h"  /* B4: flightrec_log(FLTR_K_IRQ, ...) on every injected LR */
#include "smp.h"     /* SMP_MAX_CPUS, smp_cpu_id() — Phase 2 P1 per-core
                      * state, see the inventory comment above struct
                      * vg_percpu below. Header-only (smp_cpu_id() is
                      * `static inline`): no new link dependency on smp.o,
                      * so main_vgic_qemu.c's target (which links vgic_qemu.o
                      * — this same source — but NOT smp.o) still links. */

/* ================================================================
 *  GICH — hypervisor control interface register block (0x01c84000).
 *  Offsets are architectural (ARM IHI 0048 GICv2, section on the virtual
 *  interface control registers).
 * ================================================================ */
#define GICH(reg)   (*(volatile uint32_t *)(VGIC_GICH_BASE + (reg)))
#define GICH_HCR    0x000u   /* Hypervisor Control Register            */
#define GICH_VTR    0x004u   /* VGIC Type Register (ListRegs-1 in [5:0])*/
#define GICH_VMCR   0x008u   /* Virtual Machine Control (aliases GICV)  */
#define GICH_MISR   0x010u   /* Maintenance Interrupt Status Register   */
#define GICH_EISR0  0x020u   /* End-of-Interrupt Status Register 0      */
#define GICH_ELRSR0 0x030u   /* Empty List-register Status Register 0   */
#define GICH_APR0   0x0F0u   /* Active Priorities Register 0            */
#define GICH_LR(n)  (0x100u + 4u * (n))   /* List Registers 0..N        */

/* GICH_HCR bits. En is the only one v1 sets — no maintenance IRQ (UIE/NPIE/
 * LRENPIE/EOI-count) so INTID 25 is never generated and cannot storm. */
#define GICH_HCR_EN      (1u << 0)
#define GICH_HCR_UIE     (1u << 1)   /* underflow maint. IRQ (v2, unused)   */
#define GICH_HCR_LRENPIE (1u << 2)
#define GICH_HCR_NPIE    (1u << 3)

/* GICH_MISR bits (maintenance cause, read in vgic_maintenance). */
#define GICH_MISR_EOI    (1u << 0)
#define GICH_MISR_U      (1u << 1)

/* GICH_LR<n> field layout (GICv2, 32-bit LR):
 *   [9:0]   VirtualID    the vINTID delivered to the guest
 *   [19:10] PhysicalID   HW=1: physical INTID to deactivate on guest EOI;
 *                        HW=0: bit[19]=EOI (maintenance-on-EOI), rest reserved
 *   [22:20] reserved
 *   [27:23] Priority     top 5 bits of the 8-bit GIC priority
 *   [29:28] State        00 invalid, 01 pending, 10 active, 11 pending+active
 *   [30]    Grp1         0 = Group 0 (signalled vFIQ), 1 = Group 1 (vIRQ)
 *   [31]    HW           0 = pure virtual (v1), 1 = tied to a physical INTID
 * We inject pure-virtual (HW=0) Group-1 pending interrupts. */
#define GICH_LR_VID_MASK       0x3ffu
#define GICH_LR_PRIO_SHIFT     23
#define GICH_LR_STATE_PENDING  (1u << 28)
#define GICH_LR_STATE_ACTIVE   (1u << 29)
#define GICH_LR_GRP1           (1u << 30)
#define GICH_LR_HW             (1u << 31)
#define GICH_LR_EOI            (1u << 19)   /* HW=0 EOI-maintenance request */

/* GICH_VMCR bits (a snapshot of the virtual GICC control the guest sees;
 * the guest's own writes to GICV_CTLR/PMR update these too). We preset a
 * permissive default so even a guest that under-programs the interface still
 * receives our injected Grp-1 tick.
 *   [0] VENG0  virtual EnableGrp0
 *   [1] VENG1  virtual EnableGrp1     <- set
 *   [4] VCBPR
 *   [31:27] VPMR  virtual priority mask (top 5 bits of GICV_PMR) <- 0x1f */
#define GICH_VMCR_VENG0        (1u << 0)
#define GICH_VMCR_VENG1        (1u << 1)
#define GICH_VMCR_VPMR_SHIFT   27
#define GICH_VMCR_VPMR_OPEN    (0x1fu << GICH_VMCR_VPMR_SHIFT)

/* Group selection for injected vIRQs and the preset VMCR (see vgic.h's
 * VGIC_GROUP0 toggle). Under GICv2 the VIRTUAL interface has no security
 * banking, so a guest that thinks it enables "Grp1" via GICC_CTLR bit0 may
 * actually be enabling Grp0 on GICV; VGIC_GROUP0=1 sides with that reality
 * (KVM-style) by injecting Grp0 and presetting both group-enables in VMCR. */
#if VGIC_GROUP0
#define VGIC_LR_GRP     0u
#define VGIC_VMCR_GRP   (GICH_VMCR_VENG0 | GICH_VMCR_VENG1)
#else
#define VGIC_LR_GRP     GICH_LR_GRP1
#define VGIC_VMCR_GRP   GICH_VMCR_VENG1
#endif

/* GICv2 spurious range. */
#define GIC_SPURIOUS_MIN  1020u

/* ================================================================
 *  GICD (distributor) — the ONLY distributor writes vgic_init() makes,
 *  despite this file's "GICD is pass-through/guest's business" header
 *  comment (that comment is about the guest-visible device-interrupt
 *  configuration and the inert shadow/trap-emulate path further down,
 *  neither of which this touches). vgic_init() enables/groups/prioritizes
 *  the MAINTENANCE PPI (INTID 25) here — same NON-SECURE Group-1 offsets
 *  gic_timer.c uses for INTID 30 (CNTP) — because that interrupt is never
 *  guest-visible or guest-programmed (it exists purely to tell EL2 "an LR
 *  just freed up", driven by GICH_HCR.UIE, which THIS file toggles), so
 *  enabling it here doesn't touch anything the guest's own GICD driver
 *  owns. CNTV's own physical PPI (INTID 27) is DELIBERATELY NOT enabled
 *  here (see the comment above this block, just below vgic_init()'s body)
 *  — that one IS guest-owned state and enabling it before the guest
 *  programs CNTV_CVAL_EL0 would storm. GICC_PMR/GICC_CTLR are
 *  CPU-interface-wide (not per-INTID); main_dbg.c's gic_timer_cpuif_init()
 *  call already opens those before vgic_init() runs, so nothing more is
 *  needed here for INTID 25 either.
 * ================================================================ */
#define VGIC_GICD(reg)        (*(volatile uint32_t *)(VGIC_GICD_BASE + (reg)))
#define VGIC_GICD_IGROUPR0    0x080u
#define VGIC_GICD_ISENABLER0  0x100u
#define VGIC_GICD_IPRIORITYR_BYTE(id) \
	(*(volatile uint8_t *)(VGIC_GICD_BASE + 0x400u + (id)))

/* ================================================================
 *  CNTVOFF_EL2 — virtual counter offset. 0 => guest CNTVCT == CNTPCT.
 * ================================================================ */
static inline void write_cntvoff_el2(uint64_t v)
{
	__asm__ volatile("msr cntvoff_el2, %0\n\tisb" :: "r"(v) : "memory");
}

/* ================================================================
 *  Breadcrumb window: 0x50001c00 ("VGIC"). Sits between the vconsole ring
 *  header and gtrace (0x50002000).
 *
 *  HISTORY, because this comment was wrong for months: it used to claim
 *  "vconsole (ends 0x50001b10)". That held only while the capture ring was
 *  3 KiB. When it grew to 64 KiB the buffer stayed glued at 0x50000f10 and ran
 *  to 0x50010f10 — straight through THIS window. Read live on 2026-08-01, the
 *  words below held console text, not the magic, so anything read out of here
 *  was console bytes reinterpreted as GICH state. Project memory records a
 *  RETRACTED vtimer theory built on readings from this window. The buffer now
 *  lives at 0x50040000 (see vconsole.h) and this window is real again. Layout:
 *   [0]  magic 0x56474943 ("VGIC")
 *   [1]  vtr           raw GICH_VTR
 *   [2]  nr_lr         number of List Registers (VTR[5:0]+1; GIC-400 => 4)
 *   [3]  hcr           GICH_HCR readback after En
 *   [4]  inject_count  total vgic_inject() calls
 *   [5]  inject_ok     injects that found a free LR
 *   [6]  inject_drop   injects with no free LR (guest behind)
 *   [7]  last_lr       last LR word written
 *   [8]  last_elrsr    last GICH_ELRSR0 sampled at inject
 *   [9]  maint_count   vgic_maintenance() calls
 *   [10] last_misr     last GICH_MISR
 *   [11] last_eisr     last GICH_EISR0
 *   [12] cntvoff_set   1 once CNTVOFF_EL2=0 written
 *   [13] vmcr          GICH_VMCR readback after preset
 *   [14] gicd_isenabler0  real GICD_ISENABLER0 readback (bit27 must be 1 -
 *                      proves CNTV's PPI is actually enabled at the
 *                      distributor, not just the virtual CPU interface)
 *   --- v2 (interrupt-virtualization milestone) additions -------------
 *   [15] pendq_count   current depth of the LR-exhaustion pending queue
 *   [16] pendq_hwm     high-water mark ever reached by pendq_count
 *   [17] pendq_overflow  injections actually LOST (the pending queue itself
 *                      was full — distinct from inject_drop above, which
 *                      pre-v2 meant "no free LR, dropped"; that case now
 *                      queues instead of dropping)
 *   [18] uie_state     last GICH_HCR readback (bit1 = UIE, toggled live as
 *                      the pending queue fills/drains)
 *   [19] maint_intid_en  GICD_ISENABLER0 readback (bit25 must be 1 — the
 *                      maintenance PPI is enabled at the real distributor)
 * ================================================================ */
/* 0x50001c00, matching the window documented just above (and smp.h / hv_addrmap.h).
 * Was 0x00018000 — guest-writable SRAM, next to start.S's boot breadcrumbs; a
 * latent footgun since vgic is currently off the boot path (IMO=0, vgic
 * reverted), so this never fired, but it would have clobbered/been-clobbered
 * the instant vgic is revisited. */
#define VGIC_BC_BASE   0x50001c00UL
#define VGIC_BC_MAGIC  0x56474943u   /* "VGIC" */

static inline void vg_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VGIC_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* VGIC_PENDQ_SIZE / struct vgic_pend moved up here (were originally just
 * above vgic_pendq_push(), see the "LR-exhaustion pending-injection queue"
 * comment further down for the design rationale) because struct vg_percpu
 * below embeds a fixed-size array of them — the type and the size just need
 * to exist before that point, textually. */
#define VGIC_PENDQ_SIZE 32u

struct vgic_pend {
	uint32_t vintid;
	uint32_t pintid;
	int      priority;
};

/* ------------------------------------------------------------------ *
 * Per-core module state (Phase 2 step P1, docs/phase2-all-cores.md §1.6).
 *
 * STATE INVENTORY. Every mutable module-level variable this file had before
 * this change, cited against the pre-P1 tree, and the per-core/global
 * verdict for each:
 *
 *   vg_nr_lr           (vgic.c:247) PER-CORE. Refined from THIS core's own
 *                       GICH_VTR readback in vgic_init() — GICH is EL2-only,
 *                       per-PE-banked hypervisor state (this file's own
 *                       header, "DRIVES: GICH ... EL2-only state the guest
 *                       can never see"; gic_timer.c:445-449 independently
 *                       states "GICH_* and HCR_EL2 are banked per PE").
 *                       Every core that calls vgic_init() programs and
 *                       reads back ITS OWN GICH.
 *   vg_inject_count,   (vgic.c:248-251) PER-CORE. Counters of events THIS
 *   vg_inject_ok,          core's own IRQ handling produced — an injection
 *   vg_inject_drop,        or a maintenance pass on CPU2 is not a fact about
 *   vg_maint_count          CPU0's GICH.
 *   vg_active          (vgic.c:252) PER-CORE. Gates whether THIS core's
 *                       gic_timer_irq() should treat physical IRQs as
 *                       vgic-forwarded. Must be per-core: a core that has
 *                       never called its own vgic_init() must not inherit
 *                       "true" from a different core that has.
 *   vg_cntv_inject,    (vgic.c:253-255) PER-CORE. Counters of THIS core's
 *   vg_cntv_gate,          own guest's CNTV injection attempts — CNTV_CTL_EL0
 *   vg_cntv_drop            is itself banked per PE (gic_timer.c's own
 *                       vtimer_mask_watchdog() rationale), so there is no
 *                       single "the" CNTV state to begin with.
 *   vg_pendq[],        (vgic.c:280-285) PER-CORE. The LR-exhaustion pending
 *   vg_pendq_head/          queue exists to feed List Registers, and List
 *   tail/count/hwm/         Registers are the same per-PE-banked GICH state
 *   overflow                as vg_nr_lr above. A queue meant to feed only
 *                       one core's LRs must not be shared: two cores
 *                       injecting concurrently would interleave push/pop on
 *                       one queue and could hand core A's pending interrupt
 *                       to core B's List Register (exactly the corruption
 *                       docs/phase2-all-cores.md §1.6 warns about).
 *
 * global-but-inert (deliberately NOT converted):
 *   vgic_gicd_shadow[0x1000]  (vgic.c:606, further down this file) is a
 *                       shadow of the (single, shared) physical distributor
 *                       page for the GICD trap-and-emulate helpers. Left
 *                       GLOBAL, and this is a considered decision, not an
 *                       oversight, for two independent reasons: (1) GICD is
 *                       real, mostly-shared hardware — SPI configuration
 *                       (INTID >= 32) is genuinely system-wide, only the
 *                       PPI/SGI range (0..31) is per-PE-banked in real
 *                       GICv2 silicon, so "one struct per core" would be the
 *                       WRONG model for most of this page, not merely an
 *                       unnecessary one; a correct per-core split would need
 *                       to model that mixed banking, which is real design
 *                       work belonging to whoever actually wires this up.
 *                       (2) it is currently 100% dead: vgic_gicd_read(),
 *                       vgic_gicd_write() and vgic_gicd_fault() have ZERO
 *                       callers anywhere in this tree (grepped before this
 *                       change) — "v1: present but not wired" per this
 *                       file's own header. Converting untested, unreachable
 *                       code is speculative complexity this pass explicitly
 *                       avoids (see ORIENTATION.md's task scope: "nothing else").
 *                       Flagged here for whoever activates it later.
 *
 * All the PER-CORE state above moves into struct vg_percpu below, one
 * instance per core, indexed by smp_cpu_id() via vg_self().
 *
 * On CPU0 alone — every board target today; vgic_init() is called only once,
 * from main_dbg.c's single-core boot path, before smp_init() ever starts a
 * secondary (grepped before this change) — the index is always 0, so this
 * is a pure storage-layout change with byte-identical behaviour. Cache-line-
 * padded (64 B) exactly like smp.c's own per-core pattern (struct
 * smp_percpu, smp.c:183-188) and gic_timer.c's new struct gt_percpu (this
 * same P1 change), so a future second core running vgic_init() never shares
 * a cache line with CPU0's copy.
 *
 * BREADCRUMB NOTE (hv_addrmap.h / the breadcrumb-window-hygiene discipline).
 * VGIC_BC_BASE (0x50001c00) stays a SINGLE, file-scope window — NOT split
 * per core — for the same reason as gic_timer.c's GICT/IRQ_COUNTER windows:
 * nothing in the current tree ever calls vgic_init()/vgic_inject_hw()/etc.
 * from any core but CPU0, so there is exactly one writer today, and
 * splitting the window now would be speculative complexity with no way to
 * exercise it. Whichever later Phase-2 step actually runs the vGIC on a
 * second core MUST give it its own breadcrumb lane first — allocated in
 * hv_addrmap.h's _Static_assert chain, never squeezed into a neighbour's
 * space. Flagged here, not fixed here.
 * ------------------------------------------------------------------ */
struct vg_percpu {
	uint32_t nr_lr;               /* refined from GICH_VTR in vgic_init */
	uint32_t inject_count;
	uint32_t inject_ok;
	uint32_t inject_drop;
	uint32_t maint_count;
	uint32_t active;              /* 1 once vgic_init() has completed */
	uint32_t cntv_inject;         /* CNTV ticks actually placed in an LR   */
	uint32_t cntv_gate;           /* CNTV ticks skipped: a live 27 LR held */
	uint32_t cntv_drop;           /* CNTV ticks dropped: no free LR         */
	struct vgic_pend pendq[VGIC_PENDQ_SIZE];
	uint32_t pendq_head;          /* next slot to push into */
	uint32_t pendq_tail;          /* next slot to pop from   */
	uint32_t pendq_count;
	uint32_t pendq_hwm;           /* high-water mark, diagnostic only */
	uint32_t pendq_overflow;      /* queue itself was full: TRUE loss  */
} __attribute__((aligned(64)));

static struct vg_percpu g_vg[SMP_MAX_CPUS];

/* Defensive clamp only — same posture as gic_timer.c's gt_self() (see that
 * file's comment): MPIDR affinity0 on this SoC is architecturally 0..3
 * (SMP_MAX_CPUS==4, smp.h:42), so the out-of-range arm can never be hit on
 * real hardware or under QEMU virt (single CPU => index 0). */
static inline struct vg_percpu *vg_self(void)
{
	uint32_t cpu = smp_cpu_id();

	return &g_vg[(cpu < SMP_MAX_CPUS) ? cpu : 0u];
}

/* ================================================================
 *  LR-exhaustion pending-injection queue (v2, interrupt-virtualization
 *  milestone). A GIC-400 has only vg_nr_lr (4) List Registers; under real
 *  load (a burst of device SPIs, the guest's own CNTV tick, etc.) all 4 can
 *  be occupied at once. Before v2, vgic_inject_hw() simply DROPPED the
 *  interrupt in that case (fine for an idempotent periodic tick, WRONG for
 *  arbitrary device interrupts — an edge-triggered completion IRQ dropped on
 *  the floor never comes back). This fixed-size ring makes that loss
 *  bounded-but-vanishingly-rare instead of routine: vgic_inject_hw() queues
 *  here instead of dropping, and vgic_maintenance() (driven by the
 *  GICH_HCR.UIE underflow maintenance IRQ, INTID 25 — see vgic.h) drains the
 *  queue into List Registers as they free up. Only if THIS ring is also
 *  full (VGIC_PENDQ_SIZE simultaneously-backlogged interrupts on top of 4
 *  already-occupied LRs) is an injection ever actually lost — see
 *  vg_pendq_overflow. No allocation, fixed array, bounded loops only.
 *  (VGIC_PENDQ_SIZE / struct vgic_pend themselves now live just above the
 *  struct vg_percpu state-inventory comment — see there for why.) */

/* Push one (vintid,pintid,priority) onto `vg`'s pending queue. Returns 1 if
 * queued, 0 if the queue itself was full (counted in vg->pendq_overflow —
 * the only case where an interrupt is actually lost in v2). Enables
 * GICH_HCR.UIE the moment the queue transitions from empty to non-empty, so
 * the underflow maintenance IRQ starts firing to drive draining — see the
 * file-header rationale for why UIE is never left on unconditionally.
 * `vg` is always the CALLING core's own slot (see struct vg_percpu's
 * inventory comment for why this queue is per-core): passed in rather than
 * re-derived so a single logical push/pop/drain sequence never risks
 * mixing two different lookups of "the current core" together. */
static int vgic_pendq_push(struct vg_percpu *vg, uint32_t vintid,
                            uint32_t pintid, int priority)
{
	if (vg->pendq_count >= VGIC_PENDQ_SIZE) {
		vg->pendq_overflow++;
		vg_bc(17, vg->pendq_overflow);
		return 0;
	}

	if (vg->pendq_count == 0)
		GICH(GICH_HCR) |= GICH_HCR_UIE;   /* start driving drains */

	vg->pendq[vg->pendq_head].vintid   = vintid;
	vg->pendq[vg->pendq_head].pintid   = pintid;
	vg->pendq[vg->pendq_head].priority = priority;
	vg->pendq_head = (vg->pendq_head + 1u) % VGIC_PENDQ_SIZE;
	vg->pendq_count++;
	if (vg->pendq_count > vg->pendq_hwm)
		vg->pendq_hwm = vg->pendq_count;

	vg_bc(15, vg->pendq_count);
	vg_bc(16, vg->pendq_hwm);
	return 1;
}

/* Pop the oldest queued entry from `vg`'s queue. Returns 0 if empty. */
static int vgic_pendq_pop(struct vg_percpu *vg, uint32_t *vintid,
                           uint32_t *pintid, int *priority)
{
	if (vg->pendq_count == 0)
		return 0;

	*vintid   = vg->pendq[vg->pendq_tail].vintid;
	*pintid   = vg->pendq[vg->pendq_tail].pintid;
	*priority = vg->pendq[vg->pendq_tail].priority;
	vg->pendq_tail = (vg->pendq_tail + 1u) % VGIC_PENDQ_SIZE;
	vg->pendq_count--;
	vg_bc(15, vg->pendq_count);
	return 1;
}

/* Write one List Register in HW mode (tied to physical INTID `pintid`) —
 * the single LR-programming step shared by vgic_inject_hw()'s fast path and
 * vgic_maintenance()'s drain path, so the field layout is defined exactly
 * once. Caller has already confirmed LR `n` is free (GICH_ELRSR0 bit set),
 * on `vg`'s (the calling core's) own GICH — see the vg_nr_lr inventory note:
 * List Registers are per-PE-banked hardware, so "LR n" only means the same
 * physical register if `vg` is genuinely the executing core's slot. */
static void vgic_lr_write_hw(struct vg_percpu *vg, uint32_t n, uint32_t vintid,
                              uint32_t pintid, int priority)
{
	uint32_t prio5 = ((uint32_t)priority >> 3) & 0x1fu;
	uint32_t lr = (vintid & GICH_LR_VID_MASK) |
	              ((pintid & 0x3ffu) << 10) |
	              VGIC_LR_GRP |
	              GICH_LR_HW |
	              GICH_LR_STATE_PENDING |
	              (prio5 << GICH_LR_PRIO_SHIFT);

	GICH(GICH_LR(n)) = lr;

	vg->inject_ok++;
	vg_bc(4, vg->inject_count);
	vg_bc(5, vg->inject_ok);
	vg_bc(7, lr);
	/* B4 flight recorder: a0=vintid(==pintid for HW mode), a1=LR word. */
	flightrec_log(FLTR_K_IRQ, vintid, lr);
}

/* ================================================================
 *  Public API
 * ================================================================ */
void vgic_init(void)
{
	struct vg_percpu *vg = vg_self();
	uint32_t vtr, i, vmcr;

	vg_bc(0, VGIC_BC_MAGIC);

	vtr = GICH(GICH_VTR);
	vg->nr_lr = (vtr & 0x3fu) + 1u;   /* VTR[5:0] = ListRegs - 1 */
	if (vg->nr_lr > 16u)
		vg->nr_lr = 16u;

	/* Start from a clean slate: every LR invalid (state 00). */
	for (i = 0; i < vg->nr_lr; i++)
		GICH(GICH_LR(i)) = 0u;

	/* Preset the virtual GICC control the guest sees: Group 1 enabled,
	 * virtual PMR wide open, so an injected priority-0 Grp-1 vIRQ is
	 * delivered even before the guest programs its own GICV_CTLR/PMR. */
	vmcr = VGIC_VMCR_GRP | GICH_VMCR_VPMR_OPEN;
	GICH(GICH_VMCR) = vmcr;

	/* Enable the virtual CPU interface. En only — no maintenance IRQ in v1. */
	GICH(GICH_HCR) = GICH_HCR_EN;

	/* Guest virtual counter == physical counter (no time skew). */
	write_cntvoff_el2(0);

	/* We do NOT enable CNTV's own PPI (INTID 27) at the real distributor here.
	 * If we enable it before the guest has programmed CNTV_CVAL_EL0 (which
	 * is 0 on reset), we get an immediate and infinite interrupt storm because
	 * CNTVCT >= 0 is always true. The guest (FreeBSD) will enable it itself
	 * via its GICD driver when it registers the event timer. Since GICD is
	 * currently pass-through, the guest's write will hit the real hardware.
	 *
	 * VGIC_GICD(VGIC_GICD_IGROUPR0) |= (1u << VGIC_VTIMER_INTID);
	 * VGIC_GICD_IPRIORITYR_BYTE(VGIC_VTIMER_INTID) = 0x00u;
	 * VGIC_GICD(VGIC_GICD_ISENABLER0) |= (1u << VGIC_VTIMER_INTID);
	 */

	/* The maintenance PPI (INTID 25), by contrast, IS safe to enable at the
	 * real distributor unconditionally right here: unlike CNTV's hardware
	 * comparator, whether this line ever actually asserts is gated entirely
	 * by GICH_HCR.UIE, which WE control (left clear here, toggled on/off
	 * dynamically only while the pending-injection queue is non-empty — see
	 * vgic_inject_hw()/vgic_maintenance()). Enabling it at the distributor
	 * now with UIE clear is inert; it only matters once UIE is set. */
	VGIC_GICD(VGIC_GICD_IGROUPR0)   |= (1u << VGIC_MAINT_INTID);
	VGIC_GICD_IPRIORITYR_BYTE(VGIC_MAINT_INTID) = 0x00u;
	VGIC_GICD(VGIC_GICD_ISENABLER0) |= (1u << VGIC_MAINT_INTID);

	vg_bc(1, vtr);
	vg_bc(2, vg->nr_lr);
	vg_bc(3, GICH(GICH_HCR));
	vg_bc(12, 1u);
	vg_bc(13, GICH(GICH_VMCR));
	vg_bc(14, VGIC_GICD(VGIC_GICD_ISENABLER0)); /* readback: bit27 must be 1 */
	vg_bc(18, GICH(GICH_HCR));                  /* UIE (bit1) expected 0 here */
	vg_bc(19, VGIC_GICD(VGIC_GICD_ISENABLER0)); /* readback: bit25 must be 1 */

	vg->active = 1;
}

uint32_t vgic_active(void)
{
	/* THIS core's own flag — see struct vg_percpu's inventory comment for
	 * why a core that has never called its own vgic_init() must not read
	 * "true" from a different core's slot. */
	return vg_self()->active;
}

void vgic_inject(uint32_t vintid, int priority)
{
	struct vg_percpu *vg = vg_self();
	uint32_t elrsr, n, prio5, lr;

	vg->inject_count++;

	/* GICH_ELRSR0 bit n == 1 means LR n is empty (invalid) and reusable.
	 * A guest EOI drops its LR to invalid, so this naturally reclaims LRs
	 * without any maintenance interrupt. */
	elrsr = GICH(GICH_ELRSR0);

	for (n = 0; n < vg->nr_lr; n++) {
		if (elrsr & (1u << n)) {
			prio5 = ((uint32_t)priority >> 3) & 0x1fu;
			lr = (vintid & GICH_LR_VID_MASK) |
			     VGIC_LR_GRP |
			     GICH_LR_STATE_PENDING |
			     (prio5 << GICH_LR_PRIO_SHIFT);
			GICH(GICH_LR(n)) = lr;

			vg->inject_ok++;
			vg_bc(4, vg->inject_count);
			vg_bc(5, vg->inject_ok);
			vg_bc(7, lr);
			vg_bc(8, elrsr);
			/* B4 flight recorder: one event per IRQ actually landed in a
			 * List Register — a0=vintid, a1=the LR word written (priority +
			 * state folded in, cheaper than a second arg). */
			flightrec_log(FLTR_K_IRQ, vintid, lr);
			return;
		}
	}

	/* All LRs busy — the guest hasn't drained the previous ticks. Drop and
	 * count (a periodic tick is idempotent; losing one is harmless). */
	vg->inject_drop++;
	vg_bc(4, vg->inject_count);
	vg_bc(6, vg->inject_drop);
	vg_bc(8, elrsr);
}

/* ================================================================
 *  Virtual-timer (CNTV, INTID 27) injection — the deep-dive target.
 *  See vgic.h for the VGIC_CNTV_HW / VGIC_GROUP0 / VGIC_CNTV_EOI_TRACE
 *  toggles this honors. Breadcrumbs: [20]=cntv_inject, [21]=cntv_gate,
 *  [22]=last CNTV LR word, [23]=cntv_drop.
 * ================================================================ */
int vgic_inject_cntv(void)
{
	struct vg_percpu *vg = vg_self();
	uint32_t elrsr, n, lr, free_lr = 0xffffffffu;

	vg->inject_count++;

	/* ONE-SHOT GATE (fix C): if any List Register already holds a live
	 * (pending and/or active, i.e. NOT invalid) vINTID 27, do NOT inject a
	 * second copy — two LRs with the same VirtualID is UNPREDICTABLE per the
	 * GICv2 spec, and a periodic tick is idempotent so dropping the extra is
	 * harmless. GICH_ELRSR0 bit n == 1 means LR n is EMPTY (invalid); for the
	 * occupied ones we read the LR and compare the VirtualID field. In the
	 * same pass we remember the first free LR so we don't scan twice. */
	elrsr = GICH(GICH_ELRSR0);
	for (n = 0; n < vg->nr_lr; n++) {
		if (elrsr & (1u << n)) {
			if (free_lr == 0xffffffffu)
				free_lr = n;
			continue;
		}
		lr = GICH(GICH_LR(n));
		if ((lr & GICH_LR_VID_MASK) == VGIC_VTIMER_INTID) {
			/* A live 27 is still in flight — the guest hasn't EOIed the
			 * previous tick yet. Gate this one. */
			vg->cntv_gate++;
			vg_bc(21, vg->cntv_gate);
			vg_bc(8, elrsr);
			return 0;
		}
	}

	if (free_lr == 0xffffffffu) {
		/* All LRs busy with OTHER vINTIDs — drop this tick (idempotent). */
		vg->cntv_drop++;
		vg_bc(23, vg->cntv_drop);
		vg_bc(8, elrsr);
		return 0;
	}

	/* Build the CNTV List Register at priority 0 (top of the range). */
	n  = free_lr;
	lr = (VGIC_VTIMER_INTID & GICH_LR_VID_MASK) |
	     VGIC_LR_GRP |
	     GICH_LR_STATE_PENDING;   /* priority field 0 => prio5 == 0 */
#if VGIC_CNTV_HW
	/* HW=1: tie to the physical CNTV so the guest's virtual EOI deactivates
	 * the physical timer line in hardware. PhysicalID field = 27. */
	lr |= GICH_LR_HW | ((VGIC_VTIMER_INTID & 0x3ffu) << 10);
#else
	/* HW=0: pure virtual. The caller (gic_timer.c) has already masked and
	 * deactivated the physical side. Optionally request an EOI-maintenance
	 * IRQ (INTID 25) so EL2 sees the exact moment the guest virtual-EOIs the
	 * tick — proof-of-delivery, logged in vgic_maintenance(). */
#if VGIC_CNTV_EOI_TRACE
	lr |= GICH_LR_EOI;
	GICH(GICH_HCR) |= GICH_HCR_UIE;   /* ensure maintenance can fire */
#endif
#endif

	GICH(GICH_LR(n)) = lr;

	vg->cntv_inject++;
	vg->inject_ok++;
	vg_bc(4, vg->inject_count);
	vg_bc(5, vg->inject_ok);
	vg_bc(7, lr);
	vg_bc(8, elrsr);
	vg_bc(20, vg->cntv_inject);
	vg_bc(22, lr);
	flightrec_log(FLTR_K_IRQ, VGIC_VTIMER_INTID, lr);
	return 1;
}

void vgic_maintenance(void)
{
	struct vg_percpu *vg = vg_self();
	uint32_t misr, eisr, elrsr, n;

	vg->maint_count++;
	misr = GICH(GICH_MISR);
	eisr = GICH(GICH_EISR0);

	/* EISR bit n == 1: the guest EOIed LR n via the EOI-maintenance path.
	 * Clear it so the slot is reusable. Our HW-mode LRs (GICH_LR_HW set,
	 * GICH_LR_EOI clear) don't request this — deactivation on guest EOI is
	 * done by hardware directly, no maintenance IRQ needed for THAT — so
	 * this loop is normally a no-op in v2 too; kept for completeness/safety
	 * against any future LR that does set the EOI-maintenance bit. */
	for (n = 0; n < vg->nr_lr; n++)
		if (eisr & (1u << n))
			GICH(GICH_LR(n)) = 0u;

	/* Drain the LR-exhaustion pending queue (vgic_inject_hw()) into
	 * whichever LRs GICH_ELRSR0 shows free RIGHT NOW — this is what the
	 * underflow condition (MISR.U, the reason UIE fired this INTID 25 in
	 * the first place) is telling us: at least one LR just freed up. Bounded
	 * to vg->nr_lr iterations regardless of queue depth — never loops on the
	 * queue itself, only on the fixed LR count. */
	elrsr = GICH(GICH_ELRSR0);
	for (n = 0; n < vg->nr_lr && vg->pendq_count > 0; n++) {
		uint32_t vintid, pintid;
		int priority;

		if (!(elrsr & (1u << n)))
			continue;
		if (!vgic_pendq_pop(vg, &vintid, &pintid, &priority))
			break;
		vgic_lr_write_hw(vg, n, vintid, pintid, priority);
	}

	/* Once the queue is empty again, stop asking for underflow maintenance —
	 * see the file-header rationale (UIE left on unconditionally would make
	 * INTID 25 itself storm at idle, since <2-valid-LRs is the normal idle
	 * state of a 4-LR GIC-400). */
	if (vg->pendq_count == 0)
		GICH(GICH_HCR) &= ~GICH_HCR_UIE;

	vg_bc(9, vg->maint_count);
	vg_bc(10, misr);
	vg_bc(11, eisr);
	vg_bc(15, vg->pendq_count);
	vg_bc(18, GICH(GICH_HCR));
}

/* ================================================================
 *  GICD trap-and-emulate helpers (v1: present but not wired).
 *  Shadow the distributor's 4 KiB register page and, on a stage-2 data abort
 *  to that page, decode the faulting load/store from ESR_EL2.ISS (exactly the
 *  way vconsole_handle_fault() does for UART0) and satisfy it from the shadow.
 *  Left inert in v1 (GICD is identity-mapped/passed through); to activate,
 *  unmap the GICD page in stage-2 and call vgic_gicd_fault() from el2_trap's
 *  lower-EL data-abort arm before the generic fault record.
 * ================================================================ */
static uint8_t vgic_gicd_shadow[0x1000];

uint32_t vgic_gicd_read(uint32_t off)
{
	off &= 0xffcu;   /* word-align inside the page */
	return *(volatile uint32_t *)&vgic_gicd_shadow[off];
}

void vgic_gicd_write(uint32_t off, uint32_t val)
{
	off &= 0xffcu;
	*(volatile uint32_t *)&vgic_gicd_shadow[off] = val;
}

/* ESR_EL2.ISS decode for a lower-EL data abort (EC==0x24), same field
 * positions vconsole.c uses. */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

int vgic_gicd_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
	uint64_t addr;
	uint32_t off, srt;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	addr = frame->far;
	if (addr < VGIC_GICD_BASE || addr >= VGIC_GICD_BASE + 0x1000UL)
		return 0;   /* not the distributor page */

	if (!(esr & ESR_ISV_BIT)) {
		/* No syndrome — can't decode the register/direction. Skip the
		 * instruction rather than re-fault forever. */
		frame->elr += 4;
		return 1;
	}

	off = (uint32_t)(addr - VGIC_GICD_BASE);
	srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;

	if (esr & ESR_WNR_BIT) {
		uint32_t val = (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt];
		vgic_gicd_write(off, val);
	} else {
		uint32_t val = vgic_gicd_read(off);
		if (srt != SRT_XZR)
			frame->x[srt] = val;
	}

	frame->elr += 4;   /* fully emulated — skip the faulting access */
	return 1;
}

/* ================================================================
 *  Bare-metal self-test guest (run BEFORE FreeBSD).
 *
 *  The EL1 payload accesses "its GICC" at the physical GICC IPA
 *  (0x01c82000) — stage-2 redirects those 2 pages to GICV (0x01c86000), so
 *  every access here actually programs / reads the VIRTUAL interface.
 * ================================================================ */
#define GVIF(reg)   (*(volatile uint32_t *)(VGIC_GICC_BASE + (reg)))
#define GVIF_CTLR   0x00u
#define GVIF_PMR    0x04u
#define GVIF_IAR    0x0cu
#define GVIF_EOIR   0x10u

/* Self-test breadcrumb window: 0x50001d00 ("VGST"). Layout:
 *   [0] magic 0x56475354 ("VGST")
 *   [1] guest_el     CurrentEL sampled at EL1 (expect 1)
 *   [2] alive        guest main-loop counter (climbs => EL1 running)
 *   [3] virq_count   IRQ-handler counter (climbs => injection WORKS) <-- proof
 *   [4] last_iar     last virtual IAR read (expect 27 = CNTV vINTID)
 *   [5] spurious     count of spurious IARs (>=1020)
 *   [6] other_exc    non-IRQ EL1 exceptions (should stay 0)
 *   [7] vbar_set     VBAR_EL1 (lo) the payload installed */
/* OVERRIDABLE, and this one has a REAL BUG behind it — found 2026-08-05 by
 * actually running this self-test for the first time (under QEMU virt, see
 * main_vgic_qemu.c): 0x50001d00 is inside the hv-scratch window
 * (0x50000000 + 2 MiB) that stage2.c's stage2_build_dram_table() leaves
 * INVALID in the GUEST's stage-2 map on purpose — that exclusion IS the A1
 * guest/HV isolation. But THIS window is written by the guest, at EL1, so the
 * payload's very first store takes a stage-2 translation fault (confirmed:
 * ESR=0x93810046, DFSC=0x06 translation fault level 2, FAR=0x50001d00).
 *
 * The default is left at 0x50001d00 so no board target changes behaviour from
 * this commit; it is now overridable so the QEMU vGIC CI target can put the
 * window somewhere the guest can actually reach. NOTE for whoever revives this
 * self-test ON HARDWARE: moving this window is necessary but NOT sufficient
 * there. The payload's code and stack live in the HV's own .text/.bss, i.e.
 * inside the hv-image window (0x42000000 + 2 MiB), which stage2.c excludes
 * from the guest map for the same A1 reason — so on the board the guest cannot
 * even fetch these instructions. Running this on real hardware needs the
 * payload RELOCATED into guest-accessible DRAM, not just its breadcrumbs. */
#ifndef VGST_BC_BASE
#define VGST_BC_BASE   0x50001d00UL
#endif
#define VGST_BC_MAGIC  0x56475354u   /* "VGST" */

static inline void vgst_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VGST_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t vgst_rd(int i)
{
	volatile uint32_t *p = (volatile uint32_t *)(VGST_BC_BASE + (uint32_t)i * 4u);
	return *p;
}

/* Guest EL1 vector table + the IRQ trampoline. The guest runs at EL1h (SP_EL1),
 * so a virtual IRQ is taken through the "Current EL with SPx / IRQ" slot at
 * VBAR + 0x280. That entry saves the caller-saved GPRs on SP_EL1, calls the C
 * handler, restores, and erets. All other slots just record and return. */
extern char vgic_st_vectors[];

__asm__(
"	.section .text\n"
"	.balign 0x800\n"
"	.globl vgic_st_vectors\n"
"vgic_st_vectors:\n"
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x000 Cur SP0 Sync */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x080 Cur SP0 IRQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x100 Cur SP0 FIQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x180 Cur SP0 SErr */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x200 Cur SPx Sync */
"	.balign 0x80\n	b vgic_st_irq\n"     /* 0x280 Cur SPx IRQ  <-- */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x300 Cur SPx FIQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x380 Cur SPx SErr */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x400 Lower64 Sync */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x480 Lower64 IRQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x500 Lower64 FIQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x580 Lower64 SErr */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x600 Lower32 Sync */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x680 Lower32 IRQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x700 Lower32 FIQ  */
"	.balign 0x80\n	b vgic_st_other\n"   /* 0x780 Lower32 SErr */
"	.balign 0x80\n"
"vgic_st_irq:\n"
"	sub  sp, sp, #176\n"
"	stp  x0, x1,  [sp, #0]\n"
"	stp  x2, x3,  [sp, #16]\n"
"	stp  x4, x5,  [sp, #32]\n"
"	stp  x6, x7,  [sp, #48]\n"
"	stp  x8, x9,  [sp, #64]\n"
"	stp  x10, x11,[sp, #80]\n"
"	stp  x12, x13,[sp, #96]\n"
"	stp  x14, x15,[sp, #112]\n"
"	stp  x16, x17,[sp, #128]\n"
"	stp  x18, x30,[sp, #144]\n"
"	bl   vgic_st_irq_c\n"
"	ldp  x0, x1,  [sp, #0]\n"
"	ldp  x2, x3,  [sp, #16]\n"
"	ldp  x4, x5,  [sp, #32]\n"
"	ldp  x6, x7,  [sp, #48]\n"
"	ldp  x8, x9,  [sp, #64]\n"
"	ldp  x10, x11,[sp, #80]\n"
"	ldp  x12, x13,[sp, #96]\n"
"	ldp  x14, x15,[sp, #112]\n"
"	ldp  x16, x17,[sp, #128]\n"
"	ldp  x18, x30,[sp, #144]\n"
"	add  sp, sp, #176\n"
"	eret\n"
"vgic_st_other:\n"
"	bl   vgic_st_other_c\n"
"	eret\n"
);

/* C side of the guest IRQ trampoline: read the virtual IAR (redirected to
 * GICV), record it, EOI. This runs at EL1. */
void vgic_st_irq_c(void)
{
	uint32_t iar = GVIF(GVIF_IAR);
	uint32_t intid = iar & 0x3ffu;

	if (intid >= GIC_SPURIOUS_MIN) {
		vgst_bc(5, vgst_rd(5) + 1u);   /* spurious: no EOI (GICv2) */
		return;
	}

	vgst_bc(4, iar);
	vgst_bc(3, vgst_rd(3) + 1u);       /* THE proof: a vIRQ was delivered */

	GVIF(GVIF_EOIR) = iar;             /* virtual EOI -> drops the LR */
}

/* Any non-IRQ EL1 exception is unexpected in this self-test — just count it. */
void vgic_st_other_c(void)
{
	vgst_bc(6, vgst_rd(6) + 1u);
}

static inline void write_vbar_el1(uint64_t v)
{
	__asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(v) : "memory");
}

static inline uint32_t read_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return (uint32_t)(v >> 2);
}

void vgic_selftest_guest_el1(void)
{
	vgst_bc(0, VGST_BC_MAGIC);
	vgst_bc(1, read_currentel());               /* expect 1 (EL1) */

	/* Install our own EL1 vector table (guest_config() left VBAR_EL1=0). */
	write_vbar_el1((uint64_t)(uintptr_t)vgic_st_vectors);
	vgst_bc(7, (uint32_t)(uintptr_t)vgic_st_vectors);

	/* Enable the (virtual) CPU interface via the guest's GICC IPA, which
	 * stage-2 redirects to GICV: PMR wide open, EnableGrp1. */
	GVIF(GVIF_PMR) = 0xffu;
	GVIF(GVIF_CTLR) = 0x1u;

	/* Unmask IRQ at EL1 so the injected virtual timer tick is taken. */
	__asm__ volatile("msr daifclr, #2" ::: "memory");

	/* Spin, proving EL1 keeps running between (virtual) ticks. */
	for (;;) {
		vgst_bc(2, vgst_rd(2) + 1u);
	}
}

/* Private EL1 stack for the self-test payload (16 KiB, owned entirely here). */
#define VGIC_ST_STACK_WORDS (16384 / 8)
static uint64_t vgic_st_stack[VGIC_ST_STACK_WORDS] __attribute__((aligned(16)));

void vgic_selftest_start(void)
{
	/* Baseline EL1 config (HCR_EL2.RW, SCTLR_EL1 flat, VBAR_EL1=0 which the
	 * payload immediately overwrites). See vgic.h for the full preconditions
	 * (gic_timer_init + vgic_init + stage2 with GICC->GICV remap + EL2 IRQ
	 * unmasked) the integrator must satisfy before calling this. */
	guest_config();
	guest_enter((uint64_t)vgic_selftest_guest_el1,
	            (uint64_t)&vgic_st_stack[VGIC_ST_STACK_WORDS]);
	__builtin_unreachable();
}

void vgic_inject_hw(uint32_t vintid, uint32_t pintid, int priority)
{
	struct vg_percpu *vg = vg_self();
	uint32_t elrsr, n;

	vg->inject_count++;

	/* Fast path only when the pending queue is already empty: if it isn't,
	 * something is draining (or about to be, via the maintenance IRQ) and
	 * injecting straight into a free LR here would deliver THIS interrupt
	 * ahead of ones already queued — not incorrect (the guest doesn't
	 * require strict inter-device ordering) but needlessly surprising to
	 * reason about, so we keep FIFO order across the queue+LR combination
	 * by always queuing behind an existing backlog. */
	if (vg->pendq_count == 0) {
		elrsr = GICH(GICH_ELRSR0);
		for (n = 0; n < vg->nr_lr; n++) {
			if (elrsr & (1u << n)) {
				vgic_lr_write_hw(vg, n, vintid, pintid, priority);
				vg_bc(8, elrsr);
				return;
			}
		}
	}

	/* No free LR right now (or the queue is already draining) — queue it.
	 * vgic_maintenance() (driven by the GICH_HCR.UIE underflow maintenance
	 * IRQ this push enables) drains it the moment an LR frees up. Only the
	 * queue itself being full actually loses the interrupt
	 * (vg->pendq_overflow, counted separately from vg->inject_drop's legacy
	 * meaning below). */
	if (!vgic_pendq_push(vg, vintid, pintid, priority))
		vg->inject_drop++;   /* true loss: even the pending queue was full */

	vg_bc(4, vg->inject_count);
	vg_bc(6, vg->inject_drop);
}
