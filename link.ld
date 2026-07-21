/* link.ld — bzdOS microkernel linker script.
 * Loaded/executed by U-Boot at physical address 0x42000000 as a raw binary
 * (no relocations, position fixed). start.o must be first so entry() lands
 * exactly at 0x42000000. See PROJECT.md for the memory map / ABI contract. */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x42000000, LENGTH = 8M
}

SECTIONS
{
    . = 0x42000000;

    .text : {
        _text_start = .;
        KEEP(*(.text.start))       /* start.o's entry, forced first */
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
