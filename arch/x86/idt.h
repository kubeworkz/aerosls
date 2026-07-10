#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Explicitly packed structure matching x86_64 IDT Gate descriptor layout
struct IDTEntry {
    uint16_t isr_low;       // Lower 16 bits of ISR address
    uint16_t kernel_cs;     // Kernel Code Segment selector (from your GDT, usually 0x08)
    uint8_t  ist;           // Interrupt Stack Table offset (0 for default)
    uint8_t  attributes;    // Type and attributes (e.g., Present, Ring 0, Interrupt Gate)
    uint16_t isr_mid;       // Middle 16 bits of ISR address
    uint32_t isr_high;      // Higher 32 bits of ISR address
    uint32_t reserved;      // Reserved 32 bits (always set to 0)
} __attribute__((packed));

// Structure passed directly to the LIDT assembly instruction
struct IDTPointer {
    uint16_t limit;         // Size of IDT array minus 1
    uint64_t base;          // Linear base address of the IDT array
} __attribute__((packed));

void init_idt(void);

#endif