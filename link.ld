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

    /* The stage-2 "A1" boundary protects exactly ONE 2 MiB L2 block at
     * 0x42000000 (stage2.c HVIMG_L2_IDX) — anything the image places above
     * 0x42200000 stays identity-mapped RW to the guest, silently eroding the
     * boundary. RAM is 8 MiB but only the first 2 MiB is protected, so fail
     * the build if text+rodata+data+bss ever outgrows the protected window. */
    ASSERT(__bss_end - 0x42000000 <= 0x200000,
           "HV image exceeds the 2 MiB stage-2 protected window at 0x42000000 -- extend HVIMG protection in stage2.c or shrink the image")
}
