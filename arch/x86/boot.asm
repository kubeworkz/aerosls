; ============================================================
;  AeroSLS x86-64 Bootstrap
;  GRUB multiboot2 hands control in 32-bit protected mode.
;  We enable PAE, set up a minimal identity-map (first 1 GiB
;  via 2 MiB huge pages), set IA32_EFER.LME, enable paging,
;  load the 64-bit GDT, far-jump into long mode, then call
;  kernel_main.
; ============================================================

; ─── Multiboot2 header ───────────────────────────────────────────────────────
section .multiboot2
align 8
multiboot_start:
    dd 0xe85250d6                           ; magic
    dd 0                                    ; architecture: i386
    dd multiboot_end - multiboot_start      ; header length
    dd -(0xe85250d6 + 0 + (multiboot_end - multiboot_start))  ; checksum
    dw 0                                    ; end tag type
    dw 0                                    ; end tag flags
    dd 8                                    ; end tag size
multiboot_end:

; ─── Bootstrap stack (64 KiB) ─────────────────────────────────────────────────
section .bootstrap_stack, nobits
align 16
stack_bottom: resb 4096 * 16
stack_top:

; ─── Early page tables (BSS — zeroed by GRUB) ────────────────────────────────
section .bss
align 4096
global p4_table
p4_table:    resb 4096   ; PML4
global p3_table
p3_table:    resb 4096   ; PDPT  (4 entries cover 4 GiB)
global p2_table_0
p2_table_0:  resb 4096   ; PD for  0 –  1 GiB (512 × 2 MiB huge pages)
global p2_table_1
p2_table_1:  resb 4096   ; PD for  1 –  2 GiB
global p2_table_2
p2_table_2:  resb 4096   ; PD for  2 –  3 GiB
global p2_table_3
p2_table_3:  resb 4096   ; PD for  3 –  4 GiB  (LAPIC @ 0xFEE00000, PCIe BARs)

; Saved multiboot2 handoff registers (written in 32-bit mode, read in 64-bit)
global mb2_magic_saved
mb2_magic_saved:  resd 1   ; eax at _start (should be 0x36d76289)
global mb2_info_saved
mb2_info_saved:   resd 1   ; ebx at _start (physical addr of mb2_info struct)

; ─── 32-bit entry point ───────────────────────────────────────────────────────
section .text
bits 32
global _start

_start:
    ; ── 0. Save multiboot2 handoff values (eax=magic, ebx=info ptr) ─────────
    ;  Do this FIRST before eax is overwritten by the page-table setup below.
    mov  [mb2_magic_saved], eax
    mov  [mb2_info_saved],  ebx

    mov  esp, stack_top

    ; ── 1. Build identity-map page tables (0–4 GiB) ────────────────────────
    ; P4[0] → p3_table  (present + writable)
    mov  eax, p3_table
    or   eax, 0b11
    mov  [p4_table], eax

    ; P3[0..3] → p2_table_0..3  (4 × 1 GiB slices)
    mov  eax, p2_table_0
    or   eax, 0b11
    mov  [p3_table + 0], eax

    mov  eax, p2_table_1
    or   eax, 0b11
    mov  [p3_table + 8], eax

    mov  eax, p2_table_2
    or   eax, 0b11
    mov  [p3_table + 16], eax

    mov  eax, p2_table_3
    or   eax, 0b11
    mov  [p3_table + 24], eax

    ; Fill each PD table: 512 × 2 MiB huge pages per GiB
    ; ecx iterates 0..511 across all four tables = 2048 entries total
    xor  ecx, ecx
.fill_p2:
    mov  eax, 0x200000          ; 2 MiB
    mul  ecx                    ; eax = entry_index × 2 MiB (physical base)
    or   eax, 0b10000011        ; present + writable + huge (PS)
    ; Destination: p2_table_0 + ecx*8 (table changes every 512 entries)
    cmp  ecx, 512
    jl   .store_e0
    cmp  ecx, 1024
    jl   .store_e1
    cmp  ecx, 1536
    jl   .store_e2
    ; 1536..2047 → p2_table_3
    mov  edx, ecx
    sub  edx, 1536
    mov  [p2_table_3 + edx * 8], eax
    jmp  .next_e
.store_e0:
    mov  [p2_table_0 + ecx * 8], eax
    jmp  .next_e
.store_e1:
    mov  edx, ecx
    sub  edx, 512
    mov  [p2_table_1 + edx * 8], eax
    jmp  .next_e
.store_e2:
    mov  edx, ecx
    sub  edx, 1024
    mov  [p2_table_2 + edx * 8], eax
.next_e:
    inc  ecx
    cmp  ecx, 2048
    jne  .fill_p2

    ; ── 2. Load PML4 address into CR3 ──────────────────────────────────────
    mov  eax, p4_table
    mov  cr3, eax

    ; ── 3. Enable PAE (bit 5 of CR4) ───────────────────────────────────────
    mov  eax, cr4
    or   eax, 1 << 5
    mov  cr4, eax

    ; ── 4. Set IA32_EFER.LME (bit 8) to enable long mode ──────────────────
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 1 << 8
    wrmsr

    ; ── 5. Enable paging + ensure protected mode (CR0: PG + PE) ───────────
    mov  eax, cr0
    or   eax, (1 << 31) | (1 << 0)
    mov  cr0, eax

    ; ── 6. Load the 64-bit GDT and far-jump into long mode ─────────────────
    lgdt [gdt64.pointer]
    jmp  gdt64.code:_start64

; ─── 64-bit kernel entry ──────────────────────────────────────────────────────
bits 64
_start64:
    ; Clear segment registers (not used in long mode flat model)
    xor  ax, ax
    mov  ss, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    ; Pass multiboot2 info to kernel_main (System V AMD64 calling convention):
    ;   rdi = first arg  = mb2_magic (uint32_t)
    ;   rsi = second arg = mb2_info_phys (uint32_t physical address)
    ; Both are identity-mapped (0–4 GiB), so the physical addr = virtual addr.
    mov  edi, dword [mb2_magic_saved]
    mov  esi, dword [mb2_info_saved]

    extern kernel_main
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

; ─── Minimal 64-bit GDT (null + code descriptor) ─────────────────────────────
section .rodata
gdt64:
    dq 0                                               ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)          ; 64-bit code, ring-0
.pointer:
    dw $ - gdt64 - 1
    dq gdt64