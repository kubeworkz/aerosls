/* e7_thread_probe.S — E7 TG-1 increment I2's native two-thread probe.
 *
 * Freestanding, one .text section, _start at file offset 0 (the flat mapper
 * enters at image_entry = 0, exactly like e7_linux_probe.S).
 *
 * The probe exercises the I2 contract on a real boot:
 *   - brk(0)/brk(base+0x2000) maps the shared heap page the threads use;
 *   - clone(56) with Go's measured thread shape (0x000D0F00,
 *     VM|FS|FILES|SIGHAND|SYSVSEM|THREAD|SETTLS) returns the child TID to the
 *     parent and 0 to the child;
 *   - getpid(39) returns the group id (the leader's pid) in both threads;
 *   - gettid(186) returns distinct per-thread ids, and the child's id differs
 *     from the group id; the child publishes its tid in the shared page;
 *   - the child writes 0xE7C0FFEE into the shared heap page and the parent
 *     observes it, along with the child's tid (G1.3: one address space, not
 *     a copied page table);
 *   - exit(60) from the child is thread-local: the parent keeps running and
 *     prints its marker afterwards; exit_group(231) then reaps the group.
 *
 * I5 adds the pthread-shaped exit glue to the SAME flow, at syscall level:
 *   - clone carries CLONE_CHILD_CLEARTID (0x002D0F00) with args[3] = the
 *     shared page's tid word, and the parent stores the child's tid there —
 *     exactly what pthread_create's parent does;
 *   - the child registers a robust_list_head (set_robust_list, 273) whose one
 *     entry owns a futex word set to the child's tid, and then waits on a
 *     `go` word so it cannot exit before the parent is parked;
 *   - the parent joins by FUTEX_WAIT on the tid word (expecting the tid it
 *     stored). The child's exit must clear that word and futex-wake it, and
 *     must leave the robust word as FUTEX_OWNER_DIED|tid while waking it;
 *   - the parent then checks both words (0 and 0x40000000|tid) and prints
 *     E7TG-CTID-OK and E7TG-ROBUST-OK. The join is accepted as either a wake
 *     (rax 0) or an already-cleared word (-EAGAIN): a preemptible probe can
 *     lose the wake to the timer, and the STATE it checks is the same one
 *     pthread_join relies on. The kernel's own `[E7] clear-TID:` and
 *     `[E7] robust list: ... released 1` lines are checked alongside it.
 *
 * The child makes NO forwarded syscall after clone. A forward parks the
 * calling descriptor, and the sidecar channel's owner is the group leader,
 * so a child's write reply would wake the leader and leave the child parked
 * (per-thread park/wake is I3/I4 work). getpid/gettid/exit are kernel-local,
 * and the shared page is the only channel the child needs here.
 *
 * Serial markers (the positive evidence):
 *   E7TG-START
 *   E7TG-CHILD-OK
 *   E7TG-SHARED-OK
 * Any failure prints E7TG-FAIL and parks; tests/linux_thread_probe_check.sh
 * asserts the three positives, the absence of the failure, one clone line,
 * one thread-exit line, and exactly one group teardown.
 *
 * Build (build_thread_probe.sh; the committed .flat is its byte-exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o /tmp/e7_thread_probe.elf e7_thread_probe.S
 *   objcopy -O binary -j .text /tmp/e7_thread_probe.elf e7_thread_probe.flat
 *
 * Registers: the kernel zeroes every GPR in the child's first context (the
 * clone child returns with rax=0 and rsi=child stack only), so each thread
 * re-derives its state through syscalls. r12-r15 survive a forward's park
 * and resume (park_ctx); the parent's spin counter lives in the shared heap
 * page instead, because the child may run between its reads.
 */
    .text
    .globl _start
