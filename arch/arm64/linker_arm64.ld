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

    /* The boot stack lives at the top of BSS: __bss_start..__bss_end
     * covers it (so the entry's BSS clear pre-zeros it), and _stack_top is
     * inside the BSS PT_LOAD segment, so the ELF loader maps it.
     *
     * Sized 256 KiB, NOT the RV64 kernel's 16 KiB (measured via
     * -fstack-usage: simi_arm_translate alone has an 18,800-byte frame —
     * the M2 chain machinery (chain walk, def pool, chain emission) is
     * far heavier than simi_riscv_translate's — plus the called chain
     * helpers' frames (chain_flatten_rec etc. are ~1.6 KB each), so
     * 16 KiB overflowed: the stack clobbered the return address, the
     * kernel `ret`-ed to 0x0, and QEMU looped on the undefined
     * instruction at the empty vector table (§10.189). BSS is hundreds
     * of KiB, so the extra 240 KiB costs nothing in the image. */
    .bss : ALIGN(4K)
    {
        PROVIDE(__bss_start = .);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += 262144;
        PROVIDE(__bss_end = .);
        PROVIDE(_stack_top = .);
    }

    /* End of the static kernel memory layout footprint. */
    _end = .;
}
