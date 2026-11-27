/* e7_fault_abandon_probe.S — E7 increment 35 (P1-C): the live arm for the
 * frame a handler LEAVES instead of returning from.
 *
 * Increments 32–34's mask window is founded on the handler still being in the
 * frame its delivery built. A handler that leaves by longjmp never sends the
 * 15 that closes it: siglongjmp restores rsp to the sigsetjmp caller — the
 * interrupted context's side of the frame, strictly ABOVE it — and from then
 * on the kernel would answer that task's rt_sigprocmask out of a copy the
 * sidecar never saw. Increment 35 closes the window on the first 14 made with
 * rsp above the recorded frame (kernel/syscall_dispatch.c); this arm is the
 * boot proof.
 *
 * The arm, in order:
 *
 *   1. rt_sigaction(11) — SIGSEGV runs .Lhandler, with a real restorer (the
 *      kernel refuses a delivery it cannot return from).
 *   2. rt_sigprocmask(SIG_BLOCK, {SIGTERM}) — forwarded, so the sidecar's
 *      copy and the kernel's staged copy both hold 0x4000. This is the mask
 *      the frame RECORDS: uc_sigmask is the PRE-delivery blocked set.
 *   3. the setjmp half: the interrupted rsp is saved at 0(%r12).
 *   4. a PROT_NONE read — the #PF enters .Lhandler.
 *   5. .Lhandler blocks SIGUSR1 (0x200) — an edit answered by the KERNEL's
 *      window copy (0x4400 -> 0x4600) and never told to the sidecar — then
 *      leaves the way siglongjmp does: pop rsp to the saved value and jump.
 *   6. the first 14 AFTER the jump is the call under test: the kernel must
 *      log the abandonment, put the mask back to the frame's recorded
 *      0x4000, drop the frame, and hand the call back to the sidecar, whose
 *      oldset answer is 0x4000.
 *
 * Why each line is a proof, not a claim. If the window stayed open, the query
 * would be answered by the kernel from its own 0x4600 and E7AB-MASK is
 * written; a closure that dropped the frame but kept the dead handler's mask
 * writes the same. If the frame were not dropped, the 15 that follows would
 * restore it and the faulting load would run again — the entry counter turns
 * that into E7AB-LOOP rather than a hang. E7AB-OK is written only when the
 * query says 0x4000 AND the 15 says -EINVAL.
 *
 * The handler deliberately does not perform siglongjmp's own mask restore (a
 * 14 made INSIDE the frame): that call is kernel-owned and irrelevant to the
 * abandonment, and omitting it keeps the abandoned window's mask visible —
 * the handler's 0x4600 is the edit the closure drops, named in shim doc
 * §10.30.
 *
 * Markers: E7AB-ARM on set-up, E7AB-LEAP from the handler just before the
 * jump, E7AB-OK from the resumed code, E7AB-MASK on a wrong mask, E7AB-RET
 * if 15 restored a frame it should have found gone, E7AB-LOOP if the handler
 * was entered twice, E7AB-FAIL on a refused set-up.
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

    /* One shared page: 0 = the saved rsp, 8 = the queried oldset,
     * 16 = handler entries. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    mov     $3, %edx                /* PROT_READ|PROT_WRITE       */
    mov     $0x22, %r10             /* MAP_PRIVATE|MAP_ANONYMOUS  */
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lfail
    mov     %rax, %r12
    movq    $0, 16(%r12)

    /* rt_sigaction(11, act, NULL, 8): SA_SIGINFO, a real restorer, no mask.
     * The 32-byte word order is this tree's: handler, flags, mask, restorer. */
    sub     $64, %rsp
    lea     .Lhandler(%rip), %rax
    mov     %rax, 0(%rsp)
    movq    $4, 8(%rsp)             /* SA_SIGINFO                 */
    movq    $0, 16(%rsp)
    lea     .Lrestorer(%rip), %rax
    mov     %rax, 24(%rsp)
    mov     %rsp, %rsi
    mov     $11, %edi
    xor     %edx, %edx
    mov     $8, %r10d
    mov     $13, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail
    add     $64, %rsp

    /* rt_sigprocmask(SIG_BLOCK, {SIGTERM}, NULL, 8): a FORWARDED 14, so both
     * ends hold 0x4000 and the frame records it as its uc_sigmask. */
    sub     $16, %rsp
    movq    $0x4000, 0(%rsp)        /* the 8-byte sigset: 1 << (15-1) */
    xor     %edi, %edi              /* SIG_BLOCK                     */
    mov     %rsp, %rsi
    xor     %edx, %edx              /* oldset = NULL                 */
    mov     $8, %r10d
    mov     $14, %eax
    syscall
    add     $16, %rsp
    test    %rax, %rax
    jne     .Lfail

    /* The setjmp half: where the longjmp comes back to. */
    mov     %rsp, 0(%r12)

    /* A PROT_NONE page, then the read that faults. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    xor     %edx, %edx              /* PROT_NONE                     */
    mov     $0x22, %r10
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lfail
    movl    (%rax), %eax            /* -> #PF, delivered to .Lhandler */

    /* Reached only if the handler RETURNED (it never should): the frame it
     * was given is still live, so say so by name rather than hang. */
    jmp     .Lret

    /* ── entered in ring 3 by the kernel's frame, not called by this file ──
     * rdi = 11, rsi = &siginfo, rdx = &ucontext; r12 is the interrupted
     * value (the shared page), restored by the frame. */
