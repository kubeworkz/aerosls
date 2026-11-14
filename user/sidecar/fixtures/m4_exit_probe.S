/* m4_exit_probe.S — M4.5's live proof of identity, thread exit and the
 * exit_group sweep.
 *
 * One Linux-personality leader P:
 *   - getpid == gettid, both nonzero (the leader is the TGID);
 *   - clones C1, whose gettid is its own real TID and != getpid;
 *   - tkill(C1's TID, 0) succeeds, proving tkill follows the real TID;
 *   - C1 calls set_tid_address(ctid), which returns C1's TID (not the
 *     pointer), then waits for a release handshake;
 *   - C1 calls exit(60), not exit_group: P must survive. The kernel zeroes
 *     C1's clear_child_tid word at teardown and the sidecar wakes P, which
 *     was queued on that word with FUTEX_WAIT. P sees the word become 0;
 *   - P clones C2, waits until C2 is live, writes M4-EXITGROUP-BEFORE and
 *     calls exit_group(231). The kernel's group sweep must kill both P and
 *     C2 and prove the group's live count reached zero; the serial line is
 *     [PROC] M4.5 group sweep ... killed=2 live=0.
 *
 * The no-execution assertion is live, not inferred: P records its own
 * set_tid_address word (SH_PARENT_CTID) and sets a sentinel before the
 * sweep. C2 spins until that word becomes zero (the kernel zeroes it only
 * when P is torn down) and then executes ud2. If C2 survives the sweep, its
 * fault is a [PROC] ... 'm4.exit.probe.0.tN' exited line after the sweep
 * line, which the gate rejects. If the sweep killed it, no such line exists.
 *
 * Markers:
 *   M4-CHILD-OK            C1 passed its identity and self-tkill checks
 *   M4-IDENTITY-OK         P matched C1's TID to ptid and TGID to its own
 *   M4-TKILL-OK            P reached C1 by its real TID with sig 0
 *   M4-JOIN-OK             P observed clear_child_tid zeroed after C1's exit
 *   M4-EXITGROUP-CHILD-OK  C2 is live
 *   M4-EXITGROUP-BEFORE    P is about to call exit_group
 *   M4-EXITGROUP-AFTER     exit_group returned (failure: P should never run)
 *   M4-EXIT-FAIL           any arm refused
 *
 * Build (build_m4_exit_probe.sh; the committed .flat is this output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o m4_exit_probe.elf m4_exit_probe.S
 *   objcopy -O binary -j .text m4_exit_probe.elf m4_exit_probe.flat
 */
    .set CLONE_VM,             0x00000100
    .set CLONE_FS,             0x00000200
    .set CLONE_FILES,          0x00000400
    .set CLONE_SIGHAND,        0x00000800
    .set CLONE_THREAD,         0x00010000
    .set CLONE_PARENT_SETTID,  0x00100000
    .set CLONE_CHILD_CLEARTID, 0x00200000
    .set CLONE_CHILD_SETTID,   0x01000000
    .set CLONE_FLAGS, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                      CLONE_THREAD | CLONE_PARENT_SETTID | \
                      CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID
    .set FUTEX_WAIT,      0
    .set FUTEX_WAKE,      1
    .set FUTEX_PRIVATE,   128
    .set FUTEX_WAIT_OP,   FUTEX_WAIT | FUTEX_PRIVATE
    .set FUTEX_WAKE_OP,   FUTEX_WAKE | FUTEX_PRIVATE

    /* Shared words, addressed through r12 in both contexts. */
    .set SH_READY1,     0
    .set SH_RELEASE1,   8
    .set SH_CTID,       16
    .set SH_PTID,       24
    .set SH_CHILD_TID,  32
    .set SH_READY2,     40
    .set SH_ACK,        48
    .set SH_EXIT_GO,    56
    .set SH_PARENT_CTID, 64

    .text
    .globl _start
