bits 64
global isr14_stub
global isr32_stub
global isr8_stub
global isr6_stub
global isr7_stub
global isr11_stub
global isr12_stub
global isr13_stub
extern handle_page_fault
extern handle_ring3_fault
extern handle_device_not_available_fault
extern timer_irq_handler
extern schedule_ring3
extern cap_irq_notify

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

; ─── isr8_stub — Double Fault (#DF) ─────────────────────────────────────────
; Deliberately has a gate of its own (idt.c installs it). #DF is what any
; fault escalates to when it recurs inside fault delivery, and while it had NO
; gate the CPU had nowhere to go: it triple-faults, which resets the machine --
; and under -no-reboot that is a QEMU exit with no output at all, the failure
; mode this kernel hit twice at the same serial-log byte. #DF pushes an error
; code (always 0), so it takes the error-code stub shape -- the same macro
; every other fault stub uses, so the frame layout cannot drift from theirs.
FAULT_STUB_EC isr8_stub

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
    mov  rdx, [rbp + 40]  ; user RSP (Ring-3 faults only; 0 for Ring-0)
    mov  rcx, [rsp + 32]  ; fault-time RDI (pushed: rax@0 rcx@8 rdx@16 rsi@24 rdi@32)
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

; ─── isr7_stub: #NM Device Not Available (Gap Remediation SIMI Phase 10) ────
; Deliberately NOT built from FAULT_STUB_NOEC: that macro dispatches to the
; generic handle_ring3_fault(error_code, cs, rip) path, which this trap must
; never take -- #NM is not a fault to report/kill the offending code over,
; it is the lazy-FPU mechanism's own normal, expected control-flow trigger
; (see arch/x86/lazy_fpu.c). handle_device_not_available_fault() takes no
; arguments and, on return via iretq, the CPU re-executes the exact
; instruction that trapped -- now with FPU/AVX-512 state correctly loaded,
; not with the trap treated as complete-and-abandoned the way a real fault
; would be.
isr7_stub:
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
    call handle_device_not_available_fault
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


; --- device-vector stubs, one per vector 33..255 (Driver SDK ABI v0.1
; section 4.3/4.4). Each stub saves the caller-saved registers, calls
; cap_irq_notify(vector) -- the cap.c ISR path: registry resolve, enqueue
; one-byte notification on the bound driver's channel, wake, cap_irq_eoi
; -- and iretq. No scheduling here: a woken driver becomes runnable at the
; next timer tick (isr32_stub / schedule_ring3). Ring-3 and ring-0 sources
; both work; the stub runs on the kernel stack via TSS.
%assign IRQ_VEC 33
%rep 224
isr_irq_%+IRQ_VEC:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov  edi, IRQ_VEC
    call cap_irq_notify
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rax
    iretq
%assign IRQ_VEC IRQ_VEC+1
%endrep

; Stub address table: entry [0] = vector 33 ... [223] = vector 255.
; arch/x86/device_irq.c installs them via set_idt_gate(33 + i, ...).
global irq_stub_table
irq_stub_table:
%assign IRQ_VEC 33
%rep 224
    dq isr_irq_%+IRQ_VEC
%assign IRQ_VEC IRQ_VEC+1
%endrep
