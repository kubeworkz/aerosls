/* Minimal linker script for AeroSLS user-space programs (flat binary output).
 *
 * Code is position-independent (compiled with -fPIC) so the kernel can
 * load it at any virtual address assigned by sys_sls_valloc().
 *
 * The binary starts at offset 0; _start is the entry point.
 */
ENTRY(_start)

SECTIONS {
    . = 0;

    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Discard DWARF, exception tables, and GNU notes to keep binaries small */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
        *(.gnu*)
    }
}
