/* e7_fault_return_probe.S — E7 increment 32 (P1-C): the live arm that a
 * handler entered by a ring-3 #PF can RETURN through rt_sigreturn(15) into
 * the instruction that faulted.
 *
 * Increment 31 proved the delivery: a #PF with SIGSEGV installed runs the
 * handler and the handler is told the fault's address. Its arm then called
 * exit(60), because rt_sigreturn had no way back for a frame the sidecar
 * never delivered — 15 answered EINVAL ("no handler was entered"), which is
 * its own bookkeeping talking, and the return was the kernel's to give.
 *
 * What makes this a proof rather than a claim. The handler VERIFIES the
 * delivery and then FIXES the fault in the frame, the way real handlers do:
 *
 *   1. si_signo == SIGSEGV, si_code is MAPERR/ACCERR, and si_addr is the
 *      PROT_NONE page this program mapped (kept at 0(%r12), an address the
 *      frame restored because r12 is one of the interrupted registers).
 *   2. It then writes the interrupted r13 in its own ucontext —
 *      80(%rdx) = uc_mcontext.gregs[REG_R13] — to point at a page holding
 *      0xfeedface, and RETURNS through the restorer, which issues 15.
 *   3. The kernel restores the frame, so the retried `movl (%r13), %eax`
 *      reads the GOOD page: eax == 0xfeedface, and r13 equals the pointer
 *      the handler wrote. Only then is E7FR-OK written.
 *
 * Step 3 is why this cannot pass by accident. If 15 never restored, the
 * retried load faults again; the handler counts its own entries and writes
 * E7FR-LOOP on the second, so a missing return shows up as a NAMED line
 * instead of a silent hang. If the restore ignored the handler's edit, r13
 * would still be the trap page and the count would be the same. If it
 * restored a stale or wrong context, the load or the r13 comparison fails
 * and E7FR-BAD says which side of the contract broke.
 *
 * Markers: E7FR-ARM on set-up, E7FR-OK from the resumed program,
 * E7FR-BAD on any mismatch, E7FR-LOOP if the handler ran twice,
 * E7FR-FAIL on a refused set-up.
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

    /* One shared page: 0 = the trap page, 8 = the good page,
     * 16 = how many times the handler has been entered. */
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
    test    $0xfff, %rax
    jnz     .Lfail
    mov     %rax, %r12
    movq    $0, 16(%r12)

    /* The trap: a PROT_NONE page, exactly the allocator's guard shape. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    xor     %edx, %edx              /* PROT_NONE                  */
    mov     $0x22, %r10
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lfail
    mov     %rax, 0(%r12)           /* the page the fault must name */

    /* The page the handler will point r13 at, holding the sentinel. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    mov     $3, %edx
    mov     $0x22, %r10
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lfail
    test    $0xfff, %rax
    jnz     .Lfail
    mov     %rax, 8(%r12)
    movl    $0xfeedface, (%rax)     /* what the retried load reads */

    /* rt_sigaction(11, act, NULL, 8): SA_SIGINFO, a real restorer (the
     * kernel refuses a delivery it cannot return from), no extra mask. The
     * 32-byte struct's word order is this tree's wire order, shared with the
     * sidecar: handler, flags, mask, restorer. */
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

    /* The trap. On a delivered fault the handler fixes r13 and returns; the
     * kernel resumes HERE, at this very instruction, which then reads the
     * good page. */
    mov     0(%r12), %r13
    movl    (%r13), %eax            /* read the PROT_NONE page -> #PF */

    /* Resumed: the load above ran again, at the handler's r13. */
    cmpl    $0xfeedface, %eax
    jne     .Lbad
    cmp     8(%r12), %r13
    jne     .Lbad
    lea     .Lok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok, %edx
    mov     $1, %eax
    syscall
    xor     %edi, %edi
    mov     $60, %eax
    syscall

    /* ── entered in ring 3 by the kernel's frame, not called by this file ──
     * rdi = 11, rsi = &siginfo, rdx = &ucontext; r12 is the interrupted
     * value (the shared page), restored by the frame. */
.Lhandler:
    cmp     $11, %edi
    jne     .Lbad
    mov     8(%rsi), %eax           /* si_code */
    cmp     $1, %eax                /* SEGV_MAPERR */
    je      .Lh_code_ok
    cmp     $2, %eax                /* SEGV_ACCERR */
    jne     .Lbad
.Lh_code_ok:
    mov     16(%rsi), %rax          /* si_addr */
    cmp     0(%r12), %rax           /* …must be THIS program's trap page */
    jne     .Lbad

    /* E7 increment 33: ask for the mask this handler is RUNNING under.
     * `set` NULL is a query, so `how` is ignored; the answer must be the
     * handler's whole set — the pre-delivery 0 | sa_mask (0) | SIGSEGV's
     * bit = 0x400 — because the #PF delivery moved the KERNEL's mask and
     * the sidecar, which never heard of it, still holds the narrower 0.
     * A 0 here is exactly the defect this arm exists to catch. */
    sub     $16, %rsp
    mov     %rdx, 8(%rsp)           /* 3rd arg is the oldset here: keep
                                     * the ucontext pointer, which the fix
                                     * below writes through */
    mov     $2, %edi                /* SIG_SETMASK (ignored: set NULL) */
    xor     %esi, %esi
    mov     %rsp, %rdx
    mov     $8, %r10d
    mov     $14, %eax
    syscall
    mov     (%rsp), %rax
    mov     8(%rsp), %rdx           /* the ucontext pointer back */
    add     $16, %rsp
    cmp     $0x400, %rax
    jne     .Lmask

    /* Count the entries: the second one means the first return did not
     * reach the interrupted instruction, so say so by name. */
    incq    16(%r12)
    cmpq    $2, 16(%r12)
    jae     .Lloop

    /* Fix the fault where a real handler does — in the ucontext's own
     * gregs. 80(%rdx) is uc + 40 (uc_mcontext) + REG_R13*8; the host test
     * asserts that offset against the frame's sigcontext layout. */
    mov     8(%r12), %rax
    mov     %rax, 80(%rdx)
    ret                             /* -> .Lrestorer -> rt_sigreturn(15) */

.Lloop:
    lea     .Lloopmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lloopmsg_end-.Lloopmsg, %edx
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

.Lbad:
    lea     .Lbadmsg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lbadmsg_end-.Lbadmsg, %edx
    mov     $1, %eax
    syscall
    mov     $1, %edi
    mov     $60, %eax
    syscall

/* The frame's pretcode: the handler's `ret` lands here, and its 15 is the
 * return under test. Reached again only if the kernel hands 15 back with an
 * errno instead of resuming — which is the failure this arm exists to see. */
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
    .ascii  "E7FR-ARM\n"
.Larm_end:
.Lok:
    .ascii  "E7FR-OK\n"
.Lok_end:
.Lmaskmsg:
    .ascii  "E7FR-MASK\n"
.Lmaskmsg_end:
.Lbadmsg:
    .ascii  "E7FR-BAD\n"
.Lbadmsg_end:
.Lloopmsg:
    .ascii  "E7FR-LOOP\n"
.Lloopmsg_end:
.Lfailmsg:
    .ascii  "E7FR-FAIL\n"
.Lfailmsg_end:
