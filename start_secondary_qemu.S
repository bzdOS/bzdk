/* SPDX-License-Identifier: BSD-2-Clause */

/* start_secondary_qemu.S — PSCI CPU_ON secondary entry for the QEMU `virt`
 * dual/SMP CI target (main_dual_qemu.c / smp-qemu-ci.sh).
 *
 * WHY A NEW FILE INSTEAD OF LINKING start.S's _start_secondary DIRECTLY:
 * start.S ALSO defines `_start` (the real-board U-Boot entry point, `.section
 * .text.start`), and every QEMU target already has its OWN `_start` in
 * start_qemu.S (a completely different boot contract — see that file's
 * header: no U-Boot, no inherited MMU-on state, own stack, never returns).
 * Linking start.o and start_qemu.o into the same image would be a duplicate
 * `_start` symbol, full stop. This file exists SOLELY to let _start_secondary
 * be linked without also dragging in start.S's _start.
 *
 * WHY THE ROUTINE ITSELF IS AN UNMODIFIED COPY, NOT A REWRITE:
 * The task this file serves is proving that start.S's EXISTING
 * _start_secondary logic — reload smp_boot_config[] (captured by smp_init()
 * on CPU0) MMU-off, then flip SCTLR_EL2 last — works as-is under QEMU's PSCI
 * CPU_ON, with ZERO new assembly logic. Investigation (see smp.h's own header
 * comment and start.S's _start_secondary banner) found that:
 *
 *   1. PSCI CPU_ON architecturally always starts the target core at the
 *      requested entry PA with the MMU OFF, VBAR at its reset value, on an
 *      undefined SP — regardless of any other core's state. This is true on
 *      real hardware AND under QEMU's `virt,virtualization=on` PSCI
 *      emulation identically; nothing QEMU-specific to accommodate here.
 *   2. start.S's _start_secondary does not depend on U-Boot at all, despite
 *      living in the "real hardware" file: it depends only on
 *      smp_boot_config[] already holding valid values by the time PSCI
 *      CPU_ON fires, which smp_init() (unmodified, called from
 *      main_dual_qemu.c exactly as main_dbg.c calls it on the board) already
 *      guarantees on ANY caller.
 *   3. Under this QEMU target, main_dual_qemu.c/start_qemu.S never turn the
 *      HV's own EL2 stage-1 MMU on at all (see start_qemu.S's header: "MMU is
 *      left OFF for the entire run... this isn't a shortcut, it's simply
 *      everything is uncached"). So the captured SCTLR_EL2 has M=0, and
 *      _start_secondary's "enable MMU + caches by writing the primary's
 *      SCTLR_EL2 value LAST" step degenerates to a no-op MMU-stays-off write
 *      — no page tables, no TTBR0 validity requirement, nothing that could
 *      fault. The elaborate cache-coherency rationale in smp.h's header
 *      (SMPEN, cacheable Normal mappings) simply doesn't apply when nothing
 *      is cached in the first place; the code that exists for the board's
 *      MMU-on case degrades safely to the QEMU MMU-off case UNCHANGED.
 *   4. The one board-specific step start.S's version already OMITS on
 *      purpose — writing CPUECTLR_EL1.SMPEN — is IMP-DEF and left to
 *      ATF/BL31 on real hardware (see that file's comment: touching it here
 *      traps to EL3 with no handler). Under QEMU there is no ATF either, but
 *      SMPEN only matters for D-cache coherency, which (per point 3) is moot
 *      with the MMU off — so omitting it is correct here for a different
 *      reason than on the board, but still correct.
 *
 * So this file is a byte-for-byte copy of start.S's _start_secondary + its
 * STAGE breadcrumb macro (both verified needed, neither changed even
 * cosmetically) in its own translation unit, PLUS a one-line change to how
 * the entry becomes reachable: none. The routine is copied verbatim
 * specifically so a future edit to start.S's real secondary-entry logic does
 * not silently desync from what this QEMU proof exercises — if start.S's
 * _start_secondary ever changes, this file must be re-diffed against it by
 * hand (there is no #include-shared-source mechanism for mixed asm/linker
 * concerns here); this is called out explicitly rather than hidden.
 *
 * All symbols referenced (smp_boot_config, smp_stacks) are ordinary .bss
 * globals in smp.o, placed by link_qemu.ld's generic `*(.bss)` catch-all —
 * no linker-script changes needed. Not `.text.start` (that section is
 * reserved for start_qemu.o's primary entry, force-placed first by
 * link_qemu.ld's `KEEP(*(.text.start))`); plain `.text` is fine since PSCI
 * CPU_ON is handed this routine's PHYSICAL ADDRESS directly (computed via
 * `adrp`/`&_start_secondary` in smp.c, not via section placement).
 */

#include "smp.h"		/* SMP_BC_BASE, SMP_CFG_*, SMP_STACK_SIZE (C decls are
				 * hidden behind #ifndef __ASSEMBLER__ in smp.h) */

/* STAGE s — stamp the per-CPU progress breadcrumb word(14+cpuid) with
 * 0xA5A5_<s>_<cpuid>. x20 must already hold the breadcrumb address, w19 the
 * cpuid. str + dc civac + dsb so a post-fault `md`/breadcrumb read sees it
 * (dc civac is a harmless VA=PA op with the MMU still off). Progressive
 * overwrite: the final value in the word is the LAST stage this core reached
 * before it either parked in smp_secondary_main() or died. Copied verbatim
 * from start.S — see this file's header. */
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

	.section .text
	.balign	0x40
	.global	_start_secondary
	.type	_start_secondary, %function
_start_secondary:
	/* cpuid = MPIDR_EL1 aff0 (0..3 on the single-cluster A64; QEMU's `-smp 4`
	 * with no explicit cluster/socket topology gives the same flat aff0
	 * numbering). */
	mrs	x19, mpidr_el1
	and	x19, x19, #0xff

	/* Breadcrumb address (held in callee-saved x20 for the whole routine):
	 * SMP_BC_BASE + (14 + cpuid)*4. */
	movz	x20, #(SMP_BC_BASE & 0xffff)
	movk	x20, #(SMP_BC_BASE >> 16), lsl #16
	add	x10, x19, #14
	add	x20, x20, x10, lsl #2

	STAGE	0x00			/* pre-MMU: reached _start_secondary */

	/* 1. SMPEN: NOT set here — see file banner point 4 (start.S omits it for
	 * a real-hardware reason; it's moot here because the MMU never turns on
	 * at all on this target, see point 3). Just mark the stage and go. */
	STAGE	0x01			/* past the (omitted) SMPEN step */

	/* 2. Reload the primary's MMU/EL config (read MMU-off from DRAM;
	 * main_dual_qemu.c's own EL2 setup already leaves SCTLR_EL2.M=0, so this
	 * reload's later SCTLR write is a no-op enable — see file banner). */
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

	/* 3. Write the primary's SCTLR_EL2 LAST (see file banner: M=0 on this
	 * target, so the MMU stays off — this write is a faithful reload, not a
	 * board-only step skipped). */
	ldr	x3, [x2, #(SMP_CFG_SCTLR * 8)]
	msr	sctlr_el2, x3
	isb
	STAGE	0x03			/* SCTLR reloaded */

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
