OUTPUT_ARCH( "riscv" )
    ENTRY(_start)
    SECTIONS {
    . = 0x80000000;
    .text : ALIGN(4K) { *(.text.init) (.text .text.) }
    .rodata : ALIGN(4K) { (.rodata .rodata.) }
    .data : ALIGN(4K) { (.data .data.) }
    .bss : ALIGN(4K) { __bss_start = .; (.sbss .sbss.) (.bss .bss.) *(COMMON) __bss_end = .; }
    _end = .;
    }