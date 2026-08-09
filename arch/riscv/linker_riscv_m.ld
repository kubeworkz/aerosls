/* Specify the entry point of the executable (defined in boot.S for RISC-V) */
OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
    /* Phase 9g: M-mode variant of linker_riscv.ld. Linked at the QEMU
     * virt DRAM base 0x80000000 because a direct M-mode payload (QEMU
     * `-bios none -kernel`) is entered by the reset vector at exactly
     * that address -- there is no OpenSBI occupying it in this boot
     * mode. The S-mode variant (linker_riscv.ld) stays at 0x80200000,
     * the canonical address under OpenSBI. The two variants share every
     * object; only the link address and -DRISCV_MMODE differ (see the
     * Makefile's riscv-elf target). */
    . = 0x80000000;

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
