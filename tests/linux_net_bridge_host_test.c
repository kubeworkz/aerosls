/*
 * linux_net_bridge_host_test.c — M1: the kernel's pure half of the Linux
 * network bridge.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * M1 routes socket(41)/connect(42)/accept(43)/sendto(44)/recvfrom(45)/
 * shutdown(48)/bind(49)/listen(50) through the sidecar to P2's socket
 * service. The kernel's share is deliberately small and pure: classify the
 * eight numbers, copy a 16-byte sockaddr_in out for connect/bind, copy sendto's
 * data exactly as write does, and stage the two multi-destination replies
 * (accept's sockaddr+addrlen, recvfrom's data+sockaddr+addrlen). Every one
 * of those decisions is a function over captured registers, so it can be
 * asserted here without a boot, a channel, or a page table.
 *
 * The teeth that would otherwise need a live TCP run:
 *   - a connect/bind whose addrlen is not 16 is EINVAL BEFORE any post;
 *   - a non-canonical sockaddr span is EFAULT before any post;
 *   - sendto's clamp rides in args[2] exactly like write's;
 *   - accept with a NULL addr owes no payload, with addr and no addrlen is
 *     EFAULT, and otherwise stages BOTH destinations;
 *   - recvfrom stages buf/addr/addrlen as three separate spans, with the
 *     data clamp the 1 KiB reply buffer imposes;
 *   - the reply SHAPE is count-truth: recvfrom's ret is its data length
 *     and accept's payload is exactly the 20-byte trailer.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_net_bridge_host_test \
 *       tests/linux_net_bridge_host_test.c kernel/linux_abi.c
 *   /tmp/linux_net_bridge_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

int main(void)
{
    uint64_t args[6];
    struct LinuxFdWritePlan wp;
    struct LinuxFdReplyPlan rp;

    /* ─── The eight numbers are the network class, and no other ─────────── */
    {
        static const uint64_t net[] = { 41, 42, 43, 44, 45, 48, 49, 50 };
        size_t i;
        int all = 1;
        for (i = 0; i < sizeof(net) / sizeof(net[0]); i++) {
            if (linux_syscall_class(net[i]) != LSC_NET)
                all = 0;
        }
        CHECK(all, "the eight M1 numbers all classify as LSC_NET");
        CHECK(strcmp(linux_syscall_class_name(LSC_NET), "net") == 0,
              "LSC_NET's name is \"net\" (the refusal line's wording)");
        CHECK(linux_syscall_class(LINUX_SOCKET) == LSC_NET &&
              linux_syscall_class(LINUX_LISTEN) == LSC_NET,
              "the header's own LINUX_SOCKET/LINUX_LISTEN constants are net rows");
    }

    /* ─── connect/bind: the 16-byte sockaddr crossing ───────────────────── */
    memset(args, 0, sizeof(args));
    args[0] = 3;                 /* fd */
    args[1] = 0x400000;          /* sockaddr_in */
    args[2] = LINUX_SOCKADDR_IN_BYTES;
    wp = linux_fd_write_plan(LINUX_CONNECT, args);
    CHECK(wp.refuse == 0 && wp.src == 0x400000 && wp.len == 16,
          "connect plans the 16-byte sockaddr_in it was given");

    wp = linux_fd_write_plan(LINUX_BIND, args);
    CHECK(wp.refuse == 0 && wp.src == 0x400000 && wp.len == 16,
          "bind plans the same crossing");

    args[2] = 8;                 /* AF_INET's sockaddr is 16, not 8 */
    wp = linux_fd_write_plan(LINUX_CONNECT, args);
    CHECK(wp.refuse == -LINUX_EINVAL && wp.len == 0,
          "a connect addrlen other than 16 is EINVAL before any post");

    args[2] = LINUX_SOCKADDR_IN_BYTES;
    args[1] = 0xFFFFFFFFFFFFF000ULL;
    wp = linux_fd_write_plan(LINUX_BIND, args);
    CHECK(wp.refuse == -LINUX_EFAULT,
          "a non-canonical sockaddr span is EFAULT before any post");

    /* ─── sendto shares write's buffer/count plan ───────────────────────── */
    memset(args, 0, sizeof(args));
    args[0] = 3;
    args[1] = 0x500000;
    args[2] = 1000;
    wp = linux_fd_write_plan(LINUX_SENDTO, args);
    CHECK(wp.refuse == 0 && wp.src == 0x500000 && wp.len == 1000,
          "sendto attaches exactly its count of data bytes");

    args[2] = 4000;
    wp = linux_fd_write_plan(LINUX_SENDTO, args);
    CHECK(wp.refuse == 0 && wp.len == LINUX_FD_WRITE_CHUNK,
          "sendto's count clamps to the one-chunk reply/attach budget");

    args[2] = 0;
    args[1] = 0;                 /* sendto(fd, NULL, 0, ...) is legal */
    wp = linux_fd_write_plan(LINUX_SENDTO, args);
    CHECK(wp.refuse == 0 && wp.len == 0,
          "a zero-length sendto plans nothing and faults nothing");

    args[4] = 0x800000;          /* destination sockaddr */
    args[5] = 8;                 /* not sizeof(sockaddr_in) */
    wp = linux_fd_write_plan(LINUX_SENDTO, args);
    CHECK(wp.refuse == -LINUX_EINVAL,
          "a sendto destination with a non-16 addrlen is EINVAL before any post");

    /* The destination decoder is pure: host-order family, network-order
     * port/address in, host-order ip/port out. */
    {
        const uint8_t sa[16] = { 2, 0, 0x1f, 0x90, 127, 0, 0, 1,
                                 0, 0, 0, 0, 0, 0, 0, 0 };
        uint32_t ip = 0;
        uint16_t port = 0;
        CHECK(linux_sendto_dest_decode(sa, &ip, &port) == 0 &&
              ip == 0x7F000001u && port == 8080u,
              "sendto destination decodes to host-order 127.0.0.1:8080");
        uint8_t bad[16] = { 1, 0, 0, 0, 0, 0, 0, 0 };
        CHECK(linux_sendto_dest_decode(bad, &ip, &port) ==
              -LINUX_EAFNOSUPPORT,
              "a non-AF_INET sendto destination is EAFNOSUPPORT");
    }

    CHECK(linux_syscall_class(LINUX_SHUTDOWN) == LSC_NET,
          "shutdown(48) is a net-class row");

    /* ─── accept: two destinations, one 20-byte trailer ─────────────────── */
    memset(args, 0, sizeof(args));
    args[0] = 3;
    args[1] = 0x600000;          /* struct sockaddr_in * */
    args[2] = 0x610000;          /* socklen_t * */
    rp = linux_fd_reply_plan(LINUX_ACCEPT, args);
    CHECK(rp.refuse == 0 &&
          rp.dst == 0x600000 && rp.dst2 == 0x610000 &&
          rp.len == LINUX_NET_ADDR_BYTES && rp.len2 == 4 &&
          rp.aux == LINUX_SOCKADDR_IN_BYTES &&
          rp.kind == LINUX_STAGE_ACCEPT,
          "accept stages sockaddr (16) and addrlen (4) from one 20-byte payload");

    args[1] = 0;                 /* addr NULL: addrlen is ignored */
    rp = linux_fd_reply_plan(LINUX_ACCEPT, args);
    CHECK(rp.refuse == 0 && rp.len == 0,
          "accept with a NULL addr owes no payload");

    args[1] = 0x600000;
    args[2] = 0;                 /* addr non-NULL, addrlen NULL */
    rp = linux_fd_reply_plan(LINUX_ACCEPT, args);
    CHECK(rp.refuse == -LINUX_EFAULT,
          "accept with addr but no addrlen is EFAULT");

    /* ─── recvfrom: data, source address and length as three spans ──────── */
    memset(args, 0, sizeof(args));
    args[0] = 3;
    args[1] = 0x700000;          /* data buffer */
    args[2] = 512;
    args[4] = 0x710000;          /* struct sockaddr_in * */
    args[5] = 0x720000;          /* socklen_t * */
    rp = linux_fd_reply_plan(LINUX_RECVFROM, args);
    CHECK(rp.refuse == 0 &&
          rp.dst == 0x700000 && rp.dst2 == 0x710000 && rp.dst3 == 0x720000 &&
          rp.len == 512 + LINUX_NET_ADDR_BYTES && rp.aux == 512 &&
          rp.len2 == LINUX_SOCKADDR_IN_BYTES &&
          rp.kind == LINUX_STAGE_RECVFROM,
          "recvfrom stages data + sockaddr + addrlen as three destinations");

    args[4] = 0;                 /* no source-address update asked for */
    args[5] = 0;
    rp = linux_fd_reply_plan(LINUX_RECVFROM, args);
    CHECK(rp.refuse == 0 && rp.dst2 == 0 && rp.dst3 == 0 &&
          rp.len == 512 + LINUX_NET_ADDR_BYTES,
          "recvfrom with NULL addr still carries the data and trailer, writes no address");

    args[4] = 0x710000;
    args[5] = 0;
    rp = linux_fd_reply_plan(LINUX_RECVFROM, args);
    CHECK(rp.refuse == -LINUX_EFAULT,
          "recvfrom with an address but no addrlen is EFAULT");

    memset(args, 0, sizeof(args));
    args[0] = 3;
    args[1] = 0x700000;
    args[2] = 4000;              /* clamp: 1004 data + 20 trailer = 1024 */
    rp = linux_fd_reply_plan(LINUX_RECVFROM, args);
    CHECK(rp.refuse == 0 && rp.aux == LINUX_NET_RECV_CAP &&
          rp.len == LINUX_FD_WRITE_CHUNK,
          "recvfrom clamps data so the trailer still fits the 1 KiB reply buffer");

    args[2] = 0;
    rp = linux_fd_reply_plan(LINUX_RECVFROM, args);
    CHECK(rp.refuse == 0 && rp.len == 0,
          "a zero-length recvfrom plans nothing");

    /* ─── Reply shape: one truth for ret, payload length and number ─────── */
    CHECK(linux_reply_shape_ok(LINUX_ACCEPT, 7, LINUX_NET_ADDR_BYTES),
          "accept's payload is exactly the 20-byte trailer (ret is the new fd)");
    CHECK(!linux_reply_shape_ok(LINUX_ACCEPT, 7, LINUX_NET_ADDR_BYTES - 1),
          "a short accept trailer is refused");
    CHECK(linux_reply_shape_ok(LINUX_RECVFROM, 10, 10 + LINUX_NET_ADDR_BYTES),
          "recvfrom's ret IS its data length");
    CHECK(!linux_reply_shape_ok(LINUX_RECVFROM, 9, 10 + LINUX_NET_ADDR_BYTES),
          "recvfrom's ret contradicting the data length is refused");
    CHECK(linux_reply_zero_ok(LINUX_RECVFROM),
          "a zero-byte recvfrom is a legal count-truth success");

    printf("linux_net_bridge_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
