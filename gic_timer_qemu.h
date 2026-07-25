/* SPDX-License-Identifier: BSD-2-Clause */

/* gic_timer_qemu.h — GICv2 + ARM Generic Timer (EL2-visible non-secure
 * physical timer, CNTP) periodic-tick driver for the QEMU `virt` CI target.
 *
 * This is a SEPARATE file from gic_timer.h/.c, not an #ifdef inside it: the
 * only thing that differs from the real-board driver is the MMIO base
 * addresses (QEMU virt's GICv2 distributor/CPU-interface are not at the
 * A64's GIC-400 addresses) — same INTID (30, CNTP), same register-level
 * GICv2 programming sequence, same "why CNTP not CNTHP" reasoning (see
 * gic_timer.c's header comment; it applies here too: firmware/QEMU hands
 * the non-secure OS the CNTP PPI as a Group 1 interrupt, which is what
 * reaches non-secure EL2). Kept separate (rather than #ifdef'd into the
 * existing file) specifically so this QEMU-only work never touches
 * gic_timer.c, which the real-board target still owns unmodified.
 *
 * Freestanding: <stdint.h> + exceptions.h (struct el2_frame) only.
 */
#ifndef BZDOS_GIC_TIMER_QEMU_H
#define BZDOS_GIC_TIMER_QEMU_H

#include <stdint.h>
#include "exceptions.h"

/* Program the GICv2 distributor + CPU interface for INTID 30 (CNTP) and arm
 * the first interval. `period_us` is the tick period in microseconds.
 * Also sets HCR_EL2.IMO=1 (read-modify-write, preserving every other bit —
 * same contract as gic_timer.c) so the physical IRQ is actually taken at
 * EL2 rather than routed to EL1. Does NOT unmask PSTATE.I/F — the caller
 * (main_qemu.c) does that once it's ready for the tick to start arriving. */
void gic_timer_qemu_init(uint32_t period_us);

/* Call from el2_trap()'s IRQ/FIQ case. Reads GICC_IAR (acknowledging),
 * and:
 *   - if the INTID is ours (30): re-arms the next interval, EOIs, bumps the
 *     internal tick counter, and returns 1.
 *   - otherwise: EOIs any non-spurious INTID anyway (so the GIC's active
 *     bit is never left stuck) and returns 0 without touching the timer.
 */
int gic_timer_qemu_irq(struct el2_frame *frame);

/* Total ticks handled so far (monotonic). */
uint64_t gic_timer_qemu_ticks(void);

#endif /* BZDOS_GIC_TIMER_QEMU_H */
