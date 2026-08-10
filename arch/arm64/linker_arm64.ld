/* arch/arm64/linker_arm64.ld — M4a/M4c/M5: the arm64 kernel's link
 * layout (plan doc §10.187, §10.191).
 *
 * qemu -M virt DRAM begins at 0x40000000; 0x40080000 is 2 MiB in — the
 * canonical Linux arm64 `-kernel` load address. The ELF's PT_LOAD
 * segments are loaded by qemu at their PHYSICAL addresses (p_paddr),
 * which is why the kernel proper is linked at a HIGH VIRTUAL base
 * (0xFFFF000040080000, the TTBR1 half) with each section's LMA set to
 * its physical address (VMA - __virt_off). The kernel then runs in the
 * TTBR1 half under the MMU (§10.191); TTBR0 is the user half.
 *
 * The exception: a tiny physical entry stub in its own page at
 * VMA=LMA=0x4007F000 (just below the kernel image, still DRAM), so the
 * ELF's e_entry is a PHYSICAL address — qemu sets the initial PC to
 * e_entry with the MMU off, so it must be where code physically lives,
 * not a high VA. The stub branches to __head_phys, the kernel body's
 * physical base. The body's LMA is EXACTLY 0x40080000, so the C
 * phys<->virt conversions (KERNEL_VIRT_OFF in mmu.h) hold with no
 * offset discontinuity. From then on the head runs at its physical
 * address with every adrp/add reference resolving to physical
 * addresses (the link-time differences are preserved in the loaded
 * image), until the head enables the MMU and jumps to the high VA
 * (§10.191). */
OUTPUT_ARCH("aarch64")
ENTRY(_start)

__virt_off = 0xFFFF000040080000 - 0x40080000;   /* 0xFFFF000000000000 */

SECTIONS
{
    /* The physical entry stub (its own 4 KiB page below the image). */
    . = 0x4007F000;
    .text.entry : { *(.text.entry) }

    /* The kernel proper: high VMA (TTBR1), physical LMA = 0x40080000. */
    . = 0xFFFF000040080000;
    __kernel_virt_base = .;
    __head_phys = ADDR(.text) - __virt_off;

    .text : AT(0x40080000) ALIGN(4K)
    {
        /* KEEP: --gc-sections cannot see the head from the entry stub
         * (its `br x0` to __head_phys is a register branch with no
         * relocation — the first M5 build silently lost the whole .text
         * to collection, §10.191). Everything else is reachable from
         * the head's adrp/bl relocations. */
        KEEP(*(.text.init)) /* the head must be first in the kernel image */
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - __virt_off) ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - __virt_off) ALIGN(4K)
    {
        *(.data .data.*)
    }

    /* The boot stack lives at the top of BSS: __bss_start..__bss_end
     * covers it (so the head's BSS clear pre-zeros it), and _stack_top
     * is inside the BSS segment. Sized 256 KiB, NOT the RV64 kernel's
     * 16 KiB (measured via -fstack-usage: simi_arm_translate alone has
     * an 18,800-byte frame — the M2 chain machinery is far heavier than
     * simi_riscv_translate's — plus the chain helpers' ~1.6 KB frames,
     * so 16 KiB overflowed and the kernel ret-ed to 0x0, §10.189). BSS
     * is hundreds of KiB, so the extra 240 KiB costs nothing. */
    .bss : AT(ADDR(.bss) - __virt_off) ALIGN(4K)
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