_start:
    /* r12 = shared page base; rbp = the real TGID (leader's getpid). */
    leaq -0x300(%rsp), %r12
    xorq %rax, %rax
    movq %rax, 0(%r12)
    movq %rax, 8(%r12)
    movq %rax, 16(%r12)
    movq %rax, 24(%r12)
    movq %rax, 32(%r12)
    movq %rax, 40(%r12)
    movq %rax, 48(%r12)
    movq %rax, 56(%r12)
    movq %rax, 64(%r12)

    /* M4.5 identity: the leader's getpid IS its gettid. */
    movq $39, %rax                  /* getpid */
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, %rbp
    movq $186, %rax                 /* gettid */
    syscall
    cmpq %rbp, %rax
    jne .Lfail

    /* P's own clear-tid word: the post-sweep trap's arming signal. It is
     * zeroed by the kernel only when P's teardown reaches detach. */
    movq $218, %rax                 /* set_tid_address */
    leaq SH_PARENT_CTID(%r12), %rdi
    syscall
    cmpq %rbp, %rax
    jne .Lfail
    movq $0x5A5A5A5A, SH_PARENT_CTID(%r12)

    /* clone(flags, child_stack, ptid, ctid, tls) */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x700(%rsp), %rsi
    leaq SH_PTID(%r12), %rdx
    leaq SH_CTID(%r12), %r10
    xorq %r8, %r8
    syscall

    testq %rax, %rax
    js .Lfail
    jz .Lchild1

    /* The kernel's ptid write and the clone return are the same real TID. */
    cmpq %rax, SH_PTID(%r12)
    jne .Lfail

    /* Wait for C1's identity checks before addressing it by TID. */
.Lparent_wait_ready:
    cmpq $0, SH_READY1(%r12)
    jne .Lparent_ready
    pause
    jmp .Lparent_wait_ready

.Lparent_ready:
    movq SH_CHILD_TID(%r12), %rax
    cmpq SH_PTID(%r12), %rax
    jne .Lfail                    /* C1's gettid is not the kernel's TID */
    cmpq %rbp, %rax
    je .Lfail                     /* a thread TID must differ from the TGID */

    /* Per-thread signal identity: tkill follows C1's real TID, not the
     * set_tid_address pointer. Signal 0 is the existence probe. */
    movq $200, %rax
    movq SH_PTID(%r12), %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    js .Lfail

    leaq .Lidentity_msg(%rip), %rsi
    movq $.Lidentity_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail
    leaq .Ltkill_msg(%rip), %rsi
    movq $.Ltkill_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail

    /* Release C1; it sets ACK and waits for EXIT_GO. */
    movq $1, SH_RELEASE1(%r12)
    movq $202, %rax
    leaq SH_RELEASE1(%r12), %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall

.Lparent_wait_ack:
    cmpq $0, SH_ACK(%r12)
    jne .Lparent_join
    pause
    jmp .Lparent_wait_ack

    /* Queue on the clear-child-tid word BEFORE releasing C1's exit. C1's
     * kernel teardown zeroes the word and the sidecar wakes this wait. */
.Lparent_join:
    cmpq $0, SH_CTID(%r12)
    je .Lparent_join_wait
    jmp .Ljoin_done
.Lparent_join_wait:
    movq $202, %rax
    leaq SH_CTID(%r12), %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Ljoin_timeout(%rip), %r10
    syscall
    jmp .Lparent_join

.Ljoin_done:
    movq $1, SH_EXIT_GO(%r12)
    movq $202, %rax
    leaq SH_EXIT_GO(%r12), %rdi
    movq $FUTEX_WAKE_OP, %rsi
    movq $1, %rdx
    syscall

    leaq .Ljoin_msg(%rip), %rsi
    movq $.Ljoin_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail

    /* Second thread for the group sweep. Its ctid is the same shared word;
     * P is done joining, and the sweep zeroes it again. */
    movq $56, %rax
    movq $CLONE_FLAGS, %rdi
    leaq -0x900(%rsp), %rsi
    leaq SH_PTID(%r12), %rdx
    leaq SH_CTID(%r12), %r10
    xorq %r8, %r8
    syscall
    testq %rax, %rax
    js .Lfail
    jz .Lchild2

.Lparent_wait_ready2:
    cmpq $0, SH_READY2(%r12)
    jne .Lexit_group
    pause
    jmp .Lparent_wait_ready2

.Lexit_group:
    leaq .Lgroup_before_msg(%rip), %rsi
    movq $.Lgroup_before_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail
    movq $231, %rax                 /* exit_group: P and C2 must both die */
    xorq %rdi, %rdi
    syscall
    /* exit_group must not return. If the sidecar could not prove the group
     * dead it answers EBUSY, and this marker makes that failure loud. */
    leaq .Lgroup_after_msg(%rip), %rsi
    movq $.Lgroup_after_len, %rdx
    call .Lputs
    movq $60, %rax
    movq $1, %rdi
    syscall
    ud2

