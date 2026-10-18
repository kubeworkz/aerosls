/* e7_linux_probe.S — E7's first Linux-personality task, as a payload
 * (AeroSLS-Linux-ABI-Shim-Design-v0.1.md §3.3 steps 5-7, boot proof).
 *
 * Freestanding, position-independent, ONE .text section, _start at the
 * section's start — the kernel's flat sidecar-image mapper
 * (kernel/cap.c cap_create_sidecar_in_personality step 5) copies the file
 * at USER_PROC_CODE_BASE and enters at image_entry = offset 0.
 *
 * Build (the committed .flat is the byte-exact output; build_probe.sh):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o probe.elf e7_linux_probe.S
 *   objcopy -O binary -j .text probe.elf e7_linux_probe.flat
 *
 * The sequence is a machine-checked argument about the round trip:
 *   218 (set_tid_address) — §5.6's store-and-ignore: the answer must be
 *       the argument echo itself (args[0] -> rax). A mismatch issues the
 *       MARKER number 391, whose ABSENCE the boot evidence asserts.
 *   39  — outside the census: the sidecar answers CAP_ENOSYS (-10) and
 *       §5.7's map must deliver -38 into rax, never the raw CAP code. A
 *       mismatch issues MARKER 392 (also asserted absent).
 *   40  — a third full round trip; its arrival is the sidecar's
 *       `[E7] linux: num=40 -> ENOSYS` line.
 *   158 (arch_prctl, ARCH_SET_FS) — §5.3's one kernel semantic: the grant
 *       must MOVE the MSR or the %fs read below walks into unmapped linear
 *       0 and this task dies with every following line absent from the log;
 *       its success latches as number 187 (0xBB, present only on success)
 *       and its wrong-place variant issues MARKER 393 (absent, like 391/392).
 *   1 (write, the fd class) — E7 increment 5's byte crossing: the bytes
 *       live in THIS task's address space (a separate cr3 by §3.5), so
 *       the kernel must copy them out before the sidecar's fd table can
 *       perform them on the console. The string .Lmsg is the POSITIVE
 *       marker: it reaches the serial log iff plan, copy, message,
 *       payload check, fd table and console all held, and iff it did, rax
 *       must equal its length. The same write to a descriptor that does
 *       not exist must answer -EBADF exactly; anything else — a short or
 *       failed write, a wrong error — issues MARKER 394 (asserted
 *       ABSENT, the 391/392/393 discipline).
 *   12 (brk, the mem class) — E7 increment 6's budget span (§5.4): brk(0)
 *       must come back as a page-aligned, nonzero break; brk(base+0x2000)
 *       must ECHO the request (the grant); the fresh heap page must READ
 *       ZERO (the kernel zero-fills every frame it maps — a recycled
 *       frame must not carry its last tenant's bytes into this task);
 *       writing "E7MEM-BRK-OK\n" into that page and write(1, page, 13)
 *       must return 13 — the bytes crossing through increment 5's seam
 *       FROM THE HEAP ITSELF is what proves the mapping is real; brk far
 *       past the 64 KiB budget must come back as the UNCHANGED current
 *       break (Linux's brk failure shape — the current break, NOT
 *       -ENOMEM); mprotect(W|X) must be exactly -EPERM. Each wrong
 *       outcome issues its own MARKER — 395 (the seam lied: bad base or
 *       grant echo), 396 (a fresh heap page was not zero), 397 (the
 *       budget refusal moved the break anyway), 398 (W|X was not
 *       refused) — all asserted ABSENT, the 391/392 discipline.
 *   231 (exit_group) — the sidecar reaps; no reply is owed and this task
 *       never runs again (the hang below is unreachable by design).
 *
 * Registers: immediates only, and the preserved values live in r12–r15 —
 * callee-saved, pushed by the entry stub and restored by the park/resume
 * (park_ctx) — because the kernel's SLS ABI does NOT promise the argument
 * registers survive a syscall return.
 */
    .text
    .globl _start
_start:
    /* 1. set_tid_address(0x1DECAF): rax must return 0x1DECAF. */
    mov     $0x1DECAF, %r12
    mov     %r12, %rdi
    mov     $218, %eax
    syscall
    cmp     %r12, %rax
    je      .Ltid_ok
    mov     $391, %eax          /* MARKER: 218's ret did not echo the arg */
    syscall
