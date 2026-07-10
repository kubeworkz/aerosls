#include "paging_riscv.h"
    extern void* allocate_physical_ram_frame(void);
    uint64_t* walk_page_tables_sv39(uint64_t root_pt_phys, uint64_t virtual_address) {
    uint64_t* current_table = (uint64_t*)root_pt_phys;
    size_t idx2 = VPN2_INDEX(virtual_address);
    if (!(current_table[idx2] & PTE_V)) {
    uint64_t* new_table = (uint64_t*)allocate_physical_ram_frame();
    if (!new_table) return 0;
    for (int i = 0; i < 512; i++) new_table[i] = 0;
    current_table[idx2] = PA_TO_PTE((uint64_t)new_table) | PTE_V;
    }
    current_table = (uint64_t*)PTE_TO_PA(current_table[idx2]);
    size_t idx1 = VPN1_INDEX(virtual_address);
    if (!(current_table[idx1] & PTE_V)) {
        uint64_t* new_table = (uint64_t*)allocate_physical_ram_frame();
        if (!new_table) return 0;
        for (int i = 0; i < 512; i++) new_table[i] = 0;
        current_table[idx1] = PA_TO_PTE((uint64_t)new_table) | PTE_V;
    }
    current_table = (uint64_t*)PTE_TO_PA(current_table[idx1]);
    return &current_table[VPN0_INDEX(virtual_address)];
    }