_start:
    /* G1.1: the start marker before any thread work. */
    lea     .Lstart(%rip), %rsi
    mov     $1, %edi
    mov     $.Lstart_end-.Lstart, %edx
    mov     $1, %eax
    syscall

    /* Map two heap pages: the first is the shared word, the second is the
     * child's user stack (the kernel never allocates a clone stack). */
    xor     %rdi, %rdi
    mov     $12, %eax
    syscall
    test    %rax, %rax
    jz      .Lfail
    test    $0xfff, %rax
    jnz     .Lfail
    mov     %rax, %r12               /* r12 = heap base */
    lea     0x2000(%r12), %r13
    mov     %r13, %rdi
    mov     $12, %eax
    syscall
    cmp     %r13, %rax
    jne     .Lfail

    /* clone(flags, child_stack, ptid, ctid, tls): the Go shape plus I5's
     * CLONE_CHILD_CLEARTID, with args[3] pointing at the shared tid word
     * (0x18 in page 0) — the flag and the word pthread_create passes. */
    mov     $0x002D0F00, %edi
    lea     0x2000(%r12), %rsi
    xor     %edx, %edx
    lea     0x18(%r12), %r10
    lea     _start(%rip), %r8
    mov     $56, %eax
    syscall
    test    %rax, %rax
    js      .Lfail                    /* clone refused: negative errno */
    jz      .Lchild                   /* the child's clone return is 0 */

    /* ── parent ──────────────────────────────────────────────────────── */
    mov     %rax, %r14                /* r14 = child tid */
    mov     $39, %eax                 /* getpid */
    syscall
    mov     %rax, %r15                /* r15 = tgid */
    mov     $186, %eax                /* gettid */
    syscall
    cmp     %r15, %rax                /* the leader's tid == tgid */
    jne     .Lfail
    cmp     %r14, %rax                /* the child's tid differs */
    je      .Lfail

    /* ── I5: the pthread join, at syscall level ────────────────────────
     * The tid word is what CLONE_CHILD_CLEARTID names, so the parent owns it
     * exactly as pthread_create's parent does: store the child's tid there,
     * release the child from its `go` spin, and park on the word. The child's
     * exit must clear it (and wake this wait) — a clear that never comes is
     * the hang this increment removes. */
    movl    %r14d, 0x18(%r12)         /* tid word: the joiner's key */
    movl    $1, 0x20(%r12)            /* go: the child may exit now   */
    lea     0x18(%r12), %rdi
    mov     $128, %esi                /* FUTEX_WAIT_PRIVATE           */
    mov     %r14d, %edx               /* expected: the tid we stored  */
    xor     %r10d, %r10d              /* no timeout                   */
    mov     $202, %eax
    syscall
    /* 0 = the exit woke us; -11 (EAGAIN) = it cleared the word before this
     * park, which is the same state. Nothing else is a join. */
    test    %rax, %rax
    jz      .Ljoined
    cmp     $-11, %rax
    jne     .Lfail
.Ljoined:
    cmpl    $0, 0x18(%r12)            /* the word the exit glue cleared */
    jne     .Lfail
    movl    0x148(%r12), %eax         /* the robust word the exit marked */
    mov     %r14d, %ecx
    or      $0x40000000, %ecx         /* FUTEX_OWNER_DIED | child tid     */
    cmp     %ecx, %eax
    jne     .Lfail
    lea     .Lctid_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lctid_ok_end-.Lctid_ok, %edx
    mov     $1, %eax
    syscall
    lea     .Lrobust_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lrobust_ok_end-.Lrobust_ok, %edx
    mov     $1, %eax
    syscall

    movq    $0, 8(%r12)               /* bounded wait for the child's write */
.Lwait:
    cmpl    $0xE7C0FFEE, (%r12)
    jne     .Lwait_more
    cmpq    %r14, 0x10(%r12)          /* and the tid the child published */
    je      .Lshared
.Lwait_more:
    incq    8(%r12)
    cmpq    $0x2000000, 8(%r12)
    jb      .Lwait
    jmp     .Lfail                    /* the shared write never arrived */

