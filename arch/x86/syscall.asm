bits 64
global syscall_entry_stub
global enter_user_process
extern do_syscall          ; C dispatcher for syscalls not in the asm table
extern sys_sls_allocate
extern sys_sls_valloc
extern sys_sls_vfree
extern sys_sls_obj_stat
extern sys_sls_obj_list
extern sys_sls_role_set
extern sys_sls_grant
extern sys_sls_select
extern sys_sls_update
extern sys_sls_insert
extern sys_sls_tx_begin
extern sys_sls_tx_commit
extern sys_sls_tx_rollback
extern sys_sls_tx_recover
extern sys_sls_svc_list
extern sys_sls_svc_crash
extern sys_sls_svc_restart
extern sys_sls_ipc_stat
extern sys_sls_ipc_post
extern sys_sls_tier_list
extern sys_sls_tier_promote
extern sys_sls_tier_demote
extern sys_sls_delete
extern sys_sls_schema_set
extern sys_sls_schema_show
extern sys_sls_query
extern sys_sls_query_scan

; MSR Address Constants
IA32_KERNEL_GS_BASE equ 0xC0000102

section .text
syscall_entry_stub:
    swapgs
    mov [gs:0], rsp
    mov rsp, [gs:8]

    ; Save callee-saved regs (C ABI requires caller to preserve these).
    ; Also save RCX (user RIP) and R11 (user RFLAGS) which SYSCALL destroys.
    ; Caller-saved regs (R8-R10, RSI, RDX, RDI) are NOT preserved here —
    ; user programs must list them as clobbers in their inline asm.
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rcx       ; user RIP (set by CPU on SYSCALL)
    push r11       ; user RFLAGS

    ; ── Route on syscall number in RAX ───────────────────────────────────────
    cmp rax, 105
    je  .do_sls_allocate
    cmp rax, 110
    je  .do_sls_valloc
    cmp rax, 111
    je  .do_sls_vfree
    cmp rax, 112
    je  .do_sls_obj_stat
    cmp rax, 113
    je  .do_sls_obj_list
    cmp rax, 114
    je  .do_sls_role_set
    cmp rax, 115
    je  .do_sls_grant
    cmp rax, 117
    je  .do_sls_select
    cmp rax, 118
    je  .do_sls_update
    cmp rax, 119
    je  .do_sls_insert
    cmp rax, 120
    je  .do_sls_tx_begin
    cmp rax, 121
    je  .do_sls_tx_commit
    cmp rax, 122
    je  .do_sls_tx_rollback
    cmp rax, 123
    je  .do_sls_tx_recover
    cmp rax, 130
    je  .do_sls_svc_list
    cmp rax, 131
    je  .do_sls_svc_crash
    cmp rax, 132
    je  .do_sls_svc_restart
    cmp rax, 133
    je  .do_sls_ipc_stat
    cmp rax, 134
    je  .do_sls_ipc_post
    cmp rax, 140
    je  .do_sls_tier_list
    cmp rax, 141
    je  .do_sls_tier_promote
    cmp rax, 142
    je  .do_sls_tier_demote
    cmp rax, 143
    je  .do_sls_delete
    cmp rax, 144
    je  .do_sls_schema_set
    cmp rax, 145
    je  .do_sls_schema_show
    cmp rax, 150
    je  .do_sls_query
    cmp rax, 151
    je  .do_sls_query_scan
    jmp .unknown_syscall

.do_sls_allocate:
    call sys_sls_allocate
    jmp  .syscall_return

.do_sls_valloc:
    ; RDI = pointer to SLSVallocRequest
    call sys_sls_valloc
    jmp  .syscall_return

.do_sls_vfree:
    ; RDI = const char* name
    call sys_sls_vfree
    jmp  .syscall_return

.do_sls_obj_stat:
    ; RDI = const char* name
    call sys_sls_obj_stat
    jmp  .syscall_return

.do_sls_obj_list:
    ; no arguments
    call sys_sls_obj_list
    jmp  .syscall_return

.do_sls_role_set:
    ; RDI = pointer to SLSRoleRequest
    call sys_sls_role_set
    jmp  .syscall_return

.do_sls_grant:
    ; RDI = pointer to args[2] = { req_ptr, is_grant }
    mov  rsi, [rdi + 8]     ; is_grant = args[1]
    mov  rdi, [rdi]         ; req      = args[0]
    call sys_sls_grant
    jmp  .syscall_return

.do_sls_select:
    call sys_sls_select
    jmp  .syscall_return

.do_sls_update:
    call sys_sls_update
    jmp  .syscall_return

.do_sls_insert:
    call sys_sls_insert
    jmp  .syscall_return

.do_sls_tx_begin:
    ; RDI = thread_id (passed as pointer-sized integer)
    ; Convert pointer to uint32_t by truncation — ABI-safe on x86_64
    call sys_sls_tx_begin
    jmp  .syscall_return

.do_sls_tx_commit:
    call sys_sls_tx_commit
    jmp  .syscall_return

.do_sls_tx_rollback:
    call sys_sls_tx_rollback
    jmp  .syscall_return

.do_sls_tx_recover:
    call sys_sls_tx_recover
    jmp  .syscall_return

.do_sls_svc_list:
    ; no arguments
    call sys_sls_svc_list
    jmp  .syscall_return

.do_sls_svc_crash:
    ; RDI = const char* name
    call sys_sls_svc_crash
    jmp  .syscall_return

.do_sls_svc_restart:
    ; RDI = const char* name
    call sys_sls_svc_restart
    jmp  .syscall_return

.do_sls_ipc_stat:
    ; no arguments — print inline
    call sys_sls_svc_list   ; reuse svc_list for combined IPC/service stats
    jmp  .syscall_return

.do_sls_ipc_post:
    ; RDI = pointer to IPCPostRequest
    call sys_sls_ipc_post
    jmp  .syscall_return

.do_sls_tier_list:
    ; no arguments
    call sys_sls_tier_list
    jmp  .syscall_return

.do_sls_tier_promote:
    ; RDI = const char* name
    call sys_sls_tier_promote
    jmp  .syscall_return

.do_sls_tier_demote:
    ; RDI = const char* name
    call sys_sls_tier_demote
    jmp  .syscall_return

.do_sls_delete:
    ; RDI = pointer to SLSRecordRequest
    call sys_sls_delete
    jmp  .syscall_return

.do_sls_schema_set:
    ; RDI = pointer to SLSSchemaRequest
    call sys_sls_schema_set
    jmp  .syscall_return

.do_sls_schema_show:
    ; RDI = const char* name
    call sys_sls_schema_show
    jmp  .syscall_return

.do_sls_query:
    ; RDI = const char* query text
    call sys_sls_query
    jmp  .syscall_return

.do_sls_query_scan:
    ; no arguments
    call sys_sls_query_scan
    jmp  .syscall_return

.unknown_syscall:
    ; Fall through to the C dispatcher for any syscall number not handled above.
    ; do_syscall(uint64_t num, void* arg): rdi = num, rsi = arg
    mov  rsi, rdi          ; arg was in RDI
    mov  rdi, rax          ; syscall number from RAX
    call do_syscall
    ; return value already in RAX
    jmp  .syscall_return

.syscall_return:
    pop  r11
    pop  rcx
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    mov  rsp, [gs:0]
    swapgs
    o64 sysret          ; return to Ring-3 (NASM 2.16: use o64 sysret, not sysretq)