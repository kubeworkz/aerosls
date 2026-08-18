#include "user_paging.h"
#include "../../kernel/kernel_io.h"
#include "../../kernel/cap.h"   /* cap_arch_* hook prototypes (strong defs below) */
#include "../../kernel/frame_pool.h"   /* Phase 2 teardown: free + owner queries */
#include <stddef.h>

/* Declared in kernel/qemu_sls_mmu.h; forward-declared here so the strong
 * cap_arch_tlb_flush() below can call it without pulling that header's
 * whole dependency tree into the page-table unit. */
extern void qemu_sls_flush_tlb(void);

extern void* allocate_physical_ram_frame(void);

/* Defined in kernel/simi_translate.c (Phase 2 teardown): 1 if paddr is one
 * of the SHARED activation-cache code frames, which every process that
 * spawns that SIMI object maps — they must NOT be freed with a single
 * process's address space. Forward-declared here, same style as the
 * allocate_physical_ram_frame extern above, to avoid pulling simi_x86.h's
 * dependency tree into the page-table unit. */
extern int simi_frame_is_cached(uint64_t paddr);

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
static uint8_t kernel_syscall_stack[8192] __attribute__((aligned(16)));

// ─── syscall_gate_init ────────────────────────────────────────────────────────
// Must be called once by the BSP during kernel_main before entering Ring-3.
void syscall_gate_init(void) {
    // Enable SYSCALL/SYSRET and No-Execute (NX) page protection in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE | EFER_NXE;
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
// Creates a new PML4 for a child process. Supervisor (kernel) PML4 slots are
// SHARED by pointer — the kernel identity map (slots 0-1) is what keeps
// interrupt/syscall handlers reachable after the CR3 switch, and its PTEs
// have U/S=0 so Ring-3 can never touch them. USER-mapped slots (U/S=1) are
// DEEP-COPIED: the child gets its own PDPT/PD/PT pages so that later
// user_map_page() calls for the child (loading its binary, mapping its stack)
// rewrite only the CHILD's tables.
//
// The all-slot shallow copy this replaces was a fork-without-COW bug: the
// child's PML4 initially shared the parent's lower-level pages, so loading
// the child's binary at the same vaddr (USER_PROC_CODE_BASE) walked into the
// SHARED PT/PD/PDPT pages and rewrote them — corrupting the PARENT's address
// space. Verified live in the two-party capability test: after the child was
// loaded, the parent "ran" the child's code at its own RIP (its PML4 mapped
// cap_peer.bin where cap_two_party.bin should be) and exited with the
// child's failure path.
static int clone_user_levels(uint64_t* dst, uint64_t* src, int depth) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & USER_PTE_PRESENT)) continue;
        if (depth == 0 && !(e & USER_PTE_USER)) {
            dst[i] = e;    /* kernel/supervisor slot: share by pointer */
            continue;
        }
        /* Leaf entries: PTEs at depth 3 are copied verbatim; at depth 1-2 a
         * set PS bit (bit 7) marks a 1 GiB / 2 MiB large page — also a leaf.
         * (At depth 3 bit 7 is PAT, which is why the depth check comes first.) */
        if (depth >= 3 || (depth > 0 && (e & 0x80))) {
            dst[i] = e;
            continue;
        }
        uint64_t* src_child = (uint64_t*)(uintptr_t)(e & USER_PTE_FRAME_MASK);
        uint64_t* new_child = alloc_page_table();
        if (!new_child) return -1;
        if (clone_user_levels(new_child, src_child, depth + 1) != 0) return -1;
        dst[i] = ((uint64_t)(uintptr_t)new_child & USER_PTE_FRAME_MASK)
                 | (e & ~USER_PTE_FRAME_MASK);
    }
    return 0;
}

