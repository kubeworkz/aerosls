bits 64
global syscall_entry_stub
extern sys_sls_allocate

; MSR Address Constants
IA32_KERNEL_GS_BASE equ 0xC0000102

section .text
syscall_entry_stub:
    ; 1. Swap user GS base with kernel GS base.
    ; This gives the kernel access to per-CPU metadata, including the Kernel Stack Pointer.
    swapgs

    ; 2. Save the User Stack Pointer (RSP) into the GS-structured scratch space
    ; For this example, assume offset [0] of GS stores the current User RSP
    mov [gs:0], rsp

    ; 3. Load the pre-allocated, safe Kernel Stack Pointer into RSP
    ; Assume offset [8] of GS stores the ready-to-use Kernel Stack Pointer
    mov rsp, [gs:8]

    ; 4. Preserve user registers on the kernel stack to prevent pollution
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rcx     ; Contains the users return RIP address
    push r11     ; Contains the users return RFLAGS state

    ; 5. Route the execution based on the requested System Call ID passed in RAX
    cmp rax, 105 ; SYS_SLS_ALLOCATE
    jne .unknown_syscall

    ; System Call arguments on x86_64 follow the System V AMD64 ABI:
    ; The user passes the pointer to the SLSAllocationRequest in RDI.
    ; This aligns perfectly with the first argument expected by our C function.
    call sys_sls_allocate
    jmp .syscall_return

.unknown_syscall:
    mov rax, 0   ; Return 0/NULL for unsupported system calls

.syscall_return:
    ; 6. Restore saved user state registers off the kernel stack
    pop r11      ; Restore original RFLAGS target state
    pop rcx      ; Restore original return RIP target location
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 7. Restore the User Stack Pointer from our temporary GS scratch register space
    mov rsp, [gs:0]

    ; 8. Swap back to the User GS base configuration before entering user space
    swapgs

    ; 9. Leap back down to Ring 3 execution.
    ; Re-loads RIP from RCX and RFLAGS from R11.
    sysretq