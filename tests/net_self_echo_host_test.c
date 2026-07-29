/*
 * net_self_echo_host_test.c — N-Node Launcher Plan Phase 2: surviving a
 * shared L2 segment. Links the REAL net/net.c.
 *
 * ─── The hazard this phase introduces ─────────────────────────────────────
 * Two nodes were wired with `-netdev socket,listen=`/`connect=`, which is
 * strictly point-to-point and never delivers a node its own transmissions.
 * N nodes need a shared broadcast segment, and QEMU's answer is
 * `-netdev socket,mcast=` -- which forces IP_MULTICAST_LOOP ON, precisely
 * so several instances on one host can hear each other. The price is that
 * every sender also hears itself.
 *
 * That is a NEW input class, not a rarer version of an old one. Nothing in
 * the stack had ever seen a frame it sent. The specific danger is not DSPP
 * (which filters on node ids already) but ARP: every node compiles in the
 * SAME static IP, 10.0.2.15 (include/config.h), because DHCP times out on a
 * segment with no server. So a node hearing its own gratuitous ARP sees its
 * own address announced from elsewhere on the wire, and every other node
 * sees a second claimant for an address it also holds.
 *
 * Scenario 2 is therefore the point of the file: the drop happens at the
 * Ethernet layer, BEFORE any protocol handler runs, so it holds for
 * protocols that have no idea this problem exists.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I net -I kernel \
 *       -o /tmp/net_self_echo_host_test \
 *       tests/net_self_echo_host_test.c net/net.c
 *   /tmp/net_self_echo_host_test
 */
#include "net/net.h"
#include "net/tcp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
volatile uint64_t kernel_tick_counter = 0;

/* Counting stubs for the three protocol handlers net_rx_dispatch() routes
 * to. The whole question is which of them get reached, so counting is
 * exactly the right fidelity -- but each keeps the real signature, so a
 * change to one of those is a compile error here rather than a silent
 * mismatch. */
static int arp_calls = 0, ipv4_calls = 0, dspp_calls = 0;
void arp_handle_packet(struct EthernetHeader* eth, struct ARPPacket* p) {
    (void)eth; (void)p; arp_calls++;
}
void ipv4_handle_packet(struct EthernetHeader* eth, struct IPv4Header* p) {
    (void)eth; (void)p; ipv4_calls++;
}
void dspp_rx_dispatch(void* buf, uint16_t len) { (void)buf; (void)len; dspp_calls++; }

/* net.c's other dependencies. Real storage for tcp_conns[] rather than a
 * pointer, because sys_sls_net_status() indexes it -- nothing here calls
 * that, but a stub of the wrong shape is a trap for whoever adds a test
 * that does. */
struct TCPConn tcp_conns[TCP_MAX_CONNS];
int  tcp_port_is_listening(uint16_t port) { (void)port; return 0; }
int  dhcp_is_bound(void) { return 0; }
void e1000_transmit(const void* f, uint16_t l) { (void)f; (void)l; }
void arp_announce(void) { }

static uint8_t frame[512];

static uint16_t build(const MACAddr* src, uint16_t ethertype, uint16_t payload) {
    memset(frame, 0, sizeof(frame));
    struct EthernetHeader* eth = (struct EthernetHeader*)frame;
    for (int i = 0; i < 6; i++) eth->dst.b[i] = 0xFF;      /* broadcast */
    eth->src = *src;
    eth->ethertype = htons(ethertype);
    return (uint16_t)(ETH_HDR_LEN + payload);
}

static void reset(void) { arp_calls = ipv4_calls = dspp_calls = 0; }

int main(void) {
    printf("=== Phase 2: a shared segment echoes your own frames back ===\n\n");

    const MACAddr me    = {{ 0x52,0x54,0x00,0xAE,0x51,0x01 }};
    const MACAddr peer  = {{ 0x52,0x54,0x00,0xAE,0x51,0x02 }};
    const MACAddr nearly= {{ 0x52,0x54,0x00,0xAE,0x51,0x81 }};  /* last octet differs */
    const MACAddr early = {{ 0x52,0x54,0x00,0xAE,0x51,0x03 }};

    net_my_mac = me;

    /* ═══ 1: a peer's frames are delivered ════════════════════════════════ */
    printf("-- 1: frames from other nodes still arrive --\n");
    {
        reset();
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_DSPP, 64));
        CHECK(dspp_calls == 1, "a peer's DSPP frame reaches the dispatcher");
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_ARP, sizeof(struct ARPPacket)));
        CHECK(arp_calls == 1, "a peer's ARP reaches the handler");
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_IPV4, sizeof(struct IPv4Header)));
        CHECK(ipv4_calls == 1, "a peer's IPv4 reaches the handler");
    }

    /* ═══ 2: our own frames are dropped, for EVERY protocol ═══════════════ */
    printf("\n-- 2: our own frames are dropped before any handler --\n");
    {
        reset();
        uint64_t before = net_self_echo_dropped;

        net_rx_dispatch(frame, build(&me, ETHERTYPE_DSPP, 64));
        net_rx_dispatch(frame, build(&me, ETHERTYPE_ARP, sizeof(struct ARPPacket)));
        net_rx_dispatch(frame, build(&me, ETHERTYPE_IPV4, sizeof(struct IPv4Header)));

        CHECK(dspp_calls == 0, "our own DSPP frame is dropped");
        CHECK(arp_calls == 0,
              "*** our own ARP is dropped -- every node shares IP 10.0.2.15, so "
              "hearing your own gratuitous ARP is hearing a duplicate claimant ***");
        CHECK(ipv4_calls == 0, "our own IPv4 is dropped");
        CHECK(net_self_echo_dropped == before + 3,
              "*** all three counted -- the guard is at the Ethernet layer, so it "
              "covers protocols that know nothing about this problem ***");
    }

    /* ═══ 3: the match is the whole address ═══════════════════════════════ */
    printf("\n-- 3: it is an exact match, not a prefix --\n");
    {
        reset();
        net_rx_dispatch(frame, build(&nearly, ETHERTYPE_DSPP, 64));
        CHECK(dspp_calls == 1,
              "*** a MAC sharing our first five octets is NOT us ***");

        /* The launcher hands out sequential MACs, so near-misses are the
         * normal case, not a contrived one. */
        MACAddr off_by_one = me;
        off_by_one.b[0] ^= 0x01;
        reset();
        net_rx_dispatch(frame, build(&off_by_one, ETHERTYPE_DSPP, 64));
        CHECK(dspp_calls == 1, "...nor is one differing in the FIRST octet");
    }

    /* ═══ 4: before the NIC is up, "our MAC" is not a fact ════════════════ */
    printf("\n-- 4: net_my_mac unknown --\n");
    {
        MACAddr zero; memset(&zero, 0, sizeof(zero));
        net_my_mac = zero;                       /* e1000_init() has not run */
        reset();
        uint64_t before = net_self_echo_dropped;

        net_rx_dispatch(frame, build(&early, ETHERTYPE_DSPP, 64));
        CHECK(dspp_calls == 1,
              "*** with our MAC still unknown, nothing is dropped -- an all-zero "
              "comparison would be matching on ignorance, not identity ***");

        /* ...and specifically, a frame that IS all-zero-sourced must not be
         * treated as ours just because we do not know who we are. */
        reset();
        net_rx_dispatch(frame, build(&zero, ETHERTYPE_DSPP, 64));
        CHECK(dspp_calls == 1, "...including a zero-sourced frame");
        CHECK(net_self_echo_dropped == before, "...and nothing is counted");

        net_my_mac = me;
    }

    /* ═══ 5: the guard cannot be reached past a short frame ═══════════════ */
    printf("\n-- 5: bounds --\n");
    {
        reset();
        uint64_t before = net_self_echo_dropped;
        net_rx_dispatch(frame, ETH_HDR_LEN - 1);
        CHECK(dspp_calls == 0 && arp_calls == 0 && ipv4_calls == 0,
              "a frame shorter than an Ethernet header is dropped");
        CHECK(net_self_echo_dropped == before,
              "...and not miscounted as a self-echo -- it was never read");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
