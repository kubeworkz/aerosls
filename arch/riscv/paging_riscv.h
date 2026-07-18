#ifndef PAGING_RISCV_H
#define PAGING_RISCV_H

#include <stdint.h>

#define PAGE_SIZE 4096

// Sv39 Virtual Address Index Extraction
#define VPN2_INDEX(va) (((va) >> 30) & 0x1FF)
#define VPN1_INDEX(va) (((va) >> 21) & 0x1FF)
#define VPN0_INDEX(va) (((va) >> 12) & 0x1FF)

// Sv39/Sv48 Page Table Entry Bitmasks
#define PTE_V     (1ULL << 0)  // Valid bit (Replaces x86 Present bit)
#define PTE_R     (1ULL << 1)  // Readable
#define PTE_W     (1ULL << 2)  // Writable
#define PTE_X     (1ULL << 3)  // Executable
#define PTE_U     (1ULL << 4)  // User Mode accessible
#define PTE_A     (1ULL << 6)  // Accessed (Set by hardware)
#define PTE_D     (1ULL << 7)  // Dirty (Set by hardware)

// RISC-V Software Reserved Bits (Bits 8-9) for SLS tracking
#define PTE_SLS_DISK (1ULL << 8)  // Page resides on persistent NVMe/SATA media

// Extracts the Physical Page Number (PPN) from a PTE (Bits 10-53)
#define PTE_TO_PA(pte) (((pte) >> 10) << 12)
#define PA_TO_PTE(pa)  (((pa) >> 12) << 10)

struct RISCVPTE {
    uint64_t entry;
};

#endif