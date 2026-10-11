#include "ipv4.h"
#include "arp.h"
#include "tcp.h"
#include "udp.h"
#include "e1000.h"
#include "../kernel/kernel_io.h"

static uint16_t ip_id_counter = 1;

// ─── IPv4 header checksum ─────────────────────────────────────────────────────
static uint16_t ip_checksum(struct IPv4Header* hdr) {
    hdr->checksum = 0;
    int hlen = (hdr->version_ihl & 0x0F) * 4;
    return net_checksum(hdr, (size_t)hlen);
}

// ─── Build and transmit an Ethernet + IPv4 frame ─────────────────────────────
// dst_ip=0xFFFFFFFF: broadcast — Ethernet ff:ff:ff:ff:ff:ff, bypasses ARP.
// Callers (udp_send) pass src_ip explicitly; ipv4_send always uses NET_MY_IP.
//
// Loopback (dst_ip == our OWN address): delivered to THIS stack's receive
// path instead of the wire. QEMU user-net (slirp) does not hairpin a guest's
// packets back into that same guest, so a connect to our own address would
// ARP-resolve, transmit, and silently vanish — the local listener never sees
// the SYN, the accept gate never runs, and the P2 guard's quota-flood
// topology (E1 connects to E2's :8082 listener, both environments inside the
// same partition) can never produce its by-name refusal. Caught live: the
// L5 arm's flood step failed with "the flood did not reach the accept gate
// (is the guest's own address hairpinned by user-net?)" — it is not, and
// cannot be made to be. One hook closes it for every protocol: the frame is
// built exactly as the wire path builds it (same headers, same checksums)
// and handed to ipv4_handle_packet, so tcp_handle_segment / udp_handle_
// packet / icmp see a byte-identical segment and every reply routes back
// through this same hook (SYN → SYN-ACK → ACK each nest once; bounded).
static void ipv4_loopback_send(uint8_t proto, void* payload, uint16_t plen) {
    uint8_t* pkt = (uint8_t*)net_alloc_buf();
    if (!pkt) return;
    struct EthernetHeader* eth = (struct EthernetHeader*)pkt;
    struct IPv4Header*     ip  = (struct IPv4Header*)(pkt + ETH_HDR_LEN);
    uint8_t* dat = (uint8_t*)ip + sizeof(struct IPv4Header);

    eth->dst       = net_my_mac;   /* ourselves, both directions */
    eth->src       = net_my_mac;
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->dscp        = 0;
    ip->total_len   = htons((uint16_t)(sizeof(struct IPv4Header) + plen));
    ip->id          = htons(ip_id_counter++);
    ip->flags_frag  = 0;
    ip->ttl         = 64;
    ip->protocol    = proto;
    ip->src         = NET_MY_IP;
    ip->dst         = NET_MY_IP;
    ip->checksum    = ip_checksum(ip);

    for (uint16_t i = 0; i < plen; i++) dat[i] = ((uint8_t*)payload)[i];

    ipv4_handle_packet(eth, ip);
    net_free_buf(pkt);
}

void ipv4_send(IPv4Addr dst_ip, uint8_t proto, void* payload, uint16_t plen) {
    if (dst_ip == NET_MY_IP) {
        ipv4_loopback_send(proto, payload, plen);
        return;
    }
    uint8_t* pkt = (uint8_t*)net_alloc_buf();
    if (!pkt) return;

    // Resolve destination MAC
    MACAddr dst_mac;
    if (dst_ip == 0xFFFFFFFFUL) {
        // Broadcast — no ARP needed
        for (int i = 0; i < 6; i++) dst_mac.b[i] = 0xFF;
    } else {
        IPv4Addr resolve_ip = dst_ip;
        // Simple subnet check: same /24 is local
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
    uint8_t*               dat = (uint8_t*)ip + sizeof(struct IPv4Header);

    eth->dst       = dst_mac;
    eth->src       = net_my_mac;
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;  // version=4, IHL=5 (20 bytes)
    ip->dscp        = 0;
    ip->total_len   = htons((uint16_t)(sizeof(struct IPv4Header) + plen));
    ip->id          = htons(ip_id_counter++);
    ip->flags_frag  = 0;
    ip->ttl         = 64;
    ip->protocol    = proto;
    ip->src         = NET_MY_IP;
    ip->dst         = dst_ip;
    ip->checksum    = ip_checksum(ip);

    for (uint16_t i = 0; i < plen; i++) dat[i] = ((uint8_t*)payload)[i];

    uint16_t total = (uint16_t)(ETH_HDR_LEN + sizeof(struct IPv4Header) + plen);
    e1000_transmit(NIC_ROLE_MGMT, pkt, total);
    net_free_buf(pkt);
}

// ─── ICMP echo reply ──────────────────────────────────────────────────────────
static void icmp_echo_reply(struct EthernetHeader* eth,
                             struct IPv4Header* ip_in,
                             uint8_t* icmp, uint16_t icmp_len) {
    (void)eth;
    // Flip type from 8 (request) to 0 (reply); recalculate checksum
    icmp[0] = 0;                     // type = reply
    icmp[2] = 0; icmp[3] = 0;        // zero checksum field
    uint16_t ck = net_checksum(icmp, icmp_len);
    icmp[2] = (uint8_t)(ck >> 8);
    icmp[3] = (uint8_t)(ck & 0xFF);
    ipv4_send(ip_in->src, IP_PROTO_ICMP, icmp, icmp_len);
}

// ─── IPv4 receive handler ─────────────────────────────────────────────────────
void ipv4_handle_packet(struct EthernetHeader* eth, struct IPv4Header* ip) {
    // Accept packets addressed to us OR to the broadcast address.
    // Broadcast acceptance is required for DHCP (OFFER/ACK go to 255.255.255.255
    // before the client has an IP, or to our IP once bound).
    if (ip->dst != NET_MY_IP && ip->dst != 0xFFFFFFFFUL) return;

    int ihl = (ip->version_ihl & 0x0F) * 4;
    uint8_t* payload = (uint8_t*)ip + ihl;
    uint16_t plen    = (uint16_t)(ntohs(ip->total_len) - (uint16_t)ihl);

    if (ip->protocol == IP_PROTO_TCP && plen >= (uint16_t)sizeof(struct TCPHeader)) {
        tcp_handle_segment(ip, (struct TCPHeader*)payload, plen);
    } else if (ip->protocol == IP_PROTO_UDP && plen >= (uint16_t)sizeof(struct UDPHeader)) {
        udp_handle_packet(ip, plen);
    } else if (ip->protocol == IP_PROTO_ICMP && plen > 0 && payload[0] == 8) {
        icmp_echo_reply(eth, ip, payload, plen);
    }
}
