/* SPDX-License-Identifier: BSD-2-Clause
 * chainload_uimg.S -- chainload_stub.S packaged as a `bootm`-able uImage
 * with the candidate U-Boot appended (.incbin PAYLOAD at +4 KiB), so the
 * STOCK bootcmd can start a candidate with no prompt and no env change:
 * stage it as microkernel-dbg.uimg for one fetch (uimg_chainload_test.sh).
 * Build: gcc -nostdlib -Wl,-Ttext=0x42000000 -DPAYLOAD='"u-boot.bin"'
 *
 * Original stub header:
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
	/* uimg variant: bootm (standalone) enters here at 0x42000000 with the
	 * candidate appended; copy it to its TEXT_BASE first. */
	adr	x0, payload
	adr	x3, payload_end
	ldr	x1, =0x4a000000
0:	ldr	x4, [x0], #8
	str	x4, [x1], #8
	cmp	x0, x3
	b.lo	0b
stub_main:
	adr	x2, stub_main
	add	x3, x2, #4096
	bl	clean_range
	ldr	x2, =0x4a000000
	ldr	x3, =0x4a100000		/* 1 MiB: u-boot.bin is ~840 KiB */
	bl	clean_range
	/* Then clean+invalidate EVERY data cache line by set/way, all levels.
	 * Clean-by-VA of two ranges is not enough: the stock loader's own
	 * data -- its relocated image, page tables, DTB copy -- sits valid in
	 * L1/L2 at the top of DRAM, exactly where the candidate relocates
	 * itself and copies its DTB with the cache off. When the candidate
	 * turns its cache on it reads those stale lines: measured 2026-09-25
	 * 14:47, fdt_path_offset() = FDT_ERR_BADSTRUCTURE on a copy whose
	 * header was intact. Classic ARMv8 set/way walk from CLIDR. */
	mrs	x0, clidr_el1
	and	w3, w0, #0x07000000
	lsr	w3, w3, #23		/* LoC * 2 */
	cbz	w3, 9f
	mov	w10, #0			/* cache level << 1 */
1:	add	w2, w10, w10, lsr #1	/* 3 * level */
	lsr	w1, w0, w2
	and	w1, w1, #7		/* cache type at this level */
	cmp	w1, #2
	b.lt	5f			/* no data/unified cache here */
	msr	csselr_el1, x10
	isb
	mrs	x1, ccsidr_el1
	and	w2, w1, #7
	add	w2, w2, #4		/* log2(line bytes) */
	mov	w4, #0x3ff
	and	w4, w4, w1, lsr #3	/* max way */
	clz	w5, w4			/* way field shift */
	mov	w7, #0x7fff
	and	w7, w7, w1, lsr #13	/* max set */
2:	mov	w9, w4			/* way */
3:	lsl	w6, w9, w5
	orr	w11, w10, w6
	lsl	w6, w7, w2
	orr	w11, w11, w6
	dc	cisw, x11
	subs	w9, w9, #1
	b.ge	3b
	subs	w7, w7, #1
	b.ge	2b
5:	add	w10, w10, #2
	cmp	w3, w10
	b.gt	1b
9:	dsb	sy
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

	.ltorg
	.balign 4096
payload:
	.incbin PAYLOAD
	.balign 8
payload_end:
