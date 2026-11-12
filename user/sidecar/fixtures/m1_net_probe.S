/* m1_net_probe.S — M1's live Linux TCP round trip, as a static payload.
 *
 * Freestanding, one .text section, _start at section offset 0 — the same
 * flat mapping e7_linux_probe.flat uses (kernel/cap.c copies the file at
 * USER_PROC_CODE_BASE and enters at image_entry = 0). No libc, no reloc
 * data: every buffer lives on the stack and every constant is immediate or
 * in .text.
 *
 * The sequence is the bridge M1 claims, including the fidelity arms:
 *   socket(41) -> bind(49) -> listen(50) -> socket(41) -> connect(42)
 *   -> accept(43) [peer sockaddr] -> sendto(44) [non-NULL destination]
 *   -> poll(7) [POLLIN] -> recvfrom(45) [same peer sockaddr]
 *   -> sendto(44) -> recvfrom(45) -> shutdown(48; EINVAL, SHUT_WR, SHUT_RD)
 *   -> close(3) x3 -> write(1) -> exit_group(231)
 *
 * The peer address is the GUEST'S OWN address (10.0.2.15), not 127.0.0.1:
 * net/ipv4.c's loopback hook fires only for NET_MY_IP, and it hands the
 * fully-built segment to ipv4_handle_packet — so the SYN, SYN-ACK and ACK
 * all stay inside the kernel's TCP stack and need no NIC or host service.
 * The listener binds the same port; the service's bind arm records only the
 * port, while connect carries the address the service decodes.
 *
 * The client's auto-bound local address is the mock driver's first
 * 127.0.0.1:10000, so accept/recvfrom must report that peer (family AF_INET,
 * a non-zero port, and 127.0.0.1). The exact port is not hard-coded: the
 * two syscalls are checked against each other.
 *
 * Build (committed .flat is the byte-exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o /tmp/m1_net_probe.elf m1_net_probe.S
 *   objcopy -O binary -j .text /tmp/m1_net_probe.elf m1_net_probe.flat
 */
    .text
    .globl _start
_start:
    /* 64 bytes of stack:
     *   0..15   server sockaddr_in
     *   16..19  peer addrlen
     *   20..35  peer sockaddr_in
     *   40..43  ping buffer
     *   44..47  pong buffer
     *   48..55  saved accept peer (family/port/ip)
     *   56..63  struct pollfd { fd, events, revents }
     */
    subq    $64, %rsp
    movl    $1, %r15d               /* failure status: stage 1 */

    /* sockaddr_in { AF_INET, htons(9999), inet_addr("10.0.2.15") }. The
     * kernel's parse_sockaddr_in reads these in memory order: 27 0F for
     * the port and 0A 00 02 0F for the address. */
    movw    $0x0002, 0(%rsp)
    movw    $0x0F27, 2(%rsp)
    movl    $0x0F02000A, 4(%rsp)
    movl    $0x00000000, 8(%rsp)
    movl    $16, 16(%rsp)             /* peer addrlen, written by accept */

    /* listener = socket(AF_INET, SOCK_STREAM, 0) */
    movl    $41, %eax
    movl    $2, %edi
    movl    $1, %esi
    xorl    %edx, %edx
    syscall
    testq   %rax, %rax
    js      .Lsocket_fail
    movq    %rax, %r12

    /* bind(listener, &addr, 16) */
    movl    $2, %r15d               /* stage: bind */
    movq    %r12, %rdi
    leaq    0(%rsp), %rsi
    movl    $16, %edx
    movl    $49, %eax
    syscall
    testq   %rax, %rax
    js      .Lbind_fail

    /* listen(listener, 1) */
    movl    $3, %r15d               /* stage: listen */
    movq    %r12, %rdi
    movl    $1, %esi
    movl    $50, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail

    /* client = socket(AF_INET, SOCK_STREAM, 0) */
    movl    $4, %r15d               /* stage: client socket */
    movl    $41, %eax
    movl    $2, %edi
    movl    $1, %esi
    xorl    %edx, %edx
    syscall
    testq   %rax, %rax
    js      .Lfail
    movq    %rax, %r13

    /* connect(client, &addr, 16) — 10.0.2.15 is our own address, so the
     * SYN is looped back into this stack by ipv4_send. */
    movl    $5, %r15d               /* stage: connect */
    movq    %r13, %rdi
    leaq    0(%rsp), %rsi
    movl    $16, %edx
    movl    $42, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail

    /* accept(listener, &peer, &peer_len) */
    movl    $6, %r15d               /* stage: accept */
    movq    %r12, %rdi
    leaq    20(%rsp), %rsi
    leaq    16(%rsp), %rdx
    movl    $43, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail
    movq    %rax, %r14

    /* The peer is the client's auto-bound 127.0.0.1:<ephemeral>. The
     * family and IP are fixed; the port is checked non-zero and against
     * recvfrom below rather than assuming today's next_port value. */
    cmpl    $16, 16(%rsp)
    jne     .Lfail
    cmpw    $0x0002, 20(%rsp)
    jne     .Lfail
    cmpw    $0, 22(%rsp)
    je      .Lfail
    cmpl    $0x0100007F, 24(%rsp)   /* 127.0.0.1 in network byte order */
    jne     .Lfail
    movq    20(%rsp), %rax           /* save family/port/ip for recvfrom */
    movq    %rax, 48(%rsp)

    /* sendto(client, "ping", 4, 0, &addr, 16) — a connected stream
     * ignores the destination, but the bridge still copies and decodes it. */
    movl    $7, %r15d               /* stage: send ping */
    movq    %r13, %rdi
    leaq    .Lping(%rip), %rsi
    movl    $4, %edx
    xorl    %r10d, %r10d
    leaq    0(%rsp), %r8
    movl    $16, %r9d
    movl    $44, %eax
    syscall
    cmpq    $4, %rax
    jne     .Lfail

    /* poll(peer, POLLIN, 1000ms): the ping is already queued. */
    movl    $11, %r15d              /* stage: poll */
    movl    %r14d, 56(%rsp)
    movw    $1, 60(%rsp)            /* POLLIN */
    movw    $0, 62(%rsp)            /* revents */
    movl    $7, %eax
    leaq    56(%rsp), %rdi
    movl    $1, %esi
    movl    $1000, %edx
    syscall
    cmpq    $1, %rax
    jne     .Lfail
    testw   $1, 62(%rsp)
    jz      .Lfail

    /* recvfrom(peer, buf, 4, 0, &peer, &peer_len) */
    movl    $8, %r15d               /* stage: recv ping */
    movq    %r14, %rdi
    leaq    40(%rsp), %rsi
    movl    $4, %edx
    xorl    %r10d, %r10d
    leaq    20(%rsp), %r8
    leaq    16(%rsp), %r9
    movl    $200, %ebx              /* bounded readiness retries */
