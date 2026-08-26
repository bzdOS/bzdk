/* SPDX-License-Identifier: BSD-2-Clause */

/* vgicd.h — trap-and-police the GIC distributor.
 *
 * WHY THIS EXISTS
 *
 * This hypervisor runs with HCR_EL2.IMO=0 (measured: HCR_EL2 = 0x84000023):
 * physical interrupts go straight to EL1 and the guest programs the GIC itself.
 * That is the static-partitioning choice, and for a SINGLE guest it is entirely
 * sound — it is cheaper than a vGIC, has no injection latency, and avoids the
 * class of bugs that made three separate IMO=1 attempts get reverted here.
 *
 * But the `dual` build runs TWO guests (FreeBSD on CPU0, Zephyr on CPU3), and
 * the GIC distributor is a SHARED device. With GICD identity-mapped at stage 2
 * — which it was, verified live: stage2_l3_uart[129] -> 0x01c81000 — either
 * guest can reconfigure the other's interrupts:
 *
 *   - write GICD_ITARGETSR to route one of its own SPIs at the OTHER guest's
 *     core, delivering an interrupt that guest has no handler for
 *   - write GICD_ICENABLER to disable an interrupt the other guest depends on
 *   - write GICD_SGIR to fire an SGI at the other guest's core
 *
 * The first of those is the one that matters most for the product story: a
 * Linux-class partition must not be able to break the real-time partition's
 * availability. That is precisely the property "one FreeBSD + one RTOS" sells,
 * and it was not enforced by anything except the guests' good behaviour.
 *
 * WHAT THIS DOES, AND DELIBERATELY DOES NOT DO
 *
 * This is NOT a virtual GIC. It does not maintain per-guest distributor state,
 * it does not virtualise INTIDs, and it does not change how interrupts are
 * delivered — they still go natively to EL1 under IMO=0. It only polices the
 * two write paths that can reach across the partition boundary, by masking the
 * affinity fields down to the writing core's own bit. Everything else passes
 * through to the real distributor unchanged.
 *
 * That is the middle ground Jailhouse occupies: deliver interrupts directly to
 * cells, but police the distributor so a cell cannot reconfigure another cell's
 * interrupts. It buys the isolation property without paying for full interrupt
 * virtualisation.
 *
 * COST
 *
 * A stage-2 fault per GICD access. GICD is touched during interrupt SETUP
 * (enable, priority, target, config) and NOT on the interrupt hot path — under
 * GICv2 the acknowledge/EOI cycle goes through the CPU interface (GICC/GICV),
 * not the distributor. So this is a per-configuration cost, not a per-interrupt
 * one, which is what makes it affordable at all.
 */
#ifndef BZDOS_VGICD_H
#define BZDOS_VGICD_H

#include <stdint.h>
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

struct el2_frame;

/* A64 GIC-400 distributor: one 4 KiB page (sun50i-a64.dtsi ethernet-adjacent
 * gic node, same base vgic.h cites). It lives in the SAME 2 MiB stage-2 block
 * as UART0, which stage2_build_mmio_tables() already splits to 4 KiB pages for
 * the UART trap — so trapping it costs one more invalid L3 entry, no new table
 * and no new level. Verified live: index 129 in that table. */
#define VGICD_BASE   SOC_A64_GICD_BASE
#define VGICD_SIZE   0x00001000UL

/* Distributor register offsets this module cares about (GICv2). */
#define GICD_CTLR        0x000u
#define GICD_ISENABLER   0x100u   /* .. 0x17C */
#define GICD_ICENABLER   0x180u   /* .. 0x1FC */
#define GICD_ITARGETSR   0x800u   /* .. 0x81C for SPIs; one BYTE per INTID */
#define GICD_ITARGETSR_END 0x820u
#define GICD_SGIR        0xF00u

/* Handle a stage-2 fault that landed in the GICD page.
 *
 * Returns 1 if the access was inside GICD and has been emulated (ELR already
 * advanced past the instruction), 0 if the fault was not ours — in which case
 * the caller must keep looking, exactly like vconsole_handle_fault(). */
int vgicd_handle_fault(struct el2_frame *frame);

#endif /* BZDOS_VGICD_H */
