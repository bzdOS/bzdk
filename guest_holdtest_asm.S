/* SPDX-License-Identifier: BSD-2-Clause */

/* guest_holdtest_asm.S — hand-written EL1 entry stubs + vector table for the
 * hold-gate/BRK-ordering QEMU diagnostic (see holdtest_guest.h / main_holdtest_qemu.c).
 *
 * Hand-assembled (not C) for ONE reason: each test entry point must begin
 * with EXACTLY one known instruction (a NOP) at a fixed, symbol-addressable
 * location, so main_holdtest_qemu.c can patch it to BRK_INSTR the same way
 * gdbstub.c's bp_insert() patches a live instruction — a C function's first
 * instruction is not a reliable, stable target (prologue, -O2 codegen,
 * relaxation) the way a dedicated asm label is.
 *
 * Both entries fall straight into a plain C helper for the rest of their
 * logic (guest_holdtest_payload.c) — asm is used ONLY where instruction
 * identity/addressability actually matters.
 */

	.section .text

	.balign 16
	.global holdtest_entryA
	.type holdtest_entryA, %function
holdtest_entryA:
	.global holdtest_entryA_patchsite
holdtest_entryA_patchsite:
	nop				/* [PATCH SITE A] EL2 overwrites this with
					 * BRK_INSTR BEFORE this eret ever
					 * happens -- the "pre-entry" hypothesis
					 * under test. */
	b	holdtest_entryA_c
	.size holdtest_entryA, . - holdtest_entryA

	.balign 16
	.global holdtest_entryB
	.type holdtest_entryB, %function
holdtest_entryB:
	b	holdtest_entryB_c	/* runs first, TDE=0, spins a while --
					 * "guest already running" control case */
	.global holdtest_entryB_patchsite
holdtest_entryB_patchsite:
	nop				/* [PATCH SITE B] EL2 patches this to
					 * BRK_INSTR only AFTER the guest has
					 * already been spinning for a while
					 * (from the HOLDTEST_HVC_B_ARM_NOW
					 * handler) -- the "post-entry" control. */
	b	holdtest_entryB_resume
	.size holdtest_entryB, . - holdtest_entryB

/* ------------------------------------------------------------------ *
 * EL1 vector table -- installed as VBAR_EL1 before either eret above.
 * Only the "Current EL, SPx" synchronous vector (index 4, offset 0x200)
 * is realistically reachable: the guest always runs EL1h (SPSel=1), so if
 * MDCR_EL2.TDE fails to route a debug/BRK exception to EL2, it self-traps
 * HERE, not into the "lower EL" vectors (this target never runs EL0). Every
 * other vector is a deliberate halt -- genuinely unexpected on this target,
 * better to freeze visibly (SP/PC intact for a debugger to inspect) than
 * silently do something undefined.
 * ------------------------------------------------------------------ */
	.balign 0x800
	.global holdtest_el1_vectors
holdtest_el1_vectors:
	.balign 0x80			/* 0: current EL, SP0, sync -- unused */
	b	.
	.balign 0x80			/* 1: current EL, SP0, IRQ */
	b	.
	.balign 0x80			/* 2: current EL, SP0, FIQ */
	b	.
	.balign 0x80			/* 3: current EL, SP0, SError */
	b	.
	.balign 0x80			/* 4: current EL, SPx, sync -- THE vector
					 * a misrouted guest BRK actually lands
					 * in (EL1h => SPSel=1). SP_EL1 is
					 * already valid at this point (set by
					 * main_holdtest_qemu.c before eret), so
					 * a plain bl is safe. */
	bl	holdtest_el1_selftrap
	.balign 0x80			/* 5: current EL, SPx, IRQ */
	b	.
	.balign 0x80			/* 6: current EL, SPx, FIQ */
	b	.
	.balign 0x80			/* 7: current EL, SPx, SError */
	b	.
	.balign 0x80			/* 8..15: lower-EL groups -- this target
					 * never runs EL0, unreachable */
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.balign 0x80
	b	.
