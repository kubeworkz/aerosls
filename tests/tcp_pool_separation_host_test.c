/*
 * tcp_pool_separation_host_test.c — Multitenant Isolation Gap Analysis §5
 * item 4 / §7 item 1, Network Fairness Phase 2 verification: a standalone
 * host-buildable test for the REAL, unmodified net/tcp.c inbound/outbound
 * pool split (TCP_INBOUND_MAX_CONNS / TCP_OUTBOUND_RESERVED_CONNS, net/
 * tcp.h), not a reimplementation of it.
 *
 * ─── Why this test only exercises tcp_listen() and tcp_handle_segment()'s
 * SYN path, not tcp_connect()/tcp_accept()/tcp_recv() ──────────────────────
 * kernel/net_event.h's net_event_hlt_wait() is a `static inline` function
 * containing raw `sti; hlt` assembly -- privileged instructions that fault
 * immediately in userspace. tcp_connect()/tcp_accept()/tcp_recv() all spin
 * on this to wait for network events, so calling any of them here would
 * crash the test process, not exercise real logic. This is a pre-existing
 * property of net/tcp.c (true long before this pass), not something this
 * change introduces -- it's the same reason no host test anywhere in this
 * suite has ever linked net/tcp.c directly before now. tcp_listen() and
 * tcp_handle_segment()'s SYN-accept branch, by contrast, touch no hardware-
 * privileged instruction at all (tcp_handle_segment()'s only real
 * dependency, tcp_send_flags(), calls ipv4_send()/net_alloc_buf()/
 * net_free_buf(), all trivially fakeable), so they get real execution here.
 *
 * tcp_connect()'s own pool-scoping change (the loop bound at net/tcp.c's
 * "2. Find a free connection slot" comment, scanning
 * [TCP_INBOUND_MAX_CONNS, TCP_MAX_CONNS) instead of [0, TCP_MAX_CONNS)) is
 * therefore verified by compile-check and direct code review only, not
 * execution -- named honestly here rather than silently claimed as tested,
 * matching this project's own "verification ceiling honesty" discipline
 * (Multi-Node Partition Scaling Roadmap §2). What IS proven by execution
 * below (Scenario 2) is the property that actually matters for starvation
 * safety: once the inbound pool is completely saturated via the real
 * inbound-only allocation paths, not one single outbound-reserved slot
 * (indices [TCP_INBOUND_MAX_CONNS, TCP_MAX_CONNS)) was touched -- which is
 * exactly the guarantee tcp_connect()'s own disjoint range then depends on
 * to never be starved.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/tcp_pool_separation_host_test \
 *       tests/tcp_pool_separation_host_test.c net/tcp.c
 *   /tmp/tcp_pool_separation_host_test
 */
#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/arp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Fakes for tcp.c's non-privileged real dependencies ──────────────────
 * None of these are ever exercised on a code path that also calls
 * net_event_hlt_wait() in this test (see header comment), so they never
 * need to simulate anything beyond "don't crash, don't corrupt state." */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print(const char* s) { (void)s; }
static uint8_t fake_pkt_buf[2048];
void* net_alloc_buf(void) { return fake_pkt_buf; }
void  net_free_buf(void* buf) { (void)buf; }
void  ipv4_send(IPv4Addr dst_ip, uint8_t proto, void* payload, uint16_t payload_len) {
    (void)dst_ip; (void)proto; (void)payload; (void)payload_len;
}
int  arp_lookup(IPv4Addr ip, MACAddr* out_mac) { (void)ip; (void)out_mac; return 0; }
void arp_send_request(IPv4Addr target_ip) { (void)target_ip; }

IPv4Addr net_my_ip = 0x0102000AUL;      /* 10.0.2.1, arbitrary */
IPv4Addr net_gw_ip = 0x0202000AUL;
IPv4Addr net_subnet_mask = 0x00FFFFFFUL;
MACAddr  net_my_mac;

/* Only referenced from inside net_event_hlt_wait() (kernel/net_event.h),
 * which this test never actually calls (see header comment) -- defined
 * here purely to satisfy the linker, since tcp_connect()/tcp_accept()/
 * tcp_recv() (which do call it) still live in the same translation unit
 * and must link even though this test never invokes them. */
volatile uint64_t cpu_idle_wait_count = 0;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* Builds and delivers one inbound SYN segment to tcp_handle_segment(),
 * targeting `dst_port` (must already have a TCP_LISTEN slot) from a
 * distinct fake (src_ip, src_port) pair per call so each is treated as a
 * genuinely new connection attempt, not a retransmit of an existing one. */
