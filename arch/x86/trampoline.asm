bits 16
    section .text
    global trampoline_start
    global trampoline_end
    trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    lgdt [0x08000 + (ap_gdt_ptr - trampoline_start)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(0x08000 + (ap_protected_mode - trampoline_start))
    bits 32
    ap_protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov eax, [0x07000]
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax
    lgdt [0x07010]
    jmp 0x08:ap_kernel_entry_bridge
    bits 64
    ap_kernel_entry_bridge:
    mov rax, [0x07020]
    mov rsp, [0x07030]
    jmp rax
    align 16
    ap_gdt: dq 0, 0x00CF9A000000FFFF, 0x00CF92000000FFFF
    ap_gdt_ptr: dw $ - ap_gdt - 1
    dd 0x08000 + (ap_gdt - trampoline_start)
    trampoline_end: