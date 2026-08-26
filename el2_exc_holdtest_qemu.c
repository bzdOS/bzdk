/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_holdtest_qemu.c — EL2 trap handler for the hold-gate/BRK-ordering
 * QEMU diagnostic (see main_holdtest_qemu.c's file banner for the full
 * rationale/background).
 *
 * Deliberately NOT a reuse of el2_exc.c (same reasoning as el2_exc_qemu.c's
 * own banner: that file is the full board debugger's dispatch table and
 * pulls in a dozen board-specific modules this diagnostic doesn't need).
 * Instead this pulls in ONLY the two pieces of el2_exc.c's/gdbstub.c's logic
 * actually under test, hand-transcribed with the SAME constants/bit
 * positions so this is a faithful mechanism test, not a strawman:
 *
 *   - set_tde() — bit-for-bit identical to gdbstub.c's set_tde() /
 *     el2_exc.c's ss_set_tde() (MDCR_EL2 bit 8).
 *   - the BRK patch + cache-maintenance sequence — bit-for-bit identical to
 *     gdbstub.c's bp_insert()/isync_patch() (same BRK_INSTR encoding
 *     0xD420FA00, same dc cvau + dsb ish + ic ivau + dsb ish + isb order).
 *   - the "EC==0x3C from a lower EL, while attached" divert condition —
 *     same ESR_EL2.EC extraction and check as el2_exc.c's el2_trap() guest
 *     divert block (minus the CPU0/CPU1 stop/resume handshake, which is a
 *     SEPARATE mechanism this target does not model — see the report for
 *     why that's out of scope for what's being tested here).
 *
 * Everything else (the two-phase state machine, the HVC sync points, final
 * verdict/poweroff) is new, test-harness-only code with no real-board
 * analogue.
 */
