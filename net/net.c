#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "../kernel/kernel_io.h"

// ─── Our MAC address (matches cluster-node1 in Makefile) ─────────────────────
MACAddr net_my_mac = {{ 0x52, 0x54, 0x00, 0x12, 0x34, 0x01 }};

// ─── Static packet buffer pool ───────────────────────────────────────────────
static uint8_t  pkt_pool[NET_PKT_BUF_COUNT][NET_PKT_BUF_SIZE]
                __attribute__((aligned(16)));
static uint8_t  pkt_used[NET_PKT_BUF_COUNT];

void net_init(void) {
    for (int i = 0; i < NET_PKT_BUF_COUNT; i++) pkt_used[i] = 0;
    kernel_serial_print("[NET] Packet buffer pool ready "
                        "(" __STRING(NET_PKT_BUF_COUNT) " x 2 KB).\n");
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
