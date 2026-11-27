/* e7_thread_groupexit_probe.S — E7 TG-1 I5b's EXIT_GROUP arm.
 *
 * The same live promise as the fault arm, on the other path I5 never covered:
 * the leader asks for exit_group (231) while one cloned thread still holds a
 * CLEARTID word and a robust list, and a second cloned thread is parked on
 * that word. The group's teardown must clear the word and wake the parked
 * joiner (n=1), walk the robust list, and only then destroy the group — with
 * no exit(60) from the thread that owned the words.
 *
 * Layout (offsets from the brk base):
 *   0x28  the joiner's "about to park" flag
 *   0x30  ts.tv_sec / 0x38 ts.tv_nsec — a 20 ms relative wait used as a delay
 *   0x58  the owning thread's ctid word (CLONE_CHILD_CLEARTID)
 *   0x60  the owning thread's "robust list registered" flag
 *   0x200 struct robust_list_head, 0x240 the one entry, 0x248 its futex word
 *
 * The leader clones the owning thread at site #1 (it registers the robust
 * list, then spins forever — it must be alive when the group is told to go)
 * and the joiner at site #2 (it parks on 0x58). Once both flags are up and a
 * two-tick delay has passed, the leader calls exit_group.
 *
 * Markers: E7TD-GROUP-ARM on the set-up; E7TD-FAIL on any refusal.
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

    /* clone site #1: the owning thread, CLEARTID on 0x58. An EAGAIN refusal
     * is the process table being momentarily full; retry two ticks later,
     * bounded (the same retry glibc does). */
    mov     $200, %rbx
.Lclone1_try:
    mov     $0x002D0F00, %edi
    lea     0x3000(%r12), %rsi
    xor     %edx, %edx
    lea     0x58(%r12), %r10
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
    jz      .Lchild_owner
    mov     %rax, %r14
    movl    %r14d, 0x58(%r12)

    /* clone site #2: the joiner, no CLEARTID. */
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

    /* Both halves must be in place before the group is told to go: the joiner
     * parked, the owner's robust list registered. */
.Lready:
    cmpl    $1, 0x28(%r12)
    jne     .Lready
    cmpl    $1, 0x60(%r12)
    jne     .Lready

    /* Two ticks of relative wait (the 20 ms timespec at 0x30/0x38) so the
     * joiner is parked, not between its flag and its park. */
    call    .Ldelay

    mov     $42, %edi
    mov     $231, %eax              /* exit_group: the whole group goes */
    syscall
    jmp     .Lhang

    /* ── the owning thread (rip = clone site #1's child) ──────────────── */
.Lchild_owner:
    lea     -0x3000(%rsi), %r12
    mov     $186, %eax
    syscall
    mov     %rax, %r14

    lea     0x200(%r12), %r15
    lea     0x240(%r12), %rax
    movq    %r15, (%rax)
    movq    %rax, (%r15)
    movq    $8, 8(%r15)
    movq    $0, 16(%r15)
    movl    %r14d, 0x248(%r12)
    mov     %r15, %rdi
    mov     $24, %esi
    mov     $273, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail
    movl    $1, 0x60(%r12)
.Lown_spin:
    jmp     .Lown_spin

    /* ── the joiner (rip = clone site #2's child) ────────────────────── */
.Lchild_join:
    lea     -0x2000(%rsi), %r12
    movl    $1, 0x28(%r12)
.Lj_try:
    movl    0x58(%r12), %edx
    test    %edx, %edx
    jz      .Lhang                  /* already cleared: nothing to join */
    lea     0x58(%r12), %rdi
    mov     $128, %esi
    xor     %r10d, %r10d
    mov     $202, %eax
    syscall
    cmp     $-11, %rax
    je      .Lj_try
    jmp     .Lhang

/* Two ticks of relative wait on 0x20, a word nobody ever writes: the delay
 * used both by the clone retry and before exit_group. Clobbers rax/rdi/rsi/
 * rdx/r10 only; r12 is the shared page. */
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
    .ascii  "E7TD-GROUP-ARM\n"
.Larm_end:
.Lfail_msg:
    .ascii  "E7TD-FAIL\n"
.Lfail_msg_end:
