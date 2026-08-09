/* SPDX-License-Identifier: BSD-2-Clause */

/* zg3_trivial_payload.ld — minimal linker script for zg3_trivial_payload.S
 * (dual-guest Step 1 QEMU proof). Places the one .text section at
 * ZG3_PA_BASE (0xBE000000, zguest_cpu3.c) so zload2_parse_and_place()'s
 * placement is a plain identity copy (see zload2.h's header comment) and so
 * the resulting ELF's entry point / this program's counter address exactly
 * match the constants zguest_cpu3.c and el2_exc_dual2_qemu.c already use.
 *
 * NOT part of the Makefile -- built standalone by dual-qemu-ci.sh with the
 * same cross-compiler ($(CROSS)gcc, i.e. aarch64-linux-gnu-gcc) this whole
 * tree uses, exactly the way zephyr-guest/build.sh produces the real Zephyr
 * ELF out of tree.
 */
ENTRY(_start)
SECTIONS
{
	. = 0xBE000000;
	.text : { *(.text) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.attributes) }
}
