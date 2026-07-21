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
 * Freestanding, no libc: <stdint.h> only. Depends on exceptions.h for
 * struct el2_frame (the GICD trap-emulate + maintenance handlers take the
 * trapped frame, matching el2_trap's call sites).
 */
#ifndef BZDOS_VGIC_H
#define BZDOS_VGIC_H

#include <stdint.h>
#include "exceptions.h"

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
 * NOT routed/enabled in v1 (see vgic.c): v1 reclaims List Registers by polling
 * GICH_ELRSR at inject time, so no maintenance IRQ is generated and there is
 * no risk of it colliding with gic_timer_irq()'s physical GICC_IAR handling. */
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

/* Service the maintenance interrupt: read GICH_MISR/GICH_EISR and clear any
 * List Register the guest has EOIed (EISR bit set). Provided for completeness
 * / the v2 path; in v1 it is unused (no maintenance IRQ is enabled). If a
 * future revision enables GICH_HCR.UIE/EOI maintenance and routes INTID 25,
 * call this from el2_trap when the acknowledged physical INTID == 25. */
void vgic_maintenance(void);

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
 * EOIR, the GIC automatically deactivates the physical interrupt 'pintid'. */
void vgic_inject_hw(uint32_t vintid, uint32_t pintid, int priority);
