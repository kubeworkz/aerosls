#include "idt.h"

// ─── PIC remapping + masking ─────────────────────────────────────────────────
// The legacy 8259A PIC maps IRQ0-7 to INT 0x08-0x0F by default.
// In long mode 0x08 = #DF (Double Fault) → triple fault when timer fires.
// Remap both PICs to 0x20-0x2F and mask all lines; LAPIC handles timing.
static inline void _outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static void pic_remap_and_mask(void) {
    _outb(0x20, 0x11); _outb(0xA0, 0x11);  // ICW1: init
    _outb(0x21, 0x20); _outb(0xA1, 0x28);  // ICW2: vector offsets 0x20/0x28
    _outb(0x21, 0x04); _outb(0xA1, 0x02);  // ICW3: cascade
    _outb(0x21, 0x01); _outb(0xA1, 0x01);  // ICW4: 8086 mode
    _outb(0x21, 0xFF); _outb(0xA1, 0xFF);  // OCW1: mask ALL IRQs
}

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
    set_idt_gate( 6, (uint64_t)isr6_stub,  0x8E);  // #UD Invalid Opcode
    set_idt_gate(11, (uint64_t)isr11_stub, 0x8E);  // #NP Segment Not Present
    set_idt_gate(12, (uint64_t)isr12_stub, 0x8E);  // #SS Stack-Segment Fault
    set_idt_gate(13, (uint64_t)isr13_stub, 0x8E);  // #GP General Protection
    set_idt_gate(14, (uint64_t)isr14_stub, 0x8E);  // #PF Page Fault

    // Remap and silence the legacy 8259A PIC BEFORE enabling interrupts.
    // Without this, IRQ0 fires as INT 0x08 (#DF) → triple fault.
    pic_remap_and_mask();

    // Load table pointer directly into the processor
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    __asm__ volatile("sti"); // Re-enable interrupts globally
}