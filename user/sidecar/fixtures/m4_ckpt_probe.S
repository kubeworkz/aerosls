/* m4_ckpt_probe.S — M4.6 increment 1's live checkpoint fixture.
 *
 * One Linux-personality task issues raw clone(56) and leaves BOTH contexts
 * alive. The child writes `M4.6-THREAD-OK` through its own tracked messenger
 * and publishes a shared word; the parent observes the word, writes
 * `M4.6-CKPT-READY`, and spins with the child. The harness triggers a
 * checkpoint only after the ready marker, so the capture sees a group whose
 * live member count is exactly two — the case increment 1 must refuse with
 * EP_REFUSE_THREADS before any payload is written.
 *
 * Build (build_m4_ckpt_probe.sh; the committed .flat is this exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_ckpt_probe.elf m4_ckpt_probe.S
 *   objcopy -O binary -j .text m4_ckpt_probe.elf m4_ckpt_probe.flat
 *
 * Flags match the M4.2 thread fixture: VM | SIGHAND | THREAD |
 * PARENT_SETTID | CHILD_CLEARTID | CHILD_SETTID. The child stack is 0x400
 * below the entry RSP; ptid/ctid/shared words sit below it in the shared
 * address space.
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
    /* r12 = shared word, r13 = ptid, r14 = ctid. */
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

    /* Parent: wait until the child's write(1) was answered and the shared
     * word was published. Timer preemption gives the child its turn. */
.Lparent_wait:
    movabsq $SHARED_MAGIC, %rax
    cmpq %rax, (%r12)
    jne .Lparent_wait

    movq $1, %rax
    movq $1, %rdi
    leaq .Lready_msg(%rip), %rsi
    movq $.Lready_len, %rdx
    syscall

    /* Both threads stay live; the checkpoint capture is the next event. */
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

    /* The child enters here with rax = 0 and the parent's r12-r14. */
.Lchild:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lthread_msg(%rip), %rsi
    movq $.Lthread_len, %rdx
    syscall
    cmpq $.Lthread_len, %rax
    jne .Lchild_failed

    /* Publish only after the sidecar answered the write. */
    movabsq $SHARED_MAGIC, %rax
    movq %rax, (%r12)

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

.Lthread_msg:
    .ascii "M4.6-THREAD-OK\n"
    .set .Lthread_len, . - .Lthread_msg
.Lready_msg:
    .ascii "M4.6-CKPT-READY\n"
    .set .Lready_len, . - .Lready_msg
.Lfail_msg:
    .ascii "M4.6-CKPT-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
.Lchild_fail_msg:
    .ascii "M4.6-CHILD-FAIL\n"
    .set .Lchild_fail_len, . - .Lchild_fail_msg
