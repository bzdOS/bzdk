/* SPDX-License-Identifier: BSD-2-Clause */

/* stage2_wx_qemu.h — the dynamic-W^X promotion hook, for the QEMU-virt CI
 * targets' own el2_trap() implementations.
 *
 * WHY THIS HEADER EXISTS (added 2026-08-26 to fix a wholly RED board-free CI):
 *
 * stage2.h's STAGE2_WX_DYNAMIC defaults to 1, which makes stage2.c map guest
 * DRAM writable-but-EXECUTE-NEVER (STAGE2_DRAM_XN_DEFAULT) and depend on a
 * stage-2 permission fault to flip each page to executable on its first
 * instruction fetch. The real board's el2_exc.c routes those faults to
 * stage2_wx_fault(). Every QEMU CI target links its OWN el2_trap() instead
 * (el2_exc_qemu.c, el2_exc_vgic_qemu.c, el2_exc_zephyr_qemu.c, ... — nine
 * variants at the time of writing) and NONE of them had the equivalent hook,
 * while all of them link stage2.o. So from the moment that default flipped,
 * every one of these targets died on the EL1 guest's very first instruction
 * fetch with an unhandled permission fault:
 *
 *     ESR=0x8200000e   EC 0x20 (instruction abort from a lower EL),
 *                      IFSC 0x0e (permission fault, level 2),
 *                      ELR == FAR == the guest entry point
 *
 * ...which each variant's report_fault() then turned into a halt. Verified
 * against a pristine HEAD checkout in a throwaway git worktree, so this was
 * never anyone's uncommitted work-in-progress: the gate documented by
 * SESSION-RULES/BRING-UP.md as the thing to pass BEFORE touching hardware had
 * simply been failing, and a red gate means every change goes straight to the
 * board with no board-free safety net.
 *
 * WHY A SHARED HEADER rather than the same block pasted nine times: the bug
 * was that a policy change in stage2.h did not reach the handlers that depend
 * on it. Nine copies would reproduce exactly that failure mode the next time
 * something about the contract changes — and a tenth CI target would be born
 * broken. One call site per variant, one explanation here.
 *
 * WHY NOT JUST BUILD THESE TARGETS WITH -DSTAGE2_WX_DYNAMIC=0: that would turn
 * the gates green while testing a memory-permission policy the board does not
 * ship. A regression gate that passes by disabling the thing under test is
 * worse than a red one. Wiring the hook means the CI now genuinely exercises
 * the W^X machinery on every run.
 *
 * CONTRACT, identical to el2_exc.c's own call site:
 *   - Call it only for a LOWER-EL synchronous exception.
 *   - Pass the already-decoded EC; the four it acts on are 0x20/0x21
 *     (instruction abort) and 0x24/0x25 (data abort).
 *   - It DOES NOT advance ELR: stage2_wx_fault() fixes the page permission and
 *     the faulting instruction is re-executed.
 *   - Returns nonzero only if it genuinely handled the fault. stage2_wx_fault()
 *     itself rejects anything that is not a guest-DRAM PERMISSION fault
 *     (translation faults, non-DRAM IPAs, the two carved-out HV windows), so a
 *     real fault still falls through to the caller's report_fault() exactly as
 *     before. That filtering is why it is safe to offer all four ECs to it
 *     unconditionally.
 */
#ifndef BZDOS_STAGE2_WX_QEMU_H
#define BZDOS_STAGE2_WX_QEMU_H

#include <stdint.h>
#include "exceptions.h"
#include "stage2.h"

/* WEAK on purpose. Not every QEMU CI target links stage2.o: `dual-qemu` proves
 * PSCI CPU_ON bring-up with no stage-2 at all, and the dual2/dual-rearm/
 * dual-zephyr targets use the disjoint stage2_zephyr.o instead (its slice is
 * mapped Normal-WB identity with XN=0, so it has no dynamic W^X to promote).
 * Declaring the reference weak lets EVERY variant carry the call
 * unconditionally: where stage2.o is absent the symbol resolves to 0, the
 * guard below skips it, and the target behaves exactly as it did before.
 *
 * This is deliberately preferred over "only add the hook to the targets that
 * need it today". The bug being fixed here was precisely that a policy living
 * in stage2.h failed to reach handlers that depended on it; a target that
 * later starts linking stage2.o must get working promotion automatically
 * rather than a silently unexecutable guest. Same weak-fallback pattern this
 * tree already uses for vnet_mmio_fault()/vblk_mmio_fault() (el2_exc.c) and
 * dbg_vcpu1 (gic_timer.c). */
extern int stage2_wx_fault(struct el2_frame *frame) __attribute__((weak));

static inline int
stage2_wx_qemu_try(struct el2_frame *frame, uint32_t ec)
{
#if STAGE2_WX_DYNAMIC
	if (stage2_wx_fault == 0)
		return 0;   /* target links no stage2.o — nothing to promote */
	if (ec == 0x20u || ec == 0x21u || ec == 0x24u || ec == 0x25u)
		return stage2_wx_fault(frame);
#else
	(void)frame;
	(void)ec;
#endif
	return 0;
}

#endif /* BZDOS_STAGE2_WX_QEMU_H */
