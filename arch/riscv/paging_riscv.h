#ifndef PAGING_RISCV_H
    #define PAGING_RISCV_H
    #include <stdint.h>
    #define VPN2_INDEX(va) (((va) >> 30) & 0x1FF)
    #define VPN1_INDEX(va) (((va) >> 21) & 0x1FF)
    #define VPN0_INDEX(va) (((va) >> 12) & 0x1FF)
    #define PTE_V     (1ULL << 0)
    #define PTE_R     (1ULL << 1)
    #define PTE_W     (1ULL << 2)
    #define PTE_X     (1ULL << 3)
    #define PTE_U     (1ULL << 4)
    #define PTE_A     (1ULL << 6)
    #define PTE_D     (1ULL << 7)
    #define PTE_SLS_DISK (1ULL << 8)
    #define PTE_TO_PA(pte) (((pte) >> 10) << 12)
    #define PA_TO_PTE(pa)  (((pa) >> 12) << 10)
    #endif