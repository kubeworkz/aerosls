/* Specify the entry point of the executable (defined in boot.asm) */
ENTRY(_start)

PHDRS
{
    load_ph    PT_LOAD FLAGS(7); /* RWE: code + rodata + data */
    bss_ph     PT_LOAD FLAGS(6); /* RW:  BSS + bootstrap_stack */
}

SECTIONS
{
    /* Begin putting sections at 1 MiB physical address */
    . = 1M;

    .text : { KEEP(*(.multiboot2)) *(.text .text.*) } :load_ph
    .rodata : { *(.rodata .rodata.*) *(.note.gnu.property) *(.eh_frame) } :load_ph
    .data : { *(.data .data.*) } :load_ph
    .bss : { *(COMMON) *(.bss .bss.*) } :bss_ph
    .bootstrap_stack (NOLOAD) : ALIGN(4096) { *(.bootstrap_stack) }

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
