/* arch/x86/device_irq.c — Driver SDK ABI v0.1 sections 4.3/4.4, arch side.
 *
 * The kernel's IDT only ever had the timer (vector 32) and the fault
 * vectors; any other device edge (a PIC/IO-APIC line routed to the LAPIC)
 * found no gate and triple-faulted. This file installs a generic stub
 * (isr_irq_33..255 from arch/x86/interrupt.asm) for every device vector;
 * each stub calls cap_irq_notify(vector), the cap.c ISR path that resolves
 * the IRQ registry, enqueues the one-byte notification on the bound
 * driver's channel, wakes a parked receiver, and EOIs.
 *
 * cap_irq_eoi: strong override of cap.c's weak no-op hook. The stub table
 * is built by NASM at link time, so the stub list and this file cannot
 * drift. */
#include "../../kernel/cap.h"
#include "idt.h"
#include "lapic.h"
#include <stdint.h>

/* Legacy 8259A PIC lines were remapped in idt.c to vectors 0x20-0x2F. */
static inline void irq_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

void cap_irq_eoi(uint32_t vector) {
    if (vector >= 0x20 && vector <= 0x2F) {
        irq_outb(0x20, 0x20);                       /* PIC1 EOI */
        if (vector >= 0x28)
            irq_outb(0xA0, 0x20);                   /* PIC2 cascade EOI */
    }
    lapic_write(LAPIC_REG_EOI, 0);                  /* LAPIC EOI */
}

extern void (*irq_stub_table[224])(void);           /* arch/x86/isr_stubs.h */

void init_device_irqs(void) {
    for (int i = 0; i < 224; i++)
        set_idt_gate(33 + i, (uint64_t)irq_stub_table[i], 0x8E);
}
