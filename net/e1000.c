#include <stdint.h>
#include "e1000.h"
#include "net.h"

uint64_t e1000_mmio_base = 0;

static struct E1000TxDesc tx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static struct E1000RxDesc rx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static uint8_t rx_bufs[E1000_RING_SIZE][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
static uint16_t tx_tail = 0;
static uint16_t rx_tail = 0;

static inline volatile uint32_t* reg(uint32_t off) {
    return (volatile uint32_t*)(e1000_mmio_base + off);
}

void e1000_init(uint64_t mmio_base) {
    e1000_mmio_base = mmio_base;

    // ── TX ring ──────────────────────────────────────────────────────────────
    for (int i = 0; i < E1000_RING_SIZE; i++) {
        tx_ring[i].buffer_addr = 0;
        tx_ring[i].status      = 0xF;  // mark all as done
    }
    *reg(E1000_REG_TDBAL) = (uint32_t)(uint64_t)tx_ring;
    *reg(E1000_REG_TDBAH) = (uint32_t)((uint64_t)tx_ring >> 32);
    *reg(E1000_REG_TDLEN) = E1000_RING_SIZE * sizeof(struct E1000TxDesc);
    *reg(E1000_REG_TDH)   = 0;
    *reg(E1000_REG_TDT)   = 0;
    *reg(E1000_REG_TCTL)  = E1000_TCTL_EN | E1000_TCTL_PSP;

    // ── RX ring ──────────────────────────────────────────────────────────────
    for (int i = 0; i < E1000_RING_SIZE; i++) {
        rx_ring[i].buffer_addr = (uint64_t)rx_bufs[i];
        rx_ring[i].status      = 0;
    }
    *reg(E1000_REG_RDBAL) = (uint32_t)(uint64_t)rx_ring;
    *reg(E1000_REG_RDBAH) = (uint32_t)((uint64_t)rx_ring >> 32);
    *reg(E1000_REG_RDLEN) = E1000_RING_SIZE * sizeof(struct E1000RxDesc);
    *reg(E1000_REG_RDH)   = 0;
    *reg(E1000_REG_RDT)   = E1000_RING_SIZE - 1;
    *reg(E1000_REG_RCTL)  = E1000_RCTL_EN | E1000_RCTL_BAM;
}

void e1000_transmit_packet(void* physical_buffer, uint16_t size) {
    // Disable interrupts for the duration of the TX descriptor write to prevent
    // the timer ISR from entering e1000_poll_rx mid-transmission.
    __asm__ volatile("cli");

    struct E1000TxDesc* desc = &tx_ring[tx_tail];
    desc->buffer_addr = (uint64_t)physical_buffer;
    desc->length      = size;
    desc->cmd         = (1 << 0) | (1 << 1); // EOP + RS
    desc->status      = 0;

    tx_tail = (uint16_t)((tx_tail + 1) % E1000_RING_SIZE);
    *reg(E1000_REG_TDT) = tx_tail;

    while (!(desc->status & 0x01))
        __asm__ volatile("pause");

    __asm__ volatile("sti");
}

void e1000_poll_rx(void) {
    for (;;) {
        uint16_t head = (uint16_t)(*reg(E1000_REG_RDH) % E1000_RING_SIZE);
        uint16_t next = (uint16_t)((rx_tail + 1) % E1000_RING_SIZE);
        if (next == head) break;  // ring empty

        struct E1000RxDesc* desc = &rx_ring[next];
        if (!(desc->status & 0x01)) break;  // descriptor not done

        // Pass frame to the network stack
        net_rx_dispatch(rx_bufs[next], desc->length);

        // Return descriptor to hardware
        desc->status = 0;
        *reg(E1000_REG_RDT) = next;
        rx_tail = next;
    }
}
