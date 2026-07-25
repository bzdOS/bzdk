/* SPDX-License-Identifier: BSD-2-Clause */

/* onebp.h — a one-shot software breakpoint for the guest's earliest boot
 * code, where HW breakpoints/watchpoints/single-step are all masked
 * (PSTATE.D=1 until cninit — see the "chimp-guest-debug-masked" writeup).
 *
 * Mechanism: patch one guest instruction with `hvc #imm` (HVC is a
 * synchronous exception, NOT gated by PSTATE.D, so it fires regardless of
 * guest debug-mask state). On the trap, dump the requested GPRs to a
 * breadcrumb, restore the original instruction, rewind ELR by 4, and let
 * the guest re-execute the real instruction for real. Self-removing —
 * fires exactly once.
 *
 * Freestanding: <stdint.h> only.
 */
#ifndef BZDOS_ONEBP_H
#define BZDOS_ONEBP_H

#include <stdint.h>
#include "exceptions.h"

/* Arm the breakpoint at guest physical address `pa` (must hold a real A64
 * instruction word) using HVC immediate `imm` (caller picks something that
 * doesn't collide with gtrace's vector range 0..15 — e.g. 0x90+). Saves the
 * original word and overwrites it with `hvc #imm`, with full I/D cache
 * maintenance since this is self-modifying code. */
void onebp_arm(uint64_t pa, uint32_t imm);

/* Call from el2_trap on EC==0x16 (HVC), BEFORE gtrace_handle_hvc — this
 * claims only ITS OWN imm value. Returns 1 (handled: dumped x0..x3, restored
 * the original instruction, rewound ELR by 4) or 0 (not this breakpoint's
 * imm — caller should try the next handler). */
int onebp_handle_hvc(struct el2_frame *frame);

#endif /* BZDOS_ONEBP_H */
