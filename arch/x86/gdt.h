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

#endif