#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct GDTSystemEntry {
    struct GDTEntry common;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct GDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct TaskStateSegment {
    uint32_t reserved0;
    uint64_t rsp0;  // Privilege level 0 (Kernel) Stack Pointer
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7]; // Interrupt Stack Table pointers
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void init_gdt(void);

/* Point the TSS RSP0 at the stack a CPL-3 entry must land on. A low-linked
 * ET_EXEC lives at 0x400000, which overlaps the static interruption_stack
 * in the kernel's low .bss; every switch into a user task therefore points
 * RSP0 at that task's dedicated (supervisor, identity-mapped) kernel
 * syscall stack instead. */
void tss_set_rsp0(uint64_t top);

/* The high canonical aliases of the kernel's GDT/TSS/IDT. init_gdt() and
 * init_idt() load the CPU's descriptors from these, so a low-linked ET_EXEC
 * cannot shadow the tables it needs on every privilege transition. */
uint64_t kernel_tables_gdt_va(void);
uint64_t kernel_tables_tss_va(void);
uint64_t kernel_tables_idt_va(void);

#endif