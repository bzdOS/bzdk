/* SPDX-License-Identifier: BSD-2-Clause
 * chainload_stub.S -- jump from a running U-Boot (EL2, MMU+caches ON) into a
 * freshly TFTP'd U-Boot proper at 0x4a000000 the way SPL/FEL would enter it:
 * MMU off, caches off, nothing dirty left behind.
 *
 * Why a stub: the stock loader on the eMMC has no `dcache off` command, and
 * U-Boot's arm64 start code assumes a clean entry. Entered with the MMU on,
 * dcache_enable() skips mmu_setup() (it sees CR_M set) and keeps the OLD
 * tables -- which sit at the top of DRAM, exactly where the new image then
 * relocates itself. Measured 2026-09-25 12:38: "Starting application", six
 * seconds of progress, silent hang, no watchdog (not yet started).
 *
 * Clean-by-VA covers only what must be in DRAM: this stub and the candidate
 * image. The old loader's own dirty lines are never read again.
 */
	.text
	.globl _start
_start:
	adr	x2, _start
	add	x3, x2, #4096
	bl	clean_range
	ldr	x2, =0x4a000000
	ldr	x3, =0x4a100000		/* 1 MiB: u-boot.bin is ~840 KiB */
	bl	clean_range
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	mrs	x0, sctlr_el2
	bic	x0, x0, #(1 << 0)	/* M: MMU */
	bic	x0, x0, #(1 << 2)	/* C: dcache */
	bic	x0, x0, #(1 << 12)	/* I: icache */
	msr	sctlr_el2, x0
	isb
	tlbi	alle2
	dsb	sy
	isb
	/* Arm the SoC watchdog (16 s, whole-system reset) so a candidate that
	 * hangs before its own initr_watchdog() costs a warm reset into the
	 * stock loader, not a power-cycle -- and DRAM, with the candidate's
	 * progress marks at 0x4bf00000, survives that reset. Attempts 1 and 2
	 * (2026-09-25) each cost a trip to the switch for want of this. */
	ldr	x5, =0x01c20cb4
	mov	w6, #1
	str	w6, [x5]		/* WDOG_CFG: system reset */
	mov	w6, #0xb1
	str	w6, [x5, #4]		/* WDOG_MODE: 16 s + enable */
	ldr	w6, =0x14af
	str	w6, [x5, #-4]		/* WDOG_CTRL: restart key */
	dsb	sy
	/* progress mark 0: stub reached the jump */
	ldr	x5, =0x4bf00000
	mov	w6, #0xb0
	str	w6, [x5]
	dsb	sy
	ldr	x4, =0x4a000000
	br	x4

clean_range:				/* [x2, x3) by 64-byte lines */
1:	dc	cvac, x2
	add	x2, x2, #64
	cmp	x2, x3
	b.lo	1b
	dsb	sy
	ret
