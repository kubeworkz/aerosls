#ifndef ARP_H
#define ARP_H

#include "net.h"

#define ARP_TABLE_SIZE 16

struct ARPEntry {
    IPv4Addr ip;
    MACAddr  mac;
    uint8_t  valid;
};

extern struct ARPEntry arp_table[ARP_TABLE_SIZE];

void    arp_update(IPv4Addr ip, MACAddr mac);
int     arp_lookup(IPv4Addr ip, MACAddr* out_mac);
void    arp_send_request(IPv4Addr target_ip);
void    arp_announce(void);
void    arp_handle_packet(struct EthernetHeader* eth, struct ARPPacket* pkt);

#endif /* ARP_H */
