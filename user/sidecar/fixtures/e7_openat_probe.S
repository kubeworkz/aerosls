/* e7_openat_probe.S — E7 TG-1 increment I4's native openat probe.
 *
 * Freestanding, one .text section, _start at file offset 0 (the flat mapper
 * enters at image_entry = 0, like e7_thread_probe.S).
 *
 * The probe exercises I4 on a real boot: the CLONED SIBLING — not the group
 * leader — forwards the syscall. That is the case the group forward lease
 * exists for: v1's channel pair belongs to the group leader's cap table, so
 * the child's request travels the leader's channels and its park must be
 * woken and its reply delivered to the CHILD.
 *
 *   - brk(0)/brk(base+0x2000) maps the shared word page and the child stack;
 *   - clone(56) with Go's thread shape creates one sibling (the group);
 *   - the parent does NOT forward anything; it spins on the shared page;
 *   - the child gettid(186)s (kernel-local), then openat(257,
 *     AT_FDCWD, "/", O_RDONLY|O_DIRECTORY, 0) — a really forwarded, really
 *     measured path open. It publishes the fd and close(3)s it, a second
 *     forward from the same sibling, and publishes close's 0;
 *   - the child exit(60)s (thread-local); the parent's exit_group(231) tears
 *     the group down once.
 *
 * Both replies are applied to the child: open must return fd >= 3 and close
 * must return 0, so a wake delivered to the leader (or a reply applied to the
 * wrong descriptor) fails the probe.
 *
 * Serial markers (the positive evidence):
 *   E7OA-START      the program ran and cloned
 *   E7OA-OPEN-OK    the sibling's openat returned a real fd
 *   E7OA-CLOSE-OK   the sibling's close(that fd) returned 0
 *   E7OA-OK         the whole exchange completed
 * Any failure prints E7OA-FAIL and hangs; tests/linux_openat_probe_check.sh
 * asserts the positives, the absence of the failure, the kernel's
 * `forward lease ... acquired for syscall 257` line for the CHILD's TID, the
 * child's thread exit and exactly one group teardown.
 *
 * Build (build_openat_probe.sh; the committed .flat is its byte-exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o /tmp/e7_openat_probe.elf e7_openat_probe.S
 *   objcopy -O binary -j .text /tmp/e7_openat_probe.elf e7_openat_probe.flat
 *
 * Shared page layout (base in r12):
 *   0x00 u32 done        child: 1 both syscalls returned, 2 openat failed,
 *                        3 close failed
 *   0x08 u64 fd          child: openat's return (signed)
 *   0x10 u64 close_ret   child: close's return (signed)
 *   0x18 u64 child_tid   child: its own gettid
 *   0x20 u64 spins       parent: bounded-wait counter
 */

    .text
    .globl _start
_start:
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

    /* ── parent: no forward at all, just watch the shared page ───────── */
    mov     %rax, %r14                /* r14 = child tid from clone */
    movq    $0, 0x20(%r12)
.Lwait_done:
    movl    (%r12), %eax
    test    %eax, %eax
    jnz     .Lcheck
    incq    0x20(%r12)
    cmpq    $0x4000000, 0x20(%r12)
    jb      .Lwait_done
    jmp     .Lfail                    /* child never finished */

.Lcheck:
    cmpl    $1, %eax
    jne     .Lfail                    /* 2 = openat failed, 3 = close failed */
    mov     0x08(%r12), %rax
    cmp     $3, %rax                  /* a real fd, not 0/1/2 */
    jl      .Lfail
    mov     0x18(%r12), %rbx          /* child-published gettid */
    cmp     %r14, %rbx
    jne     .Lfail

    lea     .Lopen_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lopen_ok_end-.Lopen_ok, %edx
    mov     $1, %eax
    syscall

    mov     0x10(%r12), %rax
    test    %rax, %rax
    jnz     .Lfail
    lea     .Lclose_ok(%rip), %rsi
    mov     $1, %edi
    mov     $.Lclose_ok_end-.Lclose_ok, %edx
    mov     $1, %eax
    syscall

    /* Let the child observe nothing further and take its exit(60) before
     * exit_group reaps the group (so the live log shows both halves). */
    movq    $0, 0x20(%r12)
.Lsettle:
    incq    0x20(%r12)
    cmpq    $0x2000000, 0x20(%r12)
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
    /* The clone context carries rsi = the child stack (base+0x2000). */
    lea     -0x2000(%rsi), %r12

    mov     $186, %eax                /* gettid */
    syscall
    mov     %rax, 0x18(%r12)

    /* openat(AT_FDCWD, "/", O_RDONLY|O_DIRECTORY, 0): the measured path
     * open, forwarded by the SIBLING. The path is in this image, so it is
     * mapped in the child's address space at the same RIP-relative offset. */
    mov     $257, %eax
    mov     $-100, %rdi               /* AT_FDCWD */
    lea     .Lroot(%rip), %rsi
    mov     $0x10000, %edx            /* O_DIRECTORY */
    xor     %r10d, %r10d              /* mode 0 (no O_CREAT) */
    syscall
    mov     %rax, 0x08(%r12)
    test    %rax, %rax
    js      .Lchild_open_fail

    mov     %rax, %rdi                /* close(that fd): the second forward */
    mov     $3, %eax
    syscall
    mov     %rax, 0x10(%r12)
    test    %rax, %rax
    jnz     .Lchild_close_fail

    movl    $1, (%r12)
    jmp     .Lchild_exit
.Lchild_open_fail:
    movl    $2, (%r12)
    jmp     .Lchild_exit
.Lchild_close_fail:
    movl    $3, (%r12)
.Lchild_exit:
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

.Lroot:
    .asciz  "/"
.Lstart:
    .ascii  "E7OA-START\n"
.Lstart_end:
.Lopen_ok:
    .ascii  "E7OA-OPEN-OK\n"
.Lopen_ok_end:
.Lclose_ok:
    .ascii  "E7OA-CLOSE-OK\n"
.Lclose_ok_end:
.Lok:
    .ascii  "E7OA-OK\n"
.Lok_end:
.Lfail_msg:
    .ascii  "E7OA-FAIL\n"
.Lfail_msg_end:
