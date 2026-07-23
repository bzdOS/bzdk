/* link_qemu.ld — linker script for the QEMU `virt`-machine CI target
 * (see start_qemu.S / main_qemu.c / docs/qemu-ci.md).
 *
 * Unlike link.ld (which links at 0x42000000 to match U-Boot's `go
 * 0x42000000` on the real Allwinner A64 board), this target is loaded by
 * `qemu-system-aarch64 -kernel <elf>` on the `virt` machine, which honors
 * an ELF's own load addresses/entry point directly (arm_load_kernel() tries
 * load_elf() first) — so the base address here just needs to sit inside
 * `virt`'s RAM region (RAM starts at 0x40000000, same as the A64 — the one
 * genuinely convenient overlap between the two memory maps). 0x40080000 is
 * the conventional QEMU/Linux-arm64 "kernel" load offset (512 KiB into
 * RAM), chosen mainly so this image is trivially distinguishable in a
 * memory dump from anything QEMU itself places at the very base of RAM.
 *
 * This is a NEW, separate script — link.ld (the real-board target) is not
 * touched by this file or by anything that uses it.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40080000, LENGTH = 32M
}

SECTIONS
{
    . = 0x40080000;

    .text : {
        _text_start = .;
        KEEP(*(.text.start))       /* start_qemu.o's entry, forced first */
        *(.text)
        *(.text.*)
        _text_end = .;
    } > RAM

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } > RAM

    .data : {
        *(.data)
        *(.data.*)
    } > RAM

    . = ALIGN(8);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } > RAM
    . = ALIGN(8);
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.ARM.attributes)
    }
}
