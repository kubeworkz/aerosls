/* user/network/network.ld — link script for the network sidecar flat image.
 *
 * Same structure as init.ld: _start (crt0) at address 0, flat binary.
 * The kernel maps it at USER_PROC_CODE_BASE and enters at image.entry (0).
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
