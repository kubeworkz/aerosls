#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "tcp.h"    // Navigator-Parity Gap Roadmap Phase 5c -- tcp_conns[]/TCP_MAX_CONNS for sys_sls_net_status()
#include "dhcp.h"   // Navigator-Parity Gap Roadmap Phase 5c -- dhcp_is_bound() for sys_sls_net_status()
#include "../kernel/kernel_io.h"

// ─── Runtime network identity ─────────────────────────────────────────────────
// MAC is zero-initialised here; e1000_init() overwrites it from the NIC EEPROM.
// IP/GW default to the compile-time values from include/config.h; Phase H2
// (DHCP) will update them at runtime without a recompile.
MACAddr  net_my_mac;                         // filled by e1000_init()
IPv4Addr net_my_ip  = KERNEL_STATIC_IP;      // 10.0.2.15 by default
IPv4Addr net_gw_ip  = KERNEL_STATIC_GW;      // 10.0.2.2  by default
// Navigator-Parity Gap Roadmap Phase 5a: subnet mask, updated by DHCP
// (dhcp.c's dhcp_recv()) the same way net_my_ip/net_gw_ip already are.
IPv4Addr net_subnet_mask = KERNEL_STATIC_SUBNET;  // 255.255.255.0 by default

// ─── Static packet buffer pool ───────────────────────────────────────────────
static uint8_t  pkt_pool[NET_PKT_BUF_COUNT][NET_PKT_BUF_SIZE]
                __attribute__((aligned(16)));
static uint8_t  pkt_used[NET_PKT_BUF_COUNT];

void net_init(void) {
    for (int i = 0; i < NET_PKT_BUF_COUNT; i++) pkt_used[i] = 0;
    kernel_serial_printf("[NET] Packet buffer pool ready (%d x 2 KB).\n",
                         NET_PKT_BUF_COUNT);
    // Announce our presence via ARP
    arp_announce();
}

void* net_alloc_buf(void) {
    for (int i = 0; i < NET_PKT_BUF_COUNT; i++) {
        if (!pkt_used[i]) { pkt_used[i] = 1; return pkt_pool[i]; }
    }
    return 0;
}

void net_free_buf(void* buf) {
    for (int i = 0; i < NET_PKT_BUF_COUNT; i++) {
        if ((void*)pkt_pool[i] == buf) { pkt_used[i] = 0; return; }
    }
}

// ─── Top-level receive dispatcher ─────────────────────────────────────────────
void net_rx_dispatch(void* frame, uint16_t len) {
    if (len < ETH_HDR_LEN) return;
    struct EthernetHeader* eth = (struct EthernetHeader*)frame;
    uint16_t et = ntohs(eth->ethertype);

    if (et == ETHERTYPE_ARP) {
        if (len >= ETH_HDR_LEN + (int)sizeof(struct ARPPacket))
            arp_handle_packet(eth, (struct ARPPacket*)((uint8_t*)frame + ETH_HDR_LEN));
    } else if (et == ETHERTYPE_IPV4) {
        if (len >= ETH_HDR_LEN + (int)sizeof(struct IPv4Header))
            ipv4_handle_packet(eth, (struct IPv4Header*)((uint8_t*)frame + ETH_HDR_LEN));
    }
}

// ─── sys_sls_net_status ───────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 5c: Terminal-facing twin of GET
// /api/network/status (net/http.c's api_network_status_json()) -- same
// underlying data (net_my_ip/net_gw_ip/net_subnet_mask/net_my_mac,
// dhcp_is_bound(), tcp_conns[]/TCP_MAX_CONNS), printed to the serial console
// instead of returned as JSON. Small local tcp_state_name() copy rather than
// sharing http.c's static one -- this codebase's established convention of
// each translation unit keeping its own small string/name helpers (see
// group_profile.c's gp_streq, authlist.c's own copy, etc.), not a shared
// module.
static const char* net_tcp_state_name(TCPState s) {
    switch (s) {
        case TCP_CLOSED:       return "CLOSED";
        case TCP_LISTEN:       return "LISTEN";
        case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_ESTABLISHED:  return "ESTABLISHED";
        case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_LAST_ACK:     return "LAST_ACK";
        case TCP_FIN_WAIT:     return "FIN_WAIT";
        case TCP_TIME_WAIT:    return "TIME_WAIT";
        case TCP_SYN_SENT:     return "SYN_SENT";
        default:               return "UNKNOWN";
    }
}

void sys_sls_net_status(void) {
    uint32_t ip = ntohl(net_my_ip);
    uint32_t gw = ntohl(net_gw_ip);
    uint32_t sm = ntohl(net_subnet_mask);
    kernel_serial_printf(
        "\n[NET] Network Status\n"
        " IP:          %u.%u.%u.%u\n"
        " Gateway:     %u.%u.%u.%u\n"
        " Subnet Mask: %u.%u.%u.%u\n"
        " MAC:         %02x:%02x:%02x:%02x:%02x:%02x\n"
        " DHCP Bound:  %s\n",
        (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF,
        (gw>>24)&0xFF, (gw>>16)&0xFF, (gw>>8)&0xFF, gw&0xFF,
        (sm>>24)&0xFF, (sm>>16)&0xFF, (sm>>8)&0xFF, sm&0xFF,
        net_my_mac.b[0], net_my_mac.b[1], net_my_mac.b[2],
        net_my_mac.b[3], net_my_mac.b[4], net_my_mac.b[5],
        dhcp_is_bound() ? "yes" : "no (static fallback)");

    uint32_t active = 0;
    uint32_t by_state[TCP_SYN_SENT + 1];
    for (uint32_t s = 0; s <= TCP_SYN_SENT; s++) by_state[s] = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!tcp_conns[i].active) continue;
        active++;
        by_state[tcp_conns[i].state]++;
    }
    kernel_serial_printf(" TCP Pool:    %u/%u active\n", active, (uint32_t)TCP_MAX_CONNS);
    for (uint32_t s = 0; s <= TCP_SYN_SENT; s++) {
        if (!by_state[s]) continue;
        kernel_serial_printf("   %-14s %u\n", net_tcp_state_name((TCPState)s), by_state[s]);
    }
    kernel_serial_print("\n");
}