.Lchild1:
    /* Real per-thread identity. rbp is the leader's TGID, inherited. */
    movq $186, %rax                 /* gettid */
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, SH_CHILD_TID(%r12)
    movq $39, %rax                  /* getpid */
    syscall
    cmpq %rbp, %rax
    jne .Lfail

    /* set_tid_address returns the TID, never the user pointer. */
    movq $218, %rax
    leaq SH_CTID(%r12), %rdi
    syscall
    cmpq SH_CHILD_TID(%r12), %rax
    jne .Lfail
    /* A nonzero sentinel: the join arm below must observe the kernel's
     * exit zero, not a word that was never set. */
    movq $0x5A5A5A5A, SH_CTID(%r12)

    /* Self tkill by the real TID: the per-thread existence probe. */
    movq $200, %rax
    movq SH_CHILD_TID(%r12), %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    js .Lfail

    leaq .Lchild_msg(%rip), %rsi
    movq $.Lchild_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail

    movq $1, SH_READY1(%r12)

.Lchild_wait_release:
    cmpq $0, SH_RELEASE1(%r12)
    jne .Lchild_released
    movq $202, %rax
    leaq SH_RELEASE1(%r12), %rdi
    movq $FUTEX_WAIT_OP, %rsi
    xorq %rdx, %rdx
    leaq .Llong_timeout(%rip), %r10
    syscall
    jmp .Lchild_wait_release

.Lchild_released:
    movq $1, SH_ACK(%r12)
.Lchild_wait_exit_go:
    cmpq $0, SH_EXIT_GO(%r12)
    jne .Lchild_exit
    pause
    jmp .Lchild_wait_exit_go

.Lchild_exit:
    /* exit(60), NOT exit_group: the leader must survive this arm. */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall
    ud2

.Lchild2:
    movq $186, %rax                 /* gettid */
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, SH_CHILD_TID(%r12)
    movq $39, %rax
    syscall
    cmpq %rbp, %rax
    jne .Lfail

    leaq .Lgroup_child_msg(%rip), %rsi
    movq $.Lgroup_child_len, %rdx
    call .Lputs
    testq %rax, %rax
    jle .Lfail
    movq $1, SH_READY2(%r12)
    /* Wait for P's teardown to zero its own clear-tid word. On a correct
     * sweep C2 is already dead and never reaches the ud2; a C2 that runs
     * after the sweep faults here, naming itself in the exit log. */
.Lchild2_wait_parent_gone:
    cmpq $0, SH_PARENT_CTID(%r12)
    je .Lchild2_after_sweep
    pause
    jmp .Lchild2_wait_parent_gone
.Lchild2_after_sweep:
    ud2

.Lfail:
    leaq .Lfail_msg(%rip), %rsi
    movq $.Lfail_len, %rdx
    call .Lputs
    movq $231, %rax
    movq $1, %rdi
    syscall
    ud2

/* write(1, rsi, rdx); returns the syscall result in rax. */
.Lputs:
    movq $1, %rax
    movq $1, %rdi
    syscall
    ret

    .align 8
.Ljoin_timeout:
    .quad 2, 0
.Llong_timeout:
    .quad 2, 0
.Lchild_msg:
    .ascii "M4-CHILD-OK\n"
    .set .Lchild_len, . - .Lchild_msg
.Lidentity_msg:
    .ascii "M4-IDENTITY-OK\n"
    .set .Lidentity_len, . - .Lidentity_msg
.Ltkill_msg:
    .ascii "M4-TKILL-OK\n"
    .set .Ltkill_len, . - .Ltkill_msg
.Ljoin_msg:
    .ascii "M4-JOIN-OK\n"
    .set .Ljoin_len, . - .Ljoin_msg
.Lgroup_child_msg:
    .ascii "M4-EXITGROUP-CHILD-OK\n"
    .set .Lgroup_child_len, . - .Lgroup_child_msg
.Lgroup_before_msg:
    .ascii "M4-EXITGROUP-BEFORE\n"
    .set .Lgroup_before_len, . - .Lgroup_before_msg
.Lgroup_after_msg:
    .ascii "M4-EXITGROUP-AFTER\n"
    .set .Lgroup_after_len, . - .Lgroup_after_msg
.Lfail_msg:
    .ascii "M4-EXIT-FAIL\n"
    .set .Lfail_len, . - .Lfail_msg
