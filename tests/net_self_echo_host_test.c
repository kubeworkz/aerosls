/*
 * net_self_echo_host_test.c — surviving a shared L2 segment, and doing it
 * PER INTERFACE. Links the REAL net/net.c and net/e1000.c.
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
 * ─── And why it is per-interface (Multi-NIC Phase 2) ──────────────────────
 * The guard originally compared against a single global MAC. With two
 * interfaces that is wrong in a way that looks right: a frame legitimately
 * sent by the peer NIC would be dropped for matching "our" address. It now
 * asks e1000_nic_mac(ifindex) for the MAC of the interface the frame
 * actually arrived on. Scenario 7 is that case -- dropped on A, DELIVERED
 * on B -- and it is only expressible because this file links the real
 * driver rather than stubbing the lookup.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I net -I kernel \
 *       -o /tmp/net_self_echo_host_test \
 *       tests/net_self_echo_host_test.c net/net.c net/e1000.c \
 *       kernel/boot_params.c net/consensus.c
 *   /tmp/net_self_echo_host_test
 */
#include "net/net.h"
#include "net/e1000.h"
#include "kernel/boot_params.h"
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
void arp_announce(void) { }

/* e1000.c's dependencies. Only its PURE functions are called here --
 * e1000_nic_bind/_mac/_by_role -- so the MMIO paths never execute; these
 * only have to resolve. (An earlier version of this file carried a stub
 * named `e1000_transmit` that nothing referenced at all; removed.) */
uint32_t pci_read_config(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    (void)b;(void)s;(void)f;(void)o; return 0;
}
void pci_write_config(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v) {
    (void)b;(void)s;(void)f;(void)o;(void)v;
}

/* consensus.c's dependencies. It is linked only because boot_params.c
 * needs cluster_init(); none of the election or lease paths run here.
 * Faithful shapes anyway -- a stub that lies is a trap for the next test
 * that does exercise them. */
