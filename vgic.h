/* SPDX-License-Identifier: BSD-2-Clause */

/* vgic.h — GICv2 (GIC-400) virtualization: virtual CPU interface + list
 * registers + a virtual (CNTV) timer tick, for the bzdOS EL2 hypervisor on
 * the Allwinner A64 (Cortex-A53, GIC-400 WITH the virtualization extensions).
 *
 * This is the piece that lets an EL1 guest (a bare-metal self-test, then
 * FreeBSD/arm64) receive VIRTUAL interrupts and its own timer tick, instead
 * of only being preempted by the host's EL2 physical tick (gic_timer.c).
 *
 * ================================================================
 *  GIC-400 memory map on the A64 — CITED, not guessed.
 * ================================================================
 * /opt/bzdos/build/u-boot/arch/arm/dts/sun50i-a64.dtsi, node
 * interrupt-controller@1c81000 (compatible "arm,gic-400"):
 *
 *     reg = <0x01c81000 0x1000>,   // GICD  distributor
 *           <0x01c82000 0x2000>,   // GICC  physical CPU interface
 *           <0x01c84000 0x2000>,   // GICH  hypervisor control      <- vgic.c
 *           <0x01c86000 0x2000>;   // GICV  virtual CPU interface    <- guest
 *     interrupts = <GIC_PPI 9 ...>;// the GIC's own IRQ = MAINTENANCE, INTID 25
 *
 * NOTE: the task brief's tentative GICH≈0x01c48000 / GICV≈0x01c46000 were
 * WRONG for this SoC — the authoritative A64 DTS above gives GICH=0x01c84000
 * and GICV=0x01c86000. Both are 0x2000 (two 4 KiB pages), page-aligned. The
 * maintenance interrupt is the GIC node's own PPI 9, i.e. INTID 16+9 = 25.
 *
 * The vGIC trick (standard KVM approach): map the guest's GICC IPA
 * (0x01c82000, what the guest driver programs as "the CPU interface") onto the
 * GICV physical base (0x01c86000) in stage-2, so guest CPU-interface accesses
 * (IAR/EOIR/PMR/CTLR) transparently hit the VIRTUAL interface. The hypervisor
 * (this file) drives GICH (0x01c84000): it never touches GICV, and the guest
 * never touches GICH — they meet only through the List Registers.
 *
 * STATUS (2026-07-24): RE-ENABLED for the interrupt-virtualization milestone.
 * main_dbg.c now sets HCR_EL2.IMO=1/FMO=1 (every physical IRQ/FIQ routed to
 * and taken at EL2) and stage2.c's stage2_build_mmio_tables() re-adds the
 * GICC->GICV redirect (paired with IMO=1, as it must be — see the two prior
 * reverts documented in gic_timer.c's INTID 106/27 comments: IMO=1 alone,
 * without either the redirect or COMPLETE HW-mode LR forwarding, is exactly
 * what produced the 145 kHz EHCI storm). gic_timer_irq() (gic_timer.c) now
 * forwards EVERY non-spurious physical INTID to the guest via
 * vgic_inject_hw() once vgic_active() is true — see that file for the full
 * per-INTID dispatch and the LR-exhaustion pending-queue this file adds
 * (vgic_maintenance() below).
 *
 * Freestanding, no libc: <stdint.h> only. Depends on exceptions.h for
 * struct el2_frame (the GICD trap-emulate + maintenance handlers take the
 * trapped frame, matching el2_trap's call sites).
 */
#ifndef BZDOS_VGIC_H
#define BZDOS_VGIC_H

#include <stdint.h>
#include "exceptions.h"

/* ================================================================
 *  vGIC virtual-timer (CNTV/INTID 27) deep-dive toggles (2026-07-25).
 *  The base vGIC (IMO=1 + GICC->GICV redirect) and device-SPI HW-mode
 *  injection are PROVEN working on hardware; the one un-cracked piece is
 *  delivering the guest's VIRTUAL TIMER tick through GICV without wedging
 *  it. These compile toggles let a single build select an injection
 *  strategy for CNTV so the failure can be bisected on the board without
 *  editing code between runs. Override any of them with -D on the make
 *  line (CFLAGS change requires `make clean` — the .d deps don't track it).
 *
 *   VGIC_CNTV_HW    1 => tie the CNTV List Register to the physical INTID
 *                        (HW=1); the guest's virtual EOI deactivates the
 *                        physical timer line in hardware. 0 (default) =>
 *                        SOFTWARE vtimer: EL2 masks the physical CNTV
 *                        (CNTV_CTL.IMASK=1) so its level line de-asserts,
 *                        fully deactivates it at the GIC (EOIR+DIR), and
 *                        injects a PURE-VIRTUAL (HW=0) vIRQ the guest EOIs
 *                        on its own. The guest ISR re-arms CNTV_CVAL /
 *                        rewrites CNTV_CTL (ENABLE=1,IMASK=0) for next tick.
 *   VGIC_GROUP0     1 => inject vIRQs as Group 0 (LR.Grp1=0) and preset
 *                        VMCR.VENG0. The GICv2 VIRTUAL interface (GICV) has
 *                        NO security banking, so a guest that (believing it
 *                        is non-secure) writes GICC_CTLR bit0 to "enable
 *                        Grp1" actually sets GICV_CTLR.EnableGrp0 — i.e. a
 *                        Grp1 vINTID may never be delivered. KVM injects all
 *                        vIRQs as Grp0 for exactly this reason. 0 (default)
 *                        keeps the historical Grp1 injection.
 *   VGIC_CNTV_EOI_TRACE 1 => set the LR EOI-maintenance bit (GICH_LR_EOI)
 *                        on the CNTV LR so EL2 takes maintenance INTID 25
 *                        the instant the guest virtual-EOIs the tick —
 *                        proof-of-delivery instrumentation (logged via the
 *                        flightrec ring). Only meaningful with HW=0.
 * ================================================================ */
