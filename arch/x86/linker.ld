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
}