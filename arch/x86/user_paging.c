#include "user_paging.h"
#include "../../kernel/kernel_io.h"
#include <stddef.h>

extern void* allocate_physical_ram_frame(void);

struct PerCPUData per_cpu_data[4];

// ─── MSR helpers ──────────────────────────────────────────────────────────────
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
                     : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val>>32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ─── Kernel syscall stack (used by the swapgs RSP swap) ──────────────────────
static uint8_t kernel_syscall_stack[4096] __attribute__((aligned(16)));

// ─── syscall_gate_init ────────────────────────────────────────────────────────
// Must be called once by the BSP during kernel_main before entering Ring-3.
void syscall_gate_init(void) {
    // Enable SYSCALL / SYSRET in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // STAR: bits[47:32] = SYSCALL kernel CS (0x08); bits[63:48] = SYSRET CS base (0x10)
    // SYSRETQ: CS = (0x10 + 16) | RPL3 = 0x23 (User Code)
    //          SS = (0x10 +  8) | RPL3 = 0x1B (User Data)
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    // LSTAR: kernel entry point for SYSCALL instruction
    extern void syscall_entry_stub(void);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_stub);

    // SFMASK: clear IF (bit 9) on syscall entry so kernel runs with interrupts off
    wrmsr(MSR_SFMASK, (1 << 9));

    // Set up per-CPU data for the BSP (core 0)
    per_cpu_data[0].user_rsp   = 0;
    per_cpu_data[0].kernel_rsp = (uint64_t)(uintptr_t)
                                  &kernel_syscall_stack[sizeof(kernel_syscall_stack) - 8];

    // Write kernel GS base (pointed to by swapgs on syscall entry)
    wrmsr(MSR_KERNEL_GS, (uint64_t)(uintptr_t)&per_cpu_data[0]);
    // User GS base starts at 0
    wrmsr(MSR_GS_BASE, 0);

    kernel_serial_printf(
        "[SYSCALL] Gate initialised. STAR=0x%016lx  LSTAR=0x%016lx\n",
        star, (uint64_t)(uintptr_t)syscall_entry_stub);
}

// ─── Page table index helpers ─────────────────────────────────────────────────
#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)

// Allocate a zeroed physical frame for a page table
static uint64_t* alloc_page_table(void) {
    uint64_t* t = (uint64_t*)allocate_physical_ram_frame();
    if (!t) return 0;
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

// Get or create a child table pointer at index [idx] within parent[]
static uint64_t* get_or_alloc(uint64_t* parent, size_t idx) {
    if (!(parent[idx] & USER_PTE_PRESENT)) {
        uint64_t* child = alloc_page_table();
        if (!child) return 0;
        parent[idx] = ((uint64_t)(uintptr_t)child & USER_PTE_FRAME_MASK)
                      | USER_PTE_PRESENT | USER_PTE_WRITE | USER_PTE_USER;
        return child;
    }
    return (uint64_t*)(uintptr_t)(parent[idx] & USER_PTE_FRAME_MASK);
}

// ─── user_clone_page_table ────────────────────────────────────────────────────
// Creates a new PML4 and copies the kernel (upper-half, indices 256-511) entries.
// The user half is left empty — user mappings are added by user_map_page().
uint64_t user_clone_page_table(void) {
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    uint64_t* kernel_pml4 = (uint64_t*)(uintptr_t)(current_cr3 & USER_PTE_FRAME_MASK);

    uint64_t* new_pml4 = alloc_page_table();
    if (!new_pml4) return 0;

    // Copy kernel-space entries (upper 256 slots of PML4)
    for (int i = 256; i < 512; i++) new_pml4[i] = kernel_pml4[i];

    kernel_serial_printf("[PAGING] New user PML4 at 0x%016lx\n",
                         (uint64_t)(uintptr_t)new_pml4);
    return (uint64_t)(uintptr_t)new_pml4;
}

// ─── user_map_page ────────────────────────────────────────────────────────────
// Walk four levels of the given PML4, allocating missing intermediate tables,
// and install a leaf PTE for vaddr → paddr with the requested flags.
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t* pdpt = get_or_alloc(pml4, PML4_IDX(vaddr));
    if (!pdpt) return;
    uint64_t* pd   = get_or_alloc(pdpt, PDPT_IDX(vaddr));
    if (!pd)   return;
    uint64_t* pt   = get_or_alloc(pd,   PD_IDX(vaddr));
    if (!pt)   return;

    pt[PT_IDX(vaddr)] = (paddr & USER_PTE_FRAME_MASK) | flags;
}
