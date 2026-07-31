/* SPDX-License-Identifier: BSD-2-Clause */

/* exceptions.S — EL2 exception vector table + full-context save for the bzdOS
 * microkernel (AArch64, Allwinner A64).
 *
 * This is the RTOS bedrock: without our OWN vector table (VBAR_EL2) there is
 * no way to trap synchronous faults, IRQ, FIQ or SError at EL2 — hence no
 * preemption, no FIQ fast-path, and no fault reporting. Every one of the 16
 * architectural vectors saves the complete integer context to the current
 * stack and calls el2_trap(struct el2_frame *frame, unsigned long kind).
 *
 * We run under U-Boot (MMU on, EL2, U-Boot's stack). el2_install() saves
 * U-Boot's VBAR_EL2 and points it at our table; el2_uninstall() restores it,
 * so a clean return to the U-Boot prompt is preserved. NOTE: there is NO
 * separate dedicated exception stack — el2_common banks to the live SP_EL2
 * (after H2(a) that is CPU0's protected smp_stacks[0] for the resident phase,
 * see start.S). A fault taken while SP_EL2 is itself corrupt would push onto a
 * bad stack; tolerated only because exception entry masks DAIF, so IRQ/FIQ/
 * SError cannot nest — only a synchronous fault inside a handler (a hypervisor
 * bug, already fatal) can.
 *
 * Frame layout (offsets, bytes) — must match struct el2_frame in exceptions.h:
 *   0x000..0x0f8  x0..x30            (31 * 8 = 248)
 *   0x100         kind               (which vector fired: see EL2_KIND_*)
 *   0x108         elr_el2            (faulting / return PC)
 *   0x110         spsr_el2
 *   0x118         esr_el2            (syndrome: cause of the exception)
 *   0x120         far_el2            (faulting virtual address)
 *   0x128         sp_at_entry        (SP before we pushed the frame)
 * total 0x130 (304) -> round to 0x140 (320) for 16-byte alignment.
 */

	.set FRAME_SIZE, 0x140

/* One architectural vector slot (0x80 bytes). Stash x0 in TPIDR_EL2 (a
 * scratch sysreg we never otherwise use) so we can load the kind index
 * without clobbering the guest's x0, then jump to the common path. CAVEAT:
 * a synchronous fault taken INSIDE a handler, before el2_common stores x0 to
 * the frame, would overwrite this TPIDR_EL2 stash — acceptable only because
 * such nesting is a hypervisor bug (async exceptions can't nest: DAIF is
 * masked on entry), and no free GPR exists at vector entry to avoid the
 * scratch-sysreg round-trip. */
	.macro	VEC kind
	.balign	0x80
	msr	tpidr_el2, x0
	mov	x0, #\kind
	b	el2_common
	.endm

	.section .text
	.balign	0x800			/* VBAR must be 2 KB aligned */
	.global	el2_vectors
el2_vectors:
	/* Current EL, SP_EL0 */
	VEC 0		/* synchronous  */
	VEC 1		/* IRQ          */
	VEC 2		/* FIQ          */
	VEC 3		/* SError       */
	/* Current EL, SP_ELx */
	VEC 4
	VEC 5
	VEC 6
	VEC 7
	/* Lower EL, AArch64 (the guest, EL1) */
	VEC 8
	VEC 9
	VEC 10
	VEC 11
	/* Lower EL, AArch32 */
	VEC 12
	VEC 13
	VEC 14
	VEC 15

/* Common save/dispatch/restore. On entry: x0 = kind, original x0 in TPIDR_EL2. */
	.type	el2_common, %function
