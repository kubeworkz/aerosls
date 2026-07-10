section .multiboot2
align 8
multiboot_start:
    dd 0xe85250d6                 ; Magic number for Multiboot2
    dd 0                          ; Architecture 0 (protected mode i386)
    dd multiboot_end - multiboot_start ; Header length
    dd -(0xe85250d6 + 0 + (multiboot_end - multiboot_start)) ; Checksum

    ; Required end tag
    dw 0
    dw 0
    dd 8
multiboot_end:

section .bootstrap_stack, nobits
align 16
stack_bottom:
    resb 4096 * 4                 ; 16 KiB stack space
stack_top:

section .text
bits 32
global _start
_start:
    mov esp, stack_top            ; Set up stack pointer

    ; 1. Check if CPU supports Long Mode via CPUID
    ; 2. Set up identity paging for the first 2 megabytes
    ; 3. Enable PAE (Physical Address Extension)
    ; 4. Switch to Long Mode by setting the EFER MSR register
    ; 5. Enable Paging

    ; Load a 64-bit Global Descriptor Table (GDT)
    lgdt [gdt64.pointer]

    ; Far jump to reload code segment and enter 64-bit Long Mode
    jmp gdt64.code:_start64

bits 64
_start64:
    ; Nullify data segment registers for 64-bit mode
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    
    ; Call your C/C++ SLS Kernel Main function
    extern kernel_main
    call kernel_main

    cli
.halt:
    hlt
    jmp .halt

section .rodata
gdt64:
    dq 0 ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; kernel code segment descriptor
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
