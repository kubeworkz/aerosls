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
; Exported so the fault handler can say how deep the stack was and whether the
; addresses it is dumping are even inside it. A stack dump whose bounds are
; unknown cannot distinguish "three frames down" from "about to run off the
; bottom", and those want completely different investigations.
global stack_bottom
global stack_top

; ─── Size: 1 MiB, raised from 64 KiB ──────────────────────────────────────────
; sls_shell_execute() compiled to a 276,032-byte stack frame -- more than four
; times the old 64 KiB stack. EVERY shell command, local or over HTTP, therefore
; ran with RSP roughly 208 KiB BELOW stack_bottom, writing into whatever .bss
; followed. Three separate faults were captured at 212,768 / 212,864 / 212,880
; bytes past the bottom: the offset is constant because it is one fixed frame,
; not recursion.
;
; It went unnoticed for months because the memory being trampled -- the top of
; sls_heap, the QEMU-SLS bump arena -- was unused until TCG was linked in.
; Overwriting memory nobody reads is invisible. The moment `qemu bench` made
; TCG allocate from that arena, the shell's stack frame and tcg_init_ctx
; occupied the same bytes, and the node halted on the first TCG initialisation.
;
; 1 MiB gave ~3.8x headroom over the 276 KB frame that forced the raise. That
; frame is since gone -- one `static` in user/shell.c took it to 10,224 bytes --
; so the headroom is now far wider, and this file deliberately does not restate
; the multiple, because a hardcoded ratio here is exactly the thing that goes
; stale and misleads. tests/stack_frame_budget_check.sh computes it from the
; linked binary on every run; ask it, not this comment.
;
; The size is NOT reduced back in step with the frame. 1 MiB of nobits .bss on
; a 1 GiB node costs nothing measurable, and the whole point of the check is
; that frames are held against the stack that exists rather than the stack
; being trimmed to whatever the frames happen to need this week.
;
; It is not a licence for larger frames either: stack_frame_budget_check.sh
; asserts the two numbers stay in a fixed relationship, and X86_CFLAGS carries
; -Wframe-larger-than so a new offender is visible at compile time rather than
; as a fault address.
;
; The cost is 1 MiB of nobits .bss on a node with 1 GiB. The alternative
; considered at the time -- auditing ~1400 lines of shell branches to shrink a
; frame believed to be built from hundreds of small locals GCC was declining to
; overlap -- looked like a far larger change with far more room to introduce a
; subtler bug, so the stack was raised instead.
;
; That reading of the frame was wrong, and it is worth recording why, because
; the audit kept getting re-proposed on the strength of it. The frame being
; identical at -O0, -O1, -O2 and -Os was taken as proof the optimiser was
; refusing to overlap the locals. It meant the opposite: GCC overlaps
; branch-local structs at every level, so the size was invariant because it was
; ONE object, not hundreds. sizeof(SLSVecJoinRequest) was 265,880 of the
; 276,032 bytes. The fix was a one-line `static`, not a dispatcher rewrite.
; See the comment at the top of sls_shell_execute() in user/shell.c.
;
; Kept as its own top-level output section in arch/x86/linker.ld: nested inside
; .bss the wildcard did not match, the section became an orphan placed ABOVE
; PROVIDE(_kernel_image_end), and the frame allocator handed out live kernel
; stack. See tests/kernel_image_end_check.sh, which asserts it stays covered.
stack_bottom: resb 4096 * 256
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