/* SPDX-License-Identifier: BSD-2-Clause */

/* gic_timer.c — GICv2 (GIC-400) + ARM Generic Timer EL2-physical-timer tick.
 * See gic_timer.h for the API contract and what this module deliberately
 * does NOT do (unmask PSTATE.I, touch HCR_EL2).
 *
 * ------------------------------------------------------------------
 * GIC-400 MMIO bases — cited, not guessed.
 * ------------------------------------------------------------------
 * /opt/bzdos/build/u-boot/arch/arm/dts/sun50i-a64.dtsi:1163-1172
 *
 *     gic: interrupt-controller@1c81000 {
 *             compatible = "arm,gic-400";
 *             reg = <0x01c81000 0x1000>,   // GICD (distributor)
 *                   <0x01c82000 0x2000>,   // GICC (CPU interface)
 *                   <0x01c84000 0x2000>,   // GICH (hyp control) - unused here
 *                   <0x01c86000 0x2000>;   // GICV (virtual CPU i/f) - unused
 *             ...
 *     };
 *
 * So GICD_BASE = 0x01c81000, GICC_BASE = 0x01c82000, matching the "typical
 * A64" values given in the task brief — verified against this DTS rather
 * than assumed. GICH/GICV (virtualization extensions) are not touched:
 * this driver delivers the tick as a plain physical IRQ to EL2, no
 * virtual-interrupt list registers involved.
 *
 * ------------------------------------------------------------------
 * Which timer PPI, and why it's the one usable at EL2.
 * ------------------------------------------------------------------
 * /opt/bzdos/build/u-boot/arch/arm/dts/sun50i-a64.dtsi:199-211
 *
 *     timer {
 *             compatible = "arm,armv8-timer";
 *             ...
 *             interrupts = <GIC_PPI 13 ...>,   // secure physical   (CNTPS)
 *                          <GIC_PPI 14 ...>,   // non-secure phys.  (CNTP)
 *                          <GIC_PPI 11 ...>,   // virtual           (CNTV)
 *                          <GIC_PPI 10 ...>;   // hypervisor phys.  (CNTHP)
 *
 * The arm,armv8-timer binding fixes that interrupt order (secure-phys,
 * non-secure-phys, virtual, hyp-phys), and GIC_PPI n = INTID (16 + n), so:
 *   PPI13 -> INTID 29  CNTPS (secure physical)
 *   PPI14 -> INTID 30  CNTP  (non-secure EL1 physical)  <-- USED (v3)
 *   PPI11 -> INTID 27  CNTV  (virtual)
 *   PPI10 -> INTID 26  CNTHP (EL2 physical)  <-- tried in v1/v2, UNUSABLE
 *
 * CHOICE (v3): CNTP, INTID 30. Earlier revisions used CNTHP (INTID 26, the
 * EL2 physical timer) on the reasoning that, since everything runs at EL2,
 * the EL2-banked timer is the "native" pick. That was architecturally tidy
 * but WRONG for this board: two hardware tests proved CNTHP/INTID 26 never
 * reaches us. The decisive evidence (2nd test) was GICC_PMR reading back
 * 0xf0 after we wrote 0xff, plus GICD_IGROUPR0 bit 26 being RAZ/WI - both
 * are the signature of the NON-SECURE banked view of a two-security-state
 * GIC-400. In that configuration ATF/BL31 owns INTID 26 as a SECURE Group 0
 * interrupt; it is delivered to the secure world (EL3/secure FIQ) and can
 * NEVER be seen by our non-secure EL2, no matter how DAIF is set.
 *
 * CNTP (INTID 30) is the NON-SECURE physical timer that firmware assigns to
 * the non-secure OS as a Group 1 interrupt (this is the timer Linux uses on
 * the A64), so it is delivered to us as an IRQ. It is programmed via the
 * EL0/EL1 physical-timer registers CNTP_CVAL_EL0 / CNTP_CTL_EL0, which are
 * accessible from EL2 with no trap (we are above EL1; CNTHCTL_EL2 gating
 * only affects EL0/EL1 accesses, not EL2's own), against the same CNTPCT
 * physical counter - so the period math is unchanged.
 *
 * ------------------------------------------------------------------
 * Bits that must be set to actually TAKE this IRQ at EL2 — who sets what.
 * ------------------------------------------------------------------
 * Set BY THIS MODULE (gic_timer_init), all documented inline below:
 *   - CNTP_CVAL_EL0 : timer deadline (now + period, absolute).
 *   - CNTP_CTL_EL0.ENABLE=1, .IMASK=0 : timer counts and asserts its
 *     interrupt line when CNTPCT >= CVAL (IMASK=0 means "don't mask the
 *     timer's own output" - separate from PSTATE.I).
 *   - GICD_CTLR / GICC_CTLR = 0x3 : enable Group 1 forwarding (bit0 in the
 *     non-secure view is EnableGrp1); INTID 30 is already Group 1, so we do
 *     NOT reprogram its group (that write is RAZ/WI for us anyway).
 *   - GICD_ISENABLER0 bit 30 : enable forwarding of INTID 30 specifically.
 *   - GICD_IPRIORITYR[30] : give it a real (non-reset-garbage) priority.
 *   - GICC_PMR=0xff : priority mask wide open (reads back ~0xf0 in the NS
 *     view, still far above our priority 0x80) so our priority gets through.
 *
 * NOT set by this module - MUST be set by the integration lane
 * (exceptions.S / el2_install / main_repl.c), because it is a scheduling
 * decision this driver has no basis to make on its own:
 *   - PSTATE.I (the DAIF I bit) must be 0 for the core to accept IRQs at
 *     all, in any exception level. INTID 30 (CNTP) is a non-secure Group 1
 *     interrupt, delivered to us as an IRQ, so unmasking I is the correct
 *     trigger. Concretely: after el2_install() and after gic_timer_init(),
 *     the integrator unmasks with
 *         asm volatile("msr daifclr, #2");   // clears I (IRQ)
 *     -- OR, still RECOMMENDED, `msr daifclr, #3` (clears BOTH I and F) as
 *     cheap insurance: el2_trap already routes FIQ->gic_timer_irq, so even
 *     a board that somehow delivered this as FIQ would still tick. Until
 *     the unmask runs, gic_timer_init() has armed the timer and the GIC
 *     will latch/forward INTID 30, but the core will not vector for it - it
 *     stays pending at the CPU interface (harmless, no data loss: GICC_IAR
 *     hands it out the instant the mask is cleared).
 *   - Whatever SPSR_EL2 value is used for any `eret` back to a running
 *     context must likewise have its I bit clear if that context should
 *     be preemptible; el2_install()'s vector table itself doesn't need
 *     changes for this - it is purely a PSTATE concern at the point of
 *     entry/return, not a vector-table concern.
 *   - HCR_EL2.IMO: REQUIRED = 1, and now SET BY THIS MODULE (v4). Earlier
 *     revisions wrongly claimed IMO was unnecessary "because nothing runs
 *     below EL2". The truth is the opposite and it is why v1-v3 never took
 *     the IRQ: with IMO=0 a physical IRQ is routed to EL1, and an async
 *     exception targeting a LOWER EL than the current one is not taken - it
 *     stays pending. Running at EL2, we must route the IRQ to EL2 (IMO=1)
 *     for it to be taken here at all (then PSTATE.I gates it). gic_timer_init
 *     now sets IMO via read-modify-write. HCR_EL2 is really EL2-setup that
 *     could live in el2_install() instead; setting it here is idempotent
 *     (OR of one bit) so the integration lane may relocate it freely.
 *     FMO (FIQ->EL2) / AMO (SError->EL2) are left as-is: the tick is a
 *     Group 1 IRQ so IMO suffices; set FMO too only if some source is ever
 *     delivered as FIQ.
 *
 * Group assignment (v3, after two hardware tests): we rely on INTID 30
 * (CNTP) ALREADY being a non-secure Group 1 interrupt (firmware/ATF set it
 * up that way for the non-secure OS), rather than trying to move it - the
 * v2 attempt to reprogram INTID 26's group was RAZ/WI because that
 * interrupt is secure-owned. A non-secure Group 1 interrupt is delivered to
 * us (non-secure EL2) as an IRQ, matching the I bit the integrator unmasks.
 * We enable Group 1 forwarding on the distributor and CPU interface
 * (GICD_CTLR = GICC_CTLR = 0x3; bit0 is EnableGrp1 in the non-secure view,
 * already reading back 0x1 in the v2 test). The init-time breadcrumb words
 * 8..18 (CNTP_CTL, GICD/GICC_CTLR, IGROUPR0, CVAL vs CNTPCT, PMR,
 * ISENABLER0, and the decisive post-arm ISPENDR0 + CNTP ISTATUS) let a
 * hardware read confirm every link in the chain actually took.
 *
 * ------------------------------------------------------------------
 * How gic_timer_irq() must be called from el2_trap()'s IRQ case:
 * ------------------------------------------------------------------
 *     void el2_trap(struct el2_frame *frame, unsigned long kind)
 *     {
 *             ...
 *             if ((kind & 3u) == EL2_KIND_IRQ) {
 *                     gic_timer_irq(frame);
 *                     return;   // do NOT advance frame->elr for IRQ/FIQ
 *             }
 *             ...
 *     }
 * (el2_exc.c already gets this right: it only advances elr for
 * EL2_KIND_SYNC. gic_timer_irq() just needs to be called somewhere in the
 * kind&3==1 arm, before or after the existing breadcrumb recording - order
 * doesn't matter, gic_timer_irq() touches only its own state + the GIC/
 * timer registers + the shared jitter breadcrumb window at 0x50000500.)
 *
 * ------------------------------------------------------------------
 * Breadcrumb window: 0x00018200 ("GICT"), distinct from MUSB (0x50000000),
 * EMAC (0x50000100), REPL (0x50000300), EL2 exceptions (0x00018100),
 * jitter/TIMR (0x50000500).
 *   [0]  magic       0x47494354 ("GICT")
 *   [1]  ticks_lo    low 32 bits of the tick counter
 *   [2]  ticks_hi    high 32 bits of the tick counter
 *   [3]  last_iar    last raw GICC_IAR value read (INTID + CPUID field)
 *   [4]  period_lo   low 32 bits of the programmed period, in ticks
 *   [5]  mismatches  count of acked INTIDs that were NOT ours (30)
 *   [6]  init_done   1 once gic_timer_init() has completed
 *   [7]  ctl_live    CNTP_CTL_EL0 readback sampled in the IRQ handler
 *                    (bit0=ENABLE, bit1=IMASK, bit2=ISTATUS/firing)
 *   --- init-time diagnostic readbacks (written once by gic_timer_init) --
 *   [8]  cntp_ctl    CNTP_CTL_EL0 right after arming (expect ENABLE=1)
 *   [9]  gicd_ctlr   GICD_CTLR readback (group-enable bits; NS view => 0x1)
 *   [10] gicc_ctlr   GICC_CTLR readback (group-enable bits; NS view => 0x1)
 *   [11] igroupr0    GICD_IGROUPR0 readback (bit30 should be 1 => Grp1/IRQ)
 *   [12] cntpct_now  CNTPCT_EL0 at arm time (lo) - compare vs [13]
 *   [13] cval_lo     CNTP_CVAL deadline (lo); must be > [12] (future)
 *   [14] gicc_pmr    GICC_PMR readback (NS view halves it: 0xff => ~0xf0)
 *   [15] isenabler0  GICD_ISENABLER0 readback (bit30 must be 1)
 *   --- decisive post-arm diagnostic (bounded busy-wait, IRQs masked) ---
 *   [16] ispendr0    GICD_ISPENDR0 after deadline passed (bit30 => CNTP
 *                    latched pending in the GIC; whole word recorded so a
 *                    surprise on another PPI bit is visible too)
 *   [17] cntp_ctl2   CNTP_CTL_EL0 after deadline (bit2 ISTATUS => the timer
 *                    condition actually fired)
 *   [18] poll_ok     1 = observed CNTPCT >= CVAL during the bounded wait;
 *                    0 = hit the iteration cap (counter not advancing)
 *   --- v4 routing fix + CPU-interface probe (init, IRQs masked) --------
 *   [19] hcr_before  HCR_EL2 (lo) before we OR in IMO
 *   [20] hcr_after   HCR_EL2 (lo) after: bit4 (IMO) MUST read 1
 *   [21] rpr_before  GICC_RPR before the IAR probe (0xff = idle)
 *   [22] iar_probe   GICC_IAR: 30 => CPU-if would forward (routing was the
 *                    fault); 1023 => CPU-if masking it (GIC-side fault)
 *   [23] rpr_active  GICC_RPR while the probed IRQ is active
 *   [24] rpr_after   GICC_RPR after our balancing EOI: MUST be 0xff (idle);
 *                    anything else means a stuck running priority
 * Written once at init (words 4,6,8..24) and every REPORT_EVERY ticks
 * (words 0-3,5,7) from IRQ context - same dc-civac+dsb store pattern as the
 * rest of this codebase, bounded (fixed store count), no unbounded loops.
 */
