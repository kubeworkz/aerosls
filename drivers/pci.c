#include "../include/sls_mmu.h"

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = (uint32_t)((uint32_t)1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    asm volatile("outl %0, %1" : : "a"(address), "Nd"((uint16_t)0xCF8));
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"((uint16_t)0xCFC));
    return ret;
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = (uint32_t)((uint32_t)1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    asm volatile("outl %0, %1" : : "a"(address), "Nd"((uint16_t)0xCF8));
    asm volatile("outl %0, %1" : : "a"(value),   "Nd"((uint16_t)0xCFC));
}