.Lhandler:
    incq    16(%r12)
    cmpq    $2, 16(%r12)
    jae     .Lloop
    cmpl    $11, (%rsi)             /* si_signo: this is the fault's */
    jne     .Lbad

    /* The handler's own edit, made INSIDE the frame: the kernel's window copy
     * answers it (0x4400 -> 0x4600) and the sidecar is told nothing. */
    sub     $16, %rsp
    movq    $0x200, 0(%rsp)         /* SIGUSR1: 1 << (10-1)           */
    xor     %edi, %edi              /* SIG_BLOCK                      */
    mov     %rsp, %rsi
    xor     %edx, %edx              /* oldset = NULL                  */
    mov     $8, %r10d
    mov     $14, %eax
    syscall
    add     $16, %rsp
    test    %rax, %rax
    jne     .Lfail

    lea     .Lleap(%rip), %rsi
    mov     $1, %edi
    mov     $.Lleap_end-.Lleap, %edx
    mov     $1, %eax
    syscall

    /* The longjmp: pop rsp back to the setjmp caller, above the frame. */
    mov     0(%r12), %rsp
    jmp     .Lafter

.Lafter:
    /* The call under test: a 14 made after the frame was left. Its answer must
     * be the frame's recorded 0x4000, from the sidecar's own copy — not the
     * dead handler's 0x4600 out of a kernel that still thinks it owns them. */
    mov     $2, %edi                /* SIG_SETMASK (ignored: set NULL) */
    xor     %esi, %esi
    lea     8(%r12), %rdx           /* the oldset                   */
    mov     $8, %r10d
    mov     $14, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail
    mov     8(%r12), %rax
    cmp     $0x4000, %rax
    jne     .Lmask

    /* The frame must be GONE: 15 is the sidecar's -EINVAL again, not a restore
     * that would put the interrupted context (and its faulting load) back. */
    mov     $15, %eax
    syscall
    cmp     $-22, %rax
    jne     .Lret

    lea     .Lok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok, %edx
    mov     $1, %eax
    syscall
    xor     %edi, %edi
    mov     $60, %eax
    syscall

/* The frame's pretcode: a real, nonzero address so the delivery is one the
 * kernel will build. Reached only if a 15 was handed back with no frame. */
.Lrestorer:
    mov     $15, %eax
    syscall
    jmp     .Lret

.Lret:
    lea     .Lretmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lretmsg_end-.Lretmsg, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

.Lmask:
    lea     .Lmaskmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lmaskmsg_end-.Lmaskmsg, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

.Lloop:
    lea     .Lloopmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lloopmsg_end-.Lloopmsg, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

.Lbad:
    lea     .Lbadmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lbadmsg_end-.Lbadmsg, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

.Lfail:
    lea     .Lfailmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfailmsg_end-.Lfailmsg, %edx
    mov     $1, %eax
    syscall
.Lhang:
    jmp     .Lhang

.Larm:
    .ascii  "E7AB-ARM\n"
.Larm_end:
.Lleap:
    .ascii  "E7AB-LEAP\n"
.Lleap_end:
.Lok:
    .ascii  "E7AB-OK\n"
.Lok_end:
.Lmaskmsg:
    .ascii  "E7AB-MASK\n"
.Lmaskmsg_end:
.Lretmsg:
    .ascii  "E7AB-RET\n"
.Lretmsg_end:
.Lloopmsg:
    .ascii  "E7AB-LOOP\n"
.Lloopmsg_end:
.Lbadmsg:
    .ascii  "E7AB-BAD\n"
.Lbadmsg_end:
.Lfailmsg:
    .ascii  "E7AB-FAIL\n"
.Lfailmsg_end:
