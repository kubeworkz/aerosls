#include "net.h"
#include "arp.h"
#include "ipv4.h"
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
