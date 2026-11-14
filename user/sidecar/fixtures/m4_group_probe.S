/* m4_group_probe.S — M4.3's live proof of shared sidecar group state.
 *
 * One Linux-personality task opens a file (fd 3 in the leader's group),
 * clones a thread with CLONE_FILES, and the child writes to that fd without
 * opening it itself. The child also publishes a shared stack magic and
 * increments a shared counter; the parent checks both, then writes its own
 * marker and exits. This is the live twin of linux::LinuxGroup:
 *
 *   - fds/heap/dispositions live in the group, so the child can write a
 *     descriptor only the leader opened;
 *   - the clone child has its own channel, yet the write reaches the same
 *     VFS task table (CLONE_FILES shares the VFS table object);
 *   - the shared counter proves the two contexts are in one address space;
 *   - a missing sidecar tracking/release leaves the write unanswered and the
 *     parent never sees M4-GROUP-OK.
 *
 * It is intentionally raw assembly, not a libc pthread probe: pthread_join
 * needs futex wait/wake, which is M4.4. The markers are the truthful
 * "pthread-shaped" live arm available before that gate.
 *
 * Build (build_m4_group_probe.sh; the committed .flat is this output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_group_probe.elf m4_group_probe.S
 *   objcopy -O binary -j .text m4_group_probe.elf m4_group_probe.flat
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
    .set OPEN_FLAGS,   0x00088241  /* O_WRONLY|O_CREAT|O_TRUNC|O_LARGEFILE|O_CLOEXEC */
    .set GROUP_OK,     0x4d3447524f4b       /* "M4GROK" */
    .set GROUP_FAIL,   0x4d3447524641494c   /* "M4GRFAIL" */

    .text
    .globl _start
_start:
    /* r12 = shared counter, r13 = shared result, r14 = ptid, r15 = fd,
     * rbx = ctid. The kernel's clone context copies callee-saved registers. */
    leaq -0x200(%rsp), %r12
    leaq -0x208(%rsp), %r13
    leaq -0x100(%rsp), %r14
    leaq -0x108(%rsp), %rbx
    movq $0, (%r12)
    movq $0, (%r13)
    movq $0, (%r14)
    movq $0, (%rbx)

    /* Leader: open /tmp/m4-group before creating the thread. The child
     * never issues open(2); its write to this fd is the shared-group proof. */
    movq $2, %rax
    leaq .Lpath(%rip), %rdi
    movq $OPEN_FLAGS, %rsi
    movq $0666, %rdx
    syscall
    testq %rax, %rax
    js .Lfail
    movq %rax, %r15

    /* clone(flags, child_stack, ptid, ctid, tls) */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x500(%rsp), %rsi
    movq %r14, %rdx
    movq %rbx, %r10
    xorq %r8, %r8
    syscall

    testq %rax, %rax
    js .Lfail
    jz .Lchild

    /* Parent: ptid/ctid must carry the child's PID. */
    cmpq %rax, (%r14)
    jne .Lfail
    cmpq %rax, (%rbx)
    jne .Lfail

    /* Wait for the child's counter, then its result. The timer preempts
     * this loop, so the child gets its turn on one CPU. */
.Lparent_wait:
    movq (%r12), %rax
    testq %rax, %rax
    jz .Lparent_wait
    movq (%r13), %rax
    movabsq $GROUP_OK, %rcx
    cmpq %rcx, %rax
    je .Lparent_ok
    movabsq $GROUP_FAIL, %rcx
    cmpq %rcx, %rax
    je .Lfail
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

.Lchild:
    /* The fd was opened only by the leader. A per-thread table (M4.2's
     * shape) answers EBADF here and the child publishes GROUP_FAIL. */
    movq $1, %rax
    movl %r15d, %edi
    leaq .Lfile_msg(%rip), %rsi
    movq $.Lfile_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lchild_fail

    movq $1, %rax
    movq $1, %rdi
    leaq .Lchild_ok_msg(%rip), %rsi
    movq $.Lchild_ok_len, %rdx
    syscall
    testq %rax, %rax
    jle .Lchild_fail

    /* Publish success only after both writes returned a positive count. */
    movabsq $GROUP_OK, %rax
    movq %rax, (%r13)
    incq (%r12)
    ud2

.Lchild_fail:
    movabsq $GROUP_FAIL, %rax
    movq %rax, (%r13)
    incq (%r12)
    movq $1, %rax
    movq $1, %rdi
    leaq .Lfail_msg(%rip), %rsi
    movq $.Lfail_len, %rdx
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

.Lpath:
    .asciz "/tmp/m4-group"
.Lfile_msg:
    .ascii "M4-GROUP-FILE\n"
    .set .Lfile_len, . - .Lfile_msg
.Lchild_ok_msg:
    .ascii "M4-GROUP-CHILD-OK\n"
    .set .Lchild_ok_len, . - .Lchild_ok_msg
.Lparent_ok_msg:
    .ascii "M4-GROUP-PARENT-OK\n"
    .set .Lparent_ok_len, . - .Lparent_ok_msg
.Lfail_msg:
    .ascii "M4-GROUP-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
