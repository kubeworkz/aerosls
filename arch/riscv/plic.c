#include <stdint.h>

// RISC-V Virt Board PLIC MMIO Register Bounds
#define PLIC_BASE_VIRT        0xFFFFFFFF40003000ULL // Mapped virtual memory window
#define PLIC_PRIORITY_BASE    0x0000
#define PLIC_ENABLE_BASE      0x2000
#define PLIC_THRESHOLD_BASE   0x200000
#define PLIC_CLAIM_BASE       0x200004

// UART Peripheral IRQ number on the QEMU Virt machine
#define UART0_IRQ 10

static inline uint32_t plic_read(uint32_t offset) {
    return *(volatile uint32_t*)(PLIC_BASE_VIRT + offset);
}

static inline void plic_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(PLIC_BASE_VIRT + offset) = (value);
}

void init_riscv_plic(uint32_t target_hart_id) {
    // 1. Set the priority of the UART interrupt vector line.
    // Priorities range from 0 (disabled) to 7 (highest). We assign a strong 5.
    plic_write(PLIC_PRIORITY_BASE + (UART0_IRQ * 4), 5);

    // 2. Enable UART IRQ 10 specifically for the target Hart's Supervisor Mode.
    // Under the RISC-V Virt layout, Hart N's Supervisor-Mode Enable register 
    // is offset by: PLIC_ENABLE_BASE + (target_hart_id * 2 + 1) * 0x80
    uint32_t s_enable_offset = PLIC_ENABLE_BASE + ((target_hart_id * 2 + 1) * 0x80);
    uint32_t current_mask = plic_read(s_enable_offset);
    current_mask |= (1 << UART0_IRQ); // Set bit 10 to unmask the line
    plic_write(s_enable_offset, current_mask);

    // 3. Set the Supervisor-Mode Priority Threshold for this Hart.
    // The PLIC will filter out any interrupts with a priority less than or equal to this threshold.
    // Setting threshold to 0 allows all interrupts with priority > 0 to pass through.
    uint32_t s_threshold_offset = PLIC_THRESHOLD_BASE + ((target_hart_id * 2 + 1) * 0x1000);
    plic_write(s_threshold_offset, 0);

    // 4. Configure the local Supervisor Interrupt Enable (sie) register on the CPU core
    // Bit 9 corresponds to Supervisor External Interrupt Enable (SEIE)
    uint64_t sie_val;
    __asm__ volatile("csrr %0, sie" : "=r"(sie_val));
    sie_val |= (1ULL << 9); // Enable SEIE
    __asm__ volatile("csrw sie, %0" : : "r"(sie_val));
}

// Polling/Acknowledgment router handler run inside 'handle_riscv_supervisor_interrupt'
uint32_t plic_claim_interrupt(uint32_t target_hart_id) {
    uint32_t claim_offset = PLIC_CLAIM_BASE + ((target_hart_id * 2 + 1) * 0x1000);
    return plic_read(claim_offset); // Returns the active IRQ number (e.g., 10)
}

void plic_complete_interrupt(uint32_t target_hart_id, uint32_t irq) {
    uint32_t claim_offset = PLIC_CLAIM_BASE + ((target_hart_id * 2 + 1) * 0x1000);
    plic_write(claim_offset, irq); // Signal the PLIC hardware that we processed the IRQ
}