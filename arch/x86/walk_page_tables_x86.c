#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_FRAME_MASK 0x000FFFFFFFFFF000ULL

extern void* allocate_physical_ram_frame(void);

static uint64_t* get_or_create_next_table(uint64_t* current_table, size_t index) {
    uint64_t entry = current_table[index];

    if (!(entry & PTE_PRESENT)) {
        uint64_t* new_table_phys = (uint64_t*)allocate_physical_ram_frame();
        if (!new_table_phys) return NULL;

        for (int i = 0; i < 512; i++) new_table_phys[i] = 0;

        current_table[index] = ((uint64_t)new_table_phys & PTE_FRAME_MASK) | PTE_WRITABLE | PTE_PRESENT;
        return new_table_phys;
    }

    return (uint64_t*)(entry & PTE_FRAME_MASK);
}

uint64_t* walk_page_tables(uint64_t virtual_address) {
    uint64_t* pml4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(pml4));

    uint64_t* pdpt = get_or_create_next_table(pml4, PML4_INDEX(virtual_address));
    if (!pdpt) return NULL;

    uint64_t* pd = get_or_create_next_table(pdpt, PDPT_INDEX(virtual_address));
    if (!pd) return NULL;

    uint64_t* pt = get_or_create_next_table(pd, PD_INDEX(virtual_address));
    if (!pt) return NULL;

    return &pt[PT_INDEX(virtual_address)];
}
