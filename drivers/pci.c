#include "../include/sls_mmu.h"
    uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((uint32_t)1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) 
    | ((uint32_t)func << 8) | (offset & 0xFC);
    asm volatile("outl %0, $0xCF8" : : "a"(address));
    uint32_t ret;
    asm volatile("inl $0xCFC, %0" : "=a"(ret));
    return ret;
    }