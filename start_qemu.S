/* start_qemu.S — bzdOS microkernel entry point for the QEMU `virt` CI target
 * (see link_qemu.ld / main_qemu.c / docs/qemu-ci.md).
 *
 * This is NOT start.S with a new label — the boot contract is fundamentally
 * different, hence a separate file:
 *
 *   start.S (real board): U-Boot's `go 0x42000000` calls us as a C function
 *   at EL2 with the MMU already ON, caches on, and a valid stack already set
 *   up (U-Boot's own) — we must preserve callee-saved regs and `ret` cleanly
 *   back into the U-Boot prompt.
 *
 *   start_qemu.S (this file): QEMU's `-kernel` direct-boot path
 *   (hw/arm/boot.c's arm_load_kernel(), which tries load_elf() first — this
 *   is why a plain ELF works with -kernel, no uImage/Image header needed)
 *   drops us straight onto bare silicon per the Linux/arm64 boot protocol:
 *   MMU and caches OFF, x0 = DTB pointer (ignored — this target hardcodes
 *   every MMIO address it needs, "cited not guessed", just for QEMU virt's
 *   addresses instead of the A64's — see gic_timer_qemu.c/pl011_qemu.c),
 *   PC = our ELF entry point, and — critically — SP is NOT initialized to
 *   anything valid. There is also no firmware to return to: this image
 *   never returns.
 *
 * We are entered at EL2 (the highest EL the vCPU implements, given
 * `-machine virt,virtualization=on` in the documented invocation) — the
 * same exception level start.S runs at, so every other file in this build
 * (exceptions.S, guest.c, stage2.c, ...) needs no EL-related changes at all.
 *
 * So, unlike start.S, this entry point must:
 *   1. Set up our OWN stack (nothing upstream has done this).
 *   2. Zero .bss (same idea as start.S, same __bss_start/__bss_end names,
 *      but provided by link_qemu.ld's own copy — a distinct link unit).
 *   3. Call main() and never return (park in wfi if it somehow does).
 *
 * MMU is left OFF for the entire run — no EL2 stage-1 tables are built at
 * all for this target. That is deliberately fine, not a shortcut: with
 * SCTLR_EL2.M=0 every access is Device memory, and per the ARMv8 ARM the
 * effective cache-enable is treated as 0 regardless of SCTLR.C/I whenever
 * M=0 — so this isn't "cache maintenance ops might misbehave", it's simply
 * "everything is uncached", which is correct, just slower. Every address
 * this freestanding, -fno-pic build touches is a plain link-time-fixed
 * physical address (link_qemu.ld), so VA==PA is automatically true with the
 * MMU off — there is nothing an EL2 stage-1 identity map would add here.
 * (Stage-2, programmed by stage2.c for the EL1 guest, is a completely
 * separate translation regime and is unaffected by this choice.)
 */

	.section .text.start, "ax"
	.global _start
	.type _start, %function
_start:
	/* Our own SP_EL2 — nothing handed us a valid one (unlike start.S,
	 * which inherits U-Boot's). Point it at the top of the static boot
	 * stack reserved below. */
	adrp	x9, qemu_boot_stack_top
	add	x9, x9, :lo12:qemu_boot_stack_top
	mov	sp, x9

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
	/* x0 (DTB pointer from QEMU) intentionally ignored — see file banner:
	 * this target hardcodes its MMIO addresses instead of parsing a DTB. */
	bl	main

	/* main() is not supposed to return on this target (there is no
	 * firmware to return to); if it somehow does, park here rather than
	 * run off into whatever bytes follow in memory. */
3:	wfi
	b	3b
	.size _start, . - _start

	.section .bss
	.balign	16
qemu_boot_stack:
	.skip	0x10000		/* 64 KiB boot stack — generous for main()'s
				 * own (small) call depth; the EL1 guest and
				 * its own tiny stack are entirely separate
				 * (see guest_qemu_payload.c). */
qemu_boot_stack_top:
