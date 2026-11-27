/* e7_thread_fault_probe.S — E7 TG-1 I5b's FAULT arm.
 *
 * The live proof that the thread-exit glue runs when a thread does NOT call
 * exit(60): a cloned thread dies of a Ring-3 #PF while a sibling in its group
 * is parked on that thread's TID word. Before I5b the fault path ran no glue,
 * so the joiner below would have stayed parked on a tid that never went away;
 * the check asserts the kernel's clear line with a real wake count (n=1), the
 * robust walk, and that no thread-exit line exists for the faulting TID.
 *
 * Layout (offsets from the brk base):
 *   0x20  go: the leader sets it once the joiner has had time to park
 *   0x28  the joiner's "about to park" flag
 *   0x30  ts.tv_sec / 0x38 ts.tv_nsec — the leader's 200 ms settle window
 *   0x40  the faulting thread's "robust list registered" flag
 *   0x50  the leader's own park word (never written, never woken)
 *   0x78  the faulting thread's ctid word (set by the leader, CLONE_CHILD_CLEARTID)
 *   0x100 struct robust_list_head {next, futex_offset, list_op_pending}
 *   0x140 the one entry {next}, 0x148 its futex word (= this thread's tid)
 *
 * Ordering is the leader's, not luck's: it waits for BOTH children's flags,
 * then sleeps 200 ms (twenty ticks) so the joiner is inside FUTEX_WAIT rather
 * than between its flag store and its syscall, and only then sets `go`. The
 * faulting thread spins on `go` and dies at once, so the wake the glue issues
 * always has a waiter to find. (Measured: with a 20 ms window and the fault
 * thread driving its own delay, the joiner was cloned but never scheduled in
 * time and the clear woke nobody — n=0, the arm failing for a scheduling
 * reason instead of testing what it claims.)
 *
 * The leader clones the faulting thread at clone site #1 and the joiner at
 * site #2, then parks on 0x50. The children resume at the call site that
 * cloned THEM — the shim captures the parent's return address as the child's
 * rip — which is how the two arms are told apart without argv.
 *
 * Markers: E7TD-FAULT-ARM on the set-up; E7TD-FAIL on any refusal.
 *
 * Build: build_thread_death.sh (the committed .flat is its byte-exact output).
 */
    .text
    .globl _start
_start:
    lea     .Larm(%rip), %rsi
    mov     $1, %edi
    mov     $.Larm_end-.Larm, %edx
    mov     $1, %eax
    syscall

    /* Two heap pages beyond the base: page 0 is the shared words, page 1 is
     * the joiner's stack, page 2 is the faulting thread's stack. */
    xor     %rdi, %rdi
    mov     $12, %eax
    syscall
    test    %rax, %rax
    jz      .Lfail
    test    $0xfff, %rax
    jnz     .Lfail
    mov     %rax, %r12
    lea     0x3000(%r12), %r13
    mov     %r13, %rdi
    mov     $12, %eax
    syscall
    cmp     %r13, %rax
    jne     .Lfail

    /* The trap: one PROT_NONE page (the measured allocator's guard shape).
     * Any access to it takes a Ring-3 #PF with no handler installed, which is
     * the kill path this arm exists to exercise. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    xor     %edx, %edx              /* PROT_NONE                  */
    mov     $0x22, %r10             /* MAP_PRIVATE|MAP_ANONYMOUS  */
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lfail
    mov     %rax, 0x48(%r12)

    /* clone site #1: the thread that will fault, CLEARTID on the shared word.
     * A refusal with EAGAIN is the process table being momentarily full (16
     * descriptors, and this boot's probe fleet fills them); glibc retries
     * that, and so does this probe — two ticks, then ask again, bounded. */
    mov     $200, %rbx
.Lclone1_try:
    mov     $0x002D0F00, %edi
    lea     0x3000(%r12), %rsi
    xor     %edx, %edx
    lea     0x78(%r12), %r10
    lea     _start(%rip), %r8
    mov     $56, %eax
    syscall
    test    %rax, %rax
    jns     .Lclone1_ok
    cmp     $-11, %rax
    jne     .Lfail
    dec     %rbx
    jz      .Lfail
    call    .Ldelay
    jmp     .Lclone1_try
.Lclone1_ok:
    test    %rax, %rax
    jz      .Lchild_fault
    mov     %rax, %r14
    movl    %r14d, 0x78(%r12)       /* the joiner's key, before it can read it */

    /* clone site #2: the joiner, no CLEARTID (it registers nothing). */
    mov     $200, %rbx
