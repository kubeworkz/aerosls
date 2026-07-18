bits 64
global isr14_stub
global isr32_stub
global isr6_stub
global isr11_stub
global isr12_stub
global isr13_stub
extern handle_page_fault
extern handle_ring3_fault
extern timer_irq_handler
extern schedule_ring3

; ─── Macro: Ring-3 fault stub (exceptions that push an error code) ─────────────
; Saves caller-saved regs, passes (error_code, saved_cs) to handle_ring3_fault.
; CPU pushes: [SS, RSP, RFLAGS, CS, RIP, error_code] for Ring-3 faults.
%macro FAULT_STUB_EC 1          ; %1 = stub label
%1:
    push rbp
    mov  rbp, rsp
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov  rdi, [rbp + 8]          ; error_code
    mov  rsi, [rbp + 24]         ; saved CS
    mov  rdx, [rbp + 16]         ; saved RIP (faulting instruction)
    call handle_ring3_fault
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rax
    pop  rbp
    add  rsp, 8                  ; discard error_code
    iretq
%endmacro

; ─── Macro: Ring-3 fault stub (no error code, e.g. #UD) ────────────────────
%macro FAULT_STUB_NOEC 1
%1:
    push rbp
    mov  rbp, rsp
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    xor  rdi, rdi                ; no error code — pass 0
    mov  rsi, [rbp + 16]         ; saved CS
    mov  rdx, [rbp + 8]          ; saved RIP
    call handle_ring3_fault
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rax
    pop  rbp
    iretq
%endmacro

section .text

isr14_stub:
    push rbp
    mov  rbp, rsp
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov  rdi, [rbp + 8]   ; error_code
    mov  rsi, [rbp + 16]  ; faulting RIP (or CS for Ring-0)
    call handle_page_fault
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rax
    pop  rbp
    add  rsp, 8
    iretq

; ─── isr32_stub — Timer IRQ0 ──────────────────────────────────────────────────
; Ring-3 path: saves full process context, calls schedule_ring3(), restores
; the selected process's context, iretq.
; Ring-0 path: runs timer handler and returns to kernel.
isr32_stub:
    ; CS is at [rsp+8] in the iretq frame.  Ring-3 CS = 0x23.
    cmp qword [rsp+8], 0x23
    jne .ring0_timer

    ; ── Ring-3: full context save (struct TaskContext order) ──────────────────
    ; Push 15 GPRs so [rsp+0]=r15 … [rsp+112]=rax, then iretq frame follows.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    call timer_irq_handler        ; housekeeping + LAPIC EOI

    mov  rdi, rsp                 ; ctx_rsp → first arg
    call schedule_ring3           ; returns (possibly updated) ctx_rsp in rax
    mov  rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

.ring0_timer:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call timer_irq_handler
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

FAULT_STUB_NOEC isr6_stub      ; #UD  Invalid Opcode
FAULT_STUB_EC   isr11_stub     ; #NP  Segment Not Present
FAULT_STUB_EC   isr12_stub     ; #SS  Stack-Segment Fault
FAULT_STUB_EC   isr13_stub     ; #GP  General Protection Fault