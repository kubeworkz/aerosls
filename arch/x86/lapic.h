#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

#define LAPIC_BASE_VIRT    0xFEE00000ULL

#define LAPIC_REG_ID       0x0020
#define LAPIC_REG_EOI      0x00B0
#define LAPIC_REG_SPURIOUS 0x00F0
#define LAPIC_REG_LVT_TMR  0x0320
#define LAPIC_REG_TICR     0x0380   // Timer Initial Count Register
#define LAPIC_REG_TCCR     0x0390   // Timer Current Count Register
#define LAPIC_REG_TDCR     0x03E0   // Timer Divide Configuration Register
#define LAPIC_REG_ICR_LOW  0x0300
#define LAPIC_REG_ICR_HIGH 0x0310

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(LAPIC_BASE_VIRT + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(LAPIC_BASE_VIRT + reg) = value;
}

// Configure the local APIC for the calling core. Must be called by each AP
// after it leaves the trampoline, and by the BSP during early boot.
static inline void init_local_apic_registers(void) {
    // Enable APIC and map spurious vector to IDT entry 255
    uint32_t spurious_reg = lapic_read(LAPIC_REG_SPURIOUS);
    spurious_reg |= (1 << 8); // Software enable bit
    spurious_reg |= 0xFF;     // Spurious vector 255
    lapic_write(LAPIC_REG_SPURIOUS, spurious_reg);

    // Mask the LVT timer until the scheduler configures it
    lapic_write(LAPIC_REG_LVT_TMR, 0x10000);

    __asm__ volatile("sti");
}

#endif // LAPIC_H