#include <stdint.h>
#include "cntpct.h"
#include "gic_timer.h"
#include "exceptions.h"
#include "timer.h"
#include "vgic.h"
#include "flightrec.h"
#include "wdt.h"     /* wdt_debug_kick() — CPU1-as-vCPU2 tick-path kick, see below */
#include "dbgmon.h"  /* dbgmon_service() — ditto */
#include "musb.h"    /* MUSB_IRQ_SPI/MUSB_IRQ_INTID — cited constants, see musb.h */
#include "usbacm.h"  /* usbacm_poll() — CPU1 MUSB-IRQ path, see musb_irq_arm_cpu1() */
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */
#ifdef HV_HDMI
#include "hdmi.h"    /* hdmi_phy_locked()/hdmi_relock() — ditto, HDMI PHY relock */
#endif
#include "smp.h"   /* SMP_MAX_CPUS, smp_cpu_id() — Phase 2 P1 per-core state,
                    * see the inventory comment above struct gt_percpu below.
                    * Header-only (smp_cpu_id() is `static inline`): does not
                    * pull in a new link dependency on smp.o. */

/* ------------------------------------------------------------------ *
 * GIC-400 MMIO bases (see citation above) and the handful of registers
 * this driver touches. Offsets are architectural (GICv2 spec).
 * ------------------------------------------------------------------ */
#define GICD_BASE SOC_A64_GICD_BASE
#define GICC_BASE SOC_A64_GICC_BASE

#define GICD_CTLR         (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_IGROUPR(n)   (*(volatile uint32_t *)(GICD_BASE + 0x080 + 4u * (n)))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4u * (n)))
/* ICENABLER (GICv2 0x180): write-1-to-CLEAR the enable bit, the counterpart of
 * ISENABLER above. Added 2026-08-26 for the MUSB storm throttle — this tree had
 * never needed to DISABLE an INTID at the distributor before, only enable. */
#define GICD_ICENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x180 + 4u * (n)))
#define GICD_ISPENDR(n)   (*(volatile uint32_t *)(GICD_BASE + 0x200 + 4u * (n)))
/* GICD_IPRIORITYR is byte-addressable, one byte per interrupt ID. */
#define GICD_IPRIORITYR_BYTE(id) (*(volatile uint8_t *)(GICD_BASE + 0x400 + (id)))
/* GICD_ITARGETSR: byte-addressable, one byte per SPI (IDs 0..31/PPIs+SGIs
 * are RO "always this CPU"), a bitmap of target CPU interfaces (bit N =
 * CPU interface N). Used ONLY for MUSB_IRQ_INTID below, to steer that one
 * real device SPI to CPU1's interface alone — see musb_irq_arm_cpu1(). */
#define GICD_ITARGETSR_BYTE(id) (*(volatile uint8_t *)(GICD_BASE + 0x800 + (id)))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))
#define GICC_RPR  (*(volatile uint32_t *)(GICC_BASE + 0x014)) /* running priority */
#define GICC_DIR  (*(volatile uint32_t *)(GICC_BASE + 0x1000)) /* deactivate interrupt */

/* Our interrupt: INTID 30, the NON-SECURE EL1 physical timer (CNTP) PPI,
 * per the sun50i-a64.dtsi timer node (GIC_PPI 14 -> 16+14 = 30).
 *
 * WHY 30 (CNTP) AND NOT 26 (CNTHP): the second hardware test proved this
 * board runs a two-security-state GIC-400 where we (EL2) execute in the
 * NON-SECURE world - the tell-tale is GICC_PMR reading back 0xf0 after we
 * wrote 0xff (the non-secure banked view exposes only the top nibble of
 * priority). In that view, GICD_IGROUPR bits for interrupts owned by the
 * SECURE world are RAZ/WI to us: our attempt to move INTID 26 to Group 1
 * silently did nothing (IGROUPR0 bit26 read back 0), because CNTHP/INTID 26
 * is a secure Group 0 interrupt that ATF/BL31 owns. A Group 0 interrupt is
 * delivered to the SECURE world (EL3/secure FIQ) and can NEVER reach our
 * non-secure EL2, regardless of PSTATE.I/F. So CNTHP is unusable from here.
 *
 * INTID 30 (CNTP) is the non-secure physical timer that firmware hands to
 * the non-secure OS as a Group 1 interrupt (that is how Linux gets its tick
 * on this SoC), so it is delivered to us as an IRQ. We program it via the
 * EL0/EL1 physical-timer registers CNTP_CVAL_EL0 / CNTP_CTL_EL0, which are
 * freely accessible from EL2 (no trap: we are above EL1, and CNTHCTL_EL2
 * gating only affects EL0/EL1 accesses, not EL2's own). The comparator uses
 * the same CNTPCT physical counter, so the deadline math is identical. */
#define TIMER_INTID   30u
#define GICD_WORD(id) ((id) / 32u)
#define GICD_BIT(id)  ((id) % 32u)
#define GICC_IAR_INTID(iar) ((iar) & 0x3ffu)

/* Priority value for our one PPI. GICv2 priorities are 8-bit, lower value
 * = higher priority. We use 0x00 (highest) deliberately: in the non-secure
 * banked view this board runs, a NS-written priority is remapped
 * (secure = (ns>>1)|0x80), so a NS 0x00 maps to secure 0x80 - still the
 * highest a non-secure interrupt can be and unambiguously below GICC_PMR
 * (which reads back ~0xf0 in the NS view). This removes any doubt that the
 * interrupt clears the priority mask at the CPU interface. */
#define TIMER_PRIORITY 0x00u

/* GICv2 spurious interrupt IDs (nothing pending). */
#define GIC_SPURIOUS_MIN 1020u

/* ------------------------------------------------------------------ *
 * CNTP (non-secure EL1 physical timer) system registers. Accessible from
 * EL2 without trapping. Comparator is against CNTPCT (physical counter).
 * ------------------------------------------------------------------ */
static inline void
write_cntp_cval(uint64_t v)
{
	__asm__ volatile("msr cntp_cval_el0, %0" :: "r"(v) : "memory");
}

static inline void
write_cntp_ctl(uint32_t v)
{
	__asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)v) : "memory");
}

static inline uint32_t
read_cntp_ctl(void)
{
	uint64_t v;
	__asm__ volatile("isb sy" ::: "memory");
	__asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v));
	return (uint32_t)v;
}

/* CNTP_CTL_EL0 bits: [0]=ENABLE, [1]=IMASK, [2]=ISTATUS (read-only, set by
 * hardware while the timer condition CNTPCT >= CVAL is met). ISTATUS is the
 * decisive "did the timer actually fire?" flag. */
#define CNTP_CTL_ENABLE  (1u << 0)
#define CNTP_CTL_IMASK   (1u << 1)
#define CNTP_CTL_ISTATUS (1u << 2)

/* ------------------------------------------------------------------ *
 * CNTV (EL1 VIRTUAL timer) — the GUEST's timer. FreeBSD uses CNTV, so
 * when EL2 owns physical IRQs (IMO=1) it takes the guest's CNTV PPI
 * (INTID 27) here and must forward it as a vIRQ (see vgic_inject_cntv).
 * These registers are accessible from EL2 and — because we run on the
 * SAME core as the guest (CPU0) and CNTVOFF_EL2=0 — read/write exactly
 * the guest's virtual-timer state (NOT a banked/other-core copy, unlike
 * the GICH/GICC gotcha the vgic memory documents). CNTV_CTL bits mirror
 * CNTP_CTL: [0]ENABLE [1]IMASK [2]ISTATUS. */
static inline uint32_t
read_cntv_ctl(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(v));
	return (uint32_t)v;
}

static inline void
write_cntv_ctl(uint32_t v)
{
	__asm__ volatile("msr cntv_ctl_el0, %0\n\tisb" :: "r"((uint64_t)v) : "memory");
}

static inline uint64_t
read_cntv_cval(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(v));
	return v;
}

static inline uint64_t
read_cntvct(void)
{
	uint64_t v;
	v = cntvct_read();   /* cntpct.h: Allwinner counter erratum */
	return v;
}

#define CNTV_CTL_ENABLE  (1u << 0)
#define CNTV_CTL_IMASK   (1u << 1)
#define CNTV_CTL_ISTATUS (1u << 2)

/* ------------------------------------------------------------------ *
 * Guest CNTV mask watchdog — recovery for the software-vtimer handshake.
 *
 * THE BUG THIS FIXES (found on hardware 2026-07-29). The VGIC_CNTV_HW=0
 * path below masks the guest's virtual timer AT THE SOURCE
 * (CNTV_CTL.IMASK=1) so the level-triggered CNTV line de-asserts, then
 * hands the tick to the guest as a pure-virtual vIRQ. Clearing that mask is
 * left ENTIRELY to the guest: its timer ISR rewrites CNTV_CTL (ENABLE=1,
 * IMASK=0) as a side effect of re-arming, which is exactly what FreeBSD's
 * one-shot eventtimer does anyway.
 *
 * That makes the guest's whole timebase depend on EVERY SINGLE injection
 * being received. Miss one — for any reason — and the mask EL2 set is never
 * cleared, no further CNTV PPI can ever fire, and the guest has no timer for
 * the rest of the boot. Observed exactly that: the flight-recorder ring
 * (flightrec.h) showed 446 healthy FLTR_K_TIMER events, each sampling
 * CNTV_CTL=5 (ENABLE=1, IMASK=0, ISTATUS=1 — i.e. the guest HAD cleared the
 * previous tick's mask, so the handshake was working), then TIMER events
 * stopped dead ~696 events before the freeze and never resumed. virtio-blk
 * kept running (its physical EDGE SPI is delivered natively), and once its
 * completions drained the guest went to WFI awaiting a tick that could never
 * arrive. CPU1 and CPU2 stayed healthy; only the guest core was idle.
 *
 * WHY THE FIX IS NOT "find the lost injection". The reason that one handoff
 * failed is not established — and deliberately not what this guards. A
 * one-shot loss (a full List Register, a guest ISR that took an unexpected
 * path, a race with the guest rewriting CNTV_CTL under us) is a hiccup; a
 * one-shot loss that permanently kills the timebase is a design defect. The
 * defect is that EL2 masks a source and then relies on the GUEST to unmask
 * it, with no recovery if the guest never does. So: EL2 must clean up after
 * itself.
 *
 * HOW. gic_timer.c already owns a periodic EL2 tick (TIMER_INTID/CNTP,
 * handled at the bottom of gic_timer_irq()) that runs ON THE GUEST'S CORE,
 * which is what makes reading the guest's banked CNTV_CTL from here
 * meaningful at all — the same read issued by the CPU1 debug core would
 * report CPU1's bank and be worthless. On each EL2 tick, if a mask WE set is
 * still standing after a short grace period, clear it ourselves.
 *
 * `gt_cntv_el2_masked` tracks OUR mask specifically, so a guest that masks
 * its own timer on purpose is never overridden — we only ever undo a write
 * this file made.
 *
 * Un-masking while the comparator is still in the past re-asserts CNTV
 * immediately, so EL2 takes the PPI again, masks again, and re-injects. That
 * is intended: it becomes a retry at EL2-tick rate (a handful of PPIs per
 * second), not the 145 kHz storm that masking exists to prevent, and each
 * retry gives the guest another chance to receive the tick. Bounded either
 * way — the grace counter only advances on our own tick.
 *
 * Self-gating: under VGIC_CNTV_HW=1, or in a build where vgic_active() is
 * false, nothing here ever sets IMASK, so gt_cntv_el2_masked stays 0 and the
 * watchdog is a no-op.
 * ------------------------------------------------------------------ */
/* EL2 ticks to wait before overriding. 2 (not 1) so a guest ISR that simply
 * lands a little after our tick boundary is never pre-empted — the normal
 * path must stay untouched; this only ever fires on a genuinely stuck mask. */
#define CNTV_MASK_GRACE_TICKS 2u

