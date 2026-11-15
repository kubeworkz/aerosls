/* m4_ckpt_probe.S — M4.6's live checkpoint fixture.
 *
 * One Linux-personality leader clones a thread. The child writes
 * `M4.6-THREAD-OK`, publishes READY, and parks in a private FUTEX_WAIT on
 * `child_word`. The parent waits for READY, writes `M4.6-CKPT-READY`, and
 * parks in a timed private FUTEX_WAIT on `parent_word`. The harness triggers
 * the checkpoint only after both `wait (futex)` lines are in the serial log,
 * so the capture sees a healthy two-member group whose leader and sibling are
 * both parked forwarded syscalls.
 *
 * After the reboot the parent's captured timeout completes. It writes
 * `M4.6-FUTEX-PARENT-WAKE`, performs FUTEX_WAKE on `child_word`, and the
 * child's restored pending FUTEX_WAIT is answered by the sidecar: the child
 * writes `M4.6-FUTEX-WOKE`, sets `done`, and the parent writes
 * `M4.6-RESTORED-OK`. A lost waiter, a lost thread context, or a channel the
 * restored sidecar table does not name leaves the waiter parked forever and
 * the harness red.
 *
 * Build (build_m4_ckpt_probe.sh; the committed .flat is this exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_ckpt_probe.elf m4_ckpt_probe.S
 *   objcopy -O binary -j .text m4_ckpt_probe.elf m4_ckpt_probe.flat
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
    .set FUTEX_WAIT,      0
    .set FUTEX_WAKE,      1
    .set FUTEX_PRIVATE,   128
    .set FUTEX_WAIT_OP,   FUTEX_WAIT | FUTEX_PRIVATE
    .set FUTEX_WAKE_OP,   FUTEX_WAKE | FUTEX_PRIVATE

    .text
    .globl _start
_start:
    /* r12 = child_word, r13 = parent_word, r14 = done, r15 = child_ready. */
    leaq -0x300(%rsp), %r12
    leaq -0x308(%rsp), %r13
    leaq -0x310(%rsp), %r14
    leaq -0x318(%rsp), %r15
    movq $0, (%r12)
    movq $0, (%r13)
    movq $0, (%r14)
    movq $0, (%r15)

    /* clone(flags, child_stack, ptid, ctid, tls) */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x400(%rsp), %rsi
    leaq -0x320(%rsp), %rdx
    leaq -0x328(%rsp), %r10
    xorq %r8, %r8
    syscall

    testq %rax, %rax
    js .Lclone_failed
    jz .Lchild

    /* Parent: wait until the child has announced its wait. The sidecar's
     * futex line is the real parked proof; this word only orders the
     * parent's own park after the child's write. */
.Lparent_ready_wait:
    cmpq $1, (%r15)
    je .Lparent_ready
    pause
    jmp .Lparent_ready_wait

.Lparent_ready:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lready_msg(%rip), %rsi
    movq $.Lready_len, %rdx
    syscall

    /* Parent parks too: a forwarded FUTEX_WAIT with a finite relative
     * timeout. The timeout is what wakes the restore's parent without an
     * external actor; the child's untimed wait is the one that must survive
     * the reboot and be woken by the parent below. */
    movq $202, %rax
    movq %r13, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Lparent_timeout(%rip), %r10
    syscall

    movq $1, %rax
    movq $1, %rdi
    leaq .Lparent_wake_msg(%rip), %rsi
    movq $.Lparent_wake_len, %rdx
    syscall

    /* Wake the child's restored waiter. */
    movq $202, %rax
    movq %r12, %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall

.Lparent_done_wait:
    cmpq $1, (%r14)
    je .Lparent_done
    pause
    jmp .Lparent_done_wait

.Lparent_done:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lrestored_msg(%rip), %rsi
    movq $.Lrestored_len, %rdx
    syscall
.Lparent_spin:
    pause
    jmp .Lparent_spin

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

    /* The child enters here with rax = 0 and the parent's r12-r15. */
.Lchild:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lthread_msg(%rip), %rsi
    movq $.Lthread_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lchild_failed

    movq $1, (%r15)

    /* Untimed private wait: this is the parked waiter the checkpoint must
     * carry through the reboot. */
    movq $202, %rax
    movq %r12, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall

    movq $1, %rax
    movq $1, %rdi
    leaq .Lwoke_msg(%rip), %rsi
    movq $.Lwoke_len, %rdx
    syscall
    movq $1, (%r14)
.Lchild_spin:
    pause
    jmp .Lchild_spin

.Lchild_failed:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lchild_fail_msg(%rip), %rsi
    movq $.Lchild_fail_len, %rdx
    syscall
.Lchild_failed_spin:
    pause
    jmp .Lchild_failed_spin

    .align 8
.Lparent_timeout:
    .quad 30, 0
.Lthread_msg:
    .ascii "M4.6-THREAD-OK\n"
    .set .Lthread_len, . - .Lthread_msg
.Lready_msg:
    .ascii "M4.6-CKPT-READY\n"
    .set .Lready_len, . - .Lready_msg
.Lparent_wake_msg:
    .ascii "M4.6-FUTEX-PARENT-WAKE\n"
    .set .Lparent_wake_len, . - .Lparent_wake_msg
.Lwoke_msg:
    .ascii "M4.6-FUTEX-WOKE\n"
    .set .Lwoke_len, . - .Lwoke_msg
.Lrestored_msg:
    .ascii "M4.6-RESTORED-OK\n"
    .set .Lrestored_len, . - .Lrestored_msg
.Lfail_msg:
    .ascii "M4.6-CKPT-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
.Lchild_fail_msg:
    .ascii "M4.6-CHILD-FAIL\n"
    .set .Lchild_fail_len, . - .Lchild_fail_msg
