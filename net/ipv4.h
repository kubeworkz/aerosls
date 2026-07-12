#ifndef IPV4_H
#define IPV4_H

#include "net.h"

void ipv4_handle_packet(struct EthernetHeader* eth, struct IPv4Header* ip);
void ipv4_send(IPv4Addr dst_ip, uint8_t proto, void* payload, uint16_t payload_len);

#endif /* IPV4_H */