/* ------------------------------------------------------------------ *
 * Per-core module state (Phase 2 step P1, docs/phase2-all-cores.md §1.6).
 *
 * STATE INVENTORY. Every mutable module-level variable this file had before
 * this change, cited against the pre-P1 tree (line numbers as read by the
 * pass that wrote docs/phase2-all-cores.md, confirmed unchanged here), and
 * the per-core/global verdict for each:
 *
 *   gt_period_ticks    (gic_timer.c:496)  PER-CORE. CNTP_CVAL_EL0/CTL_EL0
 *                       are banked per PE, so the period a core re-arms
 *                       from is inherently that core's own fact.
 *   gt_next_deadline    (gic_timer.c:497) PER-CORE, same reason — this is
 *                       the software shadow of THIS core's banked CVAL.
 *   gt_ticks            (gic_timer.c:498) PER-CORE: a count of ticks THIS
 *                       core has handled.
 *   gt_mismatches       (gic_timer.c:499) PER-CORE by the same logic. Note
 *                       for whoever reads this next: it is dead weight
 *                       either way — grepped before this change and nothing
 *                       in this file has ever incremented it (only zeroed
 *                       at init and read for the breadcrumb). Kept exactly
 *                       as vestigial as it was found; this pass is a pure
 *                       state-scoping change, not a logic fix.
 *   gt_jitter           (gic_timer.c:500) PER-CORE: measures THIS core's
 *                       own tick timing against timer_now().
 *   irq_counter[160]    (gic_timer.c:505) PER-CORE. GICC_IAR is a banked,
 *                       per-PE view (ORIENTATION.md rule 4: "a read over the
 *                       debug channel is serviced by CPU1 and tells you
 *                       nothing about the guest's core") — the INTIDs
 *                       counted here are "what THIS core's CPU interface
 *                       handed THIS core", never a system-wide fact.
 *   gt_cntv_el2_masked, (gic_timer.c:391-393) PER-CORE. CNTV_CTL_EL0 is
 *   gt_cntv_masked_ticks,   banked per PE and belongs to whichever guest
 *   gt_cntv_rescues         runs on THIS core — vtimer_mask_watchdog()'s own
 *                       header comment above already made the argument:
 *                       "the same read issued by the CPU1 debug core would
 *                       report CPU1's bank and be worthless". That was
 *                       already why this state is inherently per-core; P1
 *                       just makes the storage match the argument.
 *
 * No global-only, and no "global but only ever touched by one core today"
 * state was found among gic_timer.c's mutable module statics — every one of
 * them shadows something the hardware itself banks per PE. All of it moves
 * into struct gt_percpu below, one instance per core, indexed by
 * smp_cpu_id() via gt_self().
 *
 * On CPU0 alone — every board target today; gic_timer_init()/gic_timer_irq()
 * are called only from main_dbg.c's CPU0 boot path, grepped before this
 * change — the index is always 0, so this is a pure storage-layout change
 * with byte-identical behaviour: same values, same breadcrumbs, same IRQ
 * handling, just addressed through g_gt[0] instead of file-scope statics.
 * Cache-line-padded (64 B) exactly like smp.c's own per-core pattern
 * (struct smp_percpu, smp.c:183-188), so a future second core taking this
 * IRQ never shares a cache line with CPU0's copy.
 *
 * BREADCRUMB NOTE (hv_addrmap.h / the breadcrumb-window-hygiene discipline).
 * GICT_BC_BASE (0x00018200) and IRQ_COUNTER_BC_BASE (0x00018300) stay
 * SINGLE, file-scope windows — NOT split per core. Deliberate, not an
 * oversight: nothing in the current tree ever calls gic_timer_init() or
 * gic_timer_irq() from any core but CPU0 (see above), so there is exactly
 * one writer today, and splitting the window now would be speculative
 * complexity with no way to exercise or test it. Whichever later Phase-2
 * step (P2b/P3+) actually arms this tick on a second core MUST give that
 * core its own breadcrumb lane before doing so — allocated in
 * hv_addrmap.h's _Static_assert chain like every other window, never
 * squeezed into a neighbour's space — because at that point two cores
 * really would interleave writes into these two windows. Flagged here, not
 * fixed here: a lane for a caller that does not exist yet is exactly the
 * kind of unused complexity this tree's breadcrumb history (hv_addrmap.h's
 * own "lost six windows to a neighbour grown without slack" note) warns
 * against.
 * ------------------------------------------------------------------ */
struct gt_percpu {
	uint64_t period_ticks;
	uint64_t next_deadline;
	uint64_t ticks;
	uint32_t mismatches;
	struct jitter jitter;
	uint32_t irq_counter[160];
	uint32_t cntv_el2_masked;     /* 1 = WE set IMASK, guest hasn't cleared */
	uint32_t cntv_masked_ticks;   /* EL2 ticks our mask has survived        */
	uint32_t cntv_rescues;        /* times we had to unmask it ourselves    */
	/* MUSB storm throttle (added 2026-08-26, HARDWARE-PROVEN NECESSARY — see
	 * the MUSB_IRQ_BUDGET_PER_TICK comment at the dispatch site). Per-core for
	 * the same reason irq_counter[] is: this counts what THIS core's own CPU
	 * interface handed it. */
	uint32_t musb_irqs;           /* MUSB IRQs taken since the last tick    */
	uint32_t musb_throttles;      /* times the budget ran out and we masked */
	/* Arrivals on a core that is NOT SMP_DEBUG_CPU — i.e. the guest's own
	 * vCPU0 taking an interrupt EL2 targeted at CPU1. Nonzero means the
	 * distributor's ITARGETSR has drifted (the guest's GIC driver resets it);
	 * the tick's re-assert should drive these back to a standstill. Added
	 * 2026-08-26, when they were the whole story rather than a corner case. */
	uint32_t musb_wrong_core;
	uint32_t hdmi_wrong_core;
#ifdef HV_HDMI
	/* TCON1 vblank storm throttle, same shape as the MUSB pair above and
	 * for the same reason (HDMI_IRQ_BUDGET_PER_TICK comment at the
	 * dispatch site) -- per-core, though only CPU1 ever populates it. */
	uint32_t hdmi_irqs;           /* TCON1 vblank IRQs since the last tick  */
	uint32_t hdmi_throttles;      /* times the budget ran out and we masked */
#endif
} __attribute__((aligned(64)));

static struct gt_percpu g_gt[SMP_MAX_CPUS];

#ifdef HV_HDMI
/* Last gt->ticks value (this core's OWN per-core tick count, already
 * maintained by gic_timer_irq() below — see the "ticks" field in struct
 * gt_percpu above and vcpu1.h's breadcrumb-layout note on word [13], "this
 * core's own gic_timer tick count") at which the HDMI PHY relock tick-path
 * service (below) last called hdmi_relock(). Deliberately NOT a new
 * per-loop-pass counter like smp.c's `last_relock_iters` — that variable
 * lived in the old free-running debug loop, which this tick path replaces
 * entirely, and a tick count is what a wall-clock rate limit needs anyway
 * (see the usage site for the full 65536-iterations -> ~1s conversion
 * reasoning). Only CPU1 (SMP_DEBUG_CPU) ever reads or writes this, so a
 * single file-scope static, not a per-core array slot, is sufficient. */
static uint64_t hdmi_last_relock_tick;
#endif

/* ---- CPU1-as-third-vCPU tick-path service (vcpu1.c, EXPERIMENTAL) --------
 * See vcpu1.h for the full design/tradeoff. Weak so every target that
 * doesn't link vcpu1.o (gdb, the QEMU CI variants, fbsd/zephyr) sees a
 * permanent 0 and the block in gic_timer_irq() below is simply dead code —
 * same pattern as every other opt-in feature this tree gates this way
 * (vcpu2.c's dbg_vcpu2, vinput.c's device, etc). dbgmon_service() itself is
 * declared (non-weak) by dbgmon.h; this weak DEFINITION is what lets a
 * build without dbgmon.o (the `gdb` target links gdbstub.c instead) still
 * link — dbgmon.o's real definition overrides it wherever both are linked. */
__attribute__((weak)) volatile uint32_t dbg_vcpu1;
__attribute__((weak)) void dbgmon_service(struct el2_frame *guest_frame)
{
	(void)guest_frame;
}

#ifdef HV_HDMI
/* HDMI PHY relock (hdmi.c), same weak-fallback reasoning as dbgmon_service()
 * above, needed for the same class of reason even though the call sites
 * below are themselves wrapped in #ifdef HV_HDMI: HV_HDMI is a *target*-
 * scoped CFLAGS addition (only `dbg`'s Makefile rule sets -DHV_HDMI today —
 * see the Makefile's own "HV_HDMI belongs to the target, not to whoever
 * remembers the command line" comment, which exists precisely because this
 * tree has already been bitten once by a flag/object-list mismatch here),
 * not a promise that whichever target defines it will always also link
 * hdmi.o. Weak fallbacks make that pairing a performance/behavior nicety
 * instead of a link-time landmine: hdmi_phy_locked() reporting "locked" and
 * hdmi_relock() doing nothing are both fail-safe (never attempts a relock)
 * if hdmi.c's real, strong definitions are ever absent from a build that
 * still sets -DHV_HDMI. dbg_hdmi_relock is NOT weak here, unlike the two
 * functions: it is defined unconditionally in smp.c (no #ifdef there), and
 * smp.o is linked into every target this tree builds — the same
 * "already provides a strong definition everywhere" situation wdt_debug_kick
 * is in below, just via smp.h instead of wdt.h. */
__attribute__((weak)) int hdmi_phy_locked(void)
{
	return 1;   /* fail "locked" => the tick path never calls hdmi_relock() */
}
__attribute__((weak)) void hdmi_relock(void)
{
}
extern volatile uint32_t dbg_hdmi_relock;   /* smp.c, always linked, not weak */

/* Same weak-fallback reasoning again, for the HDMI_TCON1_IRQ_INTID dispatch
 * (hdmi_irq_arm_cpu1() below and its call site in gic_timer_irq()): these
 * run whenever HV_HDMI is defined, independent of whether hdmi.o is
 * actually linked. hdmi_vblank_poll() doing nothing and
 * hdmi_vblank_irq_enable() doing nothing are both fail-safe (never touches
 * TCON_INT0) if hdmi.c's real definitions are absent. */
__attribute__((weak)) void hdmi_vblank_poll(void)
{
}
__attribute__((weak)) void hdmi_vblank_irq_enable(void)
{
}
#endif

/* Real definition lives in el2_exc.c (or a QEMU stub); every target that
 * links THIS file already links one or the other (see el2_exc.c's own
 * g_last_guest_frame / smp.c's identical extern for the existing precedent —
 * duplicated here rather than pulled from a shared header, matching that
 * convention). Not weak: unlike dbg_vcpu1/dbgmon_service, every target
 * really does provide a strong definition of this one already. */
extern void el2_snapshot_guest_frame(struct el2_frame *out);

/* Defensive clamp only — MPIDR affinity0 on this SoC is architecturally
 * 0..3 (SMP_MAX_CPUS==4, smp.h:42), so the out-of-range arm can never be hit
 * on real hardware or under QEMU virt (single CPU => index 0). Guards the
 * array the same way smp.c's own per-core accessors do (smp.c:420,
 * smp.c:435: "if (cpu >= SMP_MAX_CPUS) return/skip") rather than trusting
 * the invariant blindly inside an IRQ handler. */
static inline struct gt_percpu *gt_self(void)
{
	uint32_t cpu = smp_cpu_id();

	return &g_gt[(cpu < SMP_MAX_CPUS) ? cpu : 0u];
}

/* Called from the EL2 tick, on the guest's core, IRQ context. Bounded: a
 * couple of system-register accesses and at most one breadcrumb store.
 * Takes the CALLING core's own gt_percpu slot (fetched once by the caller,
 * gic_timer_irq()) — see the state-inventory comment above struct gt_percpu
 * for why this trio is per-core: CNTV_CTL_EL0 is banked per PE. */
static void
vtimer_mask_watchdog(struct gt_percpu *gt)
{
	uint32_t ctl;

	if (!gt->cntv_el2_masked)
		return;                  /* nothing of ours outstanding */

	ctl = read_cntv_ctl();
	if (!(ctl & CNTV_CTL_IMASK)) {
		/* Guest ISR ran and rewrote CNTV_CTL — the handshake worked. */
		gt->cntv_el2_masked = 0;
		gt->cntv_masked_ticks = 0;
		return;
	}

	if (++gt->cntv_masked_ticks < CNTV_MASK_GRACE_TICKS)
		return;

	/* Our mask outlived the grace period: the guest never got the tick.
	 * Undo our own write so CNTV can fire again. */
	write_cntv_ctl(ctl & ~CNTV_CTL_IMASK);
	gt->cntv_el2_masked = 0;
	gt->cntv_masked_ticks = 0;
	gt->cntv_rescues++;

	/* Record it in the flight recorder and NOWHERE ELSE. This file's own
	 * GICT breadcrumb window would be the obvious place, but confirmed live
	 * 2026-07-29 that it is unusable: GICT sits at 0x00018200 in
	 * GUEST-WRITABLE SRAM and read back as random bytes (magic not "GICT"),
	 * and vgic.c's window at 0x50001c00 read back as ASCII console text
	 * because it falls inside vconsole's 64 KiB postmortem capture ring
	 * (0x50000f10..0x50010f10 — flightrec.h's header already warns that
	 * every window nominally in that range gets clobbered). Parking a value
	 * there would silently be someone else's bytes. The FLTR ring is the one
	 * instrument that survived, so use it alone.
	 *
	 * THROTTLED logging. A rescue on every tick is a legitimate steady state
	 * (it means the guest is getting no ticks at all, which is exactly the
	 * condition worth seeing), but logging each one floods the ring: measured
	 * 2026-07-29 at ~2200 events/s, which wraps FLTR's 2048 slots in under a
	 * second and destroys its value as a post-mortem record — including any
	 * one-shot entry written earlier in the boot. So sample instead: rate is
	 * still obvious from the count carried in the record, and the ring keeps
	 * holding real history.
	 *
	 * The record deliberately carries the state of the INJECTION PATH, read
	 * HERE, on the guest's own core. This is the only place in the tree that
	 * can answer "can the vGIC actually deliver?": GICH_* and HCR_EL2 are
	 * banked per PE, so reading them over the debug channel returns CPU1's
	 * bank (never initialised, reads as zero) and yields a confident wrong
	 * answer — confirmed the hard way on 2026-07-29. a0 = CNTV_CTL,
	 * a1 = (rescues << 32) | (GICH_HCR.En << 1) | HCR_EL2.IMO.
	 *
	 * Uses its OWN kind (FLTR_K_VTRESCUE) rather than tagging a bit inside
	 * FLTR_K_TIMER's payload — see that enum's comment for the bug the first
	 * attempt caused. */
	if ((gt->cntv_rescues & 0xFFu) == 1u) {
		uint64_t hcr_el2;
		uint32_t gich_hcr = *(volatile uint32_t *)(SOC_A64_GICH_BASE);

		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr_el2));
		flightrec_log(FLTR_K_VTRESCUE, ctl,
		              ((uint64_t)gt->cntv_rescues << 32)
		              | (uint64_t)((gich_hcr & 1u) << 1)
		              | (uint64_t)((hcr_el2 >> 4) & 1u));
	}
}
/* FLTR_K_TIMER (flightrec.h): a0 = CNTV_CTL_EL0 (bit0 ENABLE, bit1 IMASK,
 * bit2 ISTATUS), a1 = signed (CNTV_CVAL - CNTVCT): negative => comparator
 * already in the past (timer still asserting), large positive => the guest
 * reprogrammed the deadline into the future (its ISR ran). */

