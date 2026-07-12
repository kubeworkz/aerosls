#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include "../include/config.h"

// ─── MAC / IPv4 types ─────────────────────────────────────────────────────────
typedef struct { uint8_t b[6]; } __attribute__((packed)) MACAddr;
typedef uint32_t IPv4Addr;   // always stored in network (big-endian) byte order

// ─── Byte-order helpers (x86_64 is little-endian) ────────────────────────────
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x>>8)|(x<<8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x&0xFF)<<24)|((x&0xFF00)<<8)|((x>>8)&0xFF00)|(x>>24); }
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

// ─── Ethernet ─────────────────────────────────────────────────────────────────
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800
#define ETH_HDR_LEN     14

struct EthernetHeader {
    MACAddr  dst;
    MACAddr  src;
    uint16_t ethertype;   // big-endian
} __attribute__((packed));

// ─── ARP ──────────────────────────────────────────────────────────────────────
#define ARP_HW_ETHERNET 1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct ARPPacket {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t operation;
    MACAddr  sender_mac;
    IPv4Addr sender_ip;
    MACAddr  target_mac;
    IPv4Addr target_ip;
} __attribute__((packed));

// ─── IPv4 ─────────────────────────────────────────────────────────────────────
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6

struct IPv4Header {
    uint8_t  version_ihl;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    IPv4Addr src;
    IPv4Addr dst;
} __attribute__((packed));

// ─── TCP ──────────────────────────────────────────────────────────────────────
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

struct TCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t  data_offset;  // upper nibble = header dwords
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

// ─── 1's-complement checksum ─────────────────────────────────────────────────
static inline uint16_t net_checksum(const void* data, size_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len)         sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ─── Our addresses ────────────────────────────────────────────────────────────
// Both are runtime variables so DHCP (Phase H2) can update them without a
// recompile.  Defaults come from include/config.h (KERNEL_STATIC_IP / _GW).
// Use NET_MY_IP / NET_GW_IP as before — they now expand to the variables.
extern IPv4Addr net_my_ip;
extern IPv4Addr net_gw_ip;
#define NET_MY_IP   (net_my_ip)
#define NET_GW_IP   (net_gw_ip)

// Runtime MAC address — filled from the NIC's EEPROM by e1000_init().
// Zero until e1000_init() has run.
extern MACAddr  net_my_mac;

#define NET_HTTP_PORT  KERNEL_HTTP_PORT

// ─── Packet buffer pool ───────────────────────────────────────────────────────
#define NET_PKT_BUF_COUNT 64
#define NET_PKT_BUF_SIZE  2048

void  net_init(void);
void* net_alloc_buf(void);
void  net_free_buf(void* buf);

// ─── Top-level receive dispatcher ─────────────────────────────────────────────
// Called from e1000 interrupt handler / poll loop with the raw frame bytes.
void net_rx_dispatch(void* frame, uint16_t len);

#endif /* NET_H */
