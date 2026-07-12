#ifndef USER_PAGING_H
#define USER_PAGING_H

#include <stdint.h>

// ─── Page flag constants for user-space mappings ─────────────────────────────
#define USER_PTE_PRESENT  (1ULL << 0)
#define USER_PTE_WRITE    (1ULL << 1)
#define USER_PTE_USER     (1ULL << 2)   // hardware U/S bit — allows Ring-3 access
#define USER_PTE_ACCESSED (1ULL << 5)
#define USER_PTE_DIRTY    (1ULL << 6)
// NX bit: bit 63. We define EXEC as absence of NX.
#define USER_PTE_EXEC     0ULL          // no NX bit set = executable
#define USER_PTE_NOEXEC   (1ULL << 63)  // NX bit set = non-executable

#define USER_PTE_FRAME_MASK 0x000FFFFFFFFFF000ULL

// ─── MSR numbers for SYSCALL/SYSRET ──────────────────────────────────────────
#define MSR_EFER         0xC0000080
#define MSR_STAR         0xC0000081
#define MSR_LSTAR        0xC0000082
#define MSR_SFMASK       0xC0000084
#define MSR_GS_BASE      0xC0000101
#define MSR_KERNEL_GS    0xC0000102

#define EFER_SCE         (1ULL << 0)   // SYSCALL Enable

// ─── Per-CPU data structure (swapgs target) ───────────────────────────────────
// syscall_entry_stub accesses [gs:0] and [gs:8] for RSP swapping.
struct PerCPUData {
    uint64_t user_rsp;    // offset 0: scratch for saving user RSP on syscall entry
    uint64_t kernel_rsp;  // offset 8: kernel stack pointer loaded on syscall entry
};

extern struct PerCPUData per_cpu_data[4];  // one slot per core

// ─── Public API ───────────────────────────────────────────────────────────────

// Configure IA32_STAR / IA32_LSTAR / IA32_SFMASK / EFER.SCE
// Call once from BSP during kernel_main before spawning any user process.
void syscall_gate_init(void);

// Clone the current PML4, copying kernel (upper-half) entries only.
// Returns the physical address of the new PML4, or 0 on failure.
uint64_t user_clone_page_table(void);

// Map one 4-KiB page in the given PML4 with the specified flags.
// Allocates intermediate tables from the physical frame pool as needed.
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

#endif /* USER_PAGING_H */
