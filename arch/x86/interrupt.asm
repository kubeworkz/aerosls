bits 64
global isr14_stub
extern handle_page_fault

isr14_stub:
    push rbp               ; Save base pointer
    mov rbp, rsp           ; Form stack frame

    ; Push registers to prevent corrupting state
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    ; The CPU leaves an error code on the stack above the saved registers
    ; Pass the error code as the first argument (RDI) to your C function
    mov rdi, [rbp + 8] 
    call handle_page_fault

    ; Restore registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    pop rbp
    add rsp, 8             ; Clean up error code from stack
    iretq                  ; 64-bit Interrupt Return