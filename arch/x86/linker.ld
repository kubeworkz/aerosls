/* Specify the entry point of the executable (defined in boot.asm) */
ENTRY(_start)

SECTIONS
{
    /* Begin putting sections at 1 MiB physical address */
    . = 1M;

    /* Every wildcard below names both the bare section and its dotted
     * variants. That is not decoration. A section this script does not name
     * becomes an ORPHAN, and ld places orphans by attribute after the output
     * section they most resemble -- which, for anything resembling .bss, is
     * after the point where _kernel_image_end is computed at the bottom of
     * this file. An orphan there is memory the kernel occupies and the frame
     * allocator believes is free. See the .bootstrap_stack block below for the
     * time that actually happened. */
    .text :
    {
        /* Force the Multiboot2 header to be at the very front of the binary */
        KEEP(*(.multiboot2))
        *(.text .text.*)
    }

    /* Read-only data section (constants, string literals).
     * GCC emits string literals into .rodata.str1.1 / .rodata.str1.8 rather
     * than .rodata, so the bare name on its own matched almost nothing. */
    .rodata :
    {
        *(.rodata .rodata.*)
        *(.note.gnu.property)
        *(.eh_frame)
    }

    /* Read-write initialized data section */
    .data :
    {
        *(.data .data.*)
    }

    /* Read-write uninitialized data section.
     * COMMON, not .COMMON -- the leading dot made this match a section name
     * that does not exist. Harmless in practice only because -fno-common has
     * been GCC's default since GCC 10. */
    .bss :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* ─── The bootstrap stack, in an output section of its own ─────────────
     * This used to be one more wildcard inside .bss above, and on the real
     * build it did not match. NASM's `section .bootstrap_stack, nobits` and
     * ld's `*(.bootstrap_stack)` nested inside .bss did not meet; the section
     * became an orphan and ld placed it immediately after .bss -- which is
     * AFTER the `. = ALIGN(4096); PROVIDE(_kernel_image_end = .)` below,
     * because statements between output sections are evaluated at their
     * position in this script while orphans are inserted into the section list
     * afterwards.
     *
     * Measured on a live node:
     *
     *     bootstrap stack   [0x77e8000, 0x77f8000)
     *     _kernel_image_end  0x77e8000
     *
     * _kernel_image_end landed exactly ON stack_bottom. All 16 frames of the
     * kernel's own 64 KiB stack sat above the reservation and were free for
     * alloc_raw_frame() to hand out. It did; a stream upload memset one of them
     * and filled it with payload; the live stack became 0xCD to its last byte;
     * the next `ret` loaded a non-canonical address and raised #GP(0). The
     * whole of §9m-§9r in the roadmap is that one line of placement.
     *
     * Declaring it as its own OUTPUT section fixes it two independent ways.
     * The wildcard is now at the top level where no enclosing section's
     * attributes can shadow it -- and, more robustly, ld places an orphan into
     * an existing output section of the SAME NAME automatically. So even if the
     * input section's flags or spelling differ from what is written here, it
     * lands in this block: above .bss, below the image end. Being wrong about
     * NASM's exact output no longer costs the kernel its stack.
     *
     * NOLOAD because it is nobits -- it reserves address space and contributes
     * no bytes to the image on disk. */
    .bootstrap_stack (NOLOAD) : ALIGN(4096)
    {
        *(.bootstrap_stack)
    }

    /* End of the loaded kernel image, in PHYSICAL address terms (paging is
     * an identity map for 0-4 GiB, so physical == virtual here).
     *
     * This exists because kernel/frame_pool.c needs to know where the
     * kernel stops before it can avoid handing the kernel's own memory out
     * as free pages. Before this symbol existed there was no way for it to
     * ask, and it did not: alloc_raw_frame() started at physical frame 1
     * and walked upward straight through .text, .rodata, .data and 117 MiB
     * of .bss. See frame_pool_init().
     *
     * frame_pool_init() no longer trusts this symbol on its own. It reserves
     * below it, then reserves [stack_bottom, stack_top) again by name and
     * reports loudly if that adds anything -- which is how the orphan above
     * was finally caught, after the fault it caused had been chased through
     * four separate wrong hypotheses. tests/kernel_image_end_check.sh asserts
     * the same invariant against the linked image at build time, so the next
     * orphan is a failing test instead of a corrupted stack. */
    . = ALIGN(4096);
    PROVIDE(_kernel_image_end = .);
}
