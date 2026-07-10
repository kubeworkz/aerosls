#include <stdint.h>
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"

// Global monotonic tick counter incremented by the timer IRQ handler
volatile uint64_t kernel_tick_counter = 0;

// Called from the IRQ0 assembly stub on each timer interrupt
void timer_irq_handler(void) {
    kernel_tick_counter++;
    lapic_write(LAPIC_REG_EOI, 0);
}

// Busy-wait for the requested number of timer ticks
void kernel_sleep_ticks(uint32_t ticks) {
    uint64_t target = kernel_tick_counter + ticks;
    while (kernel_tick_counter < target) {
        __asm__ volatile("pause");
    }
}

// Register the timer IRQ0 stub in the IDT (call once during kernel init)
extern void isr32_stub(void); // Assembly stub defined in interrupt.asm

void init_timer(void) {
    // IRQ0 maps to IDT vector 32 (after the 32 reserved CPU exception vectors)
    // 0x8E: Present, Ring 0, 64-bit Interrupt Gate
    set_idt_gate(32, (uint64_t)isr32_stub, 0x8E);
}
