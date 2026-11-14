/* m4_futex_probe.S — M4.4's live proof of group-keyed futex wait/wake.
 *
 * One Linux-personality task clones a thread. The child publishes `ready`
 * with FUTEX_WAKE, then waits on `go`; the parent waits for `ready`, sets
 * `go`, wakes the child, and waits for `done`. The child returns from the
 * wait, writes M4-PTHREAD-OK, sets `done`, and wakes the parent. The parent
 * then performs a timed FUTEX_WAIT with no waker and requires -ETIMEDOUT
 * before writing M4-FUTEX-OK and M4-FUTEX-TIMEOUT-OK.
 *
 * This is a raw assembly join SHAPE, not a libc pthread: the kernel's
 * clear_child_tid zero+wake on thread exit is M4.5. The two waits are
 * private futexes on the shared stack page, so the sidecar queue key is
 * (group, uaddr); the timeout record exercises the kernel's observed word
 * and relative timespec copy.
 *
 * Build (build_m4_futex_probe.sh; the committed .flat is this output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_futex_probe.elf m4_futex_probe.S
 *   objcopy -O binary -j .text m4_futex_probe.elf m4_futex_probe.flat
 */
    .set CLONE_VM,             0x00000100
    .set CLONE_FS,             0x00000200
    .set CLONE_FILES,          0x00000400
    .set CLONE_SIGHAND,        0x00000800
    .set CLONE_THREAD,         0x00010000
    .set CLONE_PARENT_SETTID,  0x00100000
    .set CLONE_CHILD_CLEARTID, 0x00200000
    .set CLONE_CHILD_SETTID,   0x01000000
    .set CLONE_FLAGS, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                      CLONE_THREAD | CLONE_PARENT_SETTID | \
                      CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID
    .set FUTEX_WAIT,      0
    .set FUTEX_WAKE,      1
    .set FUTEX_PRIVATE,   128
    .set FUTEX_WAIT_OP,   FUTEX_WAIT | FUTEX_PRIVATE
    .set FUTEX_WAKE_OP,   FUTEX_WAKE | FUTEX_PRIVATE

    .text
    .globl _start
_start:
    /* Shared words live above the child stack; r12-r15 point at ready,
     * go, done and the timeout word respectively. The clone context
     * copies these callee-saved registers to the child. */
    leaq -0x300(%rsp), %r12
    leaq -0x308(%rsp), %r13
    leaq -0x310(%rsp), %r14
    leaq -0x318(%rsp), %r15
    leaq -0x330(%rsp), %rbx     /* shared result: 1 success, 2 failure */
    movq $0, (%r12)
    movq $0, (%r13)
    movq $0, (%r14)
    movq $0, (%r15)
    movq $0, (%rbx)

    /* clone(flags, child_stack, ptid, ctid, tls) */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x700(%rsp), %rsi
    leaq -0x320(%rsp), %rdx
    leaq -0x328(%rsp), %r10
    xorq %r8, %r8
    syscall

    testq %rax, %rax
    js .Lfail
    jz .Lchild

    /* The parent waits for the child's ready publication. A lost wake is
     * harmless: the finite timeout returns, then the word is re-checked. */
.Lparent_ready_wait:
    cmpq $0, (%r12)
    jne .Lparent_ready
    movq $202, %rax
    movq %r12, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Lparent_timeout(%rip), %r10
    syscall
    jmp .Lparent_ready_wait

.Lparent_ready:
    /* Publish go and wake the child. If the child has not entered its
     * wait yet, its own finite timeout re-checks go, so the handshake is
     * deterministic without a sleep. */
    movq $1, (%r13)
    movq $202, %rax
    movq %r13, %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall

    /* Join shape: wait for the child's done publication. */
.Lparent_done_wait:
    cmpq $0, (%r14)
    jne .Lparent_done
    movq $202, %rax
    movq %r14, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Lparent_timeout(%rip), %r10
    syscall
    jmp .Lparent_done_wait

.Lparent_done:
    cmpq $1, (%rbx)
    jne .Lfail

    /* M4.4 timeout arm: no waker, expected 0, ten milliseconds. Linux
     * returns -ETIMEDOUT (110); anything else is a failed arm. */
    movq $202, %rax
    movq %r15, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Ltimeout_10ms(%rip), %r10
    syscall
    cmpq $-110, %rax
    jne .Lfail

    movq $1, %rax
    movq $1, %rdi
    leaq .Ltimeout_msg(%rip), %rsi
    movq $.Ltimeout_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lfail
    movq $1, %rax
    movq $1, %rdi
    leaq .Lparent_ok_msg(%rip), %rsi
    movq $.Lparent_ok_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lfail

    movq $231, %rax
    xorq %rdi, %rdi
    syscall
    ud2

.Lchild:
    /* Publish ready and wake a parent that may already be waiting. */
    movq $1, (%r12)
    movq $202, %rax
    movq %r12, %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall

    /* Wait for go. A wake that arrives before the queue entry is covered
     * by the timeout: whichever returns, go is the authoritative bit. */
    movq $202, %rax
    movq %r13, %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Lchild_timeout(%rip), %r10
    syscall
    cmpq $1, (%r13)
    jne .Lchild_fail

    movq $1, %rax
    movq $1, %rdi
    leaq .Lchild_ok_msg(%rip), %rsi
    movq $.Lchild_ok_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lchild_fail

    movq $1, (%rbx)             /* shared result: success */
    movq $1, (%r14)
    movq $202, %rax
    movq %r14, %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall
    movq $231, %rax
    xorq %rdi, %rdi
    syscall
    ud2

.Lchild_fail:
    movq $2, (%rbx)             /* shared result: failure */
    movq $1, (%r14)
    movq $202, %rax
    movq %r14, %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall
    movq $1, %rax
    movq $1, %rdi
    leaq .Lfail_msg(%rip), %rsi
    movq $.Lfail_len, %rdx
    syscall
    movq $231, %rax
    movq $1, %rdi
    syscall
    ud2

.Lfail:
    movq $1, %rax
    movq $1, %rdi
    leaq .Lfail_msg(%rip), %rsi
    movq $.Lfail_len, %rdx
    syscall
    movq $231, %rax
    movq $1, %rdi
    syscall
    ud2

    .align 8
.Lparent_timeout:
    .quad 1, 0
.Lchild_timeout:
    .quad 1, 0
.Ltimeout_10ms:
    .quad 0, 10000000
.Lchild_ok_msg:
    .ascii "M4-PTHREAD-OK\n"
    .set .Lchild_ok_len, . - .Lchild_ok_msg
.Lparent_ok_msg:
    .ascii "M4-FUTEX-OK\n"
    .set .Lparent_ok_len, . - .Lparent_ok_msg
.Ltimeout_msg:
    .ascii "M4-FUTEX-TIMEOUT-OK\n"
    .set .Ltimeout_len, . - .Ltimeout_msg
.Lfail_msg:
    .ascii "M4-FUTEX-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
