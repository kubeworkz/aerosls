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
extern int simi_frame_is_cached(uint64_t paddr, uint32_t* seen);

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
// deliberately left EMPTY: the child gets a fresh, empty user half, and the
// spawn path (program_spawn/process_create) maps the child's own binary and
// stack into it.
//
// Why empty, not deep-copied (LPAR Phase 14a destroy-time finding, caught by
// the partition-teardown boot check's valloc probe): the earlier deep-copy
// gave the child its own PDPT/PD/PT pages (fixing the original all-slot
// shallow-copy bug, where loading the child at the same vaddr rewrote the
// SHARED intermediate tables) BUT still copied the parent's depth-3 leaf
// PTEs VERBATIM — sharing the parent's binary/stack physical frames. The
// loader/spawn then remapped the pages the CHILD needs, but any parent
// mapping the child didn't overwrite stayed mapped (e.g. the parent's second
// binary page when the child's binary is one page). The child's exit
// teardown then walked ITS tables and freed those frames as its own —
// freeing the PARENT's live binary/stack frames out from under it; the
// freed frames were reallocated (the next child's PML4 landed in the
// parent's binary page) and zero-filled, corrupting the still-running
// parent (its next valloc request struct read back as zeros). An empty user
// half has no leftovers to leak: every frame the child's walker frees is
// genuinely the child's.
static void clone_kernel_slots(uint64_t* dst, uint64_t* src) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & USER_PTE_PRESENT)) continue;
        if (!(e & USER_PTE_USER)) dst[i] = e;   /* kernel slot: share by pointer */
    }
}

/* The kernel's own boot identity map (arch/x86/boot.asm): the canonical
 * supervisor half every process table shares by pointer. Cloning from THIS
 * table — rather than from the current CR3 — matters for sidecar spawns
 * (Phase 5): a sidecar calls k_create_sidecar from ring 3, so the kernel
 * handles that syscall with the SIDECAR's CR3 active. That table's identity
 * slot (PML4[0]) may have been re-pointed to a USER PDPT by
 * user_map_identity, and clone_kernel_slots skips USER entries — a child
 * spawned from that state would inherit NO kernel identity map and the
 * first kernel-code fetch after its CR3 switch would #PF (observed under
 * QEMU: fault at 0x139d1e, CR2 = the schedule_ring3 continuation, right
 * after mov cr3,<child>). boot.asm's p4_table is never modified after
 * boot, so it is always the authoritative kernel half. */
extern uint64_t p4_table[512];

uint64_t user_clone_page_table(void) {
    uint64_t* new_pml4 = alloc_page_table();
    if (!new_pml4) return 0;

    clone_kernel_slots(new_pml4, p4_table);   /* user slots stay zero */

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

static uint64_t* own_table(uint64_t* parent, size_t idx, int copy_existing);
static int user_map_page_safe(uint64_t* pml4, uint64_t vaddr, uint64_t paddr,
                              uint64_t flags);

// ─── user_map_identity ────────────────────────────────────────────────────────
// Map `npages` of physical memory into a process's address space at the SAME
// virtual addresses (identity), with Ring-3 access. This is what a sidecar's
// BIB MEM cap contract needs (kernel/cap.c writes the cap's PHYSICAL base
// into the BIB and the sidecar reads it directly — Phase 5 sidecar
// bootinfo.rs), but it is NOT what user_map_page() does, and calling
// user_map_page() here is actively dangerous: the child's PML4 shares the
// kernel's low identity map by pointer (user_clone_page_table →
// clone_kernel_slots), whose PTEs are supervisor 2 MiB huge pages. The
// get_or_alloc() walk would treat a PRESENT huge-page PD entry as a table
// pointer and write the leaf PTE through physical memory, corrupting the
// huge page's frame and the SHARED kernel tables.
//
// Instead the walk re-points only the CHILD's own copies of the path:
//   - a fresh PDPT at the PML4 slot (the shared entries copied, so the rest
//     of low memory stays visible to the child's kernel-mode execution);
//   - a fresh PD at the PDPT slot (shared entries copied, same reason);
//   - a fresh PT per touched 2 MiB chunk, with the chunk's huge page
//     REPLICATED at 4 KiB granularity (same frames, same flags minus PS) so
//     the kernel keeps full visibility of the chunk, then the cap pages
//     re-flagged with the requested (USER) flags.
// The kernel's own tables are never modified — only the child's.
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    for (uint32_t p = 0; p < npages; p++) {
        uint64_t va = phys + (uint64_t)p * 4096;
        if (user_map_page_safe(pml4, va, va, flags) < 0) return -1;
    }
    return 0;
}

/* Strong override of cap.c's weak cap_arch_identity_map_user: the kernel's
 * cap_recv_msg grants a MEM cap across a channel and must make the region
 * Ring-3-readable in the RECEIVER's address space (kernel/cap.c CAP_TYPE_MEM
 * install branch). Same identity-map as user_map_identity, with flags derived
 * from the cap's CAP_PERM_* rights. cap_proc_cr3() gives the receiver's PML4. */
int cap_arch_identity_map_user(uint64_t pml4_phys, uint64_t phys,
                               uint32_t npages, uint32_t cap_perms) {
    uint64_t leaf = USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_NOEXEC;
    if (cap_perms & 0x02u) leaf |= USER_PTE_WRITE;   /* CAP_PERM_W */
    return user_map_identity((uint64_t*)(uintptr_t)pml4_phys, phys, npages,
                             leaf);
}

// Get a table the CHILD owns at parent[idx]: reuse it if the existing entry
// is already a USER table (fresh from a previous call here or from
// user_map_page's get_or_alloc); otherwise allocate a fresh zeroed table and
// re-point parent[idx] at it — copying the shared table's entries when
// `copy_existing` is set, so untouched regions keep sharing the kernel's
// mapping.
static uint64_t* own_table(uint64_t* parent, size_t idx, int copy_existing) {
    uint64_t e = parent[idx];
    if ((e & USER_PTE_PRESENT) && (e & USER_PTE_USER))
        return (uint64_t*)(uintptr_t)(e & USER_PTE_FRAME_MASK);
    uint64_t* fresh = alloc_page_table();
    if (!fresh) return 0;
    if (copy_existing && (e & USER_PTE_PRESENT)) {
        const uint64_t* shared = (const uint64_t*)(uintptr_t)(e & USER_PTE_FRAME_MASK);
        for (int i = 0; i < 512; i++) fresh[i] = shared[i];
    }
    parent[idx] = ((uint64_t)(uintptr_t)fresh & USER_PTE_FRAME_MASK)
                  | USER_PTE_PRESENT | USER_PTE_WRITE | USER_PTE_USER;
    return fresh;
}