#ifndef VGIC_CNTV_HW
#define VGIC_CNTV_HW          0
#endif
/* VGIC_GROUP0 defaults to 1: on hardware (2026-07-25) this was THE fix that let
 * the guest boot to root mount + init under IMO=1 — with Grp1 injection (=0) the
 * GICv2 virtual interface never delivered ANY vIRQ (CNTV or device SPI) because
 * GICV has no security banking, so the guest's "enable Grp1" actually enabled
 * Grp0. Only override to 0 to reproduce that historical failure. */
#ifndef VGIC_GROUP0
#define VGIC_GROUP0           1
#endif
#ifndef VGIC_CNTV_EOI_TRACE
#define VGIC_CNTV_EOI_TRACE   0
#endif

/* GIC-400 physical bases (see the DTS citation above). GICH is the only one
 * vgic.c drives directly; GICV/GICC/GICD are exposed here for the stage-2
 * mapping snippet and the GICD trap-emulate helpers. */
#define VGIC_GICD_BASE   0x01c81000UL   /* distributor (passed through in v1) */
#define VGIC_GICC_BASE   0x01c82000UL   /* physical CPU i/f — guest IPA target */
#define VGIC_GICH_BASE   0x01c84000UL   /* hypervisor control — driven here   */
#define VGIC_GICV_BASE   0x01c86000UL   /* virtual CPU i/f — mapped to guest  */

/* The guest's virtual timer PPI: CNTV = GIC_PPI 11 -> INTID 16+11 = 27 (per
 * the sun50i-a64.dtsi arm,armv8-timer node). This is the vINTID we inject on
 * each host tick so the guest's own timer handler runs. */
#define VGIC_VTIMER_INTID   27u

/* The GIC-400 maintenance interrupt (the GIC node's own PPI 9 -> INTID 25).
 * v2 (interrupt-virtualization milestone): ENABLED. vgic_init() enables it at
 * the real distributor (IGROUPR0/ISENABLER0/IPRIORITYR, same treatment as any
 * other PPI) but GICH_HCR.UIE (the bit that actually asserts this line on LR
 * underflow) is left CLEAR at init and toggled on/off dynamically by the
 * pending-injection queue (vgic_inject_hw()/vgic_maintenance() in vgic.c) —
 * only while there is a backlog waiting for a free List Register. Enabling
 * UIE unconditionally would make INTID 25 itself storm: the underflow
 * condition ("fewer than 2 valid LRs") is the NORMAL idle state of a GIC-400
 * with only 4 LRs, so it would fire continuously whenever the guest isn't
 * actively holding >=2 interrupts pending — the same class of bug this whole
 * milestone exists to fix, just self-inflicted instead of device-inflicted.
 * gic_timer_irq() routes INTID 25 to vgic_maintenance() and — unlike every
 * other (HW-mode, guest-EOI-deactivated) forwarded INTID — fully EOIs *and*
 * DIRs it itself, because this one is never placed in a List Register: it is
 * serviced entirely by EL2, so nothing else will ever deactivate it. */
#define VGIC_MAINT_INTID    25u

/* ---- Core vGIC API ---- */

/* Enable the virtual CPU interface: read GICH_VTR for the List-Register count,
 * clear all LRs, preset GICH_VMCR (virtual Grp1 enabled, virtual PMR wide
 * open) as a safe default, set GICH_HCR.En=1, and set CNTVOFF_EL2=0 so the
 * guest's virtual counter (CNTVCT) equals the physical counter. Call once,
 * after el2_install()+gic_timer_init(), before entering the guest. Idempotent.
 * Writes the VGIC breadcrumb window (0x50001c00, "VGIC"). */
void vgic_init(void);

/* Inject a pending virtual interrupt: find a free List Register (GICH_ELRSR),
 * write it as vINTID `vintid`, Group 1, state=pending, at `priority` (8-bit
 * GIC priority; the top 5 bits go into the LR). If no LR is free the injection
 * is dropped and counted (the guest is behind — harmless for a periodic tick).
 * Safe from IRQ context: bounded, no loops beyond the fixed LR scan. Call from
 * el2_trap()'s IRQ arm on each host tick with (VGIC_VTIMER_INTID, 0). */
void vgic_inject(uint32_t vintid, int priority);

