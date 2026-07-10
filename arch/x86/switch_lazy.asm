bits 64
global perform_lazy_context_switch

; Arguments received via System V AMD64 ABI:
; RDI = Pointer to current running threads ExtendedTask structure
; RSI = Pointer to next targeted thread's ExtendedTask structure

perform_lazy_context_switch:
    ; 1. Preserve normal general-purpose integer user registers onto stack
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

    ; Save current stack pointer position into the task descriptor
    mov [rdi + 8], rsp 

    ; 2. Shift the stack frame pointer over to the incoming thread context
    mov rsp, [rsi + 8]

    ; 3. LAZY TRAP ACTIVATION: Set CR0.TS = 1
    ; This forces the CPU to generate an Interrupt 7 if this new thread touches vector math
    mov rax, cr0
    or rax, 0x08        ; Bit 3 is Task Switched (TS)
    mov cr0, rax

    ; 4. Restore general purpose layout structures off the new stack context
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ret ; Return directly into the target thread execution path with vector trapping active