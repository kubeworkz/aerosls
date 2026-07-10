bits 16
section .text
global trampoline_start
global trampoline_end

trampoline_start:
    cli                         ; Disable interrupts on this AP
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary 32-bit GDT pointer to switch out of real mode
    ; Address must be absolute, calculated based on the 0x08000 placement
    lgdt [0x08000 + (ap_gdt_ptr - trampoline_start)]

    ; Set CR0 Protection Enable (PE) bit to 1
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit Protected Mode code segment (0x08)
    jmp dword 0x08:(0x08000 + (ap_protected_mode - trampoline_start))

bits 32
ap_protected_mode:
    mov ax, 0x10                ; 0x10 points to standard Data Segment
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Set up 4-level Long Mode Paging before leaping to 64-bit
    ; APs can safely reuse the BSPs CR3 top-level PML4 directory
    mov eax, [0x07000]          ; Kernel stores active CR3 value at physical 0x07000
    mov cr3, eax

    ; Enable PAE (Physical Address Extension) in CR4
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable Long Mode inside the EFER MSR register (Bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable Paging (Bit 31) and Write Protect (Bit 16) inside CR0
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    ; Load the definitive 64-bit GDT mapped during our GDT subsystem stage
    lgdt [0x07010]              ; Kernel stores the 64-bit GDTPointer struct at physical 0x07010

    ; Far jump directly into 64-bit Long Mode Code segment (0x08)
    jmp 0x08:ap_kernel_entry_bridge

bits 64
ap_kernel_entry_bridge:
    ; Spinlock-protected thread handshake pointer set up by BSP
    mov rax, [0x07020]          ; Kernel stores target 64-bit C entry address here
    
    ; Load an individual stack pointer assigned to this specific core
    mov rsp, [0x07030]          ; Kernel stores unique stack allocation block here
    
    jmp rax                     ; Jump out of trampoline and enter the C Kernel

align 16
ap_gdt:
    dq 0                        ; Null entry
    dq 0x00CF9A000000FFFF       ; 32-bit Protected Mode Code Segment
    dq 0x00CF92000000FFFF       ; 32-bit Protected Mode Data Segment
ap_gdt_ptr:
    dw $ - ap_gdt - 1
    dd 0x08000 + (ap_gdt - trampoline_start)

trampoline_end: