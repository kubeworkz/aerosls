#include "udp.h"
#include "arp.h"
#include "e1000.h"
#include "../kernel/kernel_io.h"

// ─── Port → callback table (small, fixed size) ───────────────────────────────
#define UDP_MAX_LISTENERS 4
static struct { uint16_t port; UDPCallback cb; } udp_listeners[UDP_MAX_LISTENERS];

void udp_register(uint16_t port, UDPCallback cb) {
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (udp_listeners[i].port == port || udp_listeners[i].cb == 0) {
            udp_listeners[i].port = port;
            udp_listeners[i].cb   = cb;
            return;
        }
    }
}

// ─── UDP transmit ─────────────────────────────────────────────────────────────
// Handles two special cases needed by DHCP:
//   src_ip = 0            → send from 0.0.0.0 (before we have an IP)
//   dst_ip = 0xFFFFFFFF   → broadcast, Ethernet dst = ff:ff:ff:ff:ff:ff,
//                            skips subnet check and ARP lookup
void udp_send(IPv4Addr src_ip, IPv4Addr dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void* data, uint16_t dlen)
{
    uint8_t* pkt = (uint8_t*)net_alloc_buf();
    if (!pkt) return;

    // Resolve Ethernet destination MAC
    MACAddr dst_mac;
    if (dst_ip == 0xFFFFFFFFUL) {
        // Broadcast — use ff:ff:ff:ff:ff:ff, no ARP needed
        for (int i = 0; i < 6; i++) dst_mac.b[i] = 0xFF;
    } else {
        IPv4Addr resolve_ip = dst_ip;
        if ((ntohl(dst_ip) & 0xFFFFFF00) != (ntohl(NET_MY_IP) & 0xFFFFFF00))
            resolve_ip = NET_GW_IP;
        if (!arp_lookup(resolve_ip, &dst_mac)) {
            arp_send_request(resolve_ip);
            net_free_buf(pkt);
            return;
        }
    }

    struct EthernetHeader* eth = (struct EthernetHeader*)pkt;
    struct IPv4Header*     ip  = (struct IPv4Header*)(pkt + ETH_HDR_LEN);
    struct UDPHeader*      udp = (struct UDPHeader*)((uint8_t*)ip + sizeof(*ip));
    uint8_t*               dat = (uint8_t*)udp + sizeof(*udp);

    // Ethernet header
    eth->dst       = dst_mac;
    eth->src       = net_my_mac;
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // IPv4 header
    static uint16_t _id;
    uint16_t udp_total = (uint16_t)(sizeof(struct UDPHeader) + dlen);
    ip->version_ihl = 0x45;
    ip->dscp        = 0;
    ip->total_len   = htons((uint16_t)(sizeof(struct IPv4Header) + udp_total));
    ip->id          = htons(++_id);
    ip->flags_frag  = 0;
    ip->ttl         = 64;
    ip->protocol    = IP_PROTO_UDP;
    ip->src         = src_ip ? src_ip : (IPv4Addr)0;   // 0.0.0.0 for DHCP
    ip->dst         = dst_ip;
    // Compute IPv4 checksum
    ip->checksum    = 0;
    ip->checksum    = net_checksum(ip, sizeof(struct IPv4Header));

    // UDP header (checksum = 0 = disabled for UDP over IPv4)
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_total);
    udp->checksum = 0;

    // Copy payload
    for (uint16_t i = 0; i < dlen; i++) dat[i] = ((const uint8_t*)data)[i];

    uint16_t frame_len = (uint16_t)(ETH_HDR_LEN + sizeof(*ip) + udp_total);
    e1000_transmit_packet(pkt, frame_len);
    net_free_buf(pkt);
}

// ─── UDP receive — called by ipv4_handle_packet ───────────────────────────────
void udp_handle_packet(struct IPv4Header* ip, uint16_t plen) {
    if (plen < (uint16_t)sizeof(struct UDPHeader)) return;
    struct UDPHeader* udp = (struct UDPHeader*)((uint8_t*)ip
                            + (ip->version_ihl & 0x0F) * 4);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t udp_len  = ntohs(udp->length);
    if (udp_len < sizeof(struct UDPHeader)) return;
    uint16_t data_len = (uint16_t)(udp_len - sizeof(struct UDPHeader));
    const uint8_t* data = (const uint8_t*)udp + sizeof(struct UDPHeader);

    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (udp_listeners[i].cb && udp_listeners[i].port == dst_port) {
            udp_listeners[i].cb(data, data_len, ip->src);
            return;
        }
    }
}
