#include <stdint.h>
#include "e1000.h"
#include "net.h"
#include "../kernel/kernel_io.h"

// Mark a physical address range as uncacheable via MTRR variable range register.
// MTRR type 0 = UC (Uncacheable). Size must be a power of 2 and >= 4KB.
static void mtrr_set_uc(uint64_t base, uint64_t size) {
    // Disable caches + flush with WBINVD while we change MTRRs
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    // Set CD (bit 30) and clear NW (bit 29) to disable caching
    uint64_t cr0_new = (cr0 | (1ULL<<30)) & ~(1ULL<<29);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_new) : "memory");
    __asm__ volatile("wbinvd");

    // Disable all MTRRs temporarily (clear MTRRdefType.E bit 11)
    uint32_t mdef_lo, mdef_hi;
    __asm__ volatile("rdmsr" : "=a"(mdef_lo), "=d"(mdef_hi) : "c"(0x2FF));
    __asm__ volatile("wrmsr" :: "c"(0x2FF), "a"(mdef_lo & ~(1U<<11)), "d"(mdef_hi));

    // Program MTRRphysBase0 (0x200) and MTRRphysMask0 (0x201)
    uint64_t phys_base = (base & ~0xFFFULL) | 0;  // type=0 (UC)
    uint64_t phys_mask = (~(size - 1) & 0x000FFFFFFFFFF000ULL) | (1ULL << 11);
    __asm__ volatile("wrmsr" :: "c"(0x200),
                     "a"((uint32_t)phys_base), "d"((uint32_t)(phys_base>>32)));
    __asm__ volatile("wrmsr" :: "c"(0x201),
                     "a"((uint32_t)phys_mask), "d"((uint32_t)(phys_mask>>32)));

    // Re-enable MTRRs
    __asm__ volatile("wrmsr" :: "c"(0x2FF), "a"(mdef_lo | (1U<<11)), "d"(mdef_hi));
    __asm__ volatile("wbinvd");
    // Restore caching
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}


uint64_t e1000_mmio_base = 0;

static struct E1000TxDesc tx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static struct E1000RxDesc rx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static uint8_t rx_bufs[E1000_RING_SIZE][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
static uint16_t tx_tail = 0;
// rx_tail starts at RING_SIZE-1 so the first poll checks descriptor 0.
// The e1000 fills from RDH=0; we give it back descriptors as we consume them.
static uint16_t rx_tail = E1000_RING_SIZE - 1;

static inline volatile uint32_t* reg(uint32_t off) {
    return (volatile uint32_t*)(e1000_mmio_base + off);
}

// Force a 32-bit MMIO write using inline assembly to prevent the compiler
// from reordering or caching the write.
static inline void mmio_write32(uint64_t addr, uint32_t val) {
    __asm__ volatile("movl %0, (%1)" :: "r"(val), "r"((volatile uint32_t*)addr) : "memory");
}

static inline uint32_t mmio_read32(uint64_t addr) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"((volatile uint32_t*)addr) : "memory");
    return v;
}

