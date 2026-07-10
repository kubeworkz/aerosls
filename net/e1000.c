#include <stdint.h>
#include "e1000.h"

uint64_t e1000_mmio_base = 0;

static struct E1000TxDesc tx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static uint16_t tx_tail = 0;

void e1000_transmit_packet(void* physical_buffer, uint16_t size) {
    volatile uint32_t* tdt = (volatile uint32_t*)(e1000_mmio_base + E1000_REG_TDT);

    struct E1000TxDesc* desc = &tx_ring[tx_tail];
    desc->buffer_addr = (uint64_t)physical_buffer;
    desc->length      = size;
    desc->cmd         = (1 << 0) | (1 << 1); // EOP + RS
    desc->status      = 0;

    tx_tail = (tx_tail + 1) % E1000_RING_SIZE;
    *tdt = tx_tail;

    while (!(desc->status & 0x01)) {
        __asm__ volatile("pause");
    }
}
