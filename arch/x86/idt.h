#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Explicitly packed structure matching x86_64 IDT Gate descriptor layout
struct IDTEntry {
    uint16_t isr_low;       // Lower 16 bits of ISR address
    uint16_t kernel_cs;     // Kernel Code Segment selector (from our GDT, usually 0x08)
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
void set_idt_gate(uint8_t vector, uint64_t isr_address, uint8_t attributes);

/* Load the already-built table into THIS core's IDTR.
 *
 * Every core needs this, not just the BSP. A core that leaves the trampoline
 * still carries the RESET IDTR (base 0, limit 0xFFFF) -- the trampoline `cli`s
 * and init_idt()'s `lidt` ran on the BSP only -- so the first exception, or the
 * first maskable interrupt routed to that core, reads its "gate" out of
 * physical address 0. Those bytes are boot/real-mode code, not descriptors, so
 * the resulting #NP/#DF chain ends in a triple fault: a silent machine reset, a
 * QEMU exit under -no-reboot, and NOTHING in the log. Measured on the AP while
 * it ran kernel/smp.c's service loop with interrupts enabled:
 * `IDT= 0000000000000000 0000ffff` against the BSP's `IDT= ... 00000fff`.
 * See kernel/smp.c ap_kernel_main() for the call site and why it is first. */
void idt_load_this_cpu(void);

#endif