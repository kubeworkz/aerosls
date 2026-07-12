#include "gdt.h"

// Define a 7-entry GDT (Null, KCode, KData, UData, UCode, and 2 slots for the 16-byte TSS)
static struct GDTEntry gdt[7];
struct GDTPointer gdt_ptr;   // exported for smp.c trampoline handshake
static struct TaskStateSegment tss;

// Static kernel stack dedicated strictly for handling exceptions/syscalls from user space
static uint8_t interruption_stack[4096];

void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

void set_gdt_tss(int num, uint64_t base, uint32_t limit, uint8_t access) {
    struct GDTSystemEntry* tss_entry = (struct GDTSystemEntry*)&gdt[num];
    
    set_gdt_gate(num, (uint32_t)base, limit, access, 0x00);
    tss_entry->base_upper = (uint32_t)(base >> 32);
    tss_entry->reserved   = 0;
}

void init_gdt(void) {
    gdt_ptr.limit = (sizeof(struct GDTEntry) * 7) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    // Entry 0: Null Descriptor
    set_gdt_gate(0, 0, 0, 0, 0);

    // Entry 1: Kernel Code (Ring 0). Access: Present, Ring 0, Executable, Read/Write (0x9A)
    set_gdt_gate(1, 0, 0xFFFFF, 0x9A, 0xA0); // 0xA0 sets Long-Mode 64-bit flag

    // Entry 2: Kernel Data (Ring 0). Access: Present, Ring 0, Writable (0x92)
    set_gdt_gate(2, 0, 0xFFFFF, 0x92, 0xA0);

    // Entry 3: User Data (Ring 3). Access: Present, Ring 3, Writable (0xF2)
    set_gdt_gate(3, 0, 0xFFFFF, 0xF2, 0xA0);

    // Entry 4: User Code (Ring 3). Access: Present, Ring 3, Executable (0xFA)
    set_gdt_gate(4, 0, 0xFFFFF, 0xFA, 0xA0);

    // Set up the Task State Segment
    for (int i = 0; i < sizeof(struct TaskStateSegment); i++) ((uint8_t*)&tss)[i] = 0;
    tss.rsp0 = (uint64_t)&interruption_stack[4095]; // Top of stack
    tss.iomap_base = sizeof(struct TaskStateSegment);

    // Entry 5 & 6: TSS Descriptor (Takes 16 bytes, occupying two standard slots). Access: 0x89
    set_gdt_tss(5, (uint64_t)&tss, sizeof(struct TaskStateSegment) - 1, 0x89);

    // Flush and reload registers via inline assembly
    __asm__ volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t" // 0x10 points to Kernel Data Segment (Offset 16)
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"     // 0x08 points to Kernel Code Segment (Offset 8)
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"           // Perform 64-bit far return to reload CS register
        "1:\n\t"
        "mov $0x2B, %%ax\n\t" // 0x2B points to TSS (Offset 40 + Task Attribute Ring 3 bits)
        "ltr %%ax\n\t"        // Load Task Register
        : : "m"(gdt_ptr) : "rax", "memory"
    );
}