// ─── user_map_page_safe ────────────────────────────────────────────────────────
// Single-page map into a child PML4 that SHARES the kernel's low identity map
// by pointer (clone_kernel_slots). Unlike user_map_page(), the walk must not
// follow the shared supervisor entries: a PRESENT 2 MiB huge-page PD entry
// (the kernel's 0-4 GiB identity map) is not a table pointer, and writing a
// leaf through it corrupts the kernel's own frames (observed under QEMU: the
// DEV-window map at 0x100000 wrote PTEs through phys 0x800.. and the kernel
// later faulted). Mirrors user_map_identity(): re-point only the child's own
// copies of the path, replicating a huge page at 4 KiB granularity so the
// kernel keeps full visibility of the chunk, then install the requested leaf.
// This is the arch hook behind cap_arch_map_page (DEV mmap and MEM sys_map).
static int user_map_page_safe(uint64_t* pml4, uint64_t vaddr, uint64_t paddr,
                              uint64_t flags) {
    uint64_t* pdpt = own_table(pml4, PML4_IDX(vaddr), 1);
    if (!pdpt) return -1;
    uint64_t* pd = own_table(pdpt, PDPT_IDX(vaddr), 1);
    if (!pd) return -1;

    uint64_t pe = pd[PD_IDX(vaddr)];
    uint64_t* pt;
    if ((pe & USER_PTE_PRESENT) && (pe & USER_PTE_USER)) {
        pt = (uint64_t*)(uintptr_t)(pe & USER_PTE_FRAME_MASK); /* ours already */
    } else {
        pt = alloc_page_table();
        if (!pt) return -1;
        if (pe & USER_PTE_PRESENT) {
            if (pe & (1ULL << 7)) { /* PS: 2 MiB huge page — replicate at 4 KiB */
                uint64_t base = pe & USER_PTE_FRAME_MASK;
                uint64_t keep = pe & ~(USER_PTE_FRAME_MASK | (1ULL << 7));
                for (int i = 0; i < 512; i++)
                    pt[i] = (base + (uint64_t)i * 4096) | keep;
            } else { /* 4 KiB-level table — copy its entries */
                const uint64_t* shared =
                    (const uint64_t*)(uintptr_t)(pe & USER_PTE_FRAME_MASK);
                for (int i = 0; i < 512; i++) pt[i] = shared[i];
            }
        }
        pd[PD_IDX(vaddr)] = ((uint64_t)(uintptr_t)pt & USER_PTE_FRAME_MASK)
                            | USER_PTE_PRESENT | USER_PTE_WRITE | USER_PTE_USER;
    }
    /* A grant that re-maps a page the receiver ALREADY holds as a writable
     * USER page must never silently drop its write bit. The block-cache
     * move-return hands a client its own request buffer back with the
     * grantee's (read-only, for a write request) rights; without this the
     * client's next refill of that buffer would #PF. Preserve W ONLY when
     * re-mapping the SAME frame that is already a USER mapping — a fresh
     * grant sees the replicated kernel huge-page (U/S=0) or an empty slot,
     * so a read-only grant to a NEW receiver still installs read-only. */
    {
        uint64_t prev = pt[PT_IDX(vaddr)];
        uint64_t keep_w = 0;
        if ((prev & USER_PTE_PRESENT) && (prev & USER_PTE_USER) &&
            (prev & USER_PTE_FRAME_MASK) == (paddr & USER_PTE_FRAME_MASK))
            keep_w = prev & USER_PTE_WRITE;
        pt[PT_IDX(vaddr)] = (paddr & USER_PTE_FRAME_MASK) | flags | keep_w;
    }
    return 0;
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

static uint32_t user_free_subtree(uint64_t* tbl, int depth, uint32_t* seen) {
    uint32_t freed = 0;
    for (int i = 0; i < 512; i++) {
        uint64_t e = tbl[i];
        if (!(e & USER_PTE_PRESENT)) continue;
        /* Kernel-half slots are shared by pointer with the kernel
         * (clone_kernel_slots; own_table(copy_existing=1) also copies kernel
         * entries into the child's own tables) and are NEVER this process's
         * property — neither the 2 MiB identity huge pages nor the shared
         * intermediate tables. The U/S check below is load-bearing: without
         * it every process exit freed supervisor huge-page frames out of
         * the shared kernel identity map, handing live RAM (other sidecars'
         * stacks/images, kernel infrastructure) back to the pool — observed
         * as the parked network driver's user stack being zeroed by the
         * respawned process's freshly allocated PML4 and the driver
         * resuming to a NULL return address. A us=0 leaf inside a
         * process-owned table (own_table's replicated identity chunk, a
         * supervisor-flagged DEV map) aliases a frame owned by someone
         * else; skipping it leaks nothing — its true owner frees it. */
        if (!(e & USER_PTE_USER)) continue;
        /* Leaf test mirrors clone_user_levels(): PTEs at depth 3 are
         * leaves; at depths 1-2 a set PS bit (bit 7) marks a 1 GiB / 2 MiB
         * large page — also a leaf. (At depth 3 bit 7 is PAT, hence the
         * depth check first.) */
        if (depth >= 3 || (depth > 0 && (e & 0x80))) {
            uint64_t pa = e & USER_PTE_FRAME_MASK;
            /* Phase 14c: `seen` (a per-walk bitmap owned by
             * user_destroy_page_table) makes the cached-frame check also
             * account this process's mapping of each SIMI activation
             * exactly once — a process maps every code page of an
             * activation, so without it the walker would decrement the
             * mapper refcount once per page. See simi_frame_is_cached(). */
            if (!user_frame_refused(pa) && !simi_frame_is_cached(pa, seen) &&
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
        freed += user_free_subtree(child, depth + 1, seen);
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
    uint32_t seen = 0;   /* Phase 14c: SIMI-activation bitmap for this walk */
    uint32_t freed = user_free_subtree(pml4, 0, &seen);   /* user half */
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
    if (cap_perms & CAP_PERM_DEV_UC) flags |= USER_PTE_PCD;  /* uncached */
    if (cap_perms & CAP_PERM_DEV_WC) flags |= USER_PTE_PWT;  /* write-thru */
    /* NOT user_map_page(): the child's PML4 shares the kernel's low identity
     * map by pointer (supervisor 2 MiB huge pages), and get_or_alloc() would
     * treat a present huge-page PD entry as a table pointer — writing the
     * leaf PTE through physical memory. The DEV-window scan (k_dev_mmap)
     * starts at 0x100000, squarely inside that map, so the unsafe walk
     * corrupted kernel frames and left the window unmapped (user reads
     * faulted as present+supervisor). Same fix create_sidecar's MEM caps got
     * via user_map_identity(). */
    return user_map_page_safe((uint64_t*)(uintptr_t)pml4_phys, vaddr, paddr,
                              flags);
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

/* Driver SDK ABI v0.1 §4.2 — strong port-I/O overrides for cap.c's weak
 * hooks. All port access is mediated by the kernel syscall path
 * (SYS_IO_IN/SYS_IO_OUT); only these execute privileged in/out. */
uint32_t cap_io_read(uint16_t port, uint8_t size) {
    uint32_t v = 0;
    switch (size) {
    case 1: {
        uint8_t b;
        __asm__ volatile("inb %1, %0" : "=a"(b) : "Nd"(port));
        v = b;
        break;
    }
    case 2: {
        uint16_t w;
        __asm__ volatile("inw %1, %0" : "=a"(w) : "Nd"(port));
        v = w;
        break;
    }
    default: {
        __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
        break;
    }
    }
    return v;
}

void cap_io_write(uint16_t port, uint8_t size, uint32_t val) {
    /* IRQ4 ownership hand-off (see kernel_io.h): a ring-3 MCR write to
     * COM1 transfers the port to/from the probe at the write itself —
     * ON (bit 4 set): kernel serial TX and the console poll's RX
     * handling stand down from that instruction onward; OFF: kernel TX
     * resumes immediately so the probe's post-clear verdict prints
     * reach the wire. The poll-side branch keeps ownership refreshed and
     * releases it as a fallback. Byte-size writes only: the probe
     * programs the 16550 with single outb. */
    if (size == 1 && port == 0x3FCu && serial_loopback_ownership_set)
        serial_loopback_ownership_set((val & 0x10u) ? 1 : 0);
    switch (size) {
    case 1:
        __asm__ volatile("outb %0, %1" : : "a"((uint8_t)val), "Nd"(port));
        break;
    case 2:
        __asm__ volatile("outw %0, %1" : : "a"((uint16_t)val), "Nd"(port));
        break;
    default:
        __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
        break;
    }
}