.Lrecv_ping_retry:
    movl    $45, %eax
    syscall
    cmpq    $4, %rax
    je      .Lrecv_ping_have
    cmpl    $-11, %eax              /* EAGAIN */
    jne     .Lfail
    decl    %ebx
    jnz     .Lrecv_ping_retry
    jmp     .Lfail
.Lrecv_ping_have:
    cmpl    $0x676E6970, 40(%rsp)     /* "ping" */
    jne     .Lfail
    cmpl    $16, 16(%rsp)
    jne     .Lfail
    movq    48(%rsp), %rax
    cmpq    20(%rsp), %rax
    jne     .Lfail

    /* sendto(peer, "pong", 4, 0, NULL, 0) */
    movl    $9, %r15d               /* stage: send pong */
    movq    %r14, %rdi
    leaq    .Lpong(%rip), %rsi
    movl    $4, %edx
    xorl    %r10d, %r10d
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    movl    $44, %eax
    syscall
    cmpq    $4, %rax
    jne     .Lfail

    /* recvfrom(client, buf, 4, 0, &peer, &peer_len) */
    movl    $10, %r15d              /* stage: recv pong */
    movq    %r13, %rdi
    leaq    44(%rsp), %rsi
    movl    $4, %edx
    xorl    %r10d, %r10d
    leaq    20(%rsp), %r8
    leaq    16(%rsp), %r9
    movl    $200, %ebx              /* bounded readiness retries */
.Lrecv_pong_retry:
    movl    $45, %eax
    syscall
    cmpq    $4, %rax
    je      .Lrecv_pong_have
    cmpl    $-11, %eax              /* EAGAIN */
    jne     .Lfail
    decl    %ebx
    jnz     .Lrecv_pong_retry
    jmp     .Lfail
.Lrecv_pong_have:
    cmpl    $0x676E6F70, 44(%rsp)     /* "pong" */
    jne     .Lfail
    cmpl    $16, 16(%rsp)
    jne     .Lfail
    movq    0(%rsp), %rax           /* the client's remote is the server */
    cmpq    20(%rsp), %rax
    jne     .Lfail

    /* shutdown(client, 3) must be EINVAL through the network errno map. */
    movl    $12, %r15d              /* stage: shutdown invalid */
    movq    %r13, %rdi
    movl    $3, %esi
    movl    $48, %eax
    syscall
    cmpq    $-22, %rax
    jne     .Lfail

    /* shutdown(client, SHUT_WR): FIN, receive side stays open. */
    movl    $13, %r15d              /* stage: shutdown write */
    movq    %r13, %rdi
    movl    $1, %esi
    movl    $48, %eax
    syscall
    testq   %rax, %rax
    jne     .Lfail

    /* shutdown(peer, SHUT_RD): local read-half close. */
    movl    $14, %r15d              /* stage: shutdown read */
    movq    %r14, %rdi
    xorl    %esi, %esi
    movl    $48, %eax
    syscall
    testq   %rax, %rax
    jne     .Lfail

    /* close(client), close(peer), close(listener) */
    movq    %r13, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail
    movq    %r14, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail
    movq    %r12, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail

    /* write(1, "M1-NET PASS\n", 12) — the positive marker. */
    movl    $1, %eax
    movl    $1, %edi
    leaq    .Lpass(%rip), %rsi
    movl    $.Lpass_len, %edx
    syscall

    /* exit_group(0) */
    movl    $231, %eax
    xorl    %edi, %edi
    syscall

.Lsocket_fail:
    /* socket's errno is the stage report: exit_group status = 100 + errno. */
    negl    %eax
    addl    $100, %eax
    movl    %eax, %r15d
    jmp     .Lfail

.Lbind_fail:
    /* bind's errno is the stage report: exit_group status = 200 + errno,
     * which the sidecar logs as `status=N`. Keep the marker too. */
    negl    %eax
    addl    $200, %eax
    movl    %eax, %r15d
    jmp     .Lfail

.Lfail:
    /* write(1, "M1-NET FAIL\n", 12) — the last syscall above names the stage
     * in the sidecar's own log; this marker is the visible red. */
    movl    $1, %eax
    movl    $1, %edi
    leaq    .Lfailmsg(%rip), %rsi
    movl    $.Lfail_len, %edx
    syscall
    movl    $231, %eax
    movl    %r15d, %edi
    syscall

.Lping:
    .ascii  "ping"
.Lpong:
    .ascii  "pong"
.Lpass:
    .ascii  "M1-NET PASS\n"
    .set    .Lpass_len, . - .Lpass
.Lfailmsg:
    .ascii  "M1-NET FAIL\n"
    .set    .Lfail_len, . - .Lfailmsg
