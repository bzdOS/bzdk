/* SPDX-License-Identifier: BSD-2-Clause */

/* zg3_trivial_payload.S — CPU3's payload for the dual-guest Step 1 QEMU
 * proof (main_dual2_qemu.c / dual-qemu-ci.sh). See dual-qemu-ci.sh's header
 * for the full rationale for staging THIS as a genuine standalone ELF64/
 * AArch64 image and running it through zguest_cpu3.c's REAL, unmodified
 * zephyr_cpu3_run() -> zload2_parse_and_place() code path, rather than a
 * QEMU-only bypass of it.
 *
 * This is deliberately NOT Zephyr and NOT built via the Makefile: it is a
 * standalone, hand-written 4-instruction AArch64 EL1 program, entirely
 * self-contained, built directly by dual-qemu-ci.sh with the SAME
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
 * stage2_zephyr_init()/_enable()/_isolation_selfcheck() have run) and does
 * nothing but increment a 32-bit counter at a fixed physical address,
 * forever. No MMIO, no HVC, no stack use, no .bss/.data -- so it never
 * traps (mmio_absorb.c is not even linked into this QEMU target; see
 * main_dual2_qemu.c's banner for why that is safe here) and needs nothing
 * from the linker beyond a single .text section.
 *
 * COUNTER ADDRESS: ZSTAGE2_DRAM_BASE (stage2_zephyr.h) + 0x100 == 0xBE000100.
 * Comfortably inside Zephyr's 32 MiB stage-2 slice [0xBE000000,0xC0000000)
 * and comfortably clear of this file's own handful of instructions at
 * 0xBE000000 (see zg3_trivial_payload.ld). Read directly by
 * el2_exc_dual2_qemu.c's tick handler -- EL2's own accesses are never
 * subject to CPU3's stage-2 regime at all (stage-2 only ever governs
 * EL1/EL0 accesses; see stage2_zephyr.h's per-PE-banking rationale), so a
 * plain volatile EL2 read of this physical address works regardless of
 * what CPU3's own VTCR_EL2/VTTBR_EL2 say. Kept in sync BY VALUE with that
 * file's ZG3_TEST_COUNTER_PA constant; if this address ever changes,
 * update both.
 *
 * LINKED AT (see zg3_trivial_payload.ld): p_vaddr == pa_base == 0xBE000000
 * (ZG3_PA_BASE, zguest_cpu3.c), so zload2_parse_and_place()'s general
 * relocation formula (dest = pa_base + (p_vaddr - zbase)) degenerates to a
 * plain identity copy -- see zload2.h's header comment for why that is the
 * expected common case.
 *
 * Runs with EL1 stage-1 MMU off (guest_config(), called by zguest_cpu3.c
 * before kload_enter()) and CPU3's stage-2 identity-mapping this exact
 * range -- so VA == IPA == PA throughout, exactly the same flat-physical
 * execution model guest.c's own guest_demo_el1() already relies on (for
 * CPU0, with stage-2 disabled entirely rather than identity-mapped, but the
 * flat-addressing effect on a spinning payload is the same either way).
 */
	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x0, #0x0100
	movk	x0, #0xbe00, lsl #16	/* x0 = 0xBE000100 (counter address) */
1:
	ldr	w1, [x0]
	add	w1, w1, #1
	str	w1, [x0]
	b	1b
	.size _start, . - _start