.Ltid_ok:
    /* 2. syscall 39 (outside the census): rax must return -38 (-ENOSYS),
     *    which only happens if the sidecar's CAP status was mapped. */
    mov     $39, %eax
    syscall
    cmp     $-38, %rax
    je      .Lenosys_ok
    mov     $392, %eax          /* MARKER: the reply status was not mapped */
    syscall
.Lenosys_ok:
    /* 3. a third round trip: 40 must ARRIVE (the sidecar logs it). */
    mov     $40, %eax
    syscall
    /* 4. arch_prctl(ARCH_SET_FS, &_start) — §5.3's one kernel semantic.
     *    rdi = cmd 0x1002, rsi = a user-canonical MAPPED address (the
     *    image's own _start: USER_PROC_CODE_BASE + 0). Outcomes, each
     *    visible in the serial log:
     *      - DENIED: rax != 0 -> .Lhang, and NO latch line appears;
     *      - granted but the MSR did not move: %fs:0 reads linear 0
     *        (never mapped) -> #PF -> this task dies -> nothing below
     *        runs, not even exit_group;
     *      - success: direct read == %fs read, latch 187 (0xBB) issues,
     *        and the sidecar's `[E7] linux: num=000000BB -> ENOSYS` is the
     *        POSITIVE marker — its presence IS the assertion;
     *      - the base moved but to the wrong place: MARKER 393 (asserted
     *        ABSENT — the 391/392 discipline).
     *
     *    Register discipline, learned from the live boot that faulted
     *    here: the target address rides r15 (callee-saved — park_ctx
     *    preserves r12..r15), NEVER r8: the resume clobbers caller-saved
     *    registers, and the first run read `(%r8)` with r8 left at a
     *    kernel scratch value (fault addr 0x16/0x10 across two boots).
     *    And the %fs operand is displacement 0, not the address again:
     *    linear = FS_BASE + disp, so with FS_BASE = _start the read that
     *    proves the base landed is %fs:0 — adding _start twice would ask
     *    for 0x8000_0000_0000, non-canonical by construction. */
    lea     _start(%rip), %r15
    mov     %r15, %rsi
    mov     $0x1002, %edi          /* ARCH_SET_FS */
    mov     $158, %eax
    syscall
    test    %rax, %rax
    jnz     .Lhang                 /* refused: parked forever, no latch */
    mov     (%r15), %r13           /* the eight bytes at _start, direct  */
    mov     %fs:0, %r14            /* FS_BASE + 0 == _start iff applied  */
    cmp     %r13, %r14
    je      .Lfs_ok
    mov     $393, %eax             /* MARKER: base wrong-but-mapped */
    syscall
    jmp     .Lfs_done
.Lfs_ok:
    mov     $187, %eax             /* LATCH: the applied FS base read back */
    syscall
.Lfs_done:
    /* 5. write(1, .Lmsg, .Lmsgend-.Lmsg) — the fd class (E7 increment 5):
     *    the kernel's copy out of THIS task's address space, then the
     *    sidecar's fd table, then the console. rax must equal the count;
     *    then the same write to descriptor 0x77 (never opened — open() is
     *    still ENOSYS, so no fd beyond 0/1/2 can exist) must come back
     *    exactly -EBADF (-9, CAP_EBADF's map), not a fabricated number and
     *    not a success. Either wrong issues MARKER 394, asserted ABSENT by
     *    the evidence. Argument registers are set fresh immediately before
     *    each trap — only r12/r15 survive the park, and neither is needed
     *    past the fs block above. */
    lea     .Lmsg(%rip), %rsi
    mov     $1, %edi                /* fd 1 — the task's console      */
    mov     $.Lmsgend-.Lmsg, %edx   /* count: the message's own length */
    mov     $1, %eax
    syscall
    cmp     $.Lmsgend-.Lmsg, %rax
    jne     .Lfd_bad                /* failed or short: not a write   */
    lea     .Lmsg(%rip), %rsi
    mov     $0x77, %edi             /* a descriptor that cannot exist */
    mov     $.Lmsgend-.Lmsg, %edx
    mov     $1, %eax
    syscall
    cmp     $-9, %rax               /* -EBADF, and only -EBADF        */
    je      .Lfd_done
.Lfd_bad:
    mov     $394, %eax              /* MARKER: the fd-class seam lied */
    syscall