.Lclone2_try:
    mov     $0x000D0F00, %edi
    lea     0x2000(%r12), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    lea     _start(%rip), %r8
    mov     $56, %eax
    syscall
    test    %rax, %rax
    jns     .Lclone2_ok
    cmp     $-11, %rax
    jne     .Lfail
    dec     %rbx
    jz      .Lfail
    call    .Ldelay
    jmp     .Lclone2_try
.Lclone2_ok:
    test    %rax, %rax
    jz      .Lchild_join

    /* ── leader: wait for both children, settle, release the fault ────── */
.Ll_ready:
    cmpl    $1, 0x28(%r12)          /* the joiner stored its flag */
    jne     .Ll_ready
    cmpl    $1, 0x40(%r12)          /* the owner registered its list */
    jne     .Ll_ready
    /* 200 ms: twenty ticks is far more than the joiner needs to get from its
     * flag store into the parked state, and this thread cannot be preempted
     * out of a relative futex wait without resuming (the timeout does). */
    movq    $0, 0x30(%r12)
    movq    $200000000, 0x38(%r12)
    lea     0x20(%r12), %rdi        /* go, still 0 */
    mov     $128, %esi
    xor     %edx, %edx
    lea     0x30(%r12), %r10
    mov     $202, %eax
    syscall
    movl    $1, 0x20(%r12)          /* go: the faulting thread may die */
    lea     0x50(%r12), %rdi        /* the leader parks, group stays alive */
    mov     $128, %esi
    xor     %edx, %edx
    xor     %r10d, %r10d
    mov     $202, %eax
    syscall
    jmp     .Lhang

    /* ── the thread that dies by fault (rip = clone site #1's child) ──── */
.Lchild_fault:
    lea     -0x3000(%rsi), %r12
    mov     $186, %eax              /* gettid                     */
    syscall
    mov     %rax, %r14

    lea     0x100(%r12), %r15
    lea     0x140(%r12), %rax
    movq    %r15, (%rax)            /* entry.next = &head (last)  */
    movq    %rax, (%r15)            /* head.list.next = &entry    */
    movq    $8, 8(%r15)             /* futex_offset = +8          */
    movq    $0, 16(%r15)            /* list_op_pending = NULL     */
    movl    %r14d, 0x148(%r12)      /* this thread owns the word  */
    mov     %r15, %rdi
    mov     $24, %esi
    mov     $273, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail
    movl    $1, 0x40(%r12)          /* registered */

.Lc_go:
    cmpl    $1, 0x20(%r12)          /* the leader released the fault */
    jne     .Lc_go
    mov     0x48(%r12), %rbx
    movl    (%rbx), %eax            /* read the PROT_NONE page -> #PF */
    jmp     .Lfail                  /* unreachable: a mapped read means
                                     * the soft leaf never took effect */

    /* ── the joiner (rip = clone site #2's child) ────────────────────── */
.Lchild_join:
    lea     -0x2000(%rsi), %r12
    movl    $1, 0x28(%r12)          /* "about to park" */
.Lj_try:
    movl    0x78(%r12), %edx        /* expected: the tid stored */
    test    %edx, %edx
    jz      .Lhang                  /* already cleared: nothing to join */
    lea     0x78(%r12), %rdi
    mov     $128, %esi
    xor     %r10d, %r10d
    mov     $202, %eax
    syscall
    cmp     $-11, %rax              /* EAGAIN: the word moved under us */
    je      .Lj_try
    jmp     .Lhang

/* Two ticks of relative wait on 0x48's neighbour word 0x20 (a word that is 0
 * until the leader sets it), used by the clone retries. */
.Ldelay:
    movq    $0, 0x30(%r12)
    movq    $20000000, 0x38(%r12)
    lea     0x20(%r12), %rdi
    mov     $128, %esi
    xor     %edx, %edx
    lea     0x30(%r12), %r10
    mov     $202, %eax
    syscall
    ret

.Lfail:
    lea     .Lfail_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfail_msg_end-.Lfail_msg, %edx
    mov     $1, %eax
    syscall
.Lhang:
    jmp     .Lhang

.Larm:
    .ascii  "E7TD-FAULT-ARM\n"
.Larm_end:
.Lfail_msg:
    .ascii  "E7TD-FAIL\n"
.Lfail_msg_end:
