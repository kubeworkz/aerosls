/* Specify the entry point of the executable (defined in boot.S for RISC-V) */
OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
    /* Begin putting sections 2 MiB above the OpenSBI / QEMU RAM base boundary
     * (2 GiB): with `-bios default` OpenSBI itself occupies 0x80000000, so
     * 0x80200000 is the canonical kernel load address under OpenSBI on
     * qemu-system-riscv64 -M virt (the same layout Linux uses). */
    . = 0x80200000;

    .text : ALIGN(4K)
    {
        *(.text.init) /* Early bootstrap instructions must be loaded first */
        *(.text .text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4K)
    {
        PROVIDE(__bss_start = .);
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE(__bss_end = .);
    }

    /* Provide a symbol marking the end of the static kernel memory layout footprint */
    _end = .;
}