/* Inject the guest's VIRTUAL TIMER tick (CNTV, VGIC_VTIMER_INTID) into a
 * List Register, honoring the VGIC_CNTV_HW / VGIC_GROUP0 / VGIC_CNTV_EOI_TRACE
 * toggles above. Includes a one-shot GATE: if a live LR already holds a
 * pending/active vINTID 27, no second copy is injected (a duplicate vID in two
 * LRs is UNPREDICTABLE per the GICv2 spec, and the periodic tick is
 * idempotent). Call from gic_timer_irq() on each physical CNTV arrival, AFTER
 * the caller has done the physical-side EOI/DIR/mask appropriate to the mode.
 * Returns 1 if a vIRQ was (re)injected, 0 if gated or dropped. */
int  vgic_inject_cntv(void);

/* Service the maintenance interrupt (v2: WIRED — call from gic_timer_irq()
 * when the acknowledged physical INTID == VGIC_MAINT_INTID (25), after fully
 * EOI+DIR'ing it yourself — see VGIC_MAINT_INTID's comment above for why).
 * Clears any List Register the guest EOIed via the EOI-maintenance path
 * (GICH_EISR — unused by our pure HW-mode LRs today, kept for completeness),
 * then drains the pending-injection queue (see vgic_inject_hw()) into
 * whichever List Registers GICH_ELRSR now shows free, bounded to at most
 * vg_nr_lr iterations. Clears GICH_HCR.UIE once the queue is empty again. */
void vgic_maintenance(void);

/* True once vgic_init() has completed. Lets gic_timer_irq() (gic_timer.c)
 * decide, per-build, whether to run the full vGIC forwarding path (main_dbg,
 * where vgic_init() is called) or the legacy/no-vgic behavior (REPL/GDB
 * builds, which link the same gic_timer.c but never call vgic_init()). */
uint32_t vgic_active(void);

/* ---- Distributor (GICD) trap-and-emulate helpers (v1: INERT) ----
 * In v1 the GICD page (0x01c81000) is left identity-mapped (passed through),
 * so these are not wired. They exist so a later milestone can unmap the GICD
 * page in stage-2 and emulate it against a shadow (isolating the guest from
 * the shared physical distributor). vgic_gicd_fault() decodes a stage-2 data
 * abort on the GICD page exactly like vconsole_handle_fault() does for UART0,
 * and returns 1 if it handled it (0 = not ours). */
uint32_t vgic_gicd_read(uint32_t off);
void     vgic_gicd_write(uint32_t off, uint32_t val);
int      vgic_gicd_fault(struct el2_frame *frame);

/* ---- Bare-metal self-test guest (run BEFORE FreeBSD) ----
 * A tiny EL1 payload that installs its own VBAR_EL1, enables the (virtual)
 * CPU interface via the guest's GICC IPA (which stage-2 redirects to GICV),
 * unmasks IRQ, and spins bumping an "alive" breadcrumb. Its IRQ handler reads
 * the virtual IAR, bumps a "virq received" breadcrumb (the proof injection
 * works), and EOIs. Breadcrumb window 0x50001d00 ("VGST"). Never returns.
 *
 * PRECONDITIONS the integrator must satisfy before calling vgic_selftest_start:
 *   1. el2_install(); gic_timer_init(1000); vgic_init();
 *   2. stage2_init() (INCLUDING the GICC->GICV remap snippet) + stage2_enable()
 *   3. EL2 IRQ unmasked (`msr daifclr, #2`) so the physical tick is taken and
 *      el2_trap calls vgic_inject(27,0) each tick. */
void vgic_selftest_guest_el1(void) __attribute__((noreturn));
void vgic_selftest_start(void)      __attribute__((noreturn));

#endif /* BZDOS_VGIC_H */

/* Inject a hardware-backed interrupt (HW=1). When the guest writes virtual
 * EOIR, the GIC automatically deactivates the physical interrupt 'pintid' —
 * the caller (gic_timer_irq()) must therefore EOI (priority-drop) but NEVER
 * DIR the physical interrupt itself; DIR'ing it here would deactivate it
 * before the guest ever services it, and re-arm nothing (that combination —
 * EOI+DIR a level-triggered source the guest never got to clear — is exactly
 * the documented 145 kHz EHCI/INTID-106 storm from the prior two reverts).
 *
 * LR-exhaustion handling (v2, interrupt-virtualization milestone): a GIC-400
 * has only 4 List Registers. If none is free when this is called, the
 * request is NOT dropped — it is pushed onto a small fixed-size pending-
 * injection ring (bounded, no allocation) and drained by vgic_maintenance()
 * as LRs free up (signalled by the GICH_HCR.UIE underflow maintenance IRQ,
 * INTID 25, dynamically enabled only while the ring is non-empty). Only if
 * that ring ITSELF is full (a sustained, extreme burst far beyond 4 LRs +
 * the ring depth) is an injection ever actually lost — counted separately
 * from the ordinary "LR busy, queued" case so a real overrun is
 * distinguishable from normal backlog in the breadcrumb window. */
void vgic_inject_hw(uint32_t vintid, uint32_t pintid, int priority);
