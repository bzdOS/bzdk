/* SPDX-License-Identifier: BSD-2-Clause */

/* holdtest_guest.h — shared declarations for the "hold-gate + BRK ordering"
 * QEMU diagnostic target (main_holdtest_qemu.c et al).
 *
 * THROWAWAY DIAGNOSTIC, not production code. See main_holdtest_qemu.c's file
 * banner for the full rationale: this exists to test ONE narrow hypothesis
 * (does patching a guest software BRK + arming MDCR_EL2.TDE BEFORE the
 * guest's first-ever instruction behave differently from doing the exact
 * same thing AFTER the guest has already been running for a while) in
 * isolation from the real board's SMP/gdbstub machinery, which is a
 * completely separate, much bigger pile of state this target deliberately
 * does not reproduce.
 */
#ifndef BZDOS_HOLDTEST_GUEST_H
#define BZDOS_HOLDTEST_GUEST_H

#include <stdint.h>

/* Entry stubs (guest_holdtest_asm.S). Each begins with exactly ONE
 * instruction slot (a NOP) that main_holdtest_qemu.c patches to BRK_INSTR at
 * a chosen moment — mirrors gdbstub.c's bp_insert() patch site exactly
 * (same instruction encoding, same cache-maintenance sequence, see
 * holdtest_patch_brk() / holdtest_patch_restore()). */
void holdtest_entryA(void) __attribute__((noreturn));
void holdtest_entryB(void) __attribute__((noreturn));

/* Physical/VA (MMU off, flat) addresses of the two patch-site NOPs, and of
 * the instruction immediately after each (needed only for readability in
 * printed diagnostics — the trap handler never needs to compute these,
 * ELR_EL2 already tells it exactly where the BRK fired). Populated by the
 * .S file as plain symbols. */
extern uint32_t holdtest_entryA_patchsite[];
extern uint32_t holdtest_entryB_patchsite[];

/* EL1 vector table (guest_holdtest_asm.S) — installed as VBAR_EL1 before
 * either eret. If MDCR_EL2.TDE fails to route the BRK to EL2 (the exact
 * failure mode this target is built to distinguish from "routes fine"),
 * the exception instead lands here (EL1 "Current EL, SPx" sync vector,
 * since the guest always runs EL1h/SPSel=1) and holdtest_el1_selftrap()
 * reports it distinctly rather than the run silently derailing into
 * address 0 (which is what a real, not-yet-initialized guest VBAR_EL1
 * would do). */
extern uint32_t holdtest_el1_vectors[];

/* Called from the EL1 self-trap vector (guest_holdtest_asm.S) if a debug
 * exception was NOT routed to EL2 — see above. Never returns. */
void holdtest_el1_selftrap(void) __attribute__((noreturn));

/* HVC immediates used as the test's own phase-transition sync points
 * (guest -> EL2, synchronous, unambiguous — the real hypervisor's own
 * pattern for guest->EL2 notifications, see guest.c's HVC demo). Chosen
 * arbitrarily, just need to be distinct small values the EL2 dispatcher can
 * switch on. */
#define HOLDTEST_HVC_A_DONE      1u   /* test A guest resumed past its BRK   */
#define HOLDTEST_HVC_B_ARM_NOW   2u   /* test B: please arm BRK+TDE NOW      */
#define HOLDTEST_HVC_B_DONE      3u   /* test B guest resumed past its BRK   */
#define HOLDTEST_HVC_SELFTRAP    9u   /* a debug exception self-trapped at
                                        * EL1 instead of routing to EL2 --
                                        * reported via HVC too so EL2 can
                                        * print a verdict and cleanly
                                        * poweroff instead of the run just
                                        * sitting in EL1's own wfi loop */

/* NOTE: the literal `hvc #N` immediates in guest_holdtest_payload.c MUST be
 * kept numerically in sync with the HOLDTEST_HVC_* values above by hand —
 * the HVC immediate is encoded directly into the instruction at assemble
 * time (GNU 'i' inline-asm constraints don't emit the required '#' prefix
 * AArch64 asm needs), so it cannot reference the macro syntactically. Only
 * four call sites, all in one small file — see the comment at each. */

/* Performs test A's pre-entry arm (patch holdtest_entryA's NOP to BRK_INSTR
 * + set MDCR_EL2.TDE=1) -- called once from main_holdtest_qemu.c BEFORE the
 * very first eret. Everything after that first eret (test B's arm, both
 * verdicts, poweroff) is driven entirely from inside el2_exc_holdtest_qemu.c's
 * el2_trap(), via the guest's own HVC sync points. */
void holdtest_arm_phase_a(void);

#endif /* BZDOS_HOLDTEST_GUEST_H */
