bits 64

; enter_user_process(cr3, rip, rsp)
;   rdi = new CR3 (process page table base)
;   rsi = user RIP (entry point)
;   rdx = user RSP (top of user stack)
;
; Switches to the process's page table and enters Ring-3 via SYSRETQ.
; This function does not return in the kernel context — the process runs
; until it issues a SYSCALL or receives a fault.

global enter_user_process

section .text
enter_user_process:
    ; 1. Load the process's page table
    mov   cr3, rdi

    ; 2. Set up SYSRETQ parameters:
    ;    RCX = user RIP (SYSRETQ restores RIP from RCX)
    ;    R11 = user RFLAGS (bit 9 = IF, bit 1 = reserved must be 1)
    ;    RSP = user RSP
    mov   rcx, rsi       ; user entry point
    mov   r11, 0x202     ; RFLAGS: IF=1 (interrupts enabled), bit 1 reserved=1
    mov   rsp, rdx       ; user stack pointer

    ; 3. Clear general-purpose registers so Ring-3 starts with a clean slate
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

    ; 4. Switch into Ring-3 (CPL transitions from 0 → 3)
    sysretq
