bits 64
global syscall_entry_stub
extern sys_sls_allocate
    syscall_entry_stub:
    swapgs
    mov [gs:0], rsp
    mov rsp, [gs:8]
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rcx
    push r11
    cmp rax, 105
    jne .unknown_syscall
    call sys_sls_allocate
    jmp .syscall_return
    .unknown_syscall:
    xor rax, rax
    .syscall_return:
    pop r11
    pop rcx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    mov rsp, [gs:0]
    swapgs
    sysretq