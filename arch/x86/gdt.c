#include "../../include/sls_mmu.h"
    struct GDTEntry { uint16_t limit_low; uint16_t base_low; uint8_t base_mid; uint8_t access; 
    uint8_t granularity; uint8_t base_high; } attribute((packed));
    struct GDTPointer { uint16_t limit; uint64_t base; } attribute((packed));
    static struct GDTEntry gdt;
    static struct GDTPointer gdt_ptr;
    void init_gdt(void) {
    gdt_ptr.limit = (sizeof(struct GDTEntry) * 7) - 1; gdt_ptr.base = (uint64_t)&gdt;
    asm volatile("lgdt %0" : : "m"(gdt_ptr));
    }