.Lshared:
    /* CHILD-OK is printed by the parent, not the child: the child made no
     * forwarded syscall, and the two shared words above are its evidence. */
    lea     .Lchild_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lchild_ok_end-.Lchild_ok, %edx
    mov     $1, %eax
    syscall

    /* Settle window: the child wrote the shared word BEFORE its exit(60), so
     * give the scheduler time to run it through that exit before this thread
     * asks for the group kill. Without this the parent's exit_group can reap
     * the group while the child is still live, which would hide the
     * thread-local teardown this probe exists to observe. */
    movq    $0, 8(%r12)
.Lsettle:
    incq    8(%r12)
    cmpq    $0x4000000, 8(%r12)
    jb      .Lsettle

    lea     .Lshared_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lshared_ok_end-.Lshared_ok, %edx
    mov     $1, %eax
    syscall
    mov     $42, %edi
    mov     $231, %eax                /* exit_group */
    syscall
    jmp     .Lhang

    /* ── child (rax == 0, every other GPR zero) ──────────────────────── */
.Lchild:
    /* The clone context carries rsi = the child stack (heap_base+0x2000),
     * so the base is one subtraction away. Do NOT call brk(0): the parent
     * already grew the break past the base, and Linux semantics return the
     * CURRENT break, not the span's base. */
    lea     -0x2000(%rsi), %r12

    mov     $39, %eax                 /* getpid -> the group id */
    syscall
    mov     %rax, %r13
    mov     $186, %eax                /* gettid -> this thread's id */
    syscall
    mov     %rax, %r14
    cmp     %r13, %r14                /* a non-leader tid != tgid */
    je      .Lfail

    movl    $0xE7C0FFEE, (%r12)       /* G1.3: write through the shared root */
    movq    %r14, 0x10(%r12)          /* publish this thread's own tid */

    /* ── I5: register a robust list owning one mutex word ──────────────
     * head at +0x100 {list.next, futex_offset, list_op_pending}, one entry at
     * +0x140 whose next points back at the head (the list terminator) and
     * whose lock word sits at entry+8 = +0x148 — the same "offset from the
     * embedded list node to the futex word" shape glibc hands the kernel. */
    lea     0x100(%r12), %r15         /* head                        */
    lea     0x140(%r12), %rax         /* the one entry               */
    movq    %r15, (%rax)              /* entry.next = &head (last)   */
    movq    %rax, (%r15)              /* head.list.next = &entry     */
    movq    $8, 8(%r15)               /* futex_offset = +8           */
    movq    $0, 16(%r15)              /* list_op_pending = NULL      */
    movl    %r14d, 0x148(%r12)        /* this thread owns the word   */
    mov     %r15, %rdi
    mov     $24, %esi                 /* sizeof(struct robust_list_head) */
    mov     $273, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail

    /* Wait for the parent's go — it is set only after the parent has stored
     * the tid word and is about to park on it, so the exit's wake has a
     * waiter in the ordinary case (and the parent tolerates the timer
     * preempting that one-instruction window). */
.Lchild_wait:
    cmpl    $1, 0x20(%r12)
    jne     .Lchild_wait

    xor     %edi, %edi
    mov     $60, %eax                 /* exit: thread-local, group survives */
    syscall
    jmp     .Lhang

.Lfail:
    lea     .Lfail_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfail_msg_end-.Lfail_msg, %edx
    mov     $1, %eax
    syscall
.Lhang:
    jmp     .Lhang

.Lstart:
    .ascii  "E7TG-START\n"
.Lstart_end:
.Lchild_ok:
    .ascii  "E7TG-CHILD-OK\n"
.Lchild_ok_end:
.Lshared_ok:
    .ascii  "E7TG-SHARED-OK\n"
.Lshared_ok_end:
.Lctid_ok:
    .ascii  "E7TG-CTID-OK\n"
.Lctid_ok_end:
.Lrobust_ok:
    .ascii  "E7TG-ROBUST-OK\n"
.Lrobust_ok_end:
.Lfail_msg:
    .ascii  "E7TG-FAIL\n"
.Lfail_msg_end: