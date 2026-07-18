#include <stdint.h>
#include <stddef.h>

// Source x86_64 Flag Interpretations
#define X86_PTE_PRESENT  (1ULL << 0)
#define X86_PTE_WRITABLE (1ULL << 1)
#define X86_PTE_DIRTY    (1ULL << 6)
#define X86_PTE_SLS_DISK (1ULL << 9)
#define X86_FRAME_MASK   0x000FFFFFFFFFF000ULL

// Target RISC-V Sv39 Flag Interpretations
#define RV_PTE_V        (1ULL << 0)
#define RV_PTE_R        (1ULL << 1)
#define RV_PTE_W        (1ULL << 2)
#define RV_PTE_U        (1ULL << 4)
#define RV_PTE_A        (1ULL << 6)
#define RV_PTE_D        (1ULL << 7)
#define RV_PTE_SLS_DISK (1ULL << 8)

extern void* allocate_physical_ram_frame(void);

// Processes an absolute x86_64 page directory layout block and maps it to a fresh RISC-V Sv39 array
void migrate_x86_table_to_riscv(uint64_t* x86_page_table_src, uint64_t* riscv_page_table_dest) {
    for (int i = 0; i < 512; i++) {
        uint64_t x86_entry = x86_page_table_src[i];
        
        // Skip completely unmapped or unallocated translation pathways
        if (x86_entry == 0) {
            riscv_page_table_dest[i] = 0;
            continue;
        }

        uint64_t migrated_riscv_entry = 0;

        // 1. Extract raw physical pointer addresses safely out of the x86 bit mask lanes
        uint64_t raw_physical_frame = x86_entry & X86_FRAME_MASK;

        // 2. Transcode memory resident page states
        if (x86_entry & X86_PTE_PRESENT) {
            // Memory is resident in RAM. Establish baseline RISC-V valid flags
            migrated_riscv_entry |= RV_PTE_V | RV_PTE_R | RV_PTE_U | RV_PTE_A;

            if (x86_entry & X86_PTE_WRITABLE) migrated_riscv_entry |= RV_PTE_W;
            if (x86_entry & X86_PTE_DIRTY)    migrated_riscv_entry |= RV_PTE_D;
            
        } else if (x86_entry & X86_PTE_SLS_DISK) {
            // Page is currently cold and persistent-media resident (lives on NVMe storage sectors)
            migrated_riscv_entry |= RV_PTE_SLS_DISK;
            // Retain original LBA storage sector addresses unmodified
        }

        // 3. MATHEMATICAL SHIFT CONVERSION:
        // Pack physical address bits into the target RISC-V PPN range (Bits 10-53)
        // Format: PPN = Physical Address >> 12. Packed PTE = PPN << 10.
        uint64_t riscv_ppn = (raw_physical_frame >> 12) << 10;
        migrated_riscv_entry |= riscv_ppn;

        // Commit newly synthesized structural layout parameter to the target page table array
        riscv_page_table_dest[i] = migrated_riscv_entry;
    }
}