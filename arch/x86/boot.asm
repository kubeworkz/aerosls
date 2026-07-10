section .multiboot2
align 8
multiboot_start:
    dd 0xe85250d6, 0, multiboot_end - multiboot_start, -(0xe85250d6 + 0 + (multiboot_end - multiboot_start))
    dw 0, 0
    dd 8
multiboot_end:
section .bootstrap_stack, nobits
align 16
stack_bottom: resb 4096 * 4
stack_top:
section .text
bits 32
global _start
_start:
    mov esp, stack_top
    lgdt [gdt64.pointer]
    jmp gdt64.code:_start64
bits 64
_start64:
    xor ax, ax
    mov ss, ax
    mov ds, ax
    extern kernel_main
    call kernel_main
.halt: hlt
    jmp .halt
section .rodata
gdt64: dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.pointer: dw $ - gdt64 - 1
    dq gdt64