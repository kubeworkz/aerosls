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
 *   5g-bis. mprotect(PROT_NONE) then mprotect(PROT_READ|PROT_WRITE) on
 *       the SAME heap page: the page must fault while parked, then read and
 *       write again with its 18-byte contents unchanged. The pattern
 *       "E7MEM-MPROTECT-OK\n" is written to the page, parked, restored,
 *       compared byte for byte, and finally written out through fd 1 — so
 *       the marker's presence proves the restore returned the same frame and
 *       the bytes survived. A failed park, restore, comparison or output
 *       issues MARKER 412, 413, 414 or 415 respectively (asserted ABSENT).
 *   9 (mmap, the mem class) — E7 increment 16's anonymous-map half
 *       (§5.4): mmap(NULL, 4096, RW, ANON|PRIVATE) must come back
 *       page-aligned, nonzero and INSIDE the span (the ledger's cursor
 *       hands out the span's top page); that page must READ ZERO and
 *       carry "E7MEM-MMAP-OK\n" out through write(1, page, 14) — the
 *       bytes crossing FROM THE MAPPED PAGE, the brk twin; the guard
 *       mmap(base, 4096, PROT_NONE, FIXED|ANON|PRIVATE) must echo the
 *       base EXACTLY (Linux MAP_FIXED replaces the brk heap's first
 *       page — never touched again below); mmap(W|X) must be exactly
 *       -EPERM (the §5.4 twin); and a 1 MiB arena — 16x the span —
 *       must be exactly -ENOMEM (the budget shape; unlike brk, mmap's
 *       failure shape IS an errno). Each wrong outcome issues its own
 *       MARKER — 399 (the arena grant lied: errno, alignment or
 *       span), 400 (a fresh arena page was not zero), 401 (the FIXED
 *       guard did not echo the base), 402 (mmap's W|X was not refused),
 *       403 (the over-budget arena was not -ENOMEM) — asserted ABSENT,
 *       the 391/392 discipline.
 *   11 (munmap, the mem class) — E7 increment 25's last memory seam
 *       (§5.4): munmap(arena, 0x1000) must succeed (rax 0), a second
 *       call over the same range must also succeed (POSIX: the kernel's
 *       page tables are the truth, and there is nothing left to free),
 *       and munmap(base, 0x1000) below the current break must be
 *       exactly -EINVAL (-22) — v1 keeps the break's pages with brk.
 *       The successful unmap writes "E7MEM-MUNMAP-OK\n" (16 bytes)
 *       through increment 5's seam; any wrong outcome issues MARKER 404,
 *       asserted ABSENT with the map block's.
 *   231 (exit_group) — the sidecar reaps; no reply is owed and this task
 *       never runs again (the hang below is unreachable by design).
 *
 * Registers: immediates only, and the preserved values live in r12–r15 —
 * callee-saved, pushed by the entry stub and restored by the park/resume
 * (park_ctx) — because the kernel's SLS ABI does NOT promise the argument
 * registers survive a syscall return. The mmap block's FLAGS argument
 * rides %r10, never %rcx: SYSCALL itself clobbers rcx (the psABI's fourth
 * syscall argument register is r10) — measured: flags in rcx arrive as 0
 * and the arena request is refused by name.
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
    /* 2b. 105 is Linux setuid AND SLS SYS_SLS_ALLOCATE.  This task is
     *     Linux-personality, so the answer must be -ENOSYS (-38), the
     *     census refusal; 0 would be the SLS allocation arm answering for
     *     it.  The SLS sidecar prints E7SIG-SLS-105-OK from the same
     *     number, so the two markers are one two-personality regression. */
    xor     %rdi, %rdi
    mov     $105, %eax
    syscall
    cmp     $-38, %rax
    jne     .Llnx105_bad
    lea     .Llnx105_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Llnx105_end-.Llnx105_msg, %edx
    mov     $1, %eax
    syscall
    cmp     $.Llnx105_end-.Llnx105_msg, %rax
    je      .Llnx105_done
.Llnx105_bad:
    mov     $411, %eax           /* MARKER: 105 took the SLS arm */
    syscall
.Llnx105_done:
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
    je      .Lmprotect           /* the W|X twin passed: the round trip */
    mov     $398, %eax           /* MARKER: W|X was not refused as -EPERM */
    syscall
    jmp     .Lmem_done

    /* 5g-bis. The SAME heap page crosses PROT_NONE and back. The first
     *    mprotect parks the present leaf's frame; the second restores that
     *    frame READ|WRITE. The comparisons below are reads through the
     *    restored translation, and the byte write at offset 18 is the
     *    writability half; the 18-byte pattern goes out through fd 1 from
     *    the page itself. r12 is the heap base throughout. */
.Lmprotect:
    movabs  $0x504D2D4D454D3745, %rax    /* "E7MEM-MP" little-endian */
    mov     %rax, (%r12)
    movabs  $0x4F2D544345544F52, %rax    /* "ROTECT-O" little-endian */
    mov     %rax, 8(%r12)
    movabs  $0x0000000000000A4B, %rax    /* "K\n" + NULs             */
    mov     %rax, 16(%r12)

    mov     %r12, %rdi
    mov     $0x1000, %esi
    xor     %edx, %edx             /* PROT_NONE                       */
    mov     $10, %eax
    syscall
    test    %rax, %rax
    jnz     .Lmprotect_park_bad

    mov     %r12, %rdi
    mov     $0x1000, %esi
    mov     $3, %edx               /* PROT_READ|PROT_WRITE            */
    mov     $10, %eax
    syscall
    test    %rax, %rax
    jnz     .Lmprotect_rw_bad

    movabs  $0x504D2D4D454D3745, %rax
    cmp     %rax, (%r12)
    jne     .Lmprotect_content_bad
    movabs  $0x4F2D544345544F52, %rax
    cmp     %rax, 8(%r12)
    jne     .Lmprotect_content_bad
    movabs  $0x0000000000000A4B, %rax
    cmp     %rax, 16(%r12)
    jne     .Lmprotect_content_bad

    movb    $0x21, 18(%r12)         /* a byte past the marker          */
    cmpb    $0x21, 18(%r12)
    jne     .Lmprotect_content_bad

    mov     %r12, %rsi
    mov     $1, %edi
    mov     $18, %edx
    mov     $1, %eax
    syscall
    cmp     $18, %rax
    jne     .Lmprotect_write_bad
    jmp     .Lmmap

    /* 5h. mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, ANON|PRIVATE) — the
     *    measured allocator's arena shape (increment 16). The answer
     *    must be page-aligned, nonzero, and INSIDE the span: below
     *    base+64 KiB, where the ledger's descending cursor hands out
     *    the span's TOP page. fd/offset ride -1/0 as the census
     *    measured them (ignored for ANONYMOUS). */
.Lmmap:
    xor     %rdi, %rdi          /* addr = NULL                        */
    mov     $0x1000, %esi       /* len                                */
    mov     $3, %edx            /* PROT_READ|PROT_WRITE               */
    mov     $0x22, %r10         /* flags — R10, not RCX: syscall      */
                                /* itself clobbers rcx (the psABI's    */
                                /* fourth argument register for a      */
                                /* syscall is r10 — measured: rcx lost */
                                /* the flags and the request refused)  */
    mov     $-1, %r8            /* fd  = -1 (ignored)                 */
    xor     %r9, %r9            /* off = 0                            */
    mov     $9, %eax
    syscall
    test    %rax, %rax
    jle     .Lmmap_bad          /* a negative errno, or zero: no arena */
    test    $0xfff, %rax
    jnz     .Lmmap_bad          /* the arena must be page-aligned     */
    lea     0x10000(%r12), %rcx /* the span's limit                   */
    cmp     %rcx, %rax
    jae     .Lmmap_bad          /* outside the span: the ledger lied  */
    mov     %rax, %r14          /* r14 = the granted arena            */
    /* 5i. the arena must READ ZERO — anonymous memory must not carry
     *    the pool's last tenant's bytes (the brk twin, MARKER 396's
     *    rule, over the map's own frames). Then build "E7MEM-MMAP-OK\n"
     *    (14 bytes) IN it and write(1, page, 14) through increment 5's
     *    seam — the bytes crossing FROM THE MAPPED PAGE are what prove
     *    the mapping is real; rax must return 14. */
    cmpq    $0, (%r14)
    jne     .Lmmap_zero_bad
    movabs  $0x4D4D2D4D454D3745, %rax    /* "E7MEM-MM" (little-endian) */
    mov     %rax, (%r14)
    movabs  $0x00000A4B4F2D5041, %rax    /* "AP-OK\n" + NULs          */
    mov     %rax, 8(%r14)
    mov     %r14, %rsi
    mov     $1, %edi             /* fd 1 — the task's console         */
    mov     $14, %edx
    mov     $1, %eax
    syscall
    cmp     $14, %rax
    jne     .Lmmap_bad           /* the arena write failed or short   */
    /* 5j. munmap(arena, 0x1000) — E7 increment 25: the arena's frame
     *    returns through the channel; rax must be 0. A second call over
     *    the same range is success (POSIX): the kernel's page tables are
     *    the truth, so there is nothing left to free. */
    mov     %r14, %rdi
    mov     $0x1000, %esi
    mov     $11, %eax
    syscall
    test    %rax, %rax
    jne     .Lmunmap_bad
    mov     %r14, %rdi
    mov     $0x1000, %esi
    mov     $11, %eax
    syscall
    test    %rax, %rax
    jne     .Lmunmap_bad
    /* 5k. munmap(base, 0x1000) — below the current break: v1 refuses
     *    EINVAL (-22); the break's pages are brk's seam. */
    mov     %r12, %rdi
    mov     $0x1000, %esi
    mov     $11, %eax
    syscall
    cmp     $-22, %rax
    jne     .Lmunmap_bad
    /* 5l. the on-target marker: "E7MEM-MUNMAP-OK\n" (16 bytes) through
     *    the same fd seam. */
    lea     .Lmunmap_msg(%rip), %rsi
    mov     $1, %edi
    mov     $16, %edx
    mov     $1, %eax
    syscall
    cmp     $16, %rax
    jne     .Lmunmap_bad
    /* 5m. mmap(base, 0x1000, PROT_NONE, FIXED|ANON|PRIVATE) — the
     *    measured allocator's guard page, landing ON the brk heap's
     *    first page: Linux MAP_FIXED REPLACES (the old frame returns,
     *    PROT_NONE backs none), and the reply must echo the address
     *    EXACTLY — the one-truth rule over an address. Never touch the
     *    page after this: it is PROT_NONE now, and any access faults. */
    mov     %r12, %rdi          /* addr = the heap base               */
    mov     $0x1000, %esi
    xor     %edx, %edx          /* PROT_NONE                          */
    mov     $0x32, %r10         /* flags in R10 (the syscall ABI)     */
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    cmp     %r12, %rax
    jne     .Lmmap_fixed_bad
    /* 5n. mmap(NULL, 0x1000, PROT_WRITE|PROT_EXEC, ANON|PRIVATE) —
     *    §5.4's W^X refusal must come back exactly -EPERM (-1): the
     *    same rule 5g measured over mprotect, over a prot word that
     *    arrives with a mapping ask. */
    xor     %rdi, %rdi
    mov     $0x1000, %esi
    mov     $6, %edx             /* PROT_WRITE|PROT_EXEC               */
    mov     $0x22, %r10         /* flags in R10 (the syscall ABI)     */
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    cmp     $-1, %rax
    jne     .Lmmap_wx_bad
    /* 5o. mmap(NULL, 1 MiB, RW, ANON|PRIVATE) — the whole ask is 16x
     *    the span, so the answer must be exactly -ENOMEM (-12): the
     *    cursor cannot fit it, and unlike brk's denial echo, mmap's
     *    failure shape IS an errno. */
    xor     %rdi, %rdi
    mov     $0x100000, %esi      /* 1 MiB                              */
    mov     $3, %edx
    mov     $0x22, %r10         /* flags in R10 (the syscall ABI)     */
    mov     $-1, %r8
    xor     %r9, %r9
    mov     $9, %eax
    syscall
    cmp     $-12, %rax
    jne     .Lmmap_budget_bad
    jmp     .Lmem_done           /* the whole seam held: the exit path  */

.Lmmap_bad:
    mov     $399, %eax           /* MARKER: the arena grant lied       */
    syscall
    jmp     .Lmem_done
.Lmmap_zero_bad:
    mov     $400, %eax           /* MARKER: a fresh arena page not zero */
    syscall
    jmp     .Lmem_done
.Lmmap_fixed_bad:
    mov     $401, %eax           /* MARKER: the FIXED guard no echo    */
    syscall
    jmp     .Lmem_done
.Lmmap_wx_bad:
    mov     $402, %eax           /* MARKER: mmap's W|X not refused     */
    syscall
    jmp     .Lmem_done
.Lmmap_budget_bad:
    mov     $403, %eax           /* MARKER: over-budget not -ENOMEM    */
    syscall
    jmp     .Lmem_done
.Lmunmap_bad:
    mov     $404, %eax           /* MARKER: the munmap seam lied         */
    syscall
    jmp     .Lmem_done
.Lmprotect_park_bad:
    mov     $412, %eax           /* MARKER: PROT_NONE was not accepted   */
    syscall
    jmp     .Lmem_done
.Lmprotect_rw_bad:
    mov     $413, %eax           /* MARKER: PROT_READ|WRITE not accepted */
    syscall
    jmp     .Lmem_done
.Lmprotect_content_bad:
    mov     $414, %eax           /* MARKER: contents or write changed    */
    syscall
    jmp     .Lmem_done
.Lmprotect_write_bad:
    mov     $415, %eax           /* MARKER: the pattern write was short  */
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
    /* 5p. M3: install a real handler, deliver a self-signal through
     *    tkill, and return through rt_sigreturn. The act struct lives on
     *    the stack only until the call; the handler's own frame is built
     *    by the kernel below the RSP in force at tkill.
     *
     *    POSITIVE markers: "E7SIG-OK\n" is written ONLY by the handler,
     *    so its presence proves the kernel entered the user address with
     *    the restorer word at [rsp]; "E7SIG-RET\n" is written after the
     *    handler returns, so its presence proves rt_sigreturn restored
     *    the interrupted RIP/RSP. The mask query after that must read 0 —
     *    the handler's blocked SIGUSR1 must not survive the return. A
     *    failed install or a tkill error issues MARKER 405; a mask that
     *    did not restore issues 406. */
    sub     $64, %rsp
    /* M3.2/M3.4: a 4 KiB alternate stack just below this frame, and the
     * handler carries SA_ONSTACK|SA_SIGINFO. r14/r13 are callee-saved, so
     * the handler sees the exact bounds after the tkill park. */
    lea     (%rsp), %r13           /* alt_end = the frame base          */
    lea     -4096(%rsp), %r14      /* alt_sp                            */
    mov     %r14, 32(%rsp)          /* stack_t.ss_sp                     */
    movl    $0, 40(%rsp)            /* stack_t.ss_flags                  */
    mov     $4096, %rax
    mov     %rax, 48(%rsp)          /* stack_t.ss_size                   */
    lea     32(%rsp), %rdi
    xor     %esi, %esi              /* no old stack_t                    */
    mov     $131, %eax              /* sigaltstack                       */
    syscall
    test    %rax, %rax
    jz      .Lsig_alt_ok
    cmp     $-22, %rax
    je      .Lsig_alt_einval
    jmp     .Lsig_alt_bad
.Lsig_alt_ok:
    lea     .Lhandler(%rip), %rax
    mov     %rax, 0(%rsp)          /* sa_handler */
    movq    $0x0c000004, 8(%rsp)   /* SA_RESTORER|SA_ONSTACK|SA_SIGINFO */
    lea     .Lrestorer(%rip), %rax
    mov     %rax, 16(%rsp)         /* sa_restorer */
    movq    $0, 24(%rsp)           /* sa_mask */
    mov     $10, %edi              /* SIGUSR1 */
    mov     %rsp, %rsi
    xor     %edx, %edx             /* no oldact */
    mov     $8, %r10d
    mov     $13, %eax              /* rt_sigaction */
    syscall
    add     $64, %rsp
    test    %rax, %rax
    jnz     .Lsig_bad

    /* Refresh this task's tid (the heap block reused r12), then tkill
     * ourselves with SIGUSR1. An unblocked handler-disposed signal is
     * delivered INSIDE this syscall, so the next instruction is reached
     * only after the handler's rt_sigreturn. */
    lea     .Lmsg(%rip), %rdi
    mov     $218, %eax
    syscall
    mov     %rax, %r12
    mov     %r12, %rdi
    mov     $10, %esi
    mov     $200, %eax
    syscall
    test    %rax, %rax
    jnz     .Lsig_tkill_bad

    /* The handler blocked SIGUSR1 while it ran. The completed tkill's
     * mask was 0, so rt_sigreturn must have restored 0. */
    sub     $8, %rsp
    xor     %edi, %edi             /* how is ignored when set == NULL */
    xor     %esi, %esi
    mov     %rsp, %rdx
    mov     $8, %r10d
    mov     $14, %eax              /* rt_sigprocmask query */
    syscall
    mov     (%rsp), %rax
    add     $8, %rsp
    test    %rax, %rax
    jnz     .Lsig_mask_bad

    lea     .Lsig_ret_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lsig_ret_end-.Lsig_ret_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lsig_bad:
    mov     $405, %eax
    syscall
    jmp     .Lexit
.Lsig_alt_bad:
    mov     $408, %eax
    syscall
    jmp     .Lexit
.Lsig_alt_einval:
    mov     $410, %eax
    syscall
    jmp     .Lexit
.Lsig_tkill_bad:
    mov     $409, %eax
    syscall
    jmp     .Lexit
.Lsig_mask_bad:
    mov     $406, %eax
    syscall
    jmp     .Lexit

    /* The handler runs with the kernel's frame below the interrupted RSP;
     * its only job is the positive marker, then the restorer returns. */
.Lhandler:
    /* SA_SIGINFO: rsi=&siginfo, rdx=&ucontext. SA_ONSTACK: the entry RSP
     * must be inside the 4 KiB alternate region saved in r14..r13. */
    test    %rsi, %rsi
    jz      .Lsig_info_bad
    cmpl    $10, (%rsi)             /* si_signo == SIGUSR1 */
    jne     .Lsig_info_bad
    test    %rdx, %rdx
    jz      .Lsig_info_bad
    cmp     %r14, %rsp
    jb      .Lsig_info_bad
    cmp     %r13, %rsp
    jae     .Lsig_info_bad
    lea     .Lsig_info_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lsig_info_end-.Lsig_info_msg, %edx
    mov     $1, %eax
    syscall
    lea     .Lsig_ok_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lsig_ok_end-.Lsig_ok_msg, %edx
    mov     $1, %eax
    syscall
    ret
.Lsig_info_bad:
    mov     $407, %eax              /* MARKER: SA_SIGINFO/alt-stack shape */
    syscall
    ret
.Lrestorer:
    mov     $15, %eax              /* rt_sigreturn */
    syscall
    jmp     .Lhang                 /* the restorer never returns on success */

.Lexit:
    /* 6. exit_group(42): the sidecar reaps this task. */
    mov     $42, %edi
    mov     $231, %eax
    syscall
.Lhang:
    jmp     .Lhang

    /* The fd class's positive marker: printed on the console only if the
     * whole seam performed it (build_probe.sh keeps this in .text — one
     * section, _start at offset 0). */
.Lmunmap_msg:
    .ascii  "E7MEM-MUNMAP-OK\n"

.Lmsg:
    .ascii  "E7FD-WRITE-OK\n"
.Lmsgend:

.Lsig_ok_msg:
    .ascii  "E7SIG-OK\n"
.Lsig_ok_end:
.Lsig_ret_msg:
    .ascii  "E7SIG-RET\n"
.Lsig_ret_end:
.Lsig_info_msg:
    .ascii  "E7SIG-INFO\n"
.Lsig_info_end:

.Llnx105_msg:
    .ascii  "E7SIG-LNX-105-OK\n"
.Llnx105_end:
