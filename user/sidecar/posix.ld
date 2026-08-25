/* user/sidecar/posix.ld — link script for the POSIX sidecar flat image.
 *
 * Same layout as init.ld: base 0, ENTRY(_start), crt0 at first byte.
 * The kernel maps the flat image and enters at image.entry (offset 0).
 */

ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        KEEP(*(.text._start))   /* crt0: entry point at offset 0 */
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
