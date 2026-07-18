#include "arp.h"
#include "e1000.h"
#include "../kernel/kernel_io.h"

struct ARPEntry arp_table[ARP_TABLE_SIZE];

// ─── String/memory helpers ────────────────────────────────────────────────────
static void arp_memcpy(void* d, const void* s, int n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

static int mac_eq(MACAddr a, MACAddr b) {
    for (int i = 0; i < 6; i++) if (a.b[i] != b.b[i]) return 0;
    return 1;
}

static const MACAddr MAC_BCAST = {{ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }};

// ─── ARP table helpers ────────────────────────────────────────────────────────
void arp_update(IPv4Addr ip, MACAddr mac) {
    // Overwrite existing entry for this IP if present
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            arp_table[i].mac = mac;
            return;
        }
    }
    // Find a free slot
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip    = ip;
            arp_table[i].mac   = mac;
            arp_table[i].valid = 1;
            return;
        }
    }
}

int arp_lookup(IPv4Addr ip, MACAddr* out) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            *out = arp_table[i].mac;
            return 1;
        }
    }
    return 0;
}

// ─── Build and send an Ethernet + ARP frame ───────────────────────────────────
static void arp_send(uint16_t op, MACAddr dst_mac, IPv4Addr dst_ip) {
    uint8_t* pkt = (uint8_t*)net_alloc_buf();
    if (!pkt) return;

    struct EthernetHeader* eth = (struct EthernetHeader*)pkt;
    struct ARPPacket* arp = (struct ARPPacket*)(pkt + ETH_HDR_LEN);

    eth->dst       = dst_mac;
    eth->src       = net_my_mac;
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->hw_type    = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ETHERTYPE_IPV4);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->operation  = htons(op);
    arp->sender_mac = net_my_mac;
    arp->sender_ip  = NET_MY_IP;
    arp->target_mac = dst_mac;
    arp->target_ip  = dst_ip;

    e1000_transmit_packet(pkt, (uint16_t)(ETH_HDR_LEN + sizeof(struct ARPPacket)));
    net_free_buf(pkt);
}

void arp_send_request(IPv4Addr target_ip) {
    MACAddr zero = {{ 0,0,0,0,0,0 }};
    arp_send(ARP_OP_REQUEST, MAC_BCAST, target_ip);
    (void)zero;
}

// Gratuitous ARP: tell everyone our IP→MAC mapping on startup
void arp_announce(void) {
    arp_send(ARP_OP_REQUEST, MAC_BCAST, NET_MY_IP);
    kernel_serial_printf("[ARP] Announced 10.0.2.15 -> %02x:%02x:%02x:%02x:%02x:%02x\n",
        net_my_mac.b[0], net_my_mac.b[1], net_my_mac.b[2],
        net_my_mac.b[3], net_my_mac.b[4], net_my_mac.b[5]);
}

// ─── Receive handler ──────────────────────────────────────────────────────────
void arp_handle_packet(struct EthernetHeader* eth, struct ARPPacket* pkt) {
    // Learn the sender's address
    arp_update(pkt->sender_ip, pkt->sender_mac);

    // Reply if this is a request targeting our IP
    if (ntohs(pkt->operation) == ARP_OP_REQUEST &&
        pkt->target_ip == NET_MY_IP) {
        arp_send(ARP_OP_REPLY, pkt->sender_mac, pkt->sender_ip);
    }
    (void)eth;
    (void)mac_eq;  // suppress unused warning
    (void)arp_memcpy;
}
