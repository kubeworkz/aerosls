/* arch/arm64/linker_arm64.ld — M4a: the minimal arm64 kernel's link
 * layout (plan doc §10.187). Mirrors arch/riscv/linker_riscv.ld's shape.
 *
 * qemu -M virt DRAM begins at 0x40000000; 0x40080000 is 2 MiB in — the
 * canonical Linux arm64 `-kernel` load address for the machine. The ELF
 * is loaded there by qemu-system-aarch64 -kernel (per its PT_LOAD
 * segments), and _start is the reset entry. */
OUTPUT_ARCH("aarch64")
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : ALIGN(4K)
    {
        *(.text.init) /* _start must be loaded first */
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

    /* The 16 KiB boot stack lives at the top of BSS: __bss_start..__bss_end
     * covers it (so the entry's BSS clear pre-zeros it), and _stack_top is
     * inside the BSS PT_LOAD segment, so the ELF loader maps it. */
    .bss : ALIGN(4K)
    {
        PROVIDE(__bss_start = .);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += 16384;
        PROVIDE(__bss_end = .);
        PROVIDE(_stack_top = .);
    }

    /* End of the static kernel memory layout footprint. */
    _end = .;
}
