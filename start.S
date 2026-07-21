/* start.S — bzdOS microkernel entry point (AArch64, Allwinner A64 / BPI-M64).
 *
 * U-Boot's `go 0x42000000` calls this as:
 *     long entry(int argc, char **argv)
 * at EL2, MMU already ON, running on U-Boot's own stack. Contract (see
 * PROJECT.md "ABI / calling convention (КРИТИЧНО)"):
 *   - Do NOT touch the MMU, caches, or U-Boot's stack pointer — we run on
 *     whatever SP U-Boot handed us and must leave it exactly as we found it.
 *   - Save/restore the callee-saved registers (x19-x30, i.e. x19..x28 plus
 *     fp/lr) so returning via `ret` lands cleanly back in U-Boot's prompt.
 *   - Zero our own .bss (__bss_start..__bss_end, provided by link.ld).
 *   - Call main() and propagate its return value in x0.
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
