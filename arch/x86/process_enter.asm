bits 64

global enter_user_process
global kernel_enter_ring3
global kernel_enter_sidecar

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
    ; 0. Save the callee-saved registers the C caller's continuation relies
    ;    on. This function never returns normally (SYSRETQ) and the register
    ;    clears below are a security measure, so without this a C caller's
    ;    rbx/rbp/r12-r15 would be zeroed when process_exit() rets back into
    ;    the continuation — a latent ABI bug that only shows on nested spawn
    ;    (the single-process path survived by luck). process_exit() pops
    ;    these before ret. pd->kernel_rsp is saved AFTER the pushes, so it
    ;    points at the r15 slot; process_exit pops 6 regs then rets.
    push  rbx
    push  rbp
    push  r12
    push  r13
    push  r14
    push  r15

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

; ─── kernel_enter_sidecar(rsp_save*, cr3_save*, cr3, user_rip, user_rsp, bib) ──
; kernel_enter_ring3() variant for the boot-time init sidecar (Phase 5,
; kernel/boot_image.c launch_init_sidecar). Identical in every respect —
; saves the kernel continuation, enters Ring-3 via SYSRETQ, process_exit()
; restores rsp_save — except one register: rdi is set to the sidecar's
; Boot Info Block VIRTUAL address instead of being cleared. The sidecar
; crt0 contract is "the kernel jumps to _start with the BIB pointer in
; a0/rdi" (crt0.S saves it before switching stacks), so the zeroing in
; kernel_enter_ring3 (correct for the legacy spawn path, whose programs
; read no BIB) would hand the init sidecar rdi=0 and lose its caps.
;
;   rdi = uint64_t* rsp_save   (pd->kernel_rsp)
;   rsi = uint64_t* cr3_save   (pd->kernel_cr3)
;   rdx = new cr3
;   rcx = user RIP
;   r8  = user RSP
;   r9  = BIB virtual address (ends up in user rdi)
kernel_enter_sidecar:
    push  rbx
    push  rbp
    push  r12
    push  r13
    push  r14
    push  r15

    mov   [rdi], rsp
    mov   rax, cr3
    mov   [rsi], rax

    cli

    mov   r11, 0x202
    mov   rsp, r8
    mov   rcx, rcx
    mov   cr3, rdx

    ; Clear every GPR except rdi, which carries the BIB pointer to _start.
    xor   rax, rax
    xor   rbx, rbx
    xor   rdx, rdx
    xor   rsi, rsi
    mov   rdi, r9
    xor   r8,  r8
    xor   r9,  r9
    xor   r10, r10
    xor   r12, r12
    xor   r13, r13
    xor   r14, r14
    xor   r15, r15
    xor   rbp, rbp

    o64 sysret
