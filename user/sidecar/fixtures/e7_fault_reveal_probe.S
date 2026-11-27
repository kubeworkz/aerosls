/* e7_fault_reveal_probe.S — E7 increment 34 (P1-C): the REVEAL half of an
 * unmask inside a trap-delivered handler.
 *
 * Increment 33 made the kernel the mask's authority for that window, so
 * rt_sigprocmask there reports the handler's own set. But an unmask has a
 * second half that is not arithmetic: a signal that was BLOCKED and is now
 * UNBLOCKED must be REVEALED — discarded, or its default action run right
 * inside the call, exactly where Linux's raise() dies on the unmask. Answering
 * locally left that bit pending until after the handler returned.
 *
 * The arm, in order:
 *
 *   1. set_tid_address(0xE7A11D) — this shim's tid IS that echo, so tkill
 *      below has a target.
 *   2. rt_sigaction(11) — SIGSEGV runs .Lhandler, with a real restorer (the
 *      kernel refuses a delivery it cannot return from).
 *   3. rt_sigprocmask(SIG_BLOCK, {SIGTERM}) — SIGTERM is now blocked, and its
 *      disposition is still SIG_DFL (a fresh task's), whose action is
 *      TERMINATE.
 *   4. tkill(tid, SIGTERM) — blocked, so it PENDS and the call returns 0.
 *   5. a PROT_NONE read — the #PF enters .Lhandler in ring 3.
 *   6. .Lhandler: rt_sigprocmask(SIG_UNBLOCK, {SIGTERM}). The bit was pending
 *      BEFORE the fault, so the reveal's default action must run HERE and the
 *      task must die INSIDE its handler.
 *
 * Why that is a proof and not a claim. If the reveal does not run inside the
 * handler, the unmask returns 0, the handler writes E7RV-AFTER and exits — so
 * the failure is a NAMED line, and the boot's `sig=15 default-action -> kill`
 * line is the positive: it can only come from the sidecar's reveal ladder
 * deciding that this pending signal terminates, on a syscall the handler
 * issued. Nothing else in this arm raises SIGTERM, and nothing else in the
 * boot blocks one before a fault, so the line's position between E7RV-ARM and
 * any E7RV-AFTER attributes it to this window.
 *
 * Markers: E7RV-ARM on set-up, E7RV-AFTER if the unblock returned without
 * revealing, E7RV-OK if the handler even returned (reached only on the same
 * failure), E7RV-FAIL on a refused set-up.
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

    /* The tid: set_tid_address (218 in this shim, not 96 — the census's own
     * number) echoes its argument, and tkill matches it at u32 width. */
    mov     $0xE7A11D, %edi
    mov     $218, %eax
    syscall
    test    %rax, %rax
    jz      .Lfail
    mov     %rax, %rbx              /* the tid, for tkill            */

    /* rt_sigaction(11, act, NULL, 8): SA_SIGINFO, a real restorer, no mask. */
    sub     $64, %rsp
    lea     .Lhandler(%rip), %rax
    mov     %rax, 0(%rsp)
    movq    $4, 8(%rsp)             /* SA_SIGINFO                    */
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

    /* rt_sigprocmask(SIG_BLOCK, {SIGTERM}, NULL, 8): forward the bit. */
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

    /* tkill(tid, SIGTERM): blocked, so it PENDS and answers 0. */
    mov     %rbx, %rdi
    mov     $15, %esi
    mov     $200, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail

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
    mov     %rax, %r12
    movl    (%r12), %eax            /* -> #PF, delivered to .Lhandler */

    /* Reached only if the handler RETURNED, i.e. the unblock revealed
     * nothing: the positive proof of the fix is the task dying above. */
    lea     .Lok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok, %edx
    mov     $1, %eax
    syscall
    xor     %edi, %edi
    mov     $60, %eax
    syscall

    /* ── entered in ring 3 by the kernel's frame, not called by this file ──
     * rdi = 11, rsi = &siginfo, rdx = &ucontext. */
.Lhandler:
    /* rt_sigprocmask(SIG_UNBLOCK, {SIGTERM}, NULL, 8): the call under test. */
    sub     $16, %rsp
    movq    $0x4000, 0(%rsp)        /* the same single-word sigset   */
    mov     $1, %edi                /* SIG_UNBLOCK                   */
    mov     %rsp, %rsi
    xor     %edx, %edx
    mov     $8, %r10d
    mov     $14, %eax
    syscall
    add     $16, %rsp

    /* Still alive: the pending SIGTERM was NOT revealed inside the handler. */
    lea     .Lafter(%rip), %rsi
    mov     $1, %edi
    mov     $.Lafter_end-.Lafter, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

/* The frame's pretcode: a real, nonzero address so the delivery is one the
 * kernel will build and can be returned from if this arm ever does. */
.Lrestorer:
    mov     $15, %eax
    syscall
    jmp     .Lfail

.Lfail:
    lea     .Lfailmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfailmsg_end-.Lfailmsg, %edx
    mov     $1, %eax
    syscall
.Lhang:
    jmp     .Lhang

.Larm:
    .ascii  "E7RV-ARM\n"
.Larm_end:
.Lafter:
    .ascii  "E7RV-AFTER\n"
.Lafter_end:
.Lok:
    .ascii  "E7RV-OK\n"
.Lok_end:
.Lfailmsg:
    .ascii  "E7RV-FAIL\n"
.Lfailmsg_end:
