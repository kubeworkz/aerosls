/* user/init/init.ld — link script for the init sidecar flat image.
 *
 * The kernel maps the flat image at USER_PROC_CODE_BASE and enters it at
 * `image.entry` (offset 0, per the manifest's IMAGE record), so `_start`
 * must be the FIRST byte of the image: the script pins the base at 0 and
 * places `.text._start` (the crt0, compiled from crt0.S via global_asm!)
 * before every other section.
 *
 * `boot_stack` (16 KiB, .bss) is the sidecar's OWN boot stack — the
 * kernel-provided user stack's top page carries the BIB and is abandoned
 * the moment crt0 switches stacks. It lands in the flat image as zeros
 * (objcopy -O binary includes allocated .bss up to the highest address),
 * so the manifest's image size covers it automatically.
 *
 * Passed to rustc as -Tinit.ld via user/.cargo/config.toml for the
 * x86_64-unknown-none target (cargo runs rustc from the package root, so
 * the relative path resolves to this file).
 */

ENTRY(_start)

SECTIONS
{
    /* Flat image: base 0, no ELF headers in the output. */
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
