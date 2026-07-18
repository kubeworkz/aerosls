#include "timer.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "net_event.h"

volatile uint64_t kernel_tick_counter = 0;

void timer_irq_handler(void) {
    kernel_tick_counter++;
    net_poll_tick();
    lapic_write(LAPIC_REG_EOI, 0);
}

void kernel_sleep_ticks(uint32_t ticks) {
    uint64_t target = kernel_tick_counter + ticks;
    while (kernel_tick_counter < target)
        __asm__ volatile("pause");
}

#include "../arch/x86/isr_stubs.h"

void init_timer(void) {
    // Register ISR32 in the IDT
    set_idt_gate(32, (uint64_t)isr32_stub, 0x8E);

    // Configure the Local APIC timer for periodic interrupts at ~100 Hz.
    // Divide-by-16, initial count 1,000,000 (fires roughly every 10 ms at
    // a ~1.6 GHz effective LAPIC clock; exact rate is calibration-dependent).
    lapic_write(LAPIC_REG_TDCR, 0x03);          // divide by 16
    lapic_write(LAPIC_REG_TICR, 1000000);        // initial count
    lapic_write(LAPIC_REG_LVT_TMR,               // periodic, vector 32
                0x20 | (1U << 17));
}
