#include "../../include/sls_mmu.h"
    #define PLIC_BASE_VIRT 0xFFFFFFFF40003000ULL
    void init_riscv_plic(uint32_t target_hart_id) {
        (volatile uint32_t)(PLIC_BASE_VIRT + (10 * 4)) = 5;
        uint32_t s_enable_offset = 0x2000 + ((target_hart_id * 2 + 1) * 0x80);
        (volatile uint32_t)(PLIC_BASE_VIRT + s_enable_offset) |= (1 << 10);
        uint32_t s_threshold_offset = 0x200000 + ((target_hart_id * 2 + 1) * 0x1000);
        (volatile uint32_t)(PLIC_BASE_VIRT + s_threshold_offset) = 0;
    }