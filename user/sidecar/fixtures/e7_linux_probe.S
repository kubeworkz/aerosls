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
 *   231 (exit_group) — the sidecar reaps; no reply is owed and this task
 *       never runs again (the hang below is unreachable by design).
 *
 * Registers: immediates only, and the one preserved value lives in r12 —
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
