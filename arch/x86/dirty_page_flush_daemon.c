// An SLS operating system does not have an explicit sys_sync() or write() command. Instead, a kernel thread (daemon) continuously or 
// periodically scans the virtual address space mappings.

// When the CPU modifies a page, the processor automatically sets Bit 6 (The Dirty Bit) in the page table entry to 1. The flush daemon checks 
// for this bit, copies modified RAM blocks back to disk, and resets the dirty bit back to 0.

#define PTE_DIRTY      (1ULL << 6)
#define PTE_SLS_DISK   (1ULL << 9)

extern void storage_write_block(uint64_t disk_block_id, void* ram_frame);

// Scan and flush a specific mapped object range back to persistence
void flush_dirty_sls_region(uint64_t start_vaddr, size_t size_in_bytes) {
    size_t num_pages = (size_in_bytes + (PAGE_SIZE - 1)) / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t current_vaddr = start_vaddr + (i * PAGE_SIZE);
        
        // Lookup entry state parameters
        uint64_t* pte = walk_page_tables(current_vaddr);
        if (!pte) continue;

        // Verify if it is present in memory, belongs to storage space, and has been written to
        if ((*pte & PTE_PRESENT) && (*pte & PTE_SLS_DISK) && (*pte & PTE_DIRTY)) {
            
            // Extract memory frame target location pointer addresses
            void* ram_frame = (void*)(*pte & PTE_FRAME_MASK);
            
            // Look up corresponding original destination block token markers (Assume stored index)
            // For simplicity in this layout, we parse block indexing definitions via tracking arrays
            uint64_t disk_block_id = get_object_disk_block_mapping(current_vaddr);

            // 1. Commit raw memory modifications down to permanent block storage
            storage_write_block(disk_block_id, ram_frame);

            // 2. Clear dirty flag state bit to allow tracking new write events
            *pte &= ~PTE_DIRTY;

            // 3. Invalidate system Translation Lookaside Buffer cache for this specific mapping entry
            __asm__ volatile("invlpg (%0)" :: "r"(current_vaddr) : "memory");
        }
    }
}

// Background thread loop layout logic run by the system kernel scheduler
void sls_flush_daemon_loop(void) {
    while (1) {
        // Iterate through global table arrays tracking created persistent memory spaces
        for (size_t i = 0; i < total_active_sls_objects; i++) {
            struct SLSObject obj = global_sls_object_table[i];
            flush_dirty_sls_region(obj.start_virtual_address, obj.size_in_bytes);
        }

        // Sleep/Yield CPU cycles via timer interrupt delays to prevent burning raw thread time
        kernel_sleep_ticks(100); 
    }
}