.Lfd_done:
    /* 5b. brk(0) — the mem class (E7 increment 6, §5.4): the budget
     *    span's base must come back page-aligned and nonzero (the span
     *    sits directly above this task's stack, decided at create from
     *    the manifest's BUDGET record). r12 holds it across every park
     *    below — callee-saved, like the fs block's r15. */
    xor     %rdi, %rdi
    mov     $12, %eax
    syscall
    test    %rax, %rax
    jz      .Lmem_bad            /* zero is not a break */
    test    $0xfff, %rax
    jnz     .Lmem_bad            /* the span starts page-aligned */
    mov     %rax, %r12           /* r12 = heap base */
    /* 5c. brk(base+0x2000) — a growth INSIDE the 64 KiB budget must be
     *    granted: rax must echo the request exactly (the kernel maps the
     *    granted pages with fresh partition-charged frames; the sidecar's
     *    ledger and the kernel's stamped span are the same truth). */
    lea     0x2000(%r12), %r13
    mov     %r13, %rdi
    mov     $12, %eax
    syscall
    cmp     %r13, %rax
    jne     .Lmem_bad            /* the grant did not echo the request */
    /* 5d. the fresh heap page must READ ZERO — the kernel zero-fills
     *    every frame it maps (the pool recycles frames across tasks and
     *    partitions; an unzeroed frame would leak its last tenant's
     *    bytes into this task's heap). A non-zero read is a different
     *    failure than a bad grant, hence its own marker. */
    cmpq    $0, (%r12)
    jne     .Lmem_zero_bad
    /* 5e. the memory is REAL: build "E7MEM-BRK-OK\n" (13 bytes) IN the
     *    new heap page, then write(1, page, 13) through increment 5's
     *    seam — the kernel copies the bytes out of this task's OWN
     *    freshly-mapped heap, the fd table performs them on the console.
     *    The string on serial IS the proof the page exists and is
     *    writable; rax must return 13. */
    movabs  $0x52422D4D454D3745, %rax    /* "E7MEM-BR" (little-endian) */
    mov     %rax, (%r12)
    movabs  $0x00000A4B4F2D4B, %rax      /* "K-OK\n" + NULs            */
    mov     %rax, 8(%r12)
    lea     (%r12), %rsi
    mov     $1, %edi                /* fd 1 — the task's console      */
    mov     $13, %edx
    mov     $1, %eax
    syscall
    cmp     $13, %rax
    jne     .Lmem_bad            /* the heap write failed or came back short */
    /* 5f. brk(base+128 KiB) — past the 64 KiB budget: the break must NOT
     *    move, and the answer must be the UNCHANGED current break
     *    (r13), which is Linux's own brk failure shape — the current
     *    break, NOT -ENOMEM. A negative rax here would be a lie no
     *    libc's brk wrapper is written against. */
    lea     0x20000(%r12), %r14
    mov     %r14, %rdi
    mov     $12, %eax
    syscall
    cmp     %r13, %rax
    jne     .Lmem_budget_bad
    /* 5g. mprotect(heap, 0x1000, PROT_WRITE|PROT_EXEC) — §5.4's W^X
     *    refusal must come back exactly -EPERM (-1): simultaneous
     *    write+execute is refused by the shim itself, and -1 is what the
     *    §5.7 map delivers for CAP_EPERM (-12). prot is 0x6 — WRITE(0x2)
     *    | EXEC(0x4); 0x3 would be READ|WRITE, which must NOT be refused. */
    mov     %r12, %rdi
    mov     $0x1000, %esi
    mov     $6, %edx             /* PROT_WRITE|PROT_EXEC */
    mov     $10, %eax
    syscall
    cmp     $-1, %rax
    je      .Lmem_done
    mov     $398, %eax           /* MARKER: W|X was not refused as -EPERM */
    syscall
    jmp     .Lmem_done
.Lmem_bad:
    mov     $395, %eax           /* MARKER: the brk seam lied (base/echo) */
    syscall
    jmp     .Lmem_done
.Lmem_zero_bad:
    mov     $396, %eax           /* MARKER: a fresh heap page was not zero */
    syscall
    jmp     .Lmem_done
.Lmem_budget_bad:
    mov     $397, %eax           /* MARKER: the budget refusal moved it */
    syscall
.Lmem_done:
    /* 6. exit_group(42): the sidecar reaps this task. */
    mov     $42, %edi
    mov     $231, %eax
    syscall
.Lhang:
    jmp     .Lhang

    /* The fd class's positive marker: printed on the console only if the
     * whole seam performed it (build_probe.sh keeps this in .text — one
     * section, _start at offset 0). */
.Lmsg:
    .ascii  "E7FD-WRITE-OK\n"
.Lmsgend:
