/* e7_fault_handler_probe.S — E7 increment 31 (P1-C): the live arm that a
 * ring-3 #PF with a handler installed RUNS that handler instead of killing
 * the thread, and that the handler is told the fault's ADDRESS.
 *
 * The program installs SIGSEGV with SA_SIGINFO and a restorer, then reads a
 * PROT_NONE page. Before increment 31 the fault path killed unconditionally,
 * so the handler below could never run. On the delivered path the kernel
 * builds a frame, records the interrupted registers, copies the frame onto
 * this thread's stack and returns into .Lhandler with
 *   rdi = 11 (SIGSEGV), rsi = &siginfo, rdx = &ucontext.
 *
 * What makes this a proof rather than a claim: the handler checks
 *   si_signo == SIGSEGV
 *   si_code  == SEGV_MAPERR or SEGV_ACCERR (whichever the #PF error code says)
 *   si_addr  == the PROT_NONE page THIS program mapped (kept in r13, which
 *               the frame restores because r13 is one of the interrupted
 *               registers — a frame that recorded the handler's own context
 *               would fail this comparison)
 * and writes "E7FH-OK" only when all three hold. It then exits 0 with
 * exit(60): the handler does not return, because a fault-delivered handler's
 * rt_sigreturn is not wired yet (§10.26's named remainder).
 *
 * Markers: E7FH-ARM on set-up, E7FH-OK from inside the handler,
 * E7FH-BAD on any mismatch, E7FH-FAIL on a refused set-up.
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

    /* Two heap pages beyond the base: page 0 is the shared words (the trap
     * page's address lives at 0x48), page 1 is headroom. */
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
     * A read of it takes a Ring-3 #PF and, with the handler installed, a
     * delivered SIGSEGV whose si_addr is this page. */
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

    /* rt_sigaction(11, act, NULL, 8) — SA_SIGINFO, a real restorer (the
     * kernel refuses a delivery it cannot return from), no extra mask. The
     * 32-byte struct's word order is this tree's wire order, shared with the
     * sidecar: handler, flags, mask, restorer. */
    sub     $64, %rsp
    lea     .Lhandler(%rip), %rax
    mov     %rax, 0(%rsp)           /* sa_handler                 */
    movq    $4, 8(%rsp)             /* SA_SIGINFO                 */
    movq    $0, 16(%rsp)            /* sa_mask                    */
    lea     .Lrestorer(%rip), %rax
    mov     %rax, 24(%rsp)          /* sa_restorer                */
    mov     %rsp, %rsi
    mov     $11, %edi
    xor     %edx, %edx              /* oldact = NULL              */
    mov     $8, %r10d               /* sigsetsize                 */
    mov     $13, %eax
    syscall
    test    %rax, %rax
    jne     .Lfail
    add     $64, %rsp

    /* r13 carries the trap page into the handler: the frame records the
     * INTERRUPTED r13, so the handler compares si_addr against this. */
    mov     0x48(%r12), %r13
    movl    (%r13), %eax            /* read the PROT_NONE page -> #PF */
    jmp     .Lfail                  /* unreachable on a delivered fault */

    /* ── entered in ring 3 by the kernel's frame, not called by this file ── */
.Lhandler:
    cmp     $11, %edi               /* si_signo */
    jne     .Lbad
    mov     8(%rsi), %eax           /* si_code  */
    cmp     $1, %eax                /* SEGV_MAPERR */
    je      .Lh_code_ok
    cmp     $2, %eax                /* SEGV_ACCERR — a soft-protected page */
    jne     .Lbad
.Lh_code_ok:
    mov     16(%rsi), %rax          /* si_addr — must be the trap page */
    cmp     %r13, %rax
    jne     .Lbad
    lea     .Lok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok, %edx
    mov     $1, %eax
    syscall
    xor     %edi, %edi
    mov     $60, %eax               /* exit(0): no rt_sigreturn from here */
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

/* Never executed: the frame's pretcode must be a real address, and the
 * kernel refuses a zero one. It is here so a future return trip has a lane. */
.Lrestorer:
    mov     $15, %eax
    syscall
    jmp     .Lbad

.Lfail:
    lea     .Lfailmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfailmsg_end-.Lfailmsg, %edx
    mov     $1, %eax
    syscall
.Lhang:
    jmp     .Lhang

.Larm:
    .ascii  "E7FH-ARM\n"
.Larm_end:
.Lok:
    .ascii  "E7FH-OK\n"
.Lok_end:
.Lbadmsg:
    .ascii  "E7FH-BAD\n"
.Lbadmsg_end:
.Lfailmsg:
    .ascii  "E7FH-FAIL\n"
.Lfailmsg_end:
