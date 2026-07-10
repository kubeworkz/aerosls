#include <stddef.h>
#include "../include/sls_mmu.h"

#define PAGE_SIZE 4096

// PTE flag aliases using the unified MMU header
#define PTE_PRESENT    SLS_PTE_VALID
#define PTE_DIRTY      SLS_PTE_DIRTY
#define PTE_SLS_DISK   SLS_PTE_SLS_DISK
#define PTE_FRAME_MASK SLS_FRAME_MASK

struct SLSObject {
    uint64_t start_virtual_address;
    uint64_t size_in_bytes;
};

extern uint64_t* walk_page_tables(uint64_t virtual_address);
extern void storage_write_block(uint64_t disk_block_id, void* ram_frame);
extern uint64_t get_object_disk_block_mapping(uint64_t virtual_address);
extern void kernel_sleep_ticks(uint32_t ticks);

extern struct SLSObject global_sls_object_table[];
extern size_t total_active_sls_objects;

// Scan and flush a specific mapped object range back to persistent storage
void flush_dirty_sls_region(uint64_t start_vaddr, size_t size_in_bytes) {
    size_t num_pages = (size_in_bytes + (PAGE_SIZE - 1)) / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t current_vaddr = start_vaddr + (i * PAGE_SIZE);

        uint64_t* pte = walk_page_tables(current_vaddr);
        if (!pte) continue;

        if ((*pte & PTE_PRESENT) && (*pte & PTE_SLS_DISK) && (*pte & PTE_DIRTY)) {
            void* ram_frame = (void*)(*pte & PTE_FRAME_MASK);
            uint64_t disk_block_id = get_object_disk_block_mapping(current_vaddr);

            storage_write_block(disk_block_id, ram_frame);

            *pte &= ~PTE_DIRTY;

            __asm__ volatile("invlpg (%0)" :: "r"(current_vaddr) : "memory");
        }
    }
}

// Background daemon loop executed by each AP after boot
void sls_flush_daemon_loop(void) {
    while (1) {
        for (size_t i = 0; i < total_active_sls_objects; i++) {
            struct SLSObject obj = global_sls_object_table[i];
            flush_dirty_sls_region(obj.start_virtual_address, obj.size_in_bytes);
        }

        kernel_sleep_ticks(100);
    }
}
