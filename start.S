/* start.S — bzdOS microkernel entry point (AArch64, Allwinner A64 / BPI-M64).
 *
 * U-Boot's `go 0x42000000` calls this as:
 *     long entry(int argc, char **argv)
 * at EL2, MMU already ON, running on U-Boot's own stack. Contract (see
 * PROJECT.md "ABI / calling convention (КРИТИЧНО)"):
 *   - Do NOT touch the MMU or caches; do NOT leave any lasting change to
 *     U-Boot's own memory — `ret` must land back in U-Boot exactly as if we
 *     had never run (this is what lets iteration happen without a physical
 *     power-cycle: loady a new binary, `go` it again).
 *   - Save/restore the callee-saved registers (x19-x30, i.e. x19..x28 plus
 *     fp/lr) so returning via `ret` lands cleanly back in U-Boot's prompt.
 *   - Zero our own .bss (__bss_start..__bss_end, provided by link.ld).
 *   - Call main() and propagate its return value in x0.
 *
 * H2(a) STACK HARDENING (2026-07-24): U-Boot's inherited SP lives in high
 * DRAM that stage-2 (once the guest is running) identity-maps RW to the
 * guest -- no carve-out protects it (stage2.c only excludes HVIMG_BASE/
 * HVSCR_BASE, and U-Boot's stack is neither). A buggy/malicious guest write
 * there would corrupt our live call stack and every exception frame
 * el2_common (exceptions.S) pushes on it -- el2_common never switches SP, it
 * just uses whatever SP_EL2 is already live (all 16 vectors are "current
 * EL2, SPx" or "lower EL", both of which bank to SP_EL2, the ONE stack
 * register in play from _start to `ret`).
 *
 * Fix: immediately after saving U-Boot's callee-saved regs (i.e. as early as
 * possible, before any C code -- including el2_install() and the .bss zero
 * loop -- runs), CPU0 switches SP to smp_stacks[0]: a plain 16 KiB .bss array
 * that already exists (smp.c, shared with the secondaries' per-core stacks)
 * and, being part of THIS image's .bss, already sits inside the one 2 MiB
 * window stage2.c's HVIMG_L2_IDX excludes from the guest's stage-2 map -- no
 * new carve-out, no DTB change, no link.ld change needed (link.ld's existing
 * "image must fit in 2 MiB" ASSERT already guards this the same way it
 * guards every other static allocation in the tree). Row 0 of smp_stacks[]
 * was always unused by the secondaries (_start_secondary only ever indexes
 * rows 1..SMP_MAX_CPUS-1 -- see smp.h) -- this finally gives CPU0 the "own
 * stack" smp.h's comment already claimed it had.
 *
 * U-Boot's ORIGINAL sp (captured right after the push, i.e. the value the
 * epilogue below must pop from) is stashed in el2_boot_sp_saved -- plain
 * DRAM, not a register, so it survives the `bl main` call untouched -- and
 * restored right before the epilogue's ldp sequence. Nothing between the two
 * switches ever touches U-Boot's stack memory again, so the external
 * contract (U-Boot sees identical SP/state on return) is unchanged; only the
 * stack bzdOS itself runs on while resident is now HV-owned and stage-2
 * protected.
 *
 * This section is named .text.start and is force-placed first by link.ld's
 * KEEP(*(.text.start)), so _start ends up exactly at the link base 0x42000000.
 */

#include "smp.h"		/* SMP_BC_BASE, SMP_CFG_*, SMP_STACK_SIZE (C decls are
				 * hidden behind #ifndef __ASSEMBLER__ in smp.h) */

	.section .text.start, "ax"
	.global _start
	.type _start, %function