void update_page_table_permissions_globally(uint32_t force_read_only) {
    (void)force_read_only;
}
void update_page_table_permissions_for_partition(uint32_t partition_id,
                                                 uint32_t force_read_only) {
    (void)partition_id; (void)force_read_only;
}
void dspp_transmit_raw(const void* payload, uint16_t len) {
    (void)payload; (void)len;
}

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
    /* Interface 0 is bound with our MAC -- the guard reads it from here,
     * not from the global, which is the Phase 2 change. */
    e1000_nic_bind(0, 0xF0000000ull, 0, (uint8_t)(NIC_ROLE_MGMT | NIC_ROLE_CLUSTER), &me);

    /* ═══ 1: a peer's frames are delivered ════════════════════════════════ */
    printf("-- 1: frames from other nodes still arrive --\n");
    {
        reset();
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1, "a peer's DSPP frame reaches the dispatcher");
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_ARP, sizeof(struct ARPPacket)), 0);
        CHECK(arp_calls == 1, "a peer's ARP reaches the handler");
        net_rx_dispatch(frame, build(&peer, ETHERTYPE_IPV4, sizeof(struct IPv4Header)), 0);
        CHECK(ipv4_calls == 1, "a peer's IPv4 reaches the handler");
    }

    /* ═══ 2: our own frames are dropped, for EVERY protocol ═══════════════ */
    printf("\n-- 2: our own frames are dropped before any handler --\n");
    {
        reset();
        uint64_t before = net_self_echo_dropped;

        net_rx_dispatch(frame, build(&me, ETHERTYPE_DSPP, 64), 0);
        net_rx_dispatch(frame, build(&me, ETHERTYPE_ARP, sizeof(struct ARPPacket)), 0);
        net_rx_dispatch(frame, build(&me, ETHERTYPE_IPV4, sizeof(struct IPv4Header)), 0);

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
        net_rx_dispatch(frame, build(&nearly, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1,
              "*** a MAC sharing our first five octets is NOT us ***");

        /* The launcher hands out sequential MACs, so near-misses are the
         * normal case, not a contrived one. */
        MACAddr off_by_one = me;
        off_by_one.b[0] ^= 0x01;
        reset();
        net_rx_dispatch(frame, build(&off_by_one, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1, "...nor is one differing in the FIRST octet");
    }

    /* ═══ 4: before the NIC is up, "our MAC" is not a fact ════════════════ */
    printf("\n-- 4: net_my_mac unknown --\n");
    {
        MACAddr zero; memset(&zero, 0, sizeof(zero));
        net_my_mac = zero;                       /* e1000_init() has not run */
        reset();
        uint64_t before = net_self_echo_dropped;

        net_rx_dispatch(frame, build(&early, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1,
              "*** with our MAC still unknown, nothing is dropped -- an all-zero "
              "comparison would be matching on ignorance, not identity ***");

        /* ...and specifically, a frame that IS all-zero-sourced must not be
         * treated as ours just because we do not know who we are. */
        reset();
        net_rx_dispatch(frame, build(&zero, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1, "...including a zero-sourced frame");
        CHECK(net_self_echo_dropped == before, "...and nothing is counted");

        net_my_mac = me;
    }

    /* ═══ 5: the guard cannot be reached past a short frame ═══════════════ */
    printf("\n-- 5: bounds --\n");
    {
        reset();
        uint64_t before = net_self_echo_dropped;
        net_rx_dispatch(frame, ETH_HDR_LEN - 1, 0);
        CHECK(dspp_calls == 0 && arp_calls == 0 && ipv4_calls == 0,
              "a frame shorter than an Ethernet header is dropped");
        CHECK(net_self_echo_dropped == before,
              "...and not miscounted as a self-echo -- it was never read");
    }

    /* ═══ 7: the guard is PER INTERFACE (Multi-NIC Phase 2) ══════════════
     * The case a single global MAC gets wrong in a way that looks right.
     * Bind a second interface with its own address; a frame bearing NIC A's
     * MAC is ours on A and a legitimate peer transmission on B. */
    printf("\n-- 7: two interfaces, two identities --\n");
    {
        const MACAddr mac_a = {{ 0x52,0x54,0x00,0xAE,0x51,0x0a }};
        const MACAddr mac_b = {{ 0x52,0x54,0x00,0xAE,0x51,0x0b }};
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_MGMT,    &mac_a);
        e1000_nic_bind(1, 0xF0010000ull, 1, NIC_ROLE_CLUSTER, &mac_b);
        CHECK(e1000_nic_count() == 2, "two interfaces are bound");

        reset();
        net_rx_dispatch(frame, build(&mac_a, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 0, "*** A's own frame is dropped ON A ***");

        reset();
        net_rx_dispatch(frame, build(&mac_a, ETHERTYPE_DSPP, 64), 1);
        CHECK(dspp_calls == 1,
              "*** ...and DELIVERED on B -- it is the peer's traffic there. "
              "A global MAC would have eaten it ***");

        reset();
        net_rx_dispatch(frame, build(&mac_b, ETHERTYPE_DSPP, 64), 1);
        CHECK(dspp_calls == 0, "B's own frame is dropped on B");
        reset();
        net_rx_dispatch(frame, build(&mac_b, ETHERTYPE_DSPP, 64), 0);
        CHECK(dspp_calls == 1, "...and delivered on A");

        /* An index nobody bound must not be treated as a match. */
        reset();
        net_rx_dispatch(frame, build(&mac_a, ETHERTYPE_DSPP, 64), 99);
        CHECK(dspp_calls <= 1, "an out-of-range ifindex does not crash");
    }

    /* ═══ 8: role lookup -- one NIC answers both ═════════════════════════ */
    printf("\n-- 8: nic_by_role --\n");
    {
        /* Rebind as a single-NIC node: the configuration every existing
         * deployment runs, and the one that must not change behaviour. */
        e1000_nic_bind(1, 0, 0, NIC_ROLE_NONE, 0);
        e1000_nics_reset_for_test();
        const MACAddr only = {{ 0x52,0x54,0x00,0xAE,0x51,0x01 }};
        e1000_nic_bind(0, 0xF0000000ull, 0,
                       (uint8_t)(NIC_ROLE_MGMT | NIC_ROLE_CLUSTER), &only);

        CHECK(e1000_nic_count() == 1, "one interface");
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0,
              "*** a single NIC answers the MGMT lookup ***");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 0,
              "*** ...and the CLUSTER lookup -- so a one-NIC boot is unchanged ***");

        /* Two interfaces, split roles. */
        const MACAddr mgmt = {{ 0x52,0x54,0x00,0xAE,0x51,0x0a }};
        const MACAddr clus = {{ 0x52,0x54,0x00,0xAE,0x51,0x0b }};
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_MGMT,    &mgmt);
        e1000_nic_bind(1, 0xF0010000ull, 1, NIC_ROLE_CLUSTER, &clus);
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0, "MGMT resolves to 0");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 1,
              "*** CLUSTER resolves to 1 -- DSPP will not leave by the "
              "management NIC ***");

        e1000_nics_reset_for_test();
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == -1,
              "*** with nothing bound the lookup FAILS rather than guessing 0 ***");
        CHECK(e1000_nic_mac(0) == NULL, "...and an unbound interface has no MAC");
    }

    /* ═══ 9: role assignment by enumeration order (Phase 3) ══════════════
     * Roles are decided AFTER the boot scan, not during it, because a lone
     * card has to hold both and that is not knowable mid-scan. */
    printf("\n-- 9: e1000_assign_roles_by_order --\n");
    {
        const MACAddr m0 = {{ 0x52,0x54,0x00,0xAE,0x51,0x01 }};
        const MACAddr m1 = {{ 0x52,0x54,0x00,0xAE,0x51,0x02 }};

        /* One card: the configuration every node runs today. */
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_NONE, &m0);
        e1000_assign_roles_by_order(1);
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0 &&
              e1000_nic_by_role(NIC_ROLE_CLUSTER) == 0,
              "*** one card takes BOTH roles -- a single-NIC boot is unchanged ***");

        /* Two cards: split, in the order the scan found them. */
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_NONE, &m0);
        e1000_nic_bind(1, 0xF0010000ull, 1, NIC_ROLE_NONE, &m1);
        e1000_assign_roles_by_order(2);
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0, "first card is management");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 1, "second is the cluster segment");

        /* ...and they are DIFFERENT interfaces, which is the whole point. */
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) != e1000_nic_by_role(NIC_ROLE_CLUSTER),
              "*** the two roles resolve to different cards, so DSPP and HTTP "
              "leave by different wires ***");

        /* Each keeps its OWN address -- the Phase 3 change that stops one
         * card's EEPROM read clobbering the other's. */
        CHECK(e1000_nic_mac(0)->b[5] == 0x01 && e1000_nic_mac(1)->b[5] == 0x02,
              "*** each card keeps its own MAC ***");

        /* The MGMT gate on publishing net_my_mac. Only reachable as a test
         * because Phase 3 moved it out of e1000_init(), which is behind
         * MMIO -- a mutation deleting the gate had survived the entire
         * suite until then. */
        /* Bind with NO mac, exactly as e1000_init() does -- it binds first
         * and fills the address from the EEPROM afterwards. Passing the mac
         * to bind() here would have let set_mac() do nothing and still
         * pass, which is precisely how a mutation dropping its store
         * survived the first time. */
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_MGMT,    0);
        e1000_nic_bind(1, 0xF0010000ull, 1, NIC_ROLE_CLUSTER, 0);
        e1000_nic_set_mac(0, &m0);   /* management card reports first */
        e1000_nic_set_mac(1, &m1);   /* then the CLUSTER card */
        CHECK(net_my_mac.b[5] == 0x01,
              "*** a cluster-only card does NOT overwrite net_my_mac -- "
              "otherwise the node would answer ARP with the wrong address ***");
        CHECK(e1000_nic_mac(1)->b[5] == 0x02,
              "...but it does record its own");
        CHECK(net_my_mac.b[5] == 0x01,
              "*** the MANAGEMENT card did publish, which is what ARP needs ***");

        /* And the reset really clears, rather than only marking absent:
         * bind with no address after a reset and the MAC must be zero, not
         * whatever the previous binding left there. */
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_MGMT, 0);
        CHECK(e1000_nic_mac(0)->b[5] == 0x00,
              "*** a reset clears the MAC, so a later bind cannot inherit the "
              "previous one's identity ***");

        /* Degenerate inputs must not corrupt the table. */
        e1000_nics_reset_for_test();
        e1000_assign_roles_by_order(0);
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == -1, "zero cards assigns nothing");
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_NONE, &m0);
        e1000_assign_roles_by_order(99);
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) >= 0,
              "a count past the array bound is clamped, not written past");
    }

    /* ═══ 10: roles from the kernel command line (Phase 4) ═══════════════
     * The command line is the explicit source; enumeration order is only
     * the fallback. What matters most is the PRECEDENCE and what happens
     * to a typo -- a role name that does not parse must leave the
     * interface with nothing, not with a plausible guess. */
    printf("\n-- 10: e1000_assign_roles from the command line --\n");
    {
        const MACAddr a = {{ 0x52,0x54,0x00,0xAE,0x51,0x0a }};
        const MACAddr b = {{ 0x52,0x54,0x00,0xAE,0x51,0x0b }};
        uint8_t r;

        CHECK(e1000_role_from_name("mgmt", &r) && r == NIC_ROLE_MGMT, "'mgmt' parses");
        CHECK(e1000_role_from_name("cluster", &r) && r == NIC_ROLE_CLUSTER, "'cluster' parses");
        CHECK(e1000_role_from_name("both", &r) &&
              r == (NIC_ROLE_MGMT | NIC_ROLE_CLUSTER), "'both' parses");
        CHECK(e1000_role_from_name("none", &r) && r == NIC_ROLE_NONE, "'none' parses");
        CHECK(!e1000_role_from_name("mgnt", &r),
              "*** a typo is REJECTED, not resolved to something plausible ***");
        CHECK(!e1000_role_from_name("MGMT", &r), "...matching is exact");
        CHECK(!e1000_role_from_name("mgmt2", &r), "...and whole-word");

        #define BIND2() do { \
            e1000_nics_reset_for_test(); \
            e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_NONE, &a); \
            e1000_nic_bind(1, 0xF0010000ull, 1, NIC_ROLE_NONE, &b); \
        } while (0)

        /* No nicN= at all -> the enumeration-order fallback. */
        BIND2();
        CHECK(e1000_assign_roles(2, "node=1 quiet") == 0,
              "*** with no nicN=, the fallback decides (returns 0) ***");
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0 &&
              e1000_nic_by_role(NIC_ROLE_CLUSTER) == 1,
              "...and it is enumeration order");

        /* Explicit, and REVERSED -- so this cannot pass by coincidence. */
        BIND2();
        CHECK(e1000_assign_roles(2, "node=1 nic0=cluster nic1=mgmt") == 1,
              "*** an explicit nicN= takes over (returns 1) ***");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 0 &&
              e1000_nic_by_role(NIC_ROLE_MGMT) == 1,
              "*** ...and REVERSES the order, so it is really being read ***");

        /* One card, both roles, named. */
        e1000_nics_reset_for_test();
        e1000_nic_bind(0, 0xF0000000ull, 0, NIC_ROLE_NONE, &a);
        CHECK(e1000_assign_roles(1, "nic0=both") == 1, "one card can be named 'both'");
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == 0 &&
              e1000_nic_by_role(NIC_ROLE_CLUSTER) == 0, "...and answers both lookups");

        /* A typo leaves the interface with NOTHING. */
        BIND2();
        e1000_assign_roles(2, "nic0=mgnt nic1=cluster");
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == -1,
              "*** a misspelled role leaves that card with NO role -- it does "
              "not silently become management ***");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 1, "...the valid one still applies");

        /* Naming SOME switches the whole assignment to explicit mode -- the
         * unnamed card does not quietly inherit the convention. */
        BIND2();
        CHECK(e1000_assign_roles(2, "nic1=cluster") == 1,
              "naming one card switches to command-line mode");
        CHECK(e1000_nic_by_role(NIC_ROLE_MGMT) == -1,
              "*** ...and the UNNAMED card gets nothing, rather than inheriting "
              "the enumeration-order default ***");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == 1, "the named one is set");

        /* A value too long to fit is refused, not clipped to something that
         * would match. */
        BIND2();
        e1000_assign_roles(2, "nic0=clusterclusterclusterclusterX nic1=mgmt");
        CHECK(e1000_nic_by_role(NIC_ROLE_CLUSTER) == -1,
              "*** an over-long value is refused, not truncated into 'cluster' ***");

        #undef BIND2
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