/* ------------------------------------------------------------------ *
 * HCR_EL2 — physical-interrupt routing to EL2. See the big note in
 * gic_timer_init() for why IMO must be set on this AArch64 part.
 * ------------------------------------------------------------------ */
static inline uint64_t
read_hcr_el2(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(v));
	return v;
}

static inline void
write_hcr_el2(uint64_t v)
{
	__asm__ volatile("msr hcr_el2, %0\n\tisb sy" :: "r"(v) : "memory");
}

#define HCR_EL2_IMO (1ull << 4) /* physical IRQ  routed to EL2 */
#define HCR_EL2_FMO (1ull << 3) /* physical FIQ  routed to EL2 */
#define HCR_EL2_AMO (1ull << 5) /* physical SError routed to EL2 */

/* ------------------------------------------------------------------ *
 * Module state: struct gt_percpu / g_gt[] / gt_self(), defined above (see
 * the P1 state inventory just before vtimer_mask_watchdog()). Nothing left
 * to declare here — kept as a section marker so a future reader who
 * remembers this comment's old location still finds something.
 * ------------------------------------------------------------------ */

/* INTID counter for interrupt storm diagnostics (internal task). Lives in
 * struct gt_percpu now (per-core — see the inventory above: GICC_IAR is
 * banked per PE, so this was never really a global fact). This window still
 * reports only the CALLING core's counts, which is byte-identical to before
 * since only CPU0 ever calls gic_timer_irq() today (see the breadcrumb note
 * above). Exposed via breadcrumb at 0x00018300+. */
#define IRQ_COUNTER_BC_BASE 0x00018300UL

/* Report to the shared jitter window (0x50000500) every this many ticks -
 * bounded work per IRQ, not on every single tick, to keep the handler
 * cheap; still frequent enough to watch the tick settle live. */
#define REPORT_EVERY 100u

/* Upper bound on the init-time diagnostic busy-wait (see gic_timer_init).
 * Generous: ~a few ms at 24 MHz even if each iteration is a single cheap
 * counter read, so a healthy 1 ms deadline is always observed well before
 * the cap; the cap only exists so a dead/stopped counter can't hang init. */
#define GT_POLL_MAX 8000000u

/* ------------------------------------------------------------------ *
 * Breadcrumb window: 0x00018200 ("GICT"). See file header for layout.
 * ------------------------------------------------------------------ */
#define GICT_BC_BASE 0x00018200UL
#define GICT_BC_MAGIC 0x47494354u /* "GICT" */