_start:
	/* BREADCRUMB (SRAM): _start reached */
	movz	x9, #0x5354, lsl #16
	movk	x9, #0x5254, lsl #0      /* "START" in hex */
	movz	x10, #0x0001, lsl #16
	movk	x10, #0x9010, lsl #0      /* x10 = 0x00019010 */
	str	x9, [x10]

	/* Save callee-saved regs (x19-x28, fp=x29, lr=x30) on U-Boot's stack.
	 * 6 pairs * 16 bytes = 96 bytes, keeps SP 16-byte aligned. */
	stp	x29, x30, [sp, #-96]!
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	mov	x29, sp

	/* H2(a): stash U-Boot's SP (as it stands right now, post-push -- this is
	 * exactly the value the epilogue's ldp sequence must pop from) into
	 * el2_boot_sp_saved, THEN switch to our own protected stack. Must happen
	 * before the .bss zero loop below: smp_stacks[] (which the new stack
	 * lives inside) is itself part of .bss, and nothing is pushed onto the
	 * new stack between the switch and that loop, so zeroing it in the same
	 * pass as every other .bss byte is safe (there is nothing live there to
	 * clobber yet). x9-x11 are ordinary caller-saved scratch regs here --
	 * the values U-Boot handed us in them (if any) were never part of the
	 * AAPCS callee-saved set we promised to preserve. */
	mov	x9, sp
	adrp	x10, el2_boot_sp_saved
	add	x10, x10, :lo12:el2_boot_sp_saved
	str	x9, [x10]

	adrp	x11, smp_stacks
	add	x11, x11, :lo12:smp_stacks
	add	x11, x11, #SMP_STACK_SIZE	/* top of smp_stacks[0] (row 0 --
						 * see smp.h: unused by any
						 * secondary, reserved for CPU0) */
	mov	sp, x11

	/* Zero .bss: for (p = __bss_start; p < __bss_end; p += 8) *p = 0; */
	adrp	x9, __bss_start
	add	x9, x9, :lo12:__bss_start
	adrp	x10, __bss_end
	add	x10, x10, :lo12:__bss_end
1:
	cmp	x9, x10
	b.hs	2f
	str	xzr, [x9], #8
	b	1b
