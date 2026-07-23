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
 *   stage2.c's stage2_build_mmio_tables() as of internal task); we never program
 *   GICV from here — that would fight the guest. GICD (0x01c81000) is passed
 *   through / identity-mapped to the guest (vgic_gicd_* below are the
 *   ready-but-inert trap-emulate hooks for when a later milestone unmaps it),
 *   BUT (v2, internal task) vgic_init() DOES make one real distributor write of
 *   its own: enabling/grouping/prioritizing INTID 27 (CNTV's own PPI) via
 *   GICD_ISENABLER0/IGROUPR0/IPRIORITYR, exactly like gic_timer.c already
 *   does for INTID 30. Without that, CNTV's hardware compare condition would
 *   never reach GICC_IAR at all — this is not the same thing as the guest's
 *   own GICD MMIO reads/writes (still identity/passed-through unmodified).
 * MOSTLY DOES NOT TOUCH the physical GICC path (GICC_IAR/EOIR) — gic_timer.c
 *   owns the physical CNTP tick end to end, and v1 does NOT enable any
 *   maintenance IRQ, precisely so INTID 25 never appears on gic_timer_irq()'s
 *   physical IAR. (v2, internal task): gic_timer_irq() now ALSO recognizes
 *   physical INTID 27 - the guest's real CNTV firing, routed to EL2 solely
 *   because HCR_EL2.IMO=1 catches every non-secure Group-1 physical IRQ,
 *   this one included - and calls vgic_inject() on it instead of discarding
 *   it as an unexpected PPI. See gic_timer.c for that side.
 *
 * ================================================================
 *  Delivery chain (why a virtual IRQ actually reaches the guest)
 * ================================================================
 *   1. gic_timer.c set HCR_EL2.IMO=1. On this part IMO=1 does two things:
 *      routes physical IRQ to EL2 (the host tick), AND enables the virtual CPU
 *      interface to signal a *virtual* IRQ to EL1.
 *   2. vgic_init() sets GICH_HCR.En=1 (virtual interface on) and CNTVOFF_EL2=0.
 *   3. On each physical CNTP tick, el2_trap()->vgic_inject(27,0) writes a
 *      GICH_LR with state=pending, Group 1, priority 0.
 *   4. GICV sees a pending Group-1 vIRQ above the virtual PMR/running-priority
 *      and asserts the virtual IRQ line. The guest at EL1 (PSTATE.I clear)
 *      takes it through its own VBAR_EL1, reads the virtual IAR (its "GICC"
 *      IPA, redirected to GICV), gets INTID 27, EOIs. The EOI drops the LR to
 *      invalid (GICH_ELRSR bit sets), freeing it for the next inject.
 * Freestanding, no libc: <stdint.h> only. Cache-coherent breadcrumb stores
 * (dc civac + dsb sy), same convention as every other lane in this tree.
 */
#include <stdint.h>
#include "vgic.h"
#include "guest.h"   /* guest_config(), guest_enter() for the self-test */
#include "flightrec.h"  /* B4: flightrec_log(FLTR_K_IRQ, ...) on every injected LR */

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

/* GICv2 spurious range. */
#define GIC_SPURIOUS_MIN  1020u

/* ================================================================
 *  GICD (distributor) — the ONE additional distributor write vgic_init()
 *  makes, despite this file's "does not touch GICD" header comment (that
 *  comment is about the shadow/trap-emulate path further down, which stays
 *  inert). Without this, CNTV's own physical PPI (INTID 27) is simply never
 *  enabled at the distributor - GICD_ISENABLER0 resets with it clear - so
 *  the hardware timer condition (CNTVCT >= guest's CNTV_CVAL_EL0) would
 *  never even reach GICC_IAR, real or virtual, no matter what GICH/vgic_init
 *  below does. This mirrors exactly what gic_timer.c already does for
 *  INTID 30 (CNTP): same NON-SECURE Group-1 view this board was proven to
 *  run in (see gic_timer.c's TIMER_INTID comment), same offsets. GICC_PMR/
 *  GICC_CTLR are CPU-interface-wide (not per-INTID) and gic_timer_init()
 *  already opened them, so nothing more is needed there for INTID 27 too.
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
 *  Breadcrumb window: 0x50001c00 ("VGIC"). Sits in the free gap between
 *  vconsole (ends 0x50001b10) and gtrace (starts 0x50002000). Layout:
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
 * ================================================================ */
#define VGIC_BC_BASE   0x00018000UL
#define VGIC_BC_MAGIC  0x56474943u   /* "VGIC" */

static inline void vg_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VGIC_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ---- Module state (private). ---- */
static uint32_t vg_nr_lr = 4;        /* refined from GICH_VTR in vgic_init */
static uint32_t vg_inject_count;
static uint32_t vg_inject_ok;
static uint32_t vg_inject_drop;
static uint32_t vg_maint_count;

/* ================================================================
 *  Public API
 * ================================================================ */
void vgic_init(void)
{
	uint32_t vtr, i, vmcr;

	vg_bc(0, VGIC_BC_MAGIC);

	vtr = GICH(GICH_VTR);
	vg_nr_lr = (vtr & 0x3fu) + 1u;   /* VTR[5:0] = ListRegs - 1 */
	if (vg_nr_lr > 16u)
		vg_nr_lr = 16u;

	/* Start from a clean slate: every LR invalid (state 00). */
	for (i = 0; i < vg_nr_lr; i++)
		GICH(GICH_LR(i)) = 0u;

	/* Preset the virtual GICC control the guest sees: Group 1 enabled,
	 * virtual PMR wide open, so an injected priority-0 Grp-1 vIRQ is
	 * delivered even before the guest programs its own GICV_CTLR/PMR. */
	vmcr = GICH_VMCR_VENG1 | GICH_VMCR_VPMR_OPEN;
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

	vg_bc(1, vtr);
	vg_bc(2, vg_nr_lr);
	vg_bc(3, GICH(GICH_HCR));
	vg_bc(12, 1u);
	vg_bc(13, GICH(GICH_VMCR));
	vg_bc(14, VGIC_GICD(VGIC_GICD_ISENABLER0)); /* readback: bit27 must be 1 */
}

void vgic_inject(uint32_t vintid, int priority)
{
	uint32_t elrsr, n, prio5, lr;

	vg_inject_count++;

	/* GICH_ELRSR0 bit n == 1 means LR n is empty (invalid) and reusable.
	 * A guest EOI drops its LR to invalid, so this naturally reclaims LRs
	 * without any maintenance interrupt. */
	elrsr = GICH(GICH_ELRSR0);

	for (n = 0; n < vg_nr_lr; n++) {
		if (elrsr & (1u << n)) {
			prio5 = ((uint32_t)priority >> 3) & 0x1fu;
			lr = (vintid & GICH_LR_VID_MASK) |
			     GICH_LR_GRP1 |
			     GICH_LR_STATE_PENDING |
			     (prio5 << GICH_LR_PRIO_SHIFT);
			GICH(GICH_LR(n)) = lr;

			vg_inject_ok++;
			vg_bc(4, vg_inject_count);
			vg_bc(5, vg_inject_ok);
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
	vg_inject_drop++;
	vg_bc(4, vg_inject_count);
	vg_bc(6, vg_inject_drop);
	vg_bc(8, elrsr);
}

void vgic_maintenance(void)
{
	uint32_t misr, eisr, n;

	vg_maint_count++;
	misr = GICH(GICH_MISR);
	eisr = GICH(GICH_EISR0);

	/* EISR bit n == 1: the guest EOIed LR n (an EOI-maintenance LR). Clear
	 * it so the slot is reusable. (In v1 no maintenance IRQ is enabled, so
	 * this path is exercised only if a v2 revision turns it on.) */
	for (n = 0; n < vg_nr_lr; n++)
		if (eisr & (1u << n))
			GICH(GICH_LR(n)) = 0u;

	vg_bc(9, vg_maint_count);
	vg_bc(10, misr);
	vg_bc(11, eisr);
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
#define VGST_BC_BASE   0x50001d00UL
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
	uint32_t elrsr, n, prio5, lr;

	vg_inject_count++;

	elrsr = GICH(GICH_ELRSR0);

	for (n = 0; n < vg_nr_lr; n++) {
		if (elrsr & (1u << n)) {
			prio5 = ((uint32_t)priority >> 3) & 0x1fu;
			lr = (vintid & GICH_LR_VID_MASK) |
			     ((pintid & 0x3ffu) << 10) |
			     GICH_LR_GRP1 |
			     GICH_LR_HW |
			     GICH_LR_STATE_PENDING |
			     (prio5 << GICH_LR_PRIO_SHIFT);
			GICH(GICH_LR(n)) = lr;

			vg_inject_ok++;
			vg_bc(4, vg_inject_count);
			vg_bc(5, vg_inject_ok);
			vg_bc(7, lr);
			vg_bc(8, elrsr);
			/* B4 flight recorder: a0=vintid(==pintid for HW mode), a1=LR word. */
			flightrec_log(FLTR_K_IRQ, vintid, lr);
			return;
		}
	}

	vg_inject_drop++;
	vg_bc(4, vg_inject_count);
	vg_bc(6, vg_inject_drop);
	vg_bc(8, elrsr);
}
