bits 64

global enter_user_process
global kernel_enter_ring3

section .text

; ─── enter_user_process(cr3, rip, rsp) ────────────────────────────────────────
; Legacy one-way entry. Does not return.
;   rdi = cr3, rsi = user RIP, rdx = user RSP
enter_user_process:
    mov   cr3, rdi
    mov   rcx, rsi
    mov   r11, 0x202
    mov   rsp, rdx
    xor   rax, rax
    xor   rbx, rbx
    xor   rdx, rdx
    xor   rsi, rsi
    xor   rdi, rdi
    xor   r8,  r8
    xor   r9,  r9
    xor   r10, r10
    xor   r12, r12
    xor   r13, r13
    xor   r14, r14
    xor   r15, r15
    xor   rbp, rbp
    o64 sysret

; ─── kernel_enter_ring3(rsp_save*, cr3_save*, cr3, user_rip, user_rsp) ─────────
; Saves the kernel continuation (RSP and CR3) then enters Ring-3 via SYSRETQ.
; When SYS_SLS_EXIT fires, process_exit() restores rsp_save and rets,
; returning to the instruction after the call to kernel_enter_ring3().
;
;   rdi = uint64_t* rsp_save   (pd->kernel_rsp)
;   rsi = uint64_t* cr3_save   (pd->kernel_cr3)
;   rdx = new cr3
;   rcx = user RIP
;   r8  = user RSP
kernel_enter_ring3:
    ; 1. Save kernel RSP — [rsp] is the return address back to process.c
    mov   [rdi], rsp

    ; 2. Save kernel CR3 so it can be restored when the process exits
    mov   rax, cr3
    mov   [rsi], rax

    ; 3. Disable interrupts during the Ring-3 transition (SYSRETQ re-enables
    ;    them via R11 = 0x202 which has IF set)
    cli

    ; 4. Set SYSRETQ parameters:
    ;    RCX = user RIP, R11 = RFLAGS (IF=1), RSP = user RSP
    mov   r11, 0x202
    mov   rsp, r8       ; switch to user stack
    mov   rcx, rcx      ; user RIP is already in RCX

    ; 5. Load the process's page table
    mov   cr3, rdx

    ; 6. Clear registers for security
    xor   rax, rax
    xor   rbx, rbx
    xor   rdx, rdx
    xor   rsi, rsi
    xor   rdi, rdi
    xor   r8,  r8
    xor   r9,  r9
    xor   r10, r10
    xor   r12, r12
    xor   r13, r13
    xor   r14, r14
    xor   r15, r15
    xor   rbp, rbp

    ; 7. Enter Ring-3 (restores IF via R11)
    o64 sysret
