/* tools/simi/tests/rv_trap_entry/linker_fixture.ld — Phase 9h (ISA doc
 * §16): linker script for the M-mode (bare-metal) trap-entry fixture.
 * Linked at the QEMU virt DRAM base 0x80000000 because a direct M-mode
 * payload (QEMU `-bios none -kernel`) is entered by the reset vector at
 * exactly that address — the same layout the M-mode kernel uses
 * (arch/riscv/linker_riscv_m.ld). The S-mode twin lives in
 * linker_fixture_s.ld (payload at 0x80200000, where OpenSBI loads it).
 * Much smaller than the kernel's: the fixture is _start + the trap-entry
 * test driver + the entry assembly + an 8KB stack. */
OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    /* _start must be the first thing — the reset vector jumps here. */
    .text.init : ALIGN(4)
    {
        *(.text.init)
    }

    .text : ALIGN(4)
    {
        *(.text .text.*)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(16)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(16);
        _stack_bottom = .;
        . += 8192;          /* the fixture's 8KB stack */
        _stack_top = .;
    }

    _end = .;
}
