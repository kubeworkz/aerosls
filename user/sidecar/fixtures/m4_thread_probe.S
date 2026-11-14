/* m4_thread_probe.S — M4.2's live clone(56) proof.
 *
 * One Linux-personality task (spawned through SYS_SLS_LINUX_SPAWN) issues the
 * raw clone syscall, and its thread — the second execution context in the
 * SAME LinuxThreadGroup — writes a marker and then a shared word before
 * faulting. The parent polls the shared word (timer preemption lets the child
 * run), writes its own marker, and exits. The serial check asserts both
 * markers in order, which is only possible if:
 *
 *   - the kernel created the child with the parent's PML4;
 *   - the child resumed at the instruction after `syscall` with rax = 0;
 *   - the child's write(1) reached linux::service through its OWN messenger
 *     channel and the sidecar tracked it; and
 *   - the shared stack word was visible across the two contexts.
 *
 * Build (build_m4_thread_probe.sh; the committed .flat is this exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_thread_probe.elf m4_thread_probe.S
 *   objcopy -O binary -j .text m4_thread_probe.elf m4_thread_probe.flat
 *
 * Flags: VM | SIGHAND | THREAD | PARENT_SETTID | CHILD_CLEARTID |
 * CHILD_SETTID = 0x01310900. The child stack is 0x400 below the entry RSP,
 * leaving the parent's own stack above it; ptid/ctid/shared words sit at
 * fixed negative offsets in the same (shared) stack.
 */
    .set CLONE_VM,             0x00000100
    .set CLONE_SIGHAND,        0x00000800
    .set CLONE_THREAD,         0x00010000
    .set CLONE_PARENT_SETTID,  0x00100000
    .set CLONE_CHILD_CLEARTID, 0x00200000
    .set CLONE_CHILD_SETTID,   0x01000000
    .set CLONE_FLAGS, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | \
                      CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | \
                      CLONE_CHILD_SETTID
    .set SHARED_MAGIC, 0x4d34544852454144   /* "M4THREAD" */

    .text
    .globl _start
_start:
    /* r12 = shared word, r13 = ptid, r14 = ctid; all are callee-saved and
     * the kernel's clone context copies them into the child. */
    leaq -0x200(%rsp), %r12
    leaq -0x100(%rsp), %r13
    leaq -0x108(%rsp), %r14
    movq $0, (%r12)
    movq $0, (%r13)
    movq $0, (%r14)

    /* clone(flags, child_stack, ptid, ctid, tls) */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x400(%rsp), %rsi
    movq %r13, %rdx
    movq %r14, %r10
    xorq %r8, %r8
    syscall

    testq %rax, %rax
    js .Lclone_failed
    jz .Lchild

    /* Parent: both requested tid words must carry the child's PID. This is
     * the live half of the kernel's ptid/ctid write contract; a missing or
     * drifted write takes the same named failure path. */
    cmpq %rax, (%r13)
    jne .Lclone_failed
    cmpq %rax, (%r14)
    jne .Lclone_failed

    /* Parent: wait for the child's shared word. The timer preempts this
     * loop, so the child gets its turn even on one CPU. */
.Lparent_wait:
    movabsq $SHARED_MAGIC, %rax
    cmpq %rax, (%r12)
    je .Lparent_ok
    pause
    jmp .Lparent_wait

.Lparent_ok:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lparent_ok_msg(%rip), %rsi
    movq $.Lparent_ok_len, %rdx
    syscall

    movq $231, %rax                 /* exit_group */
    xorq %rdi, %rdi
    syscall
    ud2

.Lclone_failed:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lfail_msg(%rip), %rsi
    movq $.Lfail_len, %rdx
    syscall
    movq $231, %rax
    movq $1, %rdi
    syscall
    ud2

    /* The child enters here with rax = 0 and the parent's r12-r14. */
.Lchild:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lchild_msg(%rip), %rsi
    movq $.Lchild_len, %rdx
    syscall

    /* Publish the shared word only after the sidecar answered the write. */
    movabsq $SHARED_MAGIC, %rax
    movq %rax, (%r12)

    /* Deliberate fault: the child leaves through the kernel's #UD path,
     * exercising the shared-CR3 teardown while the parent still lives. */
    ud2

.Lchild_msg:
    .ascii "M4-THREAD-OK\n"
    .set .Lchild_len, . - .Lchild_msg
.Lparent_ok_msg:
    .ascii "M4-THREAD-PARENT-OK\n"
    .set .Lparent_ok_len, . - .Lparent_ok_msg
.Lfail_msg:
    .ascii "M4-THREAD-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
