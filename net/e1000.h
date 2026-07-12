#ifndef E1000_H
#define E1000_H

#include <stdint.h>

#define E1000_REG_CTRL    0x0000
#define E1000_REG_STATUS  0x0008
#define E1000_REG_RDBAL   0x2800
#define E1000_REG_RDBAH   0x2804
#define E1000_REG_RDLEN   0x2808
#define E1000_REG_RDH     0x2810
#define E1000_REG_RDT     0x2818
#define E1000_REG_RCTL    0x0100   // Receive Control
#define E1000_REG_TDBAL   0x3800
#define E1000_REG_TDBAH   0x3804
#define E1000_REG_TDLEN   0x3808
#define E1000_REG_TDH     0x3810
#define E1000_REG_TDT     0x3818
#define E1000_REG_TCTL    0x0400   // Transmit Control
#define E1000_REG_RAL0    0x5400   // Receive Address Low 0
#define E1000_REG_RAH0    0x5404   // Receive Address High 0

#define E1000_RCTL_EN     (1<<1)
#define E1000_RCTL_BAM    (1<<15)  // Broadcast Accept
#define E1000_RCTL_BSIZE  0        // 2048-byte buffers
#define E1000_TCTL_EN     (1<<1)
#define E1000_TCTL_PSP    (1<<3)

struct E1000TxDesc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct E1000RxDesc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

#define E1000_RING_SIZE   128
#define E1000_RX_BUF_SIZE 2048

extern uint64_t e1000_mmio_base;

void e1000_init(uint64_t mmio_base);
void e1000_transmit_packet(void* physical_buffer, uint16_t size);
void e1000_poll_rx(void);   // drain receive ring; dispatches to net_rx_dispatch

#endif // E1000_H
