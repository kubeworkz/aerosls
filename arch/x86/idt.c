#include "idt.h"

// Define array space for all 256 interrupts
__attribute__((aligned(0x10))) 
static struct IDTEntry idt[256];
static struct IDTPointer idt_ptr;

// External reference to the assembly wrapper for the page fault handler
#include "isr_stubs.h"

void set_idt_gate(uint8_t vector, uint64_t isr_address, uint8_t attributes) {
    idt[vector].isr_low    = (uint16_t)(isr_address & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; // Matches code segment defined in boot.asm GDT
    idt[vector].ist        = 0;
    idt[vector].attributes = attributes;
    idt[vector].isr_mid    = (uint16_t)((isr_address >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)((isr_address >> 32) & 0xFFFFFFFF);
    idt[vector].reserved   = 0;
}

void init_idt(void) {
    idt_ptr.limit = (sizeof(struct IDTEntry) * 256) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    // 0x8E: Present, Ring 0, 64-bit Interrupt Gate
    set_idt_gate(14, (uint64_t)isr14_stub, 0x8E);

    // Load table pointer directly into the processor
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    __asm__ volatile("sti"); // Re-enable interrupts globally
}