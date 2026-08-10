/* SPDX-License-Identifier: BSD-2-Clause */

/* zg3_trivial_payload.S — CPU3's payload for the dual-guest Step 1 QEMU
 * proof (main_dual2_qemu.c / dual-qemu-ci.sh) and the zunhalt re-arm repro
 * (main_dual_rearm_qemu.c / dual-rearm-qemu-ci.sh). See those scripts'
 * headers for the full rationale for staging THIS as a genuine standalone
 * ELF64/AArch64 image and running it through zguest_cpu3.c's REAL,
 * unmodified zephyr_cpu3_run() -> zload2_parse_and_place() code path,
 * rather than a QEMU-only bypass of it.
 *
 * This is deliberately NOT Zephyr and NOT built via the Makefile: it is a
 * standalone, hand-written AArch64 EL1 program, entirely self-contained,
 * built directly by dual-qemu-ci.sh/dual-rearm-qemu-ci.sh with the SAME
 * cross-compiler this whole tree uses ($(CROSS)gcc == aarch64-linux-gnu-gcc)
 * against the companion zg3_trivial_payload.ld linker script -- exactly the
 * way zephyr-guest/build.sh produces the real Zephyr ELF zephyr-qemu-ci.sh
 * stages. The difference is only WHAT is being built, not HOW it is staged
 * or loaded: same "-device loader,file=...,addr=...,force-raw=on" QEMU
 * staging technique zephyr-qemu-ci.sh/snapshot-qemu-ci.sh already use, and
 * the same raw-ELF-file-sitting-in-DRAM contract zload2_parse_and_place()
 * (zload2.c) already expects to be handed by any caller.
 *
 * WHY A HAND-BUILT TRIVIAL ELF, NOT REAL ZEPHYR, FOR STEP 1:
 * The approved plan is explicit that Step 1 needs "a second, independent
 * trivial payload under stage2_zephyr's table" -- proving the MECHANISM
 * (real ELF parse + place + isolate + enter, genuinely concurrent with
 * CPU0's own guest), not proving Zephyr itself boots under QEMU. That is a
 * separate later step, and Zephyr's own board port (see
 * zephyr-guest/boards/bzdos/bpi_m64_hv) targets real Allwinner A64 register
 * addresses QEMU's `virt` machine does not back -- a wholly different
 * question from "does the dual-guest bring-up mechanism work at all",
 * which is all Step 1 needs to answer. Exercising the REAL
 * zload2_parse_and_place()/stage2_zephyr_*()/zguest_cpu3.c code path
 * against a genuine (if tiny) ELF is strictly stronger evidence than
 * bypassing it with a QEMU-only CPU3 entry variant, and turned out to be
 * easy to build, so that is what this file is.
 *
 * WHAT IT DOES: runs at EL1 (entered via zguest_cpu3.c's REAL, unmodified
 * kload_enter(), after the REAL zload2_parse_and_place() has copied this
 * image's one PT_LOAD segment into Zephyr's stage-2 slice and the REAL
 * stage2_zephyr_init()/_enable()/_isolation_selfcheck() have run) and loops
 * forever doing TWO things per iteration:
 *
 *   1. bumps a 32-bit counter at a fixed physical address (unchanged from
 *      the original version of this file) -- kept because it costs nothing
 *      and is still a perfectly reliable liveness signal UNDER QEMU, which
 *      models no caches;
 *   2. writes three bytes ('h','b','\n') to UART0's THR register
 *      (0x01C28000, UART0_BASE -- see stage2.h/vconsole.c). Zephyr's
 *      stage-2 table (stage2_zephyr.c) maps ZERO MMIO for CPU3 -- by
 *      design, "Zephyr gets zero real MMIO passthrough" -- so this write
 *      ALWAYS takes a stage-2 data abort into EL2, on every iteration.
 *      el2_exc_dual2_qemu.c's el2_trap() routes that fault to
 *      vconsole_handle_fault(frame, 1) (vconsole.c's channel-1 emulated
 *      16550, the exact same device model + code path real Zephyr's own
 *      console output already goes through), which captures the byte into
 *      channel 1's ring and bumps its own total_bytes counter -- a quantity
 *      the HYPERVISOR itself observed via a trap, not a DRAM word this core
 *      merely hopes another core's read will see.
 *
 * WHY BOTH, AND WHY #2 IS THE ONE THAT MATTERS ON HARDWARE:
 * See docs/dual-guest.md, "Retrying a failed zboot ... and a probe that
 * lied", and open item 5 under "What's still open". #1 (the DRAM counter,
 * read directly by el2_exc_dual2_qemu.c's tick handler from CPU1/CPU0 at
 * EL2) is UNSOUND on real hardware: this payload runs with its stage-1 MMU
 * disabled, so its own stores are Device-typed, and a Device-typed write
 * does not invalidate another core's cached copy of that line -- the
 * reader can keep returning a stale value indefinitely. QEMU models no
 * caches, so #1 is perfectly reliable there, which is exactly why it lied
 * about a real, working retry path on 2026-08-10 (word never observed to
 * move) while real Zephyr, using the SAME kind of trap-and-emulate console
 * path as #2, plainly worked. #2 has no such hole: EVERY increment of
 * channel 1's total_bytes happened because THIS core's own stage-2 fault
 * was taken and serviced by EL2 -- there is no "other core's cache" in the
 * loop at all, so it cannot go stale the way #1 can. The CI scripts'
 * PASS/FAIL arithmetic is keyed off #2; #1 remains printed for information
 * only.
 *
 * COUNTER ADDRESS (signal #1): ZSTAGE2_DRAM_BASE (stage2_zephyr.h) + 0x100
 * == 0xBE000100. Comfortably inside Zephyr's 32 MiB stage-2 slice
 * [0xBE000000,0xC0000000) and comfortably clear of this file's own handful
 * of instructions at 0xBE000000 (see zg3_trivial_payload.ld). Read directly
 * by el2_exc_dual2_qemu.c's tick handler -- EL2's own accesses are never
 * subject to CPU3's stage-2 regime at all (stage-2 only ever governs
 * EL1/EL0 accesses; see stage2_zephyr.h's per-PE-banking rationale), so a
 * plain volatile EL2 read of this physical address works regardless of
 * what CPU3's own VTCR_EL2/VTTBR_EL2 say. Kept in sync BY VALUE with that
 * file's ZG3_TEST_COUNTER_PA constant; if this address ever changes,
 * update both.
 *
 * UART0 ADDRESS (signal #2): 0x01C28000 == UART0_BASE (stage2.h), the SAME
 * physical/IPA address FreeBSD's channel-0 console and real Zephyr's
 * channel-1 console both target (see vconsole.c's header comment: "the two
 * channels never collide because they run on different cores against
 * different, disjoint stage-2 tables"). Only the THR byte offset (+0x00)
 * is written -- any other offset in the page would be silently accepted
 * and ignored by vconsole.c's emulation (IER/LCR/MCR init-time writes) and
 * would NOT count as a captured byte, so this deliberately always targets
 * THR.
 *
 * LINKED AT (see zg3_trivial_payload.ld): p_vaddr == pa_base == 0xBE000000
 * (ZG3_PA_BASE, zguest_cpu3.c), so zload2_parse_and_place()'s general
 * relocation formula (dest = pa_base + (p_vaddr - zbase)) degenerates to a
 * plain identity copy -- see zload2.h's header comment for why that is the
 * expected common case.
 *
 * Runs with EL1 stage-1 MMU off (guest_config(), called by zguest_cpu3.c
 * before kload_enter()) and CPU3's stage-2 identity-mapping the DRAM
 * counter's range -- so VA == IPA == PA throughout for signal #1, exactly
 * the same flat-physical execution model guest.c's own guest_demo_el1()
 * already relies on (for CPU0, with stage-2 disabled entirely rather than
 * identity-mapped, but the flat-addressing effect on a spinning payload is
 * the same either way). UART0 (signal #2) is, by contrast, DELIBERATELY
 * left unmapped by stage2_zephyr.c, which is what makes every write to it
 * fault -- see above.
 */
	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x0, #0x0100
	movk	x0, #0xbe00, lsl #16	/* x0 = 0xBE000100 (DRAM counter, signal #1, QEMU-only) */
	movz	x2, #0x8000
	movk	x2, #0x01c2, lsl #16	/* x2 = 0x01C28000 (UART0 THR, signal #2, fault-observed) */
1:
	ldr	w1, [x0]
	add	w1, w1, #1
	str	w1, [x0]
	movz	w3, #0x68		/* 'h' */
	strb	w3, [x2]		/* stage-2 fault -> el2_exc_dual2_qemu.c -> vconsole channel 1 */
	movz	w3, #0x62		/* 'b' */
	strb	w3, [x2]
	movz	w3, #0x0a		/* '\n' */
	strb	w3, [x2]
	b	1b
	.size _start, . - _start
