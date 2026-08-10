/* tools/simi/tests/rv_trap_entry/linker_fixture_s.ld — Phase 9h (ISA doc
 * §16): linker script for the S-mode twin of the trap-entry fixture.
 * Linked at 0x80200000, the address OpenSBI loads the payload at under
 * QEMU -M virt — the same link address the S-mode kernel uses
 * (arch/riscv/linker_riscv.ld). Identical layout to linker_fixture.ld
 * (the M-mode twin); only the base differs. Much smaller than the
 * kernel's: the fixture is _start + the trap-entry test driver + the
 * entry assembly + an 8KB stack. */
OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    /* _start must be the first thing — OpenSBI jumps to the payload's
     * entry point. */
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
