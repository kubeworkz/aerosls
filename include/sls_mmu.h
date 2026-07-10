#ifndef SLS_MMU_H
#define SLS_MMU_H
#include <stdint.h>
struct SLSPte { uint64_t* raw_entry_ptr; };
#if defined(__x86_64__)
    #define SLS_PTE_VALID       (1ULL << 0)
    #define SLS_PTE_WRITABLE    (1ULL << 1)
    #define SLS_PTE_USER        (1ULL << 2)
    #define SLS_PTE_ACCESSED    (1ULL << 5)
    #define SLS_PTE_DIRTY       (1ULL << 6)
    #define SLS_PTE_SLS_DISK    (1ULL << 9)
    #define SLS_FRAME_MASK      0x000FFFFFFFFFF000ULL
    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) { return pte_val & SLS_FRAME_MASK; }
    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) { return (phys_addr & SLS_FRAME_MASK) | flags; }
#elif defined(__riscv) || defined(__riscv_xlen)
    #define SLS_PTE_VALID       (1ULL << 0)
    #define SLS_PTE_READABLE    (1ULL << 1)
    #define SLS_PTE_WRITABLE    (1ULL << 2)
    #define SLS_PTE_USER        (1ULL << 4)
    #define SLS_PTE_ACCESSED    (1ULL << 6)
    #define SLS_PTE_DIRTY       (1ULL << 7)
    #define SLS_PTE_SLS_DISK    (1ULL << 8)
    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) { return ((pte_val >> 10) << 12); }
    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) {
        uint64_t base_pte = (phys_addr >> 12) << 10;
        if (flags & SLS_PTE_WRITABLE) base_pte |= (1ULL << 1);
        return base_pte | flags;
    }
#endif
static inline void sls_mark_page_disk_resident(uint64_t* pte_entry_ptr, uint64_t block_id) {
    *pte_entry_ptr = sls_compile_pte((block_id << 12), SLS_PTE_SLS_DISK);
}
static inline int sls_is_page_dirty(uint64_t pte_val) { return (pte_val & SLS_PTE_DIRTY) ? 1 : 0; }
#endif