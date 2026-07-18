#include "../include/sls_mmu.h"
#include <stddef.h>

#define TOTAL_FRAMES 1048576

static uint64_t physical_memory_bitmap[TOTAL_FRAMES / 64];

void *allocate_physical_ram_frame(void)
{
    // Start at frame 1 (skip frame 0: address 0x0 == NULL in C)
    for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++)
    {
        if (physical_memory_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL)
        {
            for (int bit = 0; bit < 64; bit++)
            {
                // Skip the very first frame (frame 0 = address 0x0 = NULL)
                if (i == 0 && bit == 0) continue;
                if (!(physical_memory_bitmap[i] & (1ULL << bit)))
                {
                    physical_memory_bitmap[i] |= (1ULL << bit);
                    return (void *)(((i * 64) + bit) * 4096);
                }
            }
        }
    }
    return 0;
}