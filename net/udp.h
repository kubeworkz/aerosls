#ifndef UDP_H
#define UDP_H

#include "net.h"

#define IP_PROTO_UDP 17

struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;   // header + data, in network byte order
    uint16_t checksum; // optional; we send 0 (disabled)
} __attribute__((packed));

// Send a UDP datagram.
// src_ip=0 uses net_my_ip; dst_ip=0xFFFFFFFF sends a broadcast from 0.0.0.0
// (needed for DHCP DISCOVER/REQUEST before we have an IP).
void udp_send(IPv4Addr src_ip, IPv4Addr dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void* data, uint16_t len);

// Called by ipv4_handle_packet for protocol=17 packets.
void udp_handle_packet(struct IPv4Header* ip, uint16_t plen);

// Register a callback for incoming UDP on a given port.
// Only one callback per port; 0 = unregister.
typedef void (*UDPCallback)(const uint8_t* data, uint16_t len, IPv4Addr src_ip);
void udp_register(uint16_t port, UDPCallback cb);

#endif /* UDP_H */