2:
	/* argc/argv (x0/x1) from U-Boot are not used by main(); ignored. */
	bl	main
	mov	x9, x0		/* stash main()'s return value */

	/* H2(a): if main() ever returns (main_stage0.c, main.c's 'q' command,
	 * ...), switch back to U-Boot's ORIGINAL sp (saved above) before
	 * touching its stack again -- the ldp sequence below pops from
	 * whatever sp is live at the time it runs, and el2_boot_sp_saved is
	 * plain DRAM so `bl main` (and everything it called, transitively)
	 * could never have disturbed it. From this point on we are back on
	 * U-Boot's stack exactly as we found it, same as before this change. */
	adrp	x10, el2_boot_sp_saved
	add	x10, x10, :lo12:el2_boot_sp_saved
	ldr	x10, [x10]
	mov	sp, x10

	/* Restore callee-saved regs, then return x9 -> x0 to U-Boot. */
	ldp	x27, x28, [sp, #80]
	ldp	x25, x26, [sp, #64]
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96

	mov	x0, x9
	ret
	.size _start, . - _start

/* ===========================================================================
 * _start_secondary — PSCI CPU_ON entry for CPUs 1..3 (see smp.c / smp.h).
 *
 * BL31 (ARM-TF) enters us here at EL2 with the MMU OFF, caches off, on an
 * undefined SP, with the PSCI context id in x0. Unlike the primary (which
 * inherited U-Boot's MMU-on state), a secondary must build its own EL2
 * environment before it can safely touch any shared memory:
 *
 *   1. Set CPUECTLR_EL1.SMPEN=1 FIRST — on Cortex-A53 the D-cache does not
 *      join the coherency domain until SMPEN is set; enabling caches (via the
 *      MMU) without it gives this core a private, un-snooped cache and silently
 *      corrupts every shared structure. Must precede the SCTLR write below.
 *   2. Reload the primary's MMU/EL config (MAIR/TCR/TTBR0/VBAR/HCR) captured in
 *      smp_boot_config[] by smp_init(). Read MMU-OFF, so as a plain (Device)
 *      load straight from DRAM — smp_init() cleaned those lines to the PoC.
 *   3. Enable the MMU + caches by writing the primary's SCTLR_EL2 value LAST.
 *      Now this core runs the SAME cacheable Normal mappings as CPU0, and the
 *      A53 SCU keeps their D-caches coherent — the run-queue is shareable.
 *   4. Set a per-CPU stack (MPIDR-indexed into smp_stacks[]) and call
 *      smp_secondary_main(cpuid).
 *
 * All symbols referenced (smp_boot_config, smp_stacks) are identity-mapped
 * (VA==PA) under U-Boot's tables, so the addresses computed by adrp/add are
 * valid both before and after the MMU is turned on.
 * =========================================================================== */
	.section .text
	.balign	0x40
	.global	_start_secondary
	.type	_start_secondary, %function
/* STAGE s — stamp the per-CPU progress breadcrumb word(14+cpuid) with
 * 0xA5A5_<s>_<cpuid>. x20 must already hold the breadcrumb address, w19 the
 * cpuid. str + dc civac + dsb so a post-reset `md` sees it (dc civac is a
 * harmless VA=PA op with the MMU still off). Progressive overwrite: the final
 * value in the word is the LAST stage this core reached before it died. */
	.macro	STAGE s
	movz	w11, #0xa5a5, lsl #16
	.if	\s != 0
	orr	w11, w11, #(\s << 8)
	.endif
	orr	w11, w11, w19
	str	w11, [x20]
	dc	civac, x20
	dsb	sy
	.endm

_start_secondary:
	/* cpuid = MPIDR_EL1 aff0 (0..3 on the single-cluster A64). */
	mrs	x19, mpidr_el1
	and	x19, x19, #0xff

	/* Breadcrumb address (held in callee-saved x20 for the whole routine):
	 * SMP_BC_BASE + (14 + cpuid)*4. */
	movz	x20, #(SMP_BC_BASE & 0xffff)
	movk	x20, #(SMP_BC_BASE >> 16), lsl #16
	add	x10, x19, #14
	add	x20, x20, x10, lsl #2

	STAGE	0x00			/* pre-MMU: reached _start_secondary */

	/* 1. SMPEN: NOT set here. CPUECTLR_EL1 (S3_1_C15_C2_1) is IMP-DEF and
	 * accessing it from EL2 on this SoC TRAPS to EL3, where ATF has no handler
	 * -> the secondary hung right here (stage crumb stuck at 0x00, never 0x01,
	 * confirmed live 2026-07-18). ATF/BL31 already sets CPUECTLR_EL1.SMPEN as
	 * part of its per-core power-on (the primary _start never touches it either
	 * and boots fully cache-coherent), so the D-cache is already in the
	 * coherency domain by the time we get here. Just mark the stage and go. */
	STAGE	0x01			/* past the (removed) SMPEN step */

	/* 2. Reload the primary's MMU/EL config (read MMU-off from DRAM). */
	adrp	x2, smp_boot_config
	add	x2, x2, :lo12:smp_boot_config
	ldr	x3, [x2, #(SMP_CFG_MAIR  * 8)]
	msr	mair_el2, x3
	ldr	x3, [x2, #(SMP_CFG_TCR   * 8)]
	msr	tcr_el2, x3
	ldr	x3, [x2, #(SMP_CFG_TTBR0 * 8)]
	msr	ttbr0_el2, x3
	ldr	x3, [x2, #(SMP_CFG_VBAR  * 8)]
	msr	vbar_el2, x3
	ldr	x3, [x2, #(SMP_CFG_HCR   * 8)]
	msr	hcr_el2, x3
	dsb	sy
	tlbi	alle2			/* drop any stale EL2 TLB before enabling */
	dsb	sy
	ic	iallu			/* clean I-cache state before SCTLR.I takes effect */
	dsb	sy
	isb
	STAGE	0x02			/* MMU config reloaded, pre-SCTLR */

	/* 3. Enable MMU + caches: write the primary's SCTLR_EL2 (M=C=I=1) LAST. */
	ldr	x3, [x2, #(SMP_CFG_SCTLR * 8)]
	msr	sctlr_el2, x3
	isb
	/* From here the MMU is on and this core is cache-coherent with CPU0. */
	STAGE	0x03			/* MMU ON, running translated */

	/* 4. Per-CPU stack: sp = &smp_stacks[cpuid][SMP_STACK_SIZE]
	 *      = smp_stacks + (cpuid + 1) * SMP_STACK_SIZE. */
	adrp	x4, smp_stacks
	add	x4, x4, :lo12:smp_stacks
	add	x5, x19, #1
	mov	x6, #SMP_STACK_SIZE
	mul	x5, x5, x6
	add	x4, x4, x5
	mov	sp, x4

	STAGE	0x04			/* stack set, about to enter C */

	/* smp_secondary_main(cpuid) — noreturn. Park in WFI if it ever returns. */
	mov	x0, x19
	bl	smp_secondary_main
1:	wfi
	b	1b
	.size _start_secondary, . - _start_secondary

/* H2(a) stack-hardening scratch word: U-Boot's original SP (captured in
 * _start right after the callee-saved-regs push), so it can be restored
 * before `ret` even though CPU0 spends the resident phase on smp_stacks[0]
 * instead. One CPU (CPU0, before SMP bring-up) ever writes or reads this --
 * no locking needed.
 *
 * MUST be in .data, NOT .bss: _start writes this word BEFORE the .bss zero
 * loop runs (the SP capture is right after the register push, the switch and
 * capture precede the zero loop). If it lived in .bss, that very loop would
 * zero the just-saved SP back to 0, and any build whose main() returns
 * (main_stage0.c, main.c's 'q') would `ret` to U-Boot with SP=0 and crash.
 * .data is loaded from the image and never touched by the .bss zero loop, so
 * the captured value survives. It still sits inside the 2 MiB HVIMG stage-2
 * window (dead weight to a guest — an address, not a code pointer). */
	.section .data
	.balign 8
el2_boot_sp_saved:
	.quad 0