uint64_t user_clone_page_table(void) {
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    uint64_t* kernel_pml4 = (uint64_t*)(uintptr_t)(current_cr3 & USER_PTE_FRAME_MASK);

    uint64_t* new_pml4 = alloc_page_table();
    if (!new_pml4) return 0;

    // PML4 level: share supervisor slots, deep-copy user slots (which recurses
    // down through PDPT and PD; PTEs at depth 3 are copied verbatim).
    if (clone_user_levels(new_pml4, kernel_pml4, 0) != 0) return 0;

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

// ─── user_unmap_page ──────────────────────────────────────────────────────────
// Clear the leaf PTE for vaddr. Unlike get_or_alloc(), the walk never
// allocates: a missing intermediate level means there is nothing mapped and
// the call is a no-op.
static uint64_t* present_child(uint64_t* parent, size_t idx) {
    return (parent[idx] & USER_PTE_PRESENT)
        ? (uint64_t*)(uintptr_t)(parent[idx] & USER_PTE_FRAME_MASK) : 0;
}

void user_unmap_page(uint64_t* pml4, uint64_t vaddr) {
    uint64_t* pdpt = present_child(pml4, PML4_IDX(vaddr));
    if (!pdpt) return;
    uint64_t* pd = present_child(pdpt, PDPT_IDX(vaddr));
    if (!pd)   return;
    uint64_t* pt = present_child(pd, PD_IDX(vaddr));
    if (!pt)   return;
    pt[PT_IDX(vaddr)] = 0;
}

/* ─── Phase 2 teardown: user_destroy_page_table ───────────────────────────────
 * Free every frame a process's address space owns, recursively from the
 * user half of its PML4 down through the intermediate tables to the leaf
 * data frames, then the page-table frames themselves (PML4 last).
 *
 * Why THIS walker is safe where a naive one was not (the named gap in the
 * LPAR Phase 14 / Gap Remediation findings): it frees ONLY
 *   - user-half (U/S=1) intermediate tables, which user_clone_page_table()
 *     deep-copied (and user_map_page()/get_or_alloc() extended) for THIS
 *     process alone — kernel-half slots are shared by pointer with the
 *     kernel and are skipped entirely, and
 *   - user-half LEAF frames that the REFUSAL set (user_frame_refused,
 *     below) does not protect: the kernel image / low memory (below
 *     _kernel_image_end — covers the image and the bootstrap stack, which
 *     the boot reservation verified sits inside it), the cap ARENA (via
 *     cap_frame_in_arena — precise where frame_pool_frame_is_machine_owned
 *     is not: the arena carve folds into the reserved_below watermark,
 *     which also covers every ALLOCATABLE frame below the arena, so that
 *     check would wrongly refuse live process frames — caught live as
 *     "0 frame(s) freed" for every exited process), and the SHARED SIMI
 *     activation-cache code frames (mapped into EVERY process that spawns
 *     that object; they must outlive any one of them).
 *
 * Everything else that can appear in a user slot is a frame the pool
 * allocated for THIS process (binary pages, user stack, SIMI scratch,
 * page-table internals), and free_physical_ram_frame_for_partition()'s own
 * validation (aligned, in-range, bit set) is the backstop against anything
 * bogus. Leaves are freed with frame_pool_frame_owner()'s recorded
 * partition id, so the right quota counter is decremented for accounted
 * loader/stack frames AND the unaccounted page-table/SIMI-scratch frames
 * (both tag PARTITION_SYSTEM).
 *
 * Ordering contract: cap_table_teardown() runs FIRST and unmaps every
 * cap-derived PTE, so the only user-half leaves remaining here are frames
 * this process truly owns (binary, stack, SIMI scratch) — and even a
 * stray cap PTE could not free an arena frame, thanks to the refusal set.
 * Called from process_exit() once the process's exit syscall has begun:
 * execution from there on lives in the kernel half (shared by pointer,
 * never freed), and the freed frames' contents stay physically intact
 * until the CR3 switch in the exit path abandons the address space (no
 * allocation can re-hand them in between — single CPU, IF=0 inside the
 * syscall). */

/* The linker's image-end symbol (same declaration style as kernel/cap.c).
 * The kernel image, low memory, and the bootstrap stack (verified inside
 * the image-end reservation by frame_pool_init) all live below it. */
extern char _kernel_image_end[];

static int user_frame_refused(uint64_t pa) {
    if (pa < (uint64_t)(uintptr_t)_kernel_image_end) return 1;  /* image + low memory */
    if (cap_frame_in_arena(pa)) return 1;                        /* the cap arena */
    return 0;
}

static uint32_t user_free_subtree(uint64_t* tbl, int depth) {
    uint32_t freed = 0;
    for (int i = 0; i < 512; i++) {
        uint64_t e = tbl[i];
        if (!(e & USER_PTE_PRESENT)) continue;
        /* Leaf test mirrors clone_user_levels(): PTEs at depth 3 are
         * leaves; at depths 1-2 a set PS bit (bit 7) marks a 1 GiB / 2 MiB
         * large page — also a leaf. (At depth 3 bit 7 is PAT, hence the
         * depth check first.) */
        if (depth >= 3 || (depth > 0 && (e & 0x80))) {
            uint64_t pa = e & USER_PTE_FRAME_MASK;
            if (!user_frame_refused(pa) && !simi_frame_is_cached(pa) &&
                free_physical_ram_frame_for_partition(
                    (void*)(uintptr_t)pa,
                    frame_pool_frame_owner(pa / 4096)) == 0) {
                freed++;
            }
            continue;
        }
        /* Intermediate table: recurse first (its children are read before
         * anything is freed), then free the table frame itself. */
        uint64_t* child = (uint64_t*)(uintptr_t)(e & USER_PTE_FRAME_MASK);
        freed += user_free_subtree(child, depth + 1);
        uint64_t cpa = (uint64_t)(uintptr_t)child;
        if (!user_frame_refused(cpa) &&
            free_physical_ram_frame_for_partition(
                child, frame_pool_frame_owner(cpa / 4096)) == 0) {
            freed++;
        }
    }
    return freed;
}

void user_destroy_page_table(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)pml4_phys;
    uint32_t freed = user_free_subtree(pml4, 0);   /* user half: tables + leaves */
    if (!user_frame_refused(pml4_phys) &&
        free_physical_ram_frame_for_partition(
            (void*)(uintptr_t)pml4_phys,
            frame_pool_frame_owner(pml4_phys / 4096)) == 0) {
        freed++;
    }
    kernel_serial_printf(
        "[TORE] address space 0x%016lx destroyed: %u frame(s) freed\n",
        (unsigned long long)pml4_phys, freed);
}

/* ─── Capability-layer arch hooks (kernel/cap.h contract) ────────────────────
 * Strong definitions that override cap.c's weak stubs in the real kernel;
 * host tests that fake a page table define their own instead. The PML4 is
 * addressed by its physical address (a process's cr3); the kernel's 0-4 GiB
 * identity map makes the cast to a pointer valid on x86-64. */
int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                      uint32_t cap_perms) {
    if (!pml4_phys) return -1;
    uint64_t flags = USER_PTE_PRESENT | USER_PTE_USER;
    if (cap_perms & 0x02) flags |= USER_PTE_WRITE;      /* CAP_PERM_W */
    if (!(cap_perms & 0x04)) flags |= USER_PTE_NOEXEC;  /* CAP_PERM_X */
    user_map_page((uint64_t*)(uintptr_t)pml4_phys, vaddr, paddr, flags);
    return 0;
}

int cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    if (!pml4_phys) return -1;
    user_unmap_page((uint64_t*)(uintptr_t)pml4_phys, vaddr);
    return 0;
}

void cap_arch_tlb_flush(void) {
    qemu_sls_flush_tlb();
}

/* See the declaration in user_paging.h for why this is a function and not an
 * inline. */
uint64_t arch_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Full non-global TLB flush by CR3 reload. Lives here beside arch_read_cr3()
 * and for the same reason: privileged, so a host test needs a seam. */
void qemu_sls_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}