static void send_fake_syn(uint16_t dst_port, uint16_t src_port, IPv4Addr src_ip) {
    struct IPv4Header ip;
    memset(&ip, 0, sizeof(ip));
    ip.src = src_ip;
    ip.dst = net_my_ip;

    struct TCPHeader seg;
    memset(&seg, 0, sizeof(seg));
    seg.src_port    = htons(src_port);
    seg.dst_port    = htons(dst_port);
    seg.seq         = htonl(1000);
    seg.data_offset = (uint8_t)(5 << 4);
    seg.flags       = TCP_FLAG_SYN;

    tcp_handle_segment(&ip, &seg, (uint16_t)sizeof(seg));
}

static int count_active(int lo, int hi) {
    int n = 0;
    for (int i = lo; i < hi; i++) if (tcp_conns[i].active) n++;
    return n;
}

int main(void) {
    tcp_init();

    /* ── Scenario 1: tcp_listen() itself is scoped to the inbound pool --
     * exactly TCP_INBOUND_MAX_CONNS successful listens/accepts-worth of
     * slots exist in that range, and the range past it is never touched. ── */
    {
        int listen_ids[TCP_INBOUND_MAX_CONNS];
        int got = 0;
        for (int i = 0; i < TCP_INBOUND_MAX_CONNS; i++) {
            int id = tcp_listen((uint16_t)(20000 + i));
            if (id < 0) break;
            CHECK(id >= 0 && id < TCP_INBOUND_MAX_CONNS,
                  "s1: every slot tcp_listen() hands out is within the inbound range");
            listen_ids[got++] = id;
        }
        CHECK(got == TCP_INBOUND_MAX_CONNS,
              "s1: tcp_listen() can fill the ENTIRE inbound pool (TCP_INBOUND_MAX_CONNS successful calls)");
        CHECK(tcp_listen(30000) == -1,
              "s1: with the inbound pool fully saturated, one more tcp_listen() call correctly fails (-1), not silently reusing a slot");
        CHECK(count_active(TCP_INBOUND_MAX_CONNS, TCP_MAX_CONNS) == 0,
              "s1: after saturating the inbound pool via tcp_listen() alone, the outbound-reserved range is still completely untouched");
        (void)listen_ids;
    }

    tcp_init();   // reset for the next scenario

    /* ── Scenario 2: the real starvation property this whole phase exists
     * to prevent -- an inbound SYN flood that completely fills the inbound
     * pool (via the REAL tcp_handle_segment() SYN-accept path, exercised
     * for real here) can NEVER touch a single outbound-reserved slot,
     * proven by direct inspection of tcp_conns[] after the flood. ────────── */
    {
        int listen_id = tcp_listen(3000);
        CHECK(listen_id >= 0, "s2: setup -- bind a listener on port 3000");

        // Flood with SYNs from distinct fake sources until the inbound pool
        // (listener + every accepted connection) is completely saturated.
        // TCP_INBOUND_MAX_CONNS - 1 more slots remain after the listener.
        for (int i = 0; i < TCP_INBOUND_MAX_CONNS - 1; i++) {
            send_fake_syn(3000, (uint16_t)(40000 + i), (IPv4Addr)(0x0A000001UL + (uint32_t)i));
        }
        CHECK(count_active(0, TCP_INBOUND_MAX_CONNS) == TCP_INBOUND_MAX_CONNS,
              "s2: the listener plus every accepted SYN together exactly saturate the inbound pool");
        CHECK(count_active(TCP_INBOUND_MAX_CONNS, TCP_MAX_CONNS) == 0,
              "s2: the outbound-reserved range remains completely untouched at full inbound saturation");

        // One more SYN, from yet another distinct fake source, once the
        // inbound pool is genuinely full: must be silently dropped (no
        // free inbound slot), not spill into the outbound range.
        send_fake_syn(3000, 50000, 0x0A0000FFUL);
        CHECK(count_active(0, TCP_INBOUND_MAX_CONNS) == TCP_INBOUND_MAX_CONNS,
              "s2: one more SYN past full inbound saturation changes nothing in the inbound range (correctly dropped, 'no free slots')");
        CHECK(count_active(TCP_INBOUND_MAX_CONNS, TCP_MAX_CONNS) == 0,
              "s2: and critically, that dropped SYN did NOT spill into the outbound-reserved range either -- the exact starvation this phase closes");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