static inline void
gict_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(GICT_BC_BASE + (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * CPU-interface-wide + distributor group-enable setup — the half of the old
 * monolithic gic_timer_init() that is NOT specific to INTID 30/CNTP. See
 * gic_timer.h for the full rationale (factored out for main_dbg.c's
 * interrupt-virtualization policy, which wants this but not the CNTP tick).
 * ------------------------------------------------------------------ */
void
gic_timer_cpuif_init(void)
{
	/* Enable groups at the distributor. In the non-secure view (which the
	 * v2 test proved we are in) bit 0 is EnableGrp1 - exactly what every
	 * Group 1 interrupt (device SPIs, CNTP, CNTV, the vgic maintenance PPI)
	 * needs to reach the CPU interface at all. Writing 0x3 also covers a
	 * combined/secure view (bit1 = EnableGrp1); the extra bit is RAZ/WI in
	 * the NS view. */
	GICD_CTLR = 0x3u;

	/* PMR wide open so any priority we or the vgic assign always clears the
	 * mask (note: the NS view exposes only the top nibble, so 0xff reads
	 * back as 0xf0 - still well above any priority 0x00-0x80 we ever use).
	 * Enable Group 1 forwarding (bit0 in NS view) + EOImode=1 (bit9) for
	 * hardware virtualization: EOIR only drops priority, DIR deactivates —
	 * required so an HW-mode List Register's guest-EOI-triggers-physical-
	 * deactivate magic works (see vgic.c), and so gic_timer_irq() can EOI a
	 * device IRQ without deactivating it out from under an in-flight LR. */
	GICC_PMR = 0xffu;
	GICC_CTLR = 0x3u | (1u << 9);
}

/* ------------------------------------------------------------------ *
 * Init: GIC (distributor + CPU interface, one PPI only) + CNTHP.
 * ------------------------------------------------------------------ */
void
gic_timer_arm_preserving_cntvoff(uint32_t period_us)
{
	uint64_t saved;

	/* See this function's header comment for why the guest's virtual timebase
	 * must survive arming the tick. Read-arm-restore rather than duplicating
	 * gic_timer_init()'s body, so the two can never drift apart. */
	__asm__ volatile("mrs %0, cntvoff_el2" : "=r"(saved));
	gic_timer_init(period_us);
	__asm__ volatile("msr cntvoff_el2, %0\n\tisb" :: "r"(saved) : "memory");
}

void
gic_timer_init(uint32_t period_us)
{
	struct gt_percpu *gt = gt_self();
	uint64_t freq = timer_freq();
	uint64_t period_ticks;
	uint64_t now;

	if (freq == 0)
		freq = 24000000ull; /* A64-typical fallback, matches timer.c */

	/* CNTHCTL_EL2 is intentionally NOT modified here. On this board ATF/BL31
	 * sets CNTHCTL_EL2=0x3 (EL1PCTEN=1, EL1PCEN=1), so the guest already has
	 * native EL1 access to CNTPCT_EL0 and CNTP_CTL/CVAL — no trap needed.
	 * This was verified live via `sr2` (cnthctl_el2=0x0000000000000003).
	 * The guest's DELAY() reads CNTPCT directly and works. The physical
	 * timer (CNTP_CTL/CVAL) is shared with this module's tick, but since
	 * the guest uses the virtual timer (CNTV_*) for its own interrupts,
	 * there is no conflict in practice. */

	/* CNTVOFF_EL2: ATF/BL31 sets this to a large non-zero value (observed
	 * 0xc0000240028900), making CNTVCT_EL0 = CNTPCT - CNTVOFF ≈ 0 for the
	 * guest. FreeBSD's DELAY() and getcycles() use CNTVCT_EL0 (not CNTPCT),
	 * so a near-zero virtual counter causes DELAY() to hang forever. Zero
	 * the offset so CNTVCT = CNTPCT (advancing at 24 MHz). Verified live:
	 * the fix was first applied via hot-patch + call (msr cntvoff_el2, xzr
	 * patched over onebp_arm), confirmed by reading CNTPCT before/after. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	period_ticks = (freq * (uint64_t)period_us) / 1000000ull;
	if (period_ticks == 0)
		period_ticks = 1;

	gt->period_ticks = period_ticks;
	gt->ticks = 0;
	gt->mismatches = 0;
	jitter_init(&gt->jitter, period_ticks);

	/* --- GIC distributor -------------------------------------------- *
	 * ROUTING FIX (v3): use INTID 30 (CNTP, non-secure physical timer),
	 * which firmware owns as a Group 1 (non-secure) interrupt and therefore
	 * delivers to us as an IRQ. We do NOT try to reprogram its group: the
	 * v2 attempt to move INTID 26 into Group 1 was RAZ/WI because 26/CNTHP
	 * is secure-owned (see the TIMER_INTID comment). INTID 30 should already
	 * read as Group 1 in IGROUPR0 - we record it (word 11) rather than force
	 * it, and if bit30 is unexpectedly 0 we still try the |= (harmless if
	 * RAZ/WI, effective if the GIC happens to allow it).
	 *
	 * IGROUPR0 covers INTIDs 0..31 (SGIs+PPIs), one bit each. */
	GICD_IGROUPR(GICD_WORD(TIMER_INTID)) |= (1u << GICD_BIT(TIMER_INTID));

	GICD_IPRIORITYR_BYTE(TIMER_INTID) = (uint8_t)TIMER_PRIORITY;
	GICD_ISENABLER(GICD_WORD(TIMER_INTID)) = (1u << GICD_BIT(TIMER_INTID));

	/* --- GIC distributor group-enable + CPU interface ---------------- *
	 * Factored into gic_timer_cpuif_init() (see gic_timer.h) so a caller
	 * that wants EL2 to see IRQs WITHOUT this module's own CNTP tick (e.g.
	 * main_dbg.c's interrupt-virtualization policy) can get just this half.
	 * Calling it here keeps this function's own behavior identical to
	 * before the split. */
	gic_timer_cpuif_init();

	/* --- Arm the CNTP (non-secure physical) comparator for the first    */
	/* interval. Absolute deadline (CVAL) not relative reload (TVAL) so    */
	/* the periodic tick doesn't drift by the handler's own runtime - each */
	/* re-arm adds a fixed period to the *previous* deadline (see          */
	/* gic_timer_irq()). CVAL is a FUTURE point (now + period); the timer  */
	/* asserts (and CNTP_CTL.ISTATUS sets) when CNTPCT >= CVAL.            */
	now = timer_now();
	gt->next_deadline = now + period_ticks;
	write_cntp_cval(gt->next_deadline);
	write_cntp_ctl(CNTP_CTL_ENABLE); /* ENABLE=1, IMASK=0 */

	/* --- ROUTING FIX (v4): route physical IRQ to EL2 -------------------- *
	 * The v3 hardware test proved the interrupt is pending+enabled at the
	 * GIC (ISPENDR0 bit30=1), the timer fired (CNTP_CTL.ISTATUS=1), yet the
	 * CPU never took it and there was no storm. That is the fingerprint of
	 * an AArch64 *routing* problem, not a GIC problem:
	 *
	 *   With HCR_EL2.IMO == 0 (the reset default), a physical IRQ is routed
	 *   to EL1. An asynchronous exception whose target EL is LOWER than the
	 *   current EL is not taken - it stays pending. Since this system runs
	 *   at EL2, an IRQ routed to EL1 can never be taken here regardless of
	 *   PSTATE.I. Setting HCR_EL2.IMO = 1 routes physical IRQ to EL2, so it
	 *   is taken at the current EL (gated only by PSTATE.I, which the
	 *   integrator unmasks). This corrects my earlier (wrong) claim that
	 *   IMO was unnecessary "because nothing runs below EL2" - it is exactly
	 *   because we run AT EL2 and want the IRQ HERE that IMO must be 1.
	 *
	 * We set it read-modify-write (OR in only bit 4) so no other HCR_EL2
	 * configuration U-Boot/earlier boot set is disturbed. It is idempotent:
	 * if the integration lane also sets IMO in el2_install(), this OR is a
	 * harmless no-op. FMO (FIQ->EL2) and AMO (SError->EL2) are intentionally
	 * left as-is: our tick is a Group 1 IRQ, so IMO is what matters; if the
	 * integrator additionally unmasks F and a source ever arrives as FIQ,
	 * FMO would also be needed - flagged, not set. Ownership note: HCR_EL2
	 * is EL2 setup that naturally belongs to el2_install(); we set it here
	 * to make the tick self-contained and unblock testing, and the
	 * integration lane may relocate it with no behavioural change. */
	{
		uint64_t hcr_before = read_hcr_el2();
		uint64_t hcr_after  = hcr_before | HCR_EL2_IMO;

		write_hcr_el2(hcr_after);
		gict_bc(19, (uint32_t)hcr_before); /* HCR_EL2 before (lo) */
		gict_bc(20, (uint32_t)read_hcr_el2()); /* HCR_EL2 after (lo): bit4 must be 1 */
	}

	/* Breadcrumb: static/init-time facts + register readbacks. Words 8..18
	 * capture the *actual* register state right after programming, so the
	 * next hardware read shows exactly which link in the chain is broken -
	 * no guessing. */
	gict_bc(4, (uint32_t)period_ticks);
	gict_bc(6, 1u); /* init_done */
	gict_bc(8, read_cntp_ctl());                   /* CNTP_CTL: expect ENABLE=1 */
	gict_bc(9, GICD_CTLR);                          /* distributor CTLR readback */
	gict_bc(10, GICC_CTLR);                         /* CPU-interface CTLR readback */
	gict_bc(11, GICD_IGROUPR(GICD_WORD(TIMER_INTID))); /* IGROUPR0: bit30 => Grp1 */
	gict_bc(12, (uint32_t)now);                     /* CNTPCT at arm time (lo) */
	gict_bc(13, (uint32_t)gt->next_deadline);       /* CNTP_CVAL deadline (lo) */
	gict_bc(14, GICC_PMR);                          /* PMR readback (NS: ~0xf0) */
	gict_bc(15, GICD_ISENABLER(GICD_WORD(TIMER_INTID))); /* ISENABLER0: bit30=1 */

	/* --- DIAGNOSTIC PROOF: bounded busy-wait until the deadline passes, *
	 * then sample CNTP_CTL.ISTATUS and GICD_ISPENDR0. This runs with IRQs
	 * still masked (init is called before the integrator's daifclr), so
	 * the interrupt cannot be taken here - it just latches pending, which
	 * is precisely what we want to observe. Decision table for the next
	 * hardware read of this window:
	 *   word17 ISTATUS bit2 == 1  => the CNTP timer condition really fired
	 *                                (correct timer + CVAL semantics).
	 *   word16 ISPENDR0 bit30 == 1 => the GIC latched INTID 30 pending, i.e.
	 *                                CNTP is wired to PPI 30 and enabled.
	 *   Both set but ticks still 0 => the gap is CPU-if delivery / routing.
	 *                                v3 hit exactly this; root cause was
	 *                                HCR_EL2.IMO=0 (IRQ routed to EL1, never
	 *                                taken at EL2), now fixed below. Words
	 *                                19-24 (HCR_EL2 + IAR/RPR probe) confirm.
	 *   ISTATUS 1 but ISPENDR0 bit30 0 => CNTP is NOT on PPI 30 here; check
	 *                                which ISPENDR bit did set (whole word
	 *                                is recorded, not just bit30).
	 *   ISTATUS 0 => the timer never asserted (wrong reg/CVAL/counter).
	 * The wait is bounded to GT_POLL_MAX iterations (~covers a few ms at
	 * 24 MHz); word18 records whether we observed CNTPCT>=CVAL (1) or hit
	 * the iteration cap (0, meaning the counter isn't advancing as
	 * expected). No unbounded loop. */
	{
		uint32_t i;
		uint32_t observed = 0;

		for (i = 0; i < GT_POLL_MAX; i++) {
			if (timer_now() >= gt->next_deadline) {
				observed = 1;
				break;
			}
		}
		gict_bc(16, GICD_ISPENDR(GICD_WORD(TIMER_INTID)));
		gict_bc(17, read_cntp_ctl());  /* bit2 ISTATUS = timer fired */
		gict_bc(18, observed);
	}

	/* --- DECISIVE CPU-INTERFACE PROBE (balanced IAR/EOI) ---------------- *
	 * We are still running with IRQs masked at the PSTATE level (init runs
	 * before the integrator's daifclr), but reading GICC_IAR does NOT depend
	 * on PSTATE.I - it directly asks the CPU interface "what would you hand
	 * the core right now?". This cleanly separates a GIC/CPU-interface fault
	 * from a pure routing/PSTATE fault:
	 *   word22 (IAR) == 30   => the CPU interface WOULD forward our INTID; so
	 *                           any remaining failure is routing/PSTATE only
	 *                           (i.e. the HCR_EL2.IMO fix above is the cure).
	 *   word22 (IAR) == 1023 => the CPU interface is masking it (priority /
	 *                           group / PMR) - a GIC-side fault to chase.
	 * We MUST balance the read: if IAR returns a real INTID we immediately
	 * write GICC_EOIR, otherwise the running priority (GICC_RPR) stays raised
	 * and blocks ALL future delivery - which would itself masquerade as this
	 * very bug. word21/23/24 capture RPR before / while-active / after-EOI so
	 * a stuck running priority is impossible to miss (idle RPR = 0xff).
	 *
	 * Because CNTP is level-triggered and still asserting (CVAL is now in the
	 * past, ISTATUS=1), the interrupt simply re-pends right after our EOI, so
	 * no tick is lost: once the integrator clears PSTATE.I it fires and
	 * gic_timer_irq() re-arms CVAL into the future, de-asserting it. */
	{
		uint32_t rpr_before = GICC_RPR;
		uint32_t iar        = GICC_IAR;
		uint32_t rpr_active = GICC_RPR;
		uint32_t intid      = GICC_IAR_INTID(iar);

		if (intid < GIC_SPURIOUS_MIN) {
			GICC_EOIR = iar; /* balance the acknowledge - never leave RPR raised */
			GICC_DIR = iar;
		}

		gict_bc(21, rpr_before);      /* expect 0xff (idle) before the probe */
		gict_bc(22, iar);             /* expect INTID 30; 1023 => GIC masking */
		gict_bc(23, rpr_active);      /* running prio while IAR active */
		gict_bc(24, GICC_RPR);        /* after EOI: MUST be back to 0xff idle */
	}

	gict_bc(0, GICT_BC_MAGIC);
}

/* ------------------------------------------------------------------ *
 * MUSB_IRQ_INTID (103, SPI 71, "mc" — musb.h) wiring for the CPU1-as-vCPU1
 * design (vcpu1.c, EXPERIMENTAL): make CPU1's GIC CPU interface the sole
 * target of the real MUSB aggregate SPI, so the tick-free, event-driven
 * usbacm_poll() dispatch below (gic_timer_irq()'s MUSB_IRQ_INTID arm) is
 * only ever presented to CPU1, never to CPU0 (which runs the primary guest
 * and may separately be running vgic_active()'s forwarding policy — this
 * SPI must NEVER be handed to vgic_inject_hw(), since the guest's own
 * usb@1c19000 DTB node is status="disabled" and has no driver for it; see
 * musb.h's citation and usbacm.h's "KNOWN OPEN RISK" section).
 *
 * Call ONCE from vcpu1_run(), after gic_timer_cpuif_init() (which already
 * did the CPU-interface-wide GICD_CTLR/GICC_CTLR/PMR enable this also
 * needs) and before unmasking IRQs on CPU1. Idempotent (plain register
 * writes), safe to call only when dbg_vcpu1 is being armed — nothing else
 * in this tree ever enables MUSB_IRQ_INTID at the distributor, so leaving
 * this uncalled (dbg_vcpu1 off) means the real SPI stays exactly as
 * harmless/unenabled as it always was.
 *
 * TODO(board), UNVERIFIED — read this before trusting the routing:
 *   1. GICD_ITARGETSR bit-to-core mapping. This assumes GICv2's architectural
 *      convention (target-list bit N == CPU interface N == affinity0 N),
 *      which is what every other SPI-targeting GIC-400 integration does, but
 *      NOTHING ELSE in this codebase has ever targeted an SPI at a specific
 *      core before today (every existing device-SPI path — EHCI/INTID 106,
 *      the generic vgic_inject_hw() catch-all — runs on whichever core
 *      happens to be executing gic_timer_irq(), which has only ever been
 *      CPU0 until vcpu1.c). Confirm on hardware: after calling this, read
 *      back GICD_ITARGETSR_BYTE(MUSB_IRQ_INTID) (expect 0x02, bit1) and,
 *      once traffic flows, confirm irq_counter[103] increments in CPU1's
 *      g_gt[] slot and NOT CPU0's.
 *   2. GICD_ICFGR (edge/level) for INTID 103 is deliberately left untouched
 *      here (not read, not written) — the DTB's <0 0x47 0x04> says
 *      level-high, and unlike the timer PPI (gic_timer_init()'s IGROUPR
 *      dance) nothing in this tree has ever needed to REPROGRAM a real
 *      device SPI's level/edge config, only trusted whatever firmware left
 *      it as. TODO(board): read back GICD_ICFGR for INTID 103 once and
 *      confirm it already reads as level (bit pattern 0b00 per pair) before
 *      relying on this — if it somehow reads edge, treat that as a real
 *      finding, not a copy-paste target for the vblk-emmc "prefer edge"
 *      precedent, which applies only to a SOFTWARE-pended SPI, not a real
 *      hardware line.
 *   3. GICD_IGROUPR — like gic_timer_init()'s TIMER_INTID handling, this
 *      does NOT force Group 1: it is left as whatever firmware/ATF already
 *      set for a real, firmware-known peripheral SPI (the EMAC/pinctrl SPIs
 *      this tree already services via vgic_inject_hw() on CPU0 prove
 *      device SPIs on this board DO reach non-secure EL2 as Group 1 IRQs
 *      today, which is why no explicit IGROUPR write is attempted here —
 *      but this specific INTID's group has never itself been read back).
 * ------------------------------------------------------------------ */
void
musb_irq_arm_cpu1(void)
{
	/* Priority: same highest-non-secure value as the CNTP tick (see
	 * TIMER_PRIORITY's own comment on the NS-view priority remap) — this
	 * is a real, latency-sensitive peripheral event, not diagnostics. */
	GICD_IPRIORITYR_BYTE(MUSB_IRQ_INTID) = (uint8_t)TIMER_PRIORITY;

	/* Target CPU interface 1 ONLY (bit1) — see TODO #1 above. Deliberately
	 * NOT OR'd with whatever was already there: if firmware had targeted
	 * this at CPU0 (plausible, since the node was "okay" before someone
	 * disabled it — see usbacm.h), leaving that bit set would let a real
	 * MUSB event ALSO interrupt CPU0's primary guest, which is exactly the
	 * hardware race usbacm.h's "KNOWN OPEN RISK" section warns about. A
	 * flat assignment is the safer default; if hardware testing (TODO #1)
	 * shows bit1 is not in fact CPU1, this write still leaves CPU0
	 * untargeted, which is the fail-safe direction. */
	GICD_ITARGETSR_BYTE(MUSB_IRQ_INTID) = (1u << 1);

	GICD_ISENABLER(GICD_WORD(MUSB_IRQ_INTID)) =
	    (1u << GICD_BIT(MUSB_IRQ_INTID));
}

#ifdef HV_HDMI
/* ------------------------------------------------------------------ *
 * HDMI_TCON1_IRQ_INTID (119, SPI 87, TCON1/"lcd-controller@1c0d000" — hdmi.h)
 * wiring for the CPU1-as-vCPU1 design (vcpu1.c, EXPERIMENTAL): the same
 * target-one-core treatment musb_irq_arm_cpu1() above gives the MUSB "mc"
 * SPI, this time for the real TCON1 vblank line, so the HUD-repaint/
 * real-vblank duty that CPU1's old tight poll loop used to provide (see
 * vcpu1.h's "WHAT ELSE STOPS RUNNING ON CPU1" section) is restored as a
 * genuinely event-driven interrupt rather than a revived fixed-period
 * poll — a 10 ms tick is the wrong fit for a ~16.7 ms vblank period, exactly
 * as vcpu1.h's own header explains for why this duty was NOT folded into
 * the tick path the way the eMMC pinmux fix and PHY relock were.
 *
 * Like MUSB_IRQ_INTID, this SPI must NEVER be handed to vgic_inject_hw():
 * the guest's own lcd-controller@1c0d000 DTB node is status="disabled" and
 * has no driver for it (hdmi.h's citation), so it is EL2-owned
 * unconditionally, same reasoning as MUSB's usb@1c19000.
 *
 * Call ONCE from vcpu1_run(), after gic_timer_cpuif_init() and before
 * unmasking IRQs on CPU1 — same call site as musb_irq_arm_cpu1(), see
 * vcpu1.h for exactly where it belongs (this file does not call vcpu1.c,
 * which is owned by another lane). Idempotent, safe to call only when
 * dbg_vcpu1 is being armed: nothing else in this tree ever enables
 * HDMI_TCON1_IRQ_INTID at the distributor OR sets TCON_INT0's enable bit
 * (hdmi_vblank_irq_enable(), hdmi.c), so leaving this uncalled (dbg_vcpu1
 * off) means the real SPI stays exactly as unenabled/harmless as it always
 * was, and the old smp.c poll loop (still driving hdmi_vblank_poll() in
 * that build configuration) keeps working exactly as before.
 *
 * TODO(board), UNVERIFIED — same three open questions musb_irq_arm_cpu1()
 * carries, not yet exercised on real hardware for THIS SPI:
 *   1. GICD_ITARGETSR bit-to-core mapping (expect readback 0x02, bit1).
 *   2. GICD_ICFGR level config for INTID 119 — left untouched here, trusting
 *      the DTB's <0 0x57 0x04> (level-high) the same way MUSB's <0 0x47 0x04>
 *      was trusted.
 *   3. GICD_IGROUPR — left as whatever firmware already set, same reasoning
 *      as musb_irq_arm_cpu1()'s TODO #3.
 * A fourth, HDMI-specific one: whether TCON_INT0's enable bit really is
 * BIT(30) on THIS silicon revision — hdmi_vblank_irq_enable()'s own comment
 * cites the cross-SoC-family Linux driver convention this file already
 * trusted for the STATUS bit pairing, but that specific ENABLE bit has never
 * been read back on this board. TODO(board): after calling this, confirm
 * TCON_INT0 bit30 reads back 1, and that irq_counter[119] increments in
 * CPU1's g_gt[] slot once a real vblank occurs.
 * ------------------------------------------------------------------ */
void
hdmi_irq_arm_cpu1(void)
{
	/* Same priority as MUSB's — see musb_irq_arm_cpu1()'s comment on why
	 * TIMER_PRIORITY (highest non-secure value) is right for a real,
	 * latency-sensitive peripheral event. */
	GICD_IPRIORITYR_BYTE(HDMI_TCON1_IRQ_INTID) = (uint8_t)TIMER_PRIORITY;

	/* Target CPU interface 1 ONLY (bit1), flat assignment not OR'd — same
	 * fail-safe-toward-CPU0-untargeted reasoning as musb_irq_arm_cpu1(). */
	GICD_ITARGETSR_BYTE(HDMI_TCON1_IRQ_INTID) = (1u << 1);

	GICD_ISENABLER(GICD_WORD(HDMI_TCON1_IRQ_INTID)) =
	    (1u << GICD_BIT(HDMI_TCON1_IRQ_INTID));

	/* Unlike MUSB (whose "mc" line is already live the moment the
	 * controller has traffic), TCON1's vblank line needs its OWN
	 * device-level enable bit turned on, or the SPI never asserts no
	 * matter how the GIC side is armed — see hdmi_vblank_irq_enable()'s
	 * comment (hdmi.c) for why hdmi_init() leaves it masked. Do this
	 * LAST, after the distributor already has the SPI enabled and
	 * targeted, so there is no window where the device could assert
	 * before the GIC is ready to route it anywhere. */
	hdmi_vblank_irq_enable();
}
#endif

/* ------------------------------------------------------------------ *
 * IRQ handler: called from el2_trap()'s EL2_KIND_IRQ arm. Bounded,
 * non-blocking - fixed number of MMIO accesses and breadcrumb stores,
 * no loops, no allocation.
 * ------------------------------------------------------------------ */
void
gic_timer_irq(struct el2_frame *frame)
{
	struct gt_percpu *gt = gt_self();
	uint32_t iar = GICC_IAR;
	uint32_t intid = GICC_IAR_INTID(iar);

	(void)frame; /* not needed: we don't inspect/modify the trapped context */

	/* Count this INTID for storm diagnostics. Per-core (see the state
	 * inventory above struct gt_percpu): GICC_IAR is banked per PE, so this
	 * is "what THIS core's CPU interface handed THIS core", not a global
	 * fact — same byte-identical behaviour on CPU0 today, since gt is
	 * g_gt[0] for as long as CPU0 is the only caller. */
	if (intid < 160)
		gt->irq_counter[intid]++;

	if (intid >= GIC_SPURIOUS_MIN) {
		/* 1020-1023: spurious, nothing pending for this CPU interface.
		 * No EOI for a spurious read (GICv2 spec). */
		return;
	}

	/* MUSB_IRQ_INTID (103, real "mc" SPI 71 — musb.h): checked FIRST, ahead
	 * of the vgic_active()/legacy-forwarding blocks below, for the same
	 * reason TIMER_INTID and VGIC_MAINT_INTID are pulled out early — this
	 * INTID is EL2-owned unconditionally and must NEVER fall into
	 * vgic_inject_hw()'s generic "hand it to the guest" path. Reachable
	 * only on CPU1, and only once musb_irq_arm_cpu1() (vcpu1_run()) has
	 * targeted+enabled it at the distributor — on every other core/build
	 * this INTID is never enabled, so GICC_IAR can never return it there.
	 *
	 * Service THEN EOI+DIR (real level source: musb_poll() write-1-clears
	 * REG_INTUSB and drains the EP0/EP1 CSR-level RXPKTRDY/TXPKTRDY bits
	 * that are the other two OR'd-in sources on this same line — see
	 * musb.h's "one aggregate line" citation — so by the time we DIR it,
	 * the line has actually been de-asserted at the source. DIR'ing before
	 * servicing, the way the generic device-SPI path defers to the guest's
	 * virtual EOI, would re-pend this immediately and reproduce exactly the
	 * 145 kHz INTID-106/EHCI storm this file already documents and works
	 * around below. usbacm_poll() is bounded/non-blocking (see usbacm.c) —
	 * safe to call from IRQ context, matching musb.c's own module-header
	 * contract for this being the one place besides musb_init() allowed to
	 * touch MUSB's MMIO. */
	/* MUSB_IRQ_BUDGET_PER_TICK — a HARD ceiling on how many MUSB IRQs this
	 * core will service between two of its own CNTP ticks, after which the
	 * INTID is masked at the distributor and the tick path re-enables it.
	 *
	 * THIS CEILING IS LOAD-BEARING, AND THE NUMBERS ARE REAL (2026-08-26).
	 * Measured on the board with the line correctly targeted at CPU1:
	 *
	 *     irq_counter[103] on CPU1:  ~646 interrupts per second
	 *     musb_throttles   on CPU1:  +86 in 3 s, i.e. ~29 times/second the
	 *                                32-per-tick budget was exhausted
	 *
	 * ~646/s on a completely idle CDC-ACM console is not normal traffic — it is
	 * the level-triggered aggregate "mc" line re-asserting because
	 * usbacm_poll() does not de-assert every source OR'd into it. Same shape as
	 * the 145 kHz EHCI/INTID-106 storm this file documents elsewhere, three
	 * orders of magnitude gentler, and fully contained: masking at the
	 * distributor and re-enabling on the next tick bounds it to 32/tick and
	 * keeps the tick itself — the watchdog kick and the debug channel, i.e.
	 * every recovery lever this project has — guaranteed to run.
	 *
	 * A NOTE ON HOW THIS COMMENT GOT ITS FACTS, because the flip-flop is
	 * instructive. An earlier revision claimed a measured storm; a second
	 * revision RETRACTED that, on the grounds that reading GICD_ISENABLER back
	 * live showed this INTID disabled and targeted at CPU0, "so it could not
	 * have stormed". The retraction was wrong, and wrong in a specific,
	 * repeatable way: the bit read 0 *because this very throttle had masked
	 * it*, and the target byte read 0x01 because the guest's GIC driver resets
	 * ITARGETSR (see the tick's re-assert). Reading a register back and finding
	 * the state you did not expect is not evidence the mechanism is inert — it
	 * can be evidence the mechanism is working. Only the per-core counters,
	 * which cannot be explained away like that, settled it.
	 *
	 * The EMAC channel death that the first revision blamed on this storm was
	 * genuinely a different bug (dbg_core_active stuck at 0 — see vcpu1.c's
	 * comment at the store that fixes it). Both things were true at once; the
	 * error was attributing one symptom to the other rather than measuring.
	 *
	 * The residual failure mode worth naming, unchanged: a storm keeps the
	 * hardware watchdog FED, because wdt_pet() runs on every EL2 exception and
	 * a storm supplies those in abundance — so nothing resets the board while
	 * everything that lives only in the tick block starves. A dark debug
	 * channel next to a perfectly healthy guest is that signature.
	 *
	 * WHY A BUDGET AND NOT A FIX AT THE SOURCE: draining every OR'd MUSB
	 * source correctly is real work in musb.c (and unverified, which is how
	 * this bug got here). This makes the failure IMPOSSIBLE to be fatal rather
	 * than merely unlikely: whatever musb.c does or fails to do, the tick — and
	 * with it the watchdog kick and the debug channel, i.e. every recovery
	 * lever this project has — is guaranteed to run. 32 events per 10 ms tick
	 * is ~3200/s, far more than a CDC-ACM console can generate, so a healthy
	 * MUSB never reaches the ceiling and this costs nothing in the normal case. */
#define MUSB_IRQ_BUDGET_PER_TICK 32u
	/* CORE-GATED, added 2026-08-26 after reading the live distributor back.
	 * Without this gate the whole design degenerates, and it did — measured:
	 *
	 *   CPU0 irq_counter[103] climbing ~133/s, musb_throttles 47168 ~= that
	 *   same count, while CPU1's counters sat frozen at 3783 and its
	 *   musb_irqs was 0.
	 *
	 * Two faults compounding. First, the guest's own GIC driver resets
	 * GICD_ITARGETSR across the SPI range to the boot CPU during attach, so
	 * musb_irq_arm_cpu1()'s one-shot "target CPU1 only" does NOT survive guest
	 * boot — read back live, INTID 103's target byte was 0x01, not 0x02. The
	 * line therefore fires on CPU0, the guest's primary vCPU, which is exactly
	 * the hardware race usbacm.h's "KNOWN OPEN RISK" section warns about.
	 * Second, the per-tick budget RESET lives in the
	 * `smp_cpu_id() == SMP_DEBUG_CPU && dbg_vcpu1` tick block, i.e. only on
	 * CPU1 — so on CPU0 gt->musb_irqs grew monotonically, stayed permanently
	 * above the ceiling, and masked the line on EVERY single interrupt, which
	 * CPU1's tick then dutifully re-enabled: an enable/mask ping-pong running
	 * at tick rate.
	 *
	 * So a wrong-core arrival is EOI'd and dropped rather than serviced. It
	 * must not fall through either: the generic path below would hand this
	 * INTID to vgic_inject_hw(), and the guest has no driver for a node its
	 * own DTB marks status="disabled". Dropping is safe because the tick
	 * re-asserts the targeting (see the tick block), so this self-corrects;
	 * the counter exists so "it is landing on the wrong core" is visible
	 * instead of being a silent performance leak on the guest's vCPU0. */
	if (intid == MUSB_IRQ_INTID && smp_cpu_id() != SMP_DEBUG_CPU) {
		gt->musb_wrong_core++;
		GICC_EOIR = iar;
		GICC_DIR = iar;
		return;
	}
	if (intid == MUSB_IRQ_INTID) {
		usbacm_poll();
		GICC_EOIR = iar;
		GICC_DIR = iar;
		if (++gt->musb_irqs >= MUSB_IRQ_BUDGET_PER_TICK) {
			/* Mask at the DISTRIBUTOR, not the CPU interface: PMR/priority
			 * games would also mask the tick we depend on to recover. The
			 * tick path below re-enables it unconditionally, so this is
			 * self-healing — a genuinely stuck line degrades USB-ACM to
			 * 32-events-per-tick instead of killing the core. */
			GICD_ICENABLER(GICD_WORD(MUSB_IRQ_INTID)) =
			    (1u << GICD_BIT(MUSB_IRQ_INTID));
			gt->musb_throttles++;
		}
		return;
	}

#ifdef HV_HDMI
	/* HDMI_TCON1_IRQ_INTID (119, real TCON1 vblank SPI 87 — hdmi.h): checked
	 * early, same as MUSB_IRQ_INTID just above and for the identical
	 * reason — this INTID is EL2-owned unconditionally (the guest's
	 * lcd-controller@1c0d000 node is status="disabled", no guest driver)
	 * and must NEVER fall into vgic_inject_hw()'s generic forwarding path.
	 * Reachable only on CPU1, and only once hdmi_irq_arm_cpu1()
	 * (vcpu1_run()) has targeted+enabled it at the distributor AND set
	 * TCON_INT0's device-level enable bit — on every other core/build this
	 * INTID is never enabled, so GICC_IAR can never return it there.
	 *
	 * Service THEN EOI+DIR, same ordering as MUSB and for the same reason:
	 * hdmi_vblank_poll() (hdmi.c) does the read-modify-write that clears
	 * the latching TCON_INT0 status bits at the source, so by the time we
	 * DIR it the line has actually been de-asserted. DIR'ing first would
	 * re-pend this immediately on the very next vblank tick — the same
	 * 145 kHz-storm shape this file already documents for EHCI/INTID 106,
	 * just at a display's ~60 Hz instead. */
	/* HDMI_IRQ_BUDGET_PER_TICK — a HARD ceiling on how many TCON1 vblank
	 * IRQs this core will service between two of its own CNTP ticks,
	 * mirroring MUSB_IRQ_BUDGET_PER_TICK above exactly, for the same
	 * documented failure mode: a level-triggered source this handler fails
	 * to fully de-assert (here: hdmi_vblank_poll() somehow leaving a
	 * status bit set, or clearing the wrong pipe's bit) would otherwise
	 * storm forever, feeding the watchdog on every EL2 exception while
	 * starving dbgmon_service()/wdt_debug_kick() in the tick block below —
	 * the exact signature MUSB_IRQ_BUDGET_PER_TICK's comment measured live
	 * for that INTID. UNLIKE MUSB this specific failure has NOT been
	 * reproduced for HDMI_TCON1_IRQ_INTID on hardware yet (this whole path
	 * is unexercised — see hdmi_irq_arm_cpu1()'s TODO(board) list); the
	 * budget is applied preemptively because the MUSB precedent already
	 * proved a fixed-period tick underneath is not by itself enough
	 * protection against a level source this project doesn't yet trust.
	 * A real 60 Hz vblank is ~0.6 events per 10 ms tick, three orders of
	 * magnitude under this ceiling, so a healthy TCON1 never reaches it. */
#define HDMI_IRQ_BUDGET_PER_TICK 32u
	/* Core-gated for exactly the same measured reason as MUSB above (CPU0 was
	 * taking INTID 119 at ~80/s with hdmi_throttles tracking it one-for-one
	 * while CPU1 sat frozen at 97). Dropped rather than serviced on the wrong
	 * core: servicing would run hdmi_vblank_poll() on the guest's primary vCPU
	 * and clear the latch out from under CPU1, and falling through would hand a
	 * TCON interrupt to vgic_inject_hw() for a guest whose DTB marks that node
	 * disabled. The tick's re-assert repairs the targeting. */
	if (intid == HDMI_TCON1_IRQ_INTID && smp_cpu_id() != SMP_DEBUG_CPU) {
		gt->hdmi_wrong_core++;
		GICC_EOIR = iar;
		GICC_DIR = iar;
		return;
	}
	if (intid == HDMI_TCON1_IRQ_INTID) {
		hdmi_vblank_poll();
		GICC_EOIR = iar;
		GICC_DIR = iar;
		if (++gt->hdmi_irqs >= HDMI_IRQ_BUDGET_PER_TICK) {
			/* Mask at the DISTRIBUTOR, not the CPU interface — same
			 * self-healing reasoning as MUSB's throttle: the tick
			 * (which we depend on to recover) must stay unaffected,
			 * and the tick path below re-enables this unconditionally
			 * every VCPU1_TICK_PERIOD_US. */
			GICD_ICENABLER(GICD_WORD(HDMI_TCON1_IRQ_INTID)) =
			    (1u << GICD_BIT(HDMI_TCON1_IRQ_INTID));
			gt->hdmi_throttles++;
		}
		return;
	}
#endif

	/* --- Interrupt-virtualization milestone: full vGIC forwarding ------ *
	 * Only taken once vgic_init() has actually run (main_dbg.c's policy —
	 * see vgic_active()'s doc comment). REPL/GDB builds link this same file
	 * but never call vgic_init(), so vgic_active() is permanently false
	 * there and they fall through to the legacy per-INTID handling below,
	 * completely unaffected by this branch.
	 *
	 * EVERY non-spurious INTID reaching here — including 27 (CNTV), 30
	 * (CNTP), 106 (EHCI) — is forwarded to the guest via vgic_inject_hw(),
	 * with ONE exception: the vgic maintenance PPI (25) itself, which is
	 * never guest-visible and is serviced entirely by EL2. See vgic.h's
	 * VGIC_MAINT_INTID comment for why that one gets a full EOI+DIR here
	 * while everything else gets EOI-only (priority-drop). Nothing is
	 * silently dropped: vgic_inject_hw() queues on LR exhaustion instead of
	 * discarding (see vgic.c's pending-injection queue). */
	/* TIMER_INTID (30) is EL2's OWN tick and must never be forwarded, exactly
	 * like VGIC_MAINT_INTID inside the block below. Without this exclusion the
	 * generic "Device SPI" arm at the end of that block swallows it into
	 * vgic_inject_hw(), so the tick handler further down -- and with it
	 * vtimer_mask_watchdog(), the ONLY recovery for a self-latching CNTV mask
	 * -- never runs. That is why "just arm the CNTP tick" was not by itself a
	 * fix (2026-07-30). Safe: the guest uses CNTV, never CNTP
	 * (irq_counter[30] measured 0 across a full boot). */
	if (vgic_active() && intid != TIMER_INTID) {
		if (intid == VGIC_MAINT_INTID) {
			/* Never placed in a List Register — nothing else will ever
			 * deactivate it, so fully EOI+DIR it ourselves (exactly like
			 * this module's own TIMER_INTID tick below, for the same
			 * reason: entirely EL2-serviced, not guest-facing). */
			GICC_EOIR = iar;
			GICC_DIR = iar;
			vgic_maintenance();
			return;
		}

		if (intid == VGIC_VTIMER_INTID) {
			/* The guest's VIRTUAL TIMER (CNTV) — the un-cracked piece. First
			 * OBSERVE (fix A): sample the guest's CNTV state at the exact
			 * instant EL2 takes this PPI, into the flight-recorder ring so a
			 * post-wedge dump shows the ordered SYNC/IRQ/TIMER timeline. a0 =
			 * CNTV_CTL (ENABLE/IMASK/ISTATUS), a1 = signed (CVAL - CNTVCT). */
			uint32_t vctl  = read_cntv_ctl();
			int64_t  delta = (int64_t)(read_cntv_cval() - read_cntvct());
			flightrec_log(FLTR_K_TIMER, vctl, (uint64_t)delta);

#if VGIC_CNTV_HW
			/* HW=1: priority-drop only; the guest's virtual EOI deactivates
			 * the physical CNTV in hardware (LR tied to physical INTID 27). */
			GICC_EOIR = iar;
			vgic_inject_cntv();
#else
			/* SOFTWARE vtimer (HW=0): the physical CNTV is level-triggered
			 * and still asserting (ISTATUS=1), so if we only priority-drop it
			 * it re-pends immediately and storms EL2. Instead MASK it at the
			 * source (CNTV_CTL.IMASK=1) so the line de-asserts, then fully
			 * deactivate at the GIC (EOIR+DIR), then inject a pure-virtual
			 * vIRQ. The guest's timer ISR re-arms CNTV_CVAL and rewrites
			 * CNTV_CTL (ENABLE=1, IMASK=0) which clears our mask for next
			 * tick — the same thing FreeBSD's one-shot eventtimer does anyway
			 * after each fire. */
			write_cntv_ctl(vctl | CNTV_CTL_IMASK);
			/* Claim the mask so vtimer_mask_watchdog() (see its header
			 * comment) can undo it if the guest never does. Without this
			 * one line, a single lost injection costs the guest its
			 * timebase for the rest of the boot. */
			gt->cntv_el2_masked = 1;
			gt->cntv_masked_ticks = 0;
			GICC_EOIR = iar;
			GICC_DIR  = iar;
			vgic_inject_cntv();
#endif
			return;
		}

		/* Device SPI. Priority-drop ONLY — do NOT DIR. The physical interrupt
		 * is about to be tied (HW=1) to a guest List Register; the guest's own
		 * virtual EOI is what deactivates the physical source, in hardware,
		 * the instant its driver actually services it. DIR'ing it here
		 * ourselves — deactivating a level-triggered source the guest never
		 * got to clear — is exactly the documented failure mode of the two
		 * prior (reverted) vGIC attempts (the 145 kHz EHCI/INTID-106 storm). */
		GICC_EOIR = iar;
		vgic_inject_hw(intid, intid, 0);
		return;
	}

	if (intid != TIMER_INTID) {
		/* INTID 106 (EHCI SPI 74, usb@1c1b000, level-high) storm mitigation:
		 * EOI+DIR and drop — do NOT inject to the guest. This is a documented
		 * WORKAROUND, not a fix: the level source stays asserted because
		 * nothing clears it at the device, so injecting would re-fire forever
		 * (the confirmed 145 kHz storm — see the EHCI-INTID-106 investigation).
		 * NOT silent: every drop is counted in irq_counter[106], and the
		 * storm-diagnostics breadcrumb (IRQ_COUNTER_BC_BASE below) surfaces
		 * the dominant storming INTID, so the drop rate is measurable over
		 * EMAC. The real fix is proper interrupt virtualization (vGIC/IMO=1
		 * with the device source actually serviced) — a separate milestone
		 * that was attempted and reverted once; see vgic.h's STATUS note. */
		if (intid == 106) {
			GICC_EOIR = iar;
			GICC_DIR = iar;
			return;
		}

		/* INTID 27 (CNTV, guest virtual timer PPI) storm mitigation (hubd
		 * #142): EOI+DIR and drop. Same WORKAROUND caveat as 106 — dropping
		 * the guest's own virtual-timer tick means it never arrives via this
		 * path (the guest uses CNTP instead in the current build). Counted in
		 * irq_counter[27] and surfaced via the storm breadcrumb, so this is a
		 * measured steady-state workaround, not a silent one. Root cause (why
		 * CNTV storms despite CNTVOFF_EL2=0) and the real fix (inject CNTV via
		 * the vGIC List Registers) belong to the interrupt-virtualization
		 * milestone; the prior vGIC injection attempt regressed and was
		 * reverted (see vgic.h STATUS / the vgic-revert history). */
		if (intid == 27) {
			GICC_EOIR = iar;
			GICC_DIR = iar;
			return;
		}

		/* Not our timer PPI. This is a guest interrupt (device SPI).
		 * Drop priority to unblock EL2 IRQs, then inject as HW interrupt.
		 * The guest writing virtual EOIR will automatically deactivate this
		 * physical interrupt via the GIC's HW integration. */
		GICC_EOIR = iar;
		vgic_inject_hw(intid, intid, 0);
		return;
	}

	/* Ours: sample jitter against the free-running physical counter. */
	jitter_sample(&gt->jitter, timer_now());

	/* Re-arm the next interval from the *previous* deadline (not "now"),
	 * so handler latency/runtime doesn't accumulate into the period.
	 * Writing CVAL de-asserts the current timer output (CNTPCT < new CVAL
	 * again) which is what clears the interrupt condition at the source;
	 * the GICC_EOIR below then completes it at the GIC. */
	gt->next_deadline += gt->period_ticks;
	write_cntp_cval(gt->next_deadline);
	/* Re-assert ENABLE=1, IMASK=0. Writing CVAL doesn't disturb CNTP_CTL,
	 * but this is the one place a missed write would silently stop the
	 * tick forever, so keep it explicit. */
	write_cntp_ctl(CNTP_CTL_ENABLE);

	GICC_EOIR = iar;
	GICC_DIR = iar;

	gt->ticks++;

	/* Undo our own CNTV mask if the guest never cleared it. Deliberately
	 * placed on EVERY tick, not inside the REPORT_EVERY block below: the
	 * whole point is a short, bounded recovery window, and REPORT_EVERY is
	 * a diagnostics cadence, not a control-loop period. */
	vtimer_mask_watchdog(gt);

	/* CPU1-as-third-vCPU watchdog/dbgmon service (vcpu1.c, EXPERIMENTAL).
	 * See vcpu1.h for the full rationale. This is the ENTIRE mitigation:
	 * because HCR_EL2.IMO=1 routes this core's physical timer IRQ to EL2
	 * unconditionally (see this file's own header comment on IMO), this
	 * runs every VCPU1_TICK_PERIOD_US regardless of what guest code was
	 * just preempted on this core — the same unmaskable-tick guarantee
	 * CPU0's own guest already lives under. wdt_debug_kick() first,
	 * unconditionally, before anything that could conceivably have a bug —
	 * matching wdt_debug_kick()'s own existing foreground-loop precedent
	 * (smp.c) of being the very first thing done each pass. dbgmon_service()
	 * gets a SNAPSHOT of CPU0's guest frame, not this core's own vCPU2
	 * frame, so gr/sr/gva keep meaning exactly what they mean today — see
	 * vcpu1.h's note on why that is deliberate, not an oversight. */
	if (smp_cpu_id() == SMP_DEBUG_CPU && dbg_vcpu1) {
		wdt_debug_kick();

		/* Refresh the MUSB storm budget and un-mask the line if the previous
		 * window exhausted it (see MUSB_IRQ_BUDGET_PER_TICK above). Written
		 * unconditionally rather than only when throttled: ISENABLER is
		 * write-1-to-set and idempotent, so an unconditional write is both
		 * cheaper than a read-compare and immune to losing the un-mask if
		 * musb_throttles was bumped between the check and the write. This is
		 * what makes the throttle self-healing instead of a one-way kill. */
		gt->musb_irqs = 0;
		GICD_ISENABLER(GICD_WORD(MUSB_IRQ_INTID)) =
		    (1u << GICD_BIT(MUSB_IRQ_INTID));

		/* RE-ASSERT THE TARGETING, not just the enable (added 2026-08-26).
		 * musb_irq_arm_cpu1()/hdmi_irq_arm_cpu1() run ONCE, at vcpu1 entry —
		 * and the guest's GIC driver then resets GICD_ITARGETSR across the SPI
		 * range to the boot CPU during its own attach, silently undoing them.
		 * Read back live: INTID 103's target byte was 0x01 (CPU0), not the
		 * 0x02 that was written, and CPU0 was taking the interrupts.
		 *
		 * This is the SAME class of problem as the PC5 pinmux enforcement a
		 * few lines below — "FreeBSD's own driver keeps undoing our setting" —
		 * and it gets the same, already-proven treatment: re-assert on the
		 * tick, and write only when it has actually drifted, so we are not
		 * fighting the distributor bus every 10 ms for nothing.
		 *
		 * ITARGETSR is BYTE-addressable, one byte per INTID; the byte accessor
		 * exists precisely so a single INTID can be retargeted without
		 * splattering its three neighbours (see GICD_ITARGETSR_BYTE's own
		 * comment and vgicd.c's SAS-decode note on the same hazard). */
		if (GICD_ITARGETSR_BYTE(MUSB_IRQ_INTID) != (1u << 1))
			GICD_ITARGETSR_BYTE(MUSB_IRQ_INTID) = (1u << 1);

#ifdef HV_HDMI
		/* Refresh the TCON1 vblank storm budget and un-mask the line if the
		 * previous window exhausted it — identical shape and identical
		 * reasoning to the MUSB refresh just above (see
		 * HDMI_IRQ_BUDGET_PER_TICK's comment at the dispatch site): an
		 * unconditional write is cheap, idempotent (ISENABLER is
		 * write-1-to-set), and immune to a race between checking and
		 * un-masking. This is what makes the HDMI throttle self-healing
		 * instead of a one-way kill of the vblank/HUD-repaint duty. */
		gt->hdmi_irqs = 0;
		GICD_ISENABLER(GICD_WORD(HDMI_TCON1_IRQ_INTID)) =
		    (1u << GICD_BIT(HDMI_TCON1_IRQ_INTID));

		/* Same drift-correcting re-assert as MUSB's just above, same measured
		 * reason (CPU0 was taking INTID 119 while CPU1 sat frozen). */
		if (GICD_ITARGETSR_BYTE(HDMI_TCON1_IRQ_INTID) != (1u << 1))
			GICD_ITARGETSR_BYTE(HDMI_TCON1_IRQ_INTID) = (1u << 1);

		/* Publish gt->hdmi_wrong_core / gt->hdmi_throttles into the HDMI
		 * breadcrumb (hdmi.h's BC_HDMI_BASE words [13]/[14]), same cadence
		 * as the CPU0-only storm diagnostics below (REPORT_EVERY ticks,
		 * ~1/s). ADDED while investigating a measured ~18 Hz vblank rate
		 * against a ~60 Hz mode: these counters already existed and are the
		 * cheap, direct way to confirm or rule out "wrong-core drop" and
		 * "budget throttle" as the cause, but had no live-readable export —
		 * IRQ_COUNTER_BC_BASE below is deliberately CPU0-only (see its own
		 * comment: "Rather than splitting those windows for a second
		 * writer, this core's tick simply skips the block"), and
		 * HDMI_TCON1_IRQ_INTID only ever arrives on CPU1. Same lane-
		 * ownership reasoning applies here in reverse: these two words in
		 * BC_HDMI_BASE are written ONLY from this CPU1 tick path, so there
		 * is no writer race with hdmi.c's own words in the same window
		 * (word [12]'s g_vblank_count is likewise CPU1-only in this build).
		 * Same coherent-store idiom as hdmi.c's bc_write() / this file's
		 * own IRQ_COUNTER_BC_BASE write just below (`dc civac` + `dsb sy`). */
		if ((gt->ticks % REPORT_EVERY) == 0) {
			volatile uint32_t *hdmi_bc =
			    (volatile uint32_t *)BC_HDMI_BASE;

			hdmi_bc[13] = gt->hdmi_wrong_core;
			hdmi_bc[14] = gt->hdmi_throttles;
			/* [13]/[14] are adjacent words, same cacheline -- one
			 * clean+dsb covers both, same as IRQ_COUNTER_BC_BASE's
			 * two-word write just below. */
			__asm__ volatile("dc civac, %0\n\tdsb sy"
			                  :: "r"(&hdmi_bc[13]) : "memory");
		}
#endif

		{
			struct el2_frame snap;

			el2_snapshot_guest_frame(&snap);
			dbgmon_service(&snap);
		}
		/* KNOWN GAP (board-free code-reading finding, 2026-08-27, STILL NOT
		 * fixed here, now for a DIFFERENT and more fundamental reason than
		 * when this comment was first written -- see the update below).
		 * Unlike smp.c's old SMP_DEBUG_CPU loop (the `if (&gdb_channel &&
		 * gdb_channel) { ... gdb_stop_pending ... } else {
		 * dbgmon_service(&snap); }` branch that loop still has, now dead
		 * code whenever dbg_vcpu1 is armed), THIS tick-path call to
		 * dbgmon_service() is unconditional and has NO gdb_channel
		 * awareness at all. In the `dbg` build dbgmon_service() resolves to
		 * dbgmon.c's real text monitor, which never checks gdb_channel
		 * either (its `gdb` command only sets the flag; nothing downstream
		 * reads it). Net effect: with vcpu1 armed (board-config.xml's
		 * default), issuing dbgmon's `gdb` command is a complete no-op --
		 * raw RSP bytes get fed byte-by-byte into the ASCII line parser,
		 * gdbstub_attached() can never become true, and no
		 * breakpoint/step/watchpoint trap can ever reach gdbstub.c's
		 * command loop. Source-level GDB against the guest is therefore
		 * fully unreachable in this exact configuration (`dbg` build +
		 * vcpu1 armed + runtime `gdb` command), independent of the
		 * gdbstub.c watchdog-starvation fix (see that file's file-scope
		 * wdt_debug_kick() comment) and independent of the two banked-
		 * register fixes in gdbstub.c/gdbstub_hw.c (commit 3c28d5e).
		 *
		 * UPDATE (same pass): el2_exc.c's GDB divert now HAS a
		 * core-ownership gate (`smp_cpu_id() == 0u`, see that file's "GDB
		 * divert (ROADMAP B2)" block comment) -- landed in the same change
		 * as the gdbstub.c watchdog fix, per this comment's own prior
		 * instruction that the two must ship together. That closes the
		 * self-deadlock this comment used to describe (CPU1 taking its own
		 * qualifying trap, setting gdb_stop_pending, and parking forever
		 * waiting for itself to service it).
		 *
		 * Wiring gdb_channel/gdb_stop_pending into THIS block is still
		 * deliberately NOT done, because closing the self-deadlock exposed
		 * a SEPARATE, more fundamental hazard underneath it: gdbstub.c's
		 * register access assumes the core running dispatch() is CPU1
		 * acting as an inert, guest-free debug plane -- true today only
		 * because dbg_vcpu1's tick never calls into gdbstub at all. Two
		 * concrete spots break that assumption the moment this IS wired in:
		 *
		 *   - reg_get()/reg_set() for REG_SP (gdbstub.c) do `mrs/msr
		 *     sp_el1` directly -- SP_EL1 is a per-PE BANKED register (same
		 *     class as this project's four-confidently-wrong-diagnoses
		 *     history, ORIENTATION.md rule 4). Serviced from CPU1's tick while
		 *     CPU1 is itself running vcpu1's live FreeBSD guest, a GDB `g`/
		 *     `p` register read would silently return CPU1's OWN vcpu1
		 *     guest's SP_EL1 as if it were CPU0's guest's SP -- a live,
		 *     PLAUSIBLE value, not obvious garbage, indistinguishable from
		 *     correct without cross-checking. A `G`/`P` write is worse: it
		 *     would corrupt CPU1's live, running vcpu1 guest's stack
		 *     pointer, which then resumes on that corrupted SP the moment
		 *     the tick returns.
		 *   - arm_step()/disarm_step() (gdbstub.c) toggle MDCR_EL2.TDE and
		 *     MDSCR_EL1.SS via raw mrs/msr -- also per-PE. Run from CPU1's
		 *     tick, these would toggle CPU1's OWN debug-exception routing
		 *     and single-step state for its live vcpu1 guest, not CPU0's --
		 *     harmless only by accident today (CPU1 never runs guest code
		 *     in the working design), not by anything in gdbstub.c that
		 *     checks which core it is running on.
		 *
		 * Both are dead code paths TODAY -- unreachable for the same
		 * reason this whole block is a known gap -- but wiring gdb_channel
		 * in here would make them live without touching either function.
		 * A real fix needs gdbstub.c's register path reworked to the same
		 * shape hwbp already uses for HW breakpoint ops (gdb_hw_op_pending,
		 * el2_exc.c): queue the operation, have CPU0 (the core that
		 * actually owns the frame being debugged) apply it from inside its
		 * own parked wfe loop, same as gdbstub_hw_apply_op() already does
		 * for Z1..Z4/z1..z4 -- out of scope for a board-free-only pass, so
		 * left unimplemented and undone rather than landed unverified. */
		/* CORRECTED 2026-08-25: arming vcpu1 stops smp.c's old tight loop
		 * from ever running, including its continuous eMMC clock pinmux
		 * enforcement (PC5 must stay function 3/mmc2; FreeBSD's own
		 * pinctrl driver leaves it at gpio despite PC5 being correctly
		 * listed in the DTB's own mmc2-pins group -- confirmed live, this
		 * is a FreeBSD driver bug, not a DTB completeness gap). Without
		 * this, vcpu1 would race that driver bug from cold boot with NO
		 * correction at all, not just a slower one -- eMMC (root) would
		 * very likely fail before ever reaching this tick. Same
		 * read-modify-write as smp.c's own loop, same "write only when it
		 * drifted" reasoning, just re-armed on this tick's bounded cadence
		 * (VCPU1_TICK_PERIOD_US) instead of every loop pass -- more than
		 * tight enough for a misconfiguration that happens once, early,
		 * during the guest's own pinctrl driver attach. */
		{
			volatile uint32_t *pc = (volatile uint32_t *)SOC_A64_PIO_PC_CFG0;
			uint32_t v = *pc;
			if (((v >> 20) & 0xFu) != 3u)
				*pc = (v & ~(0xFu << 20)) | (3u << 20);
		}

#ifdef HV_HDMI
		/* ADDED 2026-08-25, same "arming vcpu1 stops smp.c's old tight loop"
		 * gap as the pinmux fix just above, this time for smp.c's HDMI PHY
		 * lock-loss defense (smp.c ~682-696): FreeBSD's axp8xx driver cuts
		 * the PHY's supply (dldo1) ~1s into guest boot as "unused", dropping
		 * PHY_STATUS bit7; without this, arming vcpu1 would leave the
		 * display dark forever once that happens, with nothing left to
		 * notice or fix it.
		 *
		 * The CHECK (hdmi_phy_locked()) is one cheap MMIO read, so — exactly
		 * like the old loop's own "one cheap MMIO read every pass" framing —
		 * it runs on EVERY tick (10ms). Only the ACTUAL relock call is rate-
		 * limited, and that rate limit had to be re-derived rather than
		 * copied: the old loop gated hdmi_relock() to "at most once per
		 * 65536 loop PASSES" (smp.c's `last_relock_iters`), a free-running,
		 * unmeasured iteration count in a tight bare-metal loop with no
		 * fixed period — not a wall-clock quantity at all. This tick path
		 * has the opposite property: ticks ARE wall-clock-spaced, exactly
		 * VCPU1_TICK_PERIOD_US (10000us == 10ms, vcpu1.c) apart, so "65536"
		 * cannot be carried over as a tick count without silently changing
		 * its meaning (65536 ticks would be ~11 wall-clock MINUTES, far more
		 * conservative than the old loop ever was). Lacking any measured
		 * iters-per-second figure for that loop to convert properly, this
		 * deliberately picks a wall-clock target instead of guessing one:
		 * retry at most once per ~1s (100 ticks * 10ms). hdmi_relock() costs
		 * ~110ms (smp.c's own dbg_hdmi_relock comment), so worst case (PHY
		 * never relocks) this steals at most ~11% of CPU1's tick-path time —
		 * well under the old loop's already-accepted cost profile — while
		 * still being far more aggressive than "once every 11 minutes" would
		 * have been. Once hdmi_phy_locked() reports 1 again, this simply
		 * stops firing, same as the old loop. */
		if (dbg_hdmi_relock && !hdmi_phy_locked() &&
		    (gt->ticks - hdmi_last_relock_tick) > 100u) {
			hdmi_relock();
			hdmi_last_relock_tick = gt->ticks;
		}
#endif
	}

	/* Storm/jitter diagnostics stay CPU0-only. GICT_BC_BASE/IRQ_COUNTER_BC_
	 * BASE are single, file-scope breadcrumb windows (see the state-
	 * inventory comment above struct gt_percpu) — exactly the "Phase 2"
	 * case that comment warned about, now real. Rather than splitting those
	 * windows for a second writer, this core's tick simply skips the block:
	 * a plain, cheap correctness fix (no interleaved writes from two cores
	 * into one lane) that defers "give CPU1 its own diagnostics lane" as a
	 * real, separate follow-up rather than doing it speculatively here. */
	if (smp_cpu_id() == 0 && (gt->ticks % REPORT_EVERY) == 0) {
		jitter_report_bc(&gt->jitter);

		gict_bc(1, (uint32_t)gt->ticks);
		gict_bc(2, (uint32_t)(gt->ticks >> 32));
		gict_bc(3, iar);
		gict_bc(5, gt->mismatches);
		gict_bc(7, read_cntp_ctl()); /* live timer state from IRQ ctx */

		/* Storm diagnostics: find top storming INTID and dump to SRAM.
		 * Per-core source data now (gt->irq_counter — see the inventory
		 * above); the window itself stays a single global lane (see the
		 * breadcrumb note above struct gt_percpu), so this still reports
		 * only the calling core's counts, byte-identical to before while
		 * CPU0 remains the sole caller. */
		{
			uint32_t max_intid = 0, max_count = 0;
			uint32_t i;

			for (i = 0; i < 160; i++) {
				if (gt->irq_counter[i] > max_count) {
					max_count = gt->irq_counter[i];
					max_intid = i;
				}
			}

			volatile uint32_t *bc_ptr = (volatile uint32_t *)IRQ_COUNTER_BC_BASE;
			bc_ptr[0] = (max_intid << 16) | (max_count & 0xFFFFu);  /* top INTID + count (lo) */
			bc_ptr[1] = (max_count >> 16);                           /* count (hi) */
			__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(bc_ptr) : "memory");
		}
	}
}

uint64_t
gic_timer_ticks(void)
{
	/* Now per-core (see the state inventory above struct gt_percpu): returns
	 * the CALLING core's own tick count. Grepped before this change — this
	 * API has zero callers anywhere in the tree today, so there is no
	 * existing caller whose behaviour could change; the natural semantics
	 * for a genuinely per-core counter is "the core that asks gets its own
	 * answer", which is also byte-identical to the old global on CPU0 (the
	 * only core that ever ticks today). */
	return gt_self()->ticks;
}

const struct jitter *
gic_timer_jitter(void)
{
	/* Same per-core rule as gic_timer_ticks() just above. Returns a pointer
	 * into the calling core's own g_gt[] slot — read-only, per the header's
	 * existing contract ("do not call jitter_init/jitter_sample on it from
	 * outside gic_timer.c"). */
	return &gt_self()->jitter;
}