void e1000_init(uint64_t mmio_base) {
    e1000_mmio_base = mmio_base;

    // Enable PCI Memory Space and Bus Master (slot 4 = e1000 in QEMU with NVMe)
    {
        extern uint32_t pci_read_config(uint8_t, uint8_t, uint8_t, uint8_t);
        extern void pci_write_config(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
        uint32_t cmd = pci_read_config(0, 4, 0, 0x04);
        cmd |= (1U << 1) | (1U << 2);
        pci_write_config(0, 4, 0, 0x04, cmd);
    }

    // Mark the MMIO BAR0 region as uncacheable via MTRR.
    mtrr_set_uc(mmio_base & ~0xFFFULL, 0x20000ULL);

    // ── Link Up Setup ─────────────────────────────────────────────────────────
    #define E1000_CTRL_RST  (1U << 26)
    #define E1000_CTRL_SLU  (1U << 6)
    #define E1000_CTRL_ASDE (1U << 5)
    uint32_t ctrl = mmio_read32(mmio_base + E1000_REG_CTRL);
    ctrl = (ctrl & ~E1000_CTRL_RST) | E1000_CTRL_SLU | E1000_CTRL_ASDE;
    mmio_write32(mmio_base + E1000_REG_CTRL, ctrl);
    for (volatile int i = 0; i < 50000; i++) __asm__ volatile("pause");
    __asm__ volatile("wbinvd");

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
    // CT (bits 11:4) = 0x0F, COLD (bits 21:12) = 0x040 (full-duplex)
    *reg(E1000_REG_TCTL)  = E1000_TCTL_EN | E1000_TCTL_PSP
                          | (0x0FU << 4)   /* CT  */
                          | (0x040U << 12); /* COLD */

    // ── RX ring ──────────────────────────────────────────────────────────────
    kernel_serial_print("[E1000] init: setting up RX ring\n");

    // Program our MAC into the receive address filter (slot 0)
    // MAC = 52:54:00:12:34:01 → RAL = 0x12005452, RAH = 0x80000134
    // (bytes in memory order: [0]=52 [1]=54 [2]=00 [3]=12 → LE dword=0x12005452)
    // ([4]=34 [5]=01 → upper 16b of RAH=0x0134, plus AV bit 31)
    *reg(E1000_REG_RAL0) = 0x12005452U;
    *reg(E1000_REG_RAH0) = 0x80000134U; // AV bit (31) set

    // Also enable unicast+multicast promiscuous so we never miss a packet
    // RCTL_UPE=bit3, RCTL_MPE=bit4 — harmless in addition to BAM
    for (int i = 0; i < E1000_RING_SIZE; i++) {
        rx_ring[i].buffer_addr = (uint64_t)rx_bufs[i];
        rx_ring[i].status      = 0;
    }
    *reg(E1000_REG_RDBAL) = (uint32_t)(uint64_t)rx_ring;
    *reg(E1000_REG_RDBAH) = (uint32_t)((uint64_t)rx_ring >> 32);
    *reg(E1000_REG_RDLEN) = E1000_RING_SIZE * sizeof(struct E1000RxDesc);
    *reg(E1000_REG_RDH)   = 0;
    *reg(E1000_REG_RDT)   = E1000_RING_SIZE - 1;
    *reg(E1000_REG_RCTL)  = E1000_RCTL_EN | E1000_RCTL_BAM | (1U<<3) | (1U<<4);
}

void e1000_transmit_packet(void* physical_buffer, uint16_t size) {
    // Disable interrupts for the duration of the TX descriptor write to prevent
    // the timer ISR from entering e1000_poll_rx mid-transmission.
    __asm__ volatile("cli");

    struct E1000TxDesc* desc = &tx_ring[tx_tail];
    desc->buffer_addr = (uint64_t)physical_buffer;
    desc->length      = size;
    desc->cmd         = (1 << 0) | (1 << 1) | (1 << 3); // EOP + IFCS + RS
    desc->status      = 0;

    tx_tail = (uint16_t)((tx_tail + 1) % E1000_RING_SIZE);
    *reg(E1000_REG_TDT) = tx_tail;

    while (!(desc->status & 0x01)) {
        // Timeout so early-boot ARP doesn't hang if link is still negotiating.
        uint32_t tx_timeout = 0;
        if (++tx_timeout > 2000000) break;
        __asm__ volatile("pause");
    }

    __asm__ volatile("sti");
}

void e1000_poll_rx(void) {
    for (;;) {
        uint16_t next = (uint16_t)((rx_tail + 1) % E1000_RING_SIZE);
        struct E1000RxDesc* desc = &rx_ring[next];

        // Check DD (descriptor done) bit — hardware sets this when the
        // descriptor has been filled with a received packet.
        if (!(desc->status & 0x01)) break;  // no more completed descriptors

        // Pass frame to the network stack
        net_rx_dispatch(rx_bufs[next], desc->length);

        // Return descriptor to hardware and advance our tail pointer
        desc->status = 0;
        *reg(E1000_REG_RDT) = next;
        rx_tail = next;
    }
}