el2_common:
	sub	sp, sp, #FRAME_SIZE
	/* Save x1..x30 (x0 is still the kind; original x0 is in TPIDR_EL2). */
	stp	x1,  x2,  [sp, #0x008]
	stp	x3,  x4,  [sp, #0x018]
	stp	x5,  x6,  [sp, #0x028]
	stp	x7,  x8,  [sp, #0x038]
	stp	x9,  x10, [sp, #0x048]
	stp	x11, x12, [sp, #0x058]
	stp	x13, x14, [sp, #0x068]
	stp	x15, x16, [sp, #0x078]
	stp	x17, x18, [sp, #0x088]
	stp	x19, x20, [sp, #0x098]
	stp	x21, x22, [sp, #0x0a8]
	stp	x23, x24, [sp, #0x0b8]
	stp	x25, x26, [sp, #0x0c8]
	stp	x27, x28, [sp, #0x0d8]
	stp	x29, x30, [sp, #0x0e8]

	/* Post-GPR fields are PACKED right after x[31] (which ends at 0xf8) to
	 * match struct el2_frame exactly: kind@0xf8, elr@0x100, spsr@0x108,
	 * esr@0x110, far@0x118, sp@0x120. (An earlier 8-byte gap here made
	 * el2_trap read kind as elr -> corrupted return -> PC derailed to 4;
	 * caught via the 0x50000400 breadcrumb.) */
	mrs	x1, tpidr_el2
	str	x1, [sp, #0x000]	/* original x0 -> frame->x[0]  */
	str	x0, [sp, #0x0f8]	/* kind        -> frame->kind  */

	/* System registers describing the exception. */
	mrs	x1, elr_el2
	mrs	x2, spsr_el2
	stp	x1, x2, [sp, #0x100]	/* elr, spsr                   */
	mrs	x1, esr_el2
	mrs	x2, far_el2
	stp	x1, x2, [sp, #0x110]	/* esr, far                    */
	add	x1, sp, #FRAME_SIZE	/* SP as it was at entry       */
	str	x1, [sp, #0x120]

	/* el2_trap(frame = sp, kind). */
	mov	x0, sp
	ldr	x1, [sp, #0x0f8]
	bl	el2_trap

	/* Restore ELR/SPSR (el2_trap may have advanced ELR to skip a faulting
	 * instruction, or left it to re-run / to eret into a scheduled task). */
	ldp	x1, x2, [sp, #0x100]
	msr	elr_el2, x1
	msr	spsr_el2, x2

	/* Restore x1..x30, then x0, then pop and eret. */
	ldp	x1,  x2,  [sp, #0x008]
	ldp	x3,  x4,  [sp, #0x018]
	ldp	x5,  x6,  [sp, #0x028]
	ldp	x7,  x8,  [sp, #0x038]
	ldp	x9,  x10, [sp, #0x048]
	ldp	x11, x12, [sp, #0x058]
	ldp	x13, x14, [sp, #0x068]
	ldp	x15, x16, [sp, #0x078]
	ldp	x17, x18, [sp, #0x088]
	ldp	x19, x20, [sp, #0x098]
	ldp	x21, x22, [sp, #0x0a8]
	ldp	x23, x24, [sp, #0x0b8]
	ldp	x25, x26, [sp, #0x0c8]
	ldp	x27, x28, [sp, #0x0d8]
	ldp	x29, x30, [sp, #0x0e8]
	ldr	x0,       [sp, #0x000]
	add	sp, sp, #FRAME_SIZE
	eret
	.size	el2_common, . - el2_common

/* void el2_install(void): point VBAR_EL2 at our table, saving U-Boot's in
 * el2_saved_vbar. isb so the new vectors take effect before we return. */
	.global	el2_install
	.type	el2_install, %function
el2_install:
	mrs	x0, vbar_el2
	adrp	x1, el2_saved_vbar
	add	x1, x1, :lo12:el2_saved_vbar
	str	x0, [x1]
	adrp	x0, el2_vectors
	add	x0, x0, :lo12:el2_vectors
	msr	vbar_el2, x0
	isb
	ret
	.size	el2_install, . - el2_install

/* void el2_uninstall(void): restore U-Boot's VBAR_EL2 for a clean return. */
	.global	el2_uninstall
	.type	el2_uninstall, %function
el2_uninstall:
	adrp	x0, el2_saved_vbar
	add	x0, x0, :lo12:el2_saved_vbar
	ldr	x0, [x0]
	msr	vbar_el2, x0
	isb
	ret
	.size	el2_uninstall, . - el2_uninstall

	.section .bss
	.balign	8
el2_saved_vbar:
	.skip	8
