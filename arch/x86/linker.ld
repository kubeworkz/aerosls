/* Specify the entry point of the executable (defined in boot.asm) */
ENTRY(_start)

SECTIONS
{
    /* Begin putting sections at 1 MiB physical address */
    . = 1M;

    /* The text section contains the executable code */
    .text :
    {
        /* Force the Multiboot2 header to be at the very front of the binary */
        KEEP(*(.multiboot2))
        *(.text)
    }

    /* Read-only data section (constants, string literals) */
    .rodata :
    {
        *(.rodata)
    }

    /* Read-write initialized data section */
    .data :
    {
        *(.data)
    }

    /* Read-write uninitialized data section and stack */
    .bss :
    {
        *(.COMMON)
        *(.bss)
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
     * of .bss. See frame_pool_init(). */
    . = ALIGN(4096);
    PROVIDE(_kernel_image_end = .);
}