#include <stdint.h>
#include "exceptions.h"
#include "holdtest_guest.h"
#include "pl011_qemu.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* ---- Faithful transcription of gdbstub.c's set_tde() ------------------ */
static void
set_tde(int on)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mdcr_el2" : "=r"(v));
	if (on) v |= (1ull << 8); else v &= ~(1ull << 8);
	__asm__ volatile("msr mdcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

/* ---- Faithful transcription of gdbstub.c's isync_patch() + the relevant
 * half of bp_insert() (the actual instruction-patch, minus its bp_tab[]
 * bookkeeping, which this test does with two plain globals instead since
 * there are only ever two possible sites). ------------------------------
 *
 * KNOWN QEMU-TCG-SPECIFIC CAVEAT (found while building this test, see the
 * task report): when this EXACT sequence patches TEST B's site -- code on a
 * page QEMU has already translated/executed once (unlike test A's site,
 * patched before the guest has run even a single instruction) -- the guest
 * observably keeps EXECUTING THE OLD (pre-patch) instruction even though a
 * plain data read immediately confirms the new BRK_INSTR bytes are in
 * memory (see the "DIAG" prints in the HVC_B_ARM_NOW handler and in
 * holdtest_entryB_resume()). Widening this to a full `ic ialluis` instead of
 * the targeted `ic ivau` made NO difference -- so this is not a "wrong
 * address invalidated" bug, it looks like a QEMU TCG translated-block-cache
 * artifact specific to an EL2 store retroactively modifying EL1 code on an
 * already-executed page while both run with their stage-1 MMU off (flat
 * physical addressing, so TCG may be keying translated-block tracking by a
 * (mmu_idx, vaddr) pair that differs between the EL2 and EL1 access even
 * though they hit the identical physical byte). This is NOT expected to be
 * a real ARMv8 architectural behavior -- `dc cvau`+`ic ivau`+`dsb`+`isb`,
 * followed by an eret (itself a context-synchronizing event), is the
 * architecturally complete self-modifying-code sequence on a single PE, and
 * there is no known real-A53-silicon errata suggesting otherwise. Test B's
 * result below should therefore be read as "inconclusive due to a QEMU
 * emulation limitation", NOT as evidence about the real board. Test A sidesteps
 * this entirely (nothing has ever executed that page before the patch, so
 * there is no stale translation to go stale) and is the trustworthy result. */
#define BRK_INSTR 0xD420FA00u   /* BRK #0x7d0 -- IDENTICAL encoding to gdbstub.c */

static void
isync_patch(unsigned long pa)
{
	__asm__ volatile(
		"dc cvau, %0\n\t"
		"dsb ish\n\t"
		"ic ivau, %0\n\t"
		"dsb ish\n\t"
		"isb"
		:: "r"(pa) : "memory");
}

static uint32_t saved_a, saved_b;

static void
patch_brk(uint32_t *site, uint32_t *saved)
{
	*saved = *site;
	*site = BRK_INSTR;
	isync_patch((unsigned long)site);
}

static void
restore_orig(uint32_t *site, uint32_t saved)
{
	*site = saved;
	isync_patch((unsigned long)site);
}

/* ---- PSCI SYSTEM_OFF -- identical pattern to el2_exc_qemu.c's
 * qemu_poweroff(): cleanly terminates qemu-system-aarch64 with exit 0.
 *
 * This used to be a static function defined right here, verbatim-duplicated
 * across every QEMU CI handler; it now lives in el2_exc_qemu_common.c as
 * qemu_psci_poweroff(). It was `static` (internal linkage), so no other
 * translation unit could have been calling this file's copy by name — the
 * #define below is purely a thin forwarder so every existing call site
 * (`qemu_poweroff();`, below) keeps compiling unchanged. Behaviour is
 * unchanged: same SMC #0 with x0=SYSTEM_OFF, same wfi fallback loop. */
#define qemu_poweroff qemu_psci_poweroff

/* ---- Test state ---------------------------------------------------- */
enum { PHASE_A = 0, PHASE_B = 1 };
static int g_phase = PHASE_A;

/* Outcomes: 1 = BRK correctly diverted to EL2, -1 = self-trapped at EL1
 * instead (routing failed), 0 = neither happened yet. */
static int a_outcome, b_outcome;

#define SPSR_EL1h (0x5ull)
#define SPSR_D    (1ull << 9)
#define SPSR_A    (1ull << 8)
#define HOLDTEST_GUEST_SPSR (SPSR_EL1h | SPSR_D | SPSR_A)   /* == KLOAD_GUEST_SPSR_EL2 */

#define GUEST_STACK_WORDS (4096 / 8)
static uint64_t g_stack[GUEST_STACK_WORDS] __attribute__((aligned(16)));

static void
enter_phase_b(struct el2_frame *frame)
{
	g_phase = PHASE_B;
	pl011_puts("HV: --- starting TEST B (post-entry: TDE=0 initially, "
	           "guest runs first, arm BRK+TDE only after) ---\n");
	/* TDE=0 to start phase B "clean", exactly like a fresh boot with no
	 * debugger attached yet -- the direct opposite of phase A, where TDE
	 * was armed before the guest ever ran. */
	set_tde(0);
	__asm__ volatile("msr sp_el1, %0" :: "r"((uint64_t)&g_stack[GUEST_STACK_WORDS]));
	frame->elr  = (uint64_t)holdtest_entryB;
	frame->spsr = HOLDTEST_GUEST_SPSR;
}

static void
print_verdict(const char *label, int outcome)
{
	pl011_puts("HOLDTEST: ");
	pl011_puts(label);
	pl011_puts(" -> ");
	if (outcome == 1)
		pl011_puts("BRK correctly diverted to EL2 (mechanism OK)\n");
	else if (outcome == -1)
		pl011_puts("BRK SELF-TRAPPED AT EL1 (TDE routing FAILED)\n");
	else
		pl011_puts("INCONCLUSIVE (neither EL2 divert nor EL1 self-trap observed -- "
		           "see isync_patch()'s block comment: a known QEMU-TCG "
		           "self-modifying-code artifact on an already-executed page, "
		           "not expected on real silicon)\n");
}

/* ---- THE mechanism under test: guest software BRK, EC==0x3C, same check
 * el2_exc.c's real divert condition uses. Wired in as qemu_guest_sync()'s
 * `claim_first` hook (el2_exc_qemu_common.h) so it still gets first refusal
 * on every lower-EL sync exception, ahead of even the shared chain's own
 * unconditional dynamic W^X promotion -- not that the two would ever
 * collide (BRK's EC 0x3C is disjoint from the abort ECs 0x20/0x21/0x24/0x25
 * stage2_wx_qemu_try() acts on), but the ordering guarantee is the whole
 * point of claim_first and this is exactly the scenario it exists for. ---- */
static int
holdtest_claim_brk(struct el2_frame *frame, uint32_t ec)
{
	(void)frame;

	if (ec != 0x3Cu)
		return 0;

	if (g_phase == PHASE_A) {
		pl011_puts("HV: EL2 caught guest BRK (EC=0x3C) at "
		           "holdtest_entryA's patch site -- PASS for test A\n");
		restore_orig(holdtest_entryA_patchsite, saved_a);
		a_outcome = 1;
	} else {
		pl011_puts("HV: EL2 caught guest BRK (EC=0x3C) at "
		           "holdtest_entryB's patch site -- PASS for test B\n");
		restore_orig(holdtest_entryB_patchsite, saved_b);
		b_outcome = 1;
	}
	/* Do NOT advance ELR: the instruction at that PA is now
	 * restored to its original NOP, so resuming at the same
	 * address just (harmlessly) executes it and falls
	 * through -- mirrors gdbstub.c's own "for a BRK the stub
	 * rewinds/reprograms the instruction itself" contract. */
	return 1;
}

/* This target's own scenario config for the shared guest-sync chain
 * (el2_exc_qemu_common.h). Only claim_first is set:
 *
 *   - claim_first = holdtest_claim_brk: see that function's own comment --
 *     this diagnostic's entire reason for existing is the guest BRK divert,
 *     so it must run before anything the shared chain does.
 *
 *   - No want_hvc_ack: this target's HVC handling is NOT a blind
 *     ack-and-advance -- it's a multi-immediate phase-transition state
 *     machine (HOLDTEST_HVC_A_DONE / B_ARM_NOW / B_DONE / SELFTRAP) the
 *     shared chain has no hook for, so it stays below, hand-written, tried
 *     only after qemu_guest_sync() reports EC 0x16 as "not mine".
 *
 *   - No console/SMC/mmio-absorb: this diagnostic's guest payload
 *     (holdtest_guest.c) uses none of them.
 *
 * The shared chain still does its one unconditional thing -- dynamic W^X
 * promotion -- via qemu_guest_sync() below.
 *
 * CORRECTED 2026-08-26: an earlier draft said this file "used to call
 * stage2_wx_qemu_try(frame, ec) directly". It never did. No QEMU handler in
 * this tree had a dynamic W^X hook before stage2_wx_qemu.h existed — that
 * absence is exactly the defect which left every board-free gate red. (For
 * this target the promotion is a no-op anyway: `holdtest` links no stage2.o,
 * so the weak stage2_wx_fault reference resolves to 0.) */
static const struct qemu_guest_sync_ops holdtest_ops = {
	.claim_first = holdtest_claim_brk,
};

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		/* Nothing arms a timer on this target -- genuinely unexpected. */
		pl011_puts("HV: unexpected IRQ/FIQ, halting\n");
		for (;;)
			__asm__ volatile("wfi");
	}

	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		/* Shared chain: holdtest_claim_brk() (the mechanism under test)
		 * gets first refusal via claim_first, then the chain does the
		 * unconditional dynamic W^X promotion every QEMU guest needs --
		 * without it the guest cannot execute its first instruction
		 * under STAGE2_WX_DYNAMIC's default XN mapping. See
		 * el2_exc_qemu_common.h for what else the chain offers (none of
		 * it applies to this target -- see holdtest_ops above). */
		if (qemu_guest_sync(frame, ec, &holdtest_ops))
			return;

		/* ---- HVC: this test's own phase-transition sync points. ---- */
		if (ec == 0x16u) {
			uint32_t imm = (uint32_t)(frame->esr & 0xffffu);

			switch (imm) {
			case HOLDTEST_HVC_A_DONE:
				print_verdict("TEST A (pre-entry: TDE+BRK armed BEFORE guest ran)",
				              a_outcome);
				enter_phase_b(frame);
				return;   /* frame->elr/spsr already repointed */

			case HOLDTEST_HVC_B_ARM_NOW:
				pl011_puts("HV: guest asked to arm BRK+TDE now (guest ALREADY "
				           "ran for a while) -- arming\n");
				patch_brk(holdtest_entryB_patchsite, &saved_b);
				set_tde(1);
				pl011_puts("HV: DIAG patch site now reads 0x");
				pl011_put_hex32(*(volatile uint32_t *)holdtest_entryB_patchsite);
				pl011_puts(" (expect 0xd420fa00) saved_b=0x");
				pl011_put_hex32(saved_b);
				pl011_puts("\n");
				frame->elr += 4u;   /* skip the hvc, resume same context */
				return;

			case HOLDTEST_HVC_B_DONE:
				print_verdict("TEST A (pre-entry: TDE+BRK armed BEFORE guest ran)",
				              a_outcome);
				print_verdict("TEST B (post-entry: TDE+BRK armed AFTER guest "
				              "already ran for a while)", b_outcome);
				pl011_puts("HOLDTEST: RUN COMPLETE\n");
				qemu_poweroff();
				/* not reached */

			case HOLDTEST_HVC_SELFTRAP:
				if (g_phase == PHASE_A) {
					a_outcome = -1;
					enter_phase_b(frame);
				} else {
					b_outcome = -1;
					print_verdict("TEST A (pre-entry: TDE+BRK armed BEFORE guest ran)",
					              a_outcome);
					print_verdict("TEST B (post-entry: TDE+BRK armed AFTER guest "
					              "already ran for a while)", b_outcome);
					pl011_puts("HOLDTEST: RUN COMPLETE\n");
					qemu_poweroff();
				}
				return;

			default:
				frame->elr += 4u;
				return;
			}
		}
	}

	/* Anything else (an unexpected fault, or our own EL2 sync exception)
	 * is a genuine problem on this minimal target: report and halt. */
	pl011_puts("HV: UNEXPECTED trap kind=0x");
	pl011_put_hex32((uint32_t)kind);
	pl011_puts(" esr=0x");
	pl011_put_hex64(frame->esr);
	pl011_puts(" elr=0x");
	pl011_put_hex64(frame->elr);
	pl011_puts("\n");
	for (;;)
		__asm__ volatile("wfi");
}

/* Exposed to main_holdtest_qemu.c so it can perform the very first patch
 * (test A's pre-entry arm) before the initial eret -- everything AFTER
 * that first eret is entirely handled from inside el2_trap() above. */
void
holdtest_arm_phase_a(void)
{
	patch_brk(holdtest_entryA_patchsite, &saved_a);
	set_tde(1);
}
