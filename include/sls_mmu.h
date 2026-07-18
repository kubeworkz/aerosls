// sls_mmu.h - Unified Hardware Abstraction Layer for Page Table Architectures
#ifndef SLS_MMU_H
#define SLS_MMU_H

#include <stdint.h>

// Unified Abstract SLS Page Table Properties
struct SLSPte {
    uint64_t* raw_entry_ptr;
};

#if defined(__x86_64__)
    // ----------------------------------------------------
    // x86_64 HARDWARE COMPLIANCE ARCHITECTURE MAPPINGS
    // ----------------------------------------------------
    #define SLS_PTE_VALID       (1ULL << 0)  // x86 Present Bit
    #define SLS_PTE_WRITABLE    (1ULL << 1)  // x86 Read/Write Bit
    #define SLS_PTE_USER        (1ULL << 2)  // x86 User/Supervisor Bit
    #define SLS_PTE_ACCESSED    (1ULL << 5)  // x86 Accessed Bit
    #define SLS_PTE_DIRTY       (1ULL << 6)  // x86 Dirty Bit
    #define SLS_PTE_SLS_DISK    (1ULL << 9)  // Custom Bit 9 (Available for OS)

    #define SLS_FRAME_MASK      0x000FFFFFFFFFF000ULL

    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) {
        return pte_val & SLS_FRAME_MASK;
    }

    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) {
        return (phys_addr & SLS_FRAME_MASK) | flags;
    }

#elif defined(__riscv) || defined(__riscv_xlen)
    // ----------------------------------------------------
    // RISC-V (Sv39/Sv48) HARDWARE COMPLIANCE MAPPINGS
    // ----------------------------------------------------
    #define SLS_PTE_VALID       (1ULL << 0)  // RISC-V V bit
    #define SLS_PTE_READABLE    (1ULL << 1)  // RISC-V R bit
    #define SLS_PTE_WRITABLE    (1ULL << 2)  // RISC-V W bit
    #define SLS_PTE_USER        (1ULL << 4)  // RISC-V U bit
    #define SLS_PTE_ACCESSED    (1ULL << 6)  // RISC-V A bit
    #define SLS_PTE_DIRTY       (1ULL << 7)  // RISC-V D bit
    #define SLS_PTE_SLS_DISK    (1ULL << 8)  // Custom Bit 8 (RSW Software allocation)

    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) {
        // Shift right out of standard PTE flags field and convert back to 12-bit aligned PA
        return ((pte_val >> 10) << 12);
    }

    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) {
        // Shift absolute physical address into RISC-V target PPN field slots (Bits 10-53)
        uint64_t base_pte = (phys_addr >> 12) << 10;
        
        // On RISC-V, if a page is marked Writable, it MUST also have the Readable bit set 
        // to prevent invalid architectural flag states
        if (flags & SLS_PTE_WRITABLE) {
            base_pte |= (1ULL << 1); // Enforce implicit PTE_R
        }
        
        return base_pte | flags;
    }
#else
    #error "AeroSLS compilation target: Unsupported Processor Architecture."
#endif

// Generic, Architecture-Agnostic SLS Core API Primitives
static inline void sls_mark_page_disk_resident(uint64_t* pte_entry_ptr, uint64_t block_id) {
    // Clear presence flags, enable custom disk resident tracking flag, embed disk target id
    uint64_t clean_flags = SLS_PTE_SLS_DISK;
    *pte_entry_ptr = sls_compile_pte((block_id << 12), clean_flags);
}

static inline int sls_is_page_dirty(uint64_t pte_val) {
    return (pte_val & SLS_PTE_DIRTY) ? 1 : 0;
}

#endif