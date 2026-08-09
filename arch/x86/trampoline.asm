; AeroSLS AP startup trampoline — assembled as a flat binary loaded at 0x08000.
; With ORG 0x8000 NASM assigns correct absolute addresses to all symbols so the
; far jumps (16→32-bit and 32→64-bit) encode the right target addresses.

bits 16
org 0x8000

section .text
global trampoline_start
global trampoline_end

trampoline_start:
    cli                         ; Disable interrupts on this AP
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary 32-bit GDT for the protected-mode transition
    lgdt [ap_gdt_ptr]

    ; Set CR0.PE to enter 32-bit Protected Mode
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far jump to flush the pipeline and reload CS from the 32-bit code segment
    jmp dword 0x08:ap_protected_mode

bits 32
ap_protected_mode:
    mov ax, 0x10                ; Data segment selector (from temporary GDT)
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Reuse the BSP's PML4 — BSP stored it at 0x07000
    mov eax, [0x07000]
    mov cr3, eax

    ; Enable PAE (required for long mode)
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; Set IA32_EFER.LME
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; Enable paging + write-protect
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    ; Load the definitive 64-bit GDT — BSP copied the struct to 0x07010
    lgdt [0x07010]

    ; Far jump into 64-bit long mode using the kernel code segment (0x08)
    jmp 0x08:ap_kernel_entry_bridge

bits 64
ap_kernel_entry_bridge:
    ; Read the C entry point that the BSP stored at 0x07020
    mov rax, [0x07020]
    ; Load the per-AP kernel stack pointer from 0x07030
    mov rsp, [0x07030]
    jmp rax                     ; Enter ap_kernel_main()

; ─── Temporary 32-bit GDT ─────────────────────────────────────────────────────
align 16
ap_gdt:
    dq 0                        ; null descriptor
    dq 0x00CF9A000000FFFF       ; 32-bit protected-mode code  (selector 0x08)
    dq 0x00CF92000000FFFF       ; 32-bit protected-mode data  (selector 0x10)
ap_gdt_ptr:
    dw ap_gdt_ptr - ap_gdt - 1  ; limit
    dd ap_gdt                   ; base (absolute, because of ORG 0x8000)

trampoline_end: