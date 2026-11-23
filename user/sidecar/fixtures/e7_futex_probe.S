/* e7_futex_probe.S — E7 TG-1 increment I3's native futex probe.
 *
 * Freestanding, one .text section, _start at file offset 0 (the flat mapper
 * enters at image_entry = 0, like e7_thread_probe.S).
 *
 * The probe exercises G1.4 on a real boot:
 *   - brk(0)/brk(base+0x2000) maps the shared word page and the child's stack;
 *   - clone(56) with Go's thread shape creates one sibling in the group;
 *   - the parent FUTEX_WAIT_PRIVATE (202, op 128) on a zero word parks;
 *   - the child sets the word and FUTEX_WAKE_PRIVATE (129) returns 1, so the
 *     parent's wait returns 0 — a real block, not a busy loop;
 *   - the parent's second wait on a word nobody writes, with a 20 ms relative
 *     timespec, returns -ETIMEDOUT (110) through the timer tick;
 *   - the child observes the parent's stop flag and exit(60)s; the parent's
 *     exit_group(231) tears the group down once.
 *
 * ─── Why the child loops ──────────────────────────────────────────────────
 * A single set-then-wake would be racy: the timer can preempt the parent
 * between clone's return and its first futex_wait, letting the child's wake
 * run with no waiter. So the child is a WAKER LOOP (set gate, wake, reset,
 * short delay) and the parent retries an EAGAIN wait. Every round where the
 * parent is parked produces a real wake=1; a round that misses is harmless
 * because the parent's value re-check answers EAGAIN and it waits again. The
 * parent is parked most of the time, which is also what lets the timeout
 * phase block: the child stays runnable throughout.
 *
 * The child makes NO forwarded syscall: gettid/futex/exit are kernel-local,
 * and v1's channel owner is the group leader, so a child's write would park
 * it and wake the leader. All child-to-parent evidence travels through the
 * group's shared page.
 *
 * Serial markers (the positive evidence):
 *   E7FX-START        the program ran and cloned
 *   E7FX-WAIT-OK      the parent's wait returned 0 with a published wake=1
 *   E7FX-TIMEOUT-OK   the parent's relative timeout returned ETIMEDOUT
 *   E7FX-OK           the whole exchange completed
 * Any failure prints E7FX-FAIL and hangs; tests/linux_futex_probe_check.sh
 * asserts the positives, the absence of the failure, one clone line, a parked
 * wait, a woken=1 line, the child's thread exit and exactly one group
 * teardown for this probe's group.
 *
 * Build (build_futex_probe.sh; the committed .flat is its byte-exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o /tmp/e7_futex_probe.elf e7_futex_probe.S
 *   objcopy -O binary -j .text /tmp/e7_futex_probe.elf e7_futex_probe.flat
 *
 * Shared page layout (base in r12):
 *   0x00 u32 gate        child: 1 before each wake, 0 after
 *   0x04 u32 woke        child: 1 once a wake returned 1
 *   0x08 u32 stop        parent: 1 when the exchange is done
 *   0x0c u32 seen        child: 1 after it observes stop
 *   0x10 u32 wait_ret    parent: phase-1 futex_wait result (debug)
 *   0x18 u64 child_tid   child: its own gettid (the shared write)
 *   0x20 u32 gate2       the never-written word the timeout waits on
 *   0x28 timespec        {sec = 0, nsec = 20000000}: 20 ms = 2 ticks
 *   0x38 u64 rounds      child: per-round delay counter
 *   0x40 u64 spins       parent: bounded-wait counter
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

    /* Two pages: the shared words, then the child's user stack. */
    xor     %rdi, %rdi
    mov     $12, %eax                 /* brk(0) */
    syscall
    test    %rax, %rax
    jz      .Lfail
    test    $0xfff, %rax
    jnz     .Lfail
    mov     %rax, %r12                /* r12 = heap base */
    lea     0x2000(%r12), %r13
    mov     %r13, %rdi
    mov     $12, %eax                 /* brk(base + 0x2000) */
    syscall
    cmp     %r13, %rax
    jne     .Lfail

    /* clone(flags, child_stack, 0, 0, tls): the measured Go shape. */
    mov     $0x000D0F00, %edi
    lea     0x2000(%r12), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    lea     _start(%rip), %r8
    mov     $56, %eax
    syscall
    test    %rax, %rax
    js      .Lfail                    /* clone refused */
    jz      .Lchild                   /* child's clone return is 0 */

    /* ── parent ──────────────────────────────────────────────────────── */
    mov     %rax, %r14                /* r14 = child tid from clone */
    xor     %r15d, %r15d              /* r15 = wait attempts */
.Lwait_retry:
    mov     %r12, %rdi                /* uaddr = gate */
    mov     $128, %esi                /* FUTEX_WAIT_PRIVATE */
    xor     %edx, %edx                /* expected value 0 */
    xor     %r10d, %r10d              /* no timeout */
    mov     $202, %eax
    syscall
    test    %rax, %rax
    jz      .Lwaited                  /* 0: the child's wake reached us */
    cmp     $-11, %rax                /* EAGAIN: the gate changed first */
    jne     .Lfail
    inc     %r15
    cmp     $1000000, %r15
    jb      .Lwait_retry
    jmp     .Lfail

.Lwaited:
    movl    %eax, 0x10(%r12)          /* wait_ret, for the log's truth */

    /* The waker publishes woke=1 after its wake returns; give it time to
     * finish that store even if the timer preempted it at the syscall. */
    movq    $0, 0x40(%r12)
.Lwoke_wait:
    cmpl    $1, 4(%r12)
    je      .Lwoke_ok
    incq    0x40(%r12)
    cmpq    $0x2000000, 0x40(%r12)
    jb      .Lwoke_wait
    jmp     .Lfail
.Lwoke_ok:
    mov     0x18(%r12), %rbx          /* child-published gettid */
    cmp     %r14, %rbx                /* equals the clone's return tid */
    jne     .Lfail

    lea     .Lwait_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lwait_ok_end-.Lwait_ok, %edx
    mov     $1, %eax
    syscall

    /* Phase 2: a relative timeout on a word nobody writes. The child keeps
     * running, so the parent may park; the timer tick must resume it. */
    movq    $0, 0x28(%r12)            /* ts.tv_sec  = 0 */
    movq    $20000000, 0x30(%r12)     /* ts.tv_nsec = 20 ms */
    lea     0x20(%r12), %rdi          /* uaddr = gate2 (stays 0) */
    mov     $128, %esi
    xor     %edx, %edx
    lea     0x28(%r12), %r10
    mov     $202, %eax
    syscall
    cmp     $-110, %rax               /* -ETIMEDOUT */
    jne     .Lfail

    lea     .Ltimeout_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Ltimeout_ok_end-.Ltimeout_ok, %edx
    mov     $1, %eax
    syscall

    /* Let the child observe the stop and take its thread-local exit before
     * exit_group reaps the group (so the live log shows both halves). */
    movl    $1, 8(%r12)               /* stop = 1 */
    movq    $0, 0x40(%r12)
.Lseen_wait:
    cmpl    $1, 0x0c(%r12)
    je      .Lseen_ok
    incq    0x40(%r12)
    cmpq    $0x2000000, 0x40(%r12)
    jb      .Lseen_wait
    jmp     .Lfail
.Lseen_ok:
    movq    $0, 0x40(%r12)
.Lsettle:
    incq    0x40(%r12)
    cmpq    $0x4000000, 0x40(%r12)
    jb      .Lsettle

    lea     .Lok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok, %edx
    mov     $1, %eax
    syscall
    xor     %edi, %edi
    mov     $231, %eax                /* exit_group */
    syscall
    jmp     .Lhang

    /* ── child (rax == 0, every other GPR zero) ──────────────────────── */
.Lchild:
    /* The clone context carries rsi = the child stack (base+0x2000): the
     * heap base is one subtraction away. Do not brk(0): the parent already
     * grew the break past the base. */
    lea     -0x2000(%rsi), %r12

    mov     $186, %eax                /* gettid */
    syscall
    mov     %rax, 0x18(%r12)          /* the shared write the parent checks */

.Lloop:
    cmpl    $1, 8(%r12)               /* stop? */
    je      .Lchild_done

    movl    $1, (%r12)                /* gate = 1 */
    mov     %r12, %rdi
    mov     $129, %esi                /* FUTEX_WAKE_PRIVATE */
    mov     $1, %edx
    mov     $202, %eax
    syscall
    cmpq    $1, %rax
    jne     .Lchild_reset
    movl    $1, 4(%r12)               /* a real wake: publish it */

.Lchild_reset:
    movl    $0, (%r12)                /* reset for the next round */
    movq    $0, 0x38(%r12)
.Ldelay:
    incq    0x38(%r12)
    cmpq    $0x100000, 0x38(%r12)
    jb      .Ldelay
    jmp     .Lloop

.Lchild_done:
    movl    $1, 0x0c(%r12)            /* seen = 1 */
    xor     %edi, %edi
    mov     $60, %eax                 /* exit: thread-local */
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
    .ascii  "E7FX-START\n"
.Lstart_end:
.Lwait_ok:
    .ascii  "E7FX-WAIT-OK\n"
.Lwait_ok_end:
.Ltimeout_ok:
    .ascii  "E7FX-TIMEOUT-OK\n"
.Ltimeout_ok_end:
.Lok:
    .ascii  "E7FX-OK\n"
.Lok_end:
.Lfail_msg:
    .ascii  "E7FX-FAIL\n"
.Lfail_msg_end:
