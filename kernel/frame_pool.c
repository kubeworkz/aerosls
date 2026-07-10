#include "../include/sls_mmu.h"
    #define TOTAL_FRAMES 1048576
    static uint64_t physical_memory_bitmap[TOTAL_FRAMES / 64];
    void* allocate_physical_ram_frame(void) {
        for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++) {
            if (physical_memory_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                for (int bit = 0; bit < 64; bit++) {
                    if (!(physical_memory_bitmap[i] & (1ULL << bit))) {
                        physical_memory_bitmap[i] |= (1ULL << bit);
                        return (void*)(((i * 64) + bit) * 4096);
                    }
                }
            }
        }
        return 0;
    }