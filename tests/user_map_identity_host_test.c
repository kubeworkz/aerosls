/* tests/user_map_identity_host_test.c — host test for the identity remap
 * in arch/x86/user_paging.c (user_map_identity + own_table).
 *
 * Why this matters (observed under QEMU, Phase 5 boot): a sidecar's BIB MEM
 * caps carry the PHYSICAL base of their region and the sidecar reads it
 * directly, so cap_create_sidecar identity-maps the region with Ring-3
 * access. But the child's PML4 SHARES the kernel's low identity map by
 * pointer (user_clone_page_table → clone_kernel_slots), whose PTEs are
 * supervisor 2 MiB huge pages — a plain user_map_page() walk would treat a
 * PRESENT huge-page PD entry as a table pointer and write through physical
 * memory. user_map_identity must instead re-point only the CHILD's own
 * copies of the path (fresh PDPT/PD with the shared entries copied, fresh
 * PTs per touched 2 MiB chunk with the huge page replicated at 4 KiB) so:
 *   1. the cap page becomes Ring-3 accessible (U/S=1, right flags);
 *   2. EVERY OTHER page keeps its old mapping — in particular the kernel
 *      code chunk (PD[0], containing the scheduler at 0x139xxx) stays
 *      present, or the first schedule of the child after a CR3 switch
 *      faults fetching the next instruction (the exact QEMU failure);
 *   3. the kernel's own tables are never modified.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/user_map_identity_host_test \
 *       tests/user_map_identity_host_test.c arch/x86/user_paging.c
 *   /tmp/user_map_identity_host_test
 */
#include "arch/x86/user_paging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs (user_paging.c's kernel dependencies) ─────────────────── */
void* allocate_physical_ram_frame(void) {
    void* p = aligned_alloc(4096, 4096);
    if (p) memset(p, 0, 4096);
    return p;
}
int kernel_serial_print(const char* s) { (void)s; return 0; }
int kernel_serial_printf(const char* f, ...) { (void)f; return 0; }
int kernel_serial_putchar(char c) { (void)c; return 0; }
int simi_frame_is_cached(uint64_t paddr, uint32_t* seen) {
    (void)paddr; (void)seen; return 0;
}

/* user_paging.c takes the address of the asm syscall stub, clones the
 * boot p4_table's kernel slots, and checks image-end for frame refusal —
 * all link-provided symbols in the real kernel; provide inert stand-ins
 * (their values are never used: the host test drives user_map_page /
 * user_map_identity directly). */
void syscall_entry_stub(void) { }
uint64_t p4_table[512] = { 0 };
char _kernel_image_end[1];

/* user_paging.c's frame-refusal path consults the cap arena and the frame
 * pool (kernel/cap.c + frame_pool.c, not linked here). Permissive answers:
 * nothing is in the arena, every frame is owned by partition 0, and frees
 * no-op — the host test only exercises the map/unmap arithmetic. */
int cap_frame_in_arena(uint64_t paddr) { (void)paddr; return 0; }
uint32_t frame_pool_frame_owner(uint64_t frame_index) { (void)frame_index; return 0; }
int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id) {
    (void)frame; (void)partition_id; return 0;
}


#define FRAME_MASK 0x000FFFFFFFFFF000ULL
#define HUGEPAGE   (0x83ULL)   /* present | write | PS — supervisor */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* Build a fake "kernel" identity map: PML4[0] → PDPT[0] → PD with 512
 * supervisor 2 MiB huge pages covering 0 – 1 GiB (the boot.asm shape).
 *
 * Tables are 4096-aligned (like allocate_physical_ram_frame in the kernel):
 * entry values are (table_addr | flags), read back through FRAME_MASK, so a
 * misaligned buffer would mask to a different address (the QEMU bug class
 * this test guards against — but with heap memory, not physical). */
static uint64_t* alloc_table(void) {
    uint64_t* t = aligned_alloc(4096, 4096);
    if (t) memset(t, 0, 4096);
    return t;
}

static uint64_t* build_kernel_map(void) {
    uint64_t* pml4 = alloc_table();
    uint64_t* pdpt = alloc_table();
    uint64_t* pd   = alloc_table();
    pml4[0] = (uint64_t)(uintptr_t)pdpt | 0x3ULL;   /* supervisor */
    pdpt[0] = (uint64_t)(uintptr_t)pd   | 0x3ULL;
    for (int i = 0; i < 512; i++)
        pd[i] = ((uint64_t)i << 21) | HUGEPAGE;      /* 2 MiB huge pages */
    return pml4;
}

int main(void) {
    uint64_t* k_pml4 = build_kernel_map();
    /* The kernel's PD pointer: pml4[0] → pdpt → pd[0]. */
    uint64_t* k_pdpt = (uint64_t*)(uintptr_t)(k_pml4[0] & FRAME_MASK);
    uint64_t* k_pd = (uint64_t*)(uintptr_t)(k_pdpt[0] & FRAME_MASK);

    /* Simulate user_clone_page_table: a fresh PML4 sharing the kernel's
     * supervisor slots by pointer, user slots zero. */
    uint64_t* child = alloc_table();
    child[0] = k_pml4[0];   /* clone_kernel_slots shares non-user entries */

    /* The DM budget cap: 64 pages (256 KiB) at 0x21019000, chunk PD[264]. */
    uint64_t phys = 0x21019000ULL;
    uint32_t npages = 64;
    uint64_t flags = USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_NOEXEC
                   | USER_PTE_WRITE;
    int r = user_map_identity(child, phys, npages, flags);
    CHECK(r == 0, "user_map_identity succeeds");

    /* 1. The child's PML4[0] is now a FRESH (user) PDPT, not the kernel's. */
    CHECK((child[0] & USER_PTE_PRESENT) && (child[0] & USER_PTE_USER),
          "child PML4[0] is a fresh USER PDPT");
    CHECK((child[0] & FRAME_MASK) != (k_pml4[0] & FRAME_MASK),
          "child PML4[0] no longer shares the kernel PDPT by pointer");

    /* 2. The fresh PDPT still shares the kernel's PDs for untouched GiBs. */
    uint64_t* c_pdpt = (uint64_t*)(uintptr_t)(child[0] & FRAME_MASK);
    CHECK((c_pdpt[1] & FRAME_MASK) == (k_pdpt[1] & FRAME_MASK),
          "untouched PDPT entry still shares the kernel's PD");

    /* 3. The fresh PD[0] (kernel code chunk) is UNCHANGED — still the
     *    supervisor huge page with the same frame. This is the QEMU fault:
     *    a missing PD[0] faults the fetch right after the CR3 switch into
     *    the child (schedule_ring3). */
    uint64_t* c_pd = (uint64_t*)(uintptr_t)(c_pdpt[0] & FRAME_MASK);
    CHECK(c_pd[0] == k_pd[0] && (c_pd[0] & USER_PTE_PRESENT) && !(c_pd[0] & USER_PTE_USER),
          "kernel-code chunk PD[0] still present, supervisor, same frame");
    CHECK((c_pdpt[0] & FRAME_MASK) != (k_pdpt[0] & FRAME_MASK),
          "touched PDPT[0] is a fresh PD (untouched GiB sharing preserved)");

    /* 4. Chunk 264 (the cap's 2 MiB chunk) is a fresh USER PT. */
    uint64_t e264 = c_pd[264];
    CHECK((e264 & USER_PTE_PRESENT) && (e264 & USER_PTE_USER),
          "cap chunk PD[264] is a fresh USER page table");
    CHECK(!(e264 & (1ULL << 7)), "cap chunk PD[264] no longer a huge page");

    /* 5. The chunk's OTHER pages replicate the huge page (same frames,
     *    supervisor flags) so the kernel keeps full visibility. */
    uint64_t* pt = (uint64_t*)(uintptr_t)(e264 & FRAME_MASK);
    CHECK((pt[0] & FRAME_MASK) == (0x21000000ULL) &&
          (pt[0] & 0x83ULL) == 0x3ULL,
          "chunk slot 0 replicates the huge page frame, supervisor");
    CHECK((pt[1] & FRAME_MASK) == (0x21000000ULL + 4096),
          "chunk slot 1 replicates the next frame");

    /* 6. The cap page itself is Ring-3 mapped at its physical address. */
    uint32_t ti = (uint32_t)((phys >> 12) & 0x1FF);
    uint64_t cap_pte = pt[ti];
    CHECK((cap_pte & FRAME_MASK) == (phys & FRAME_MASK),
          "cap page maps the physical frame at the same address");
    CHECK((cap_pte & USER_PTE_PRESENT) && (cap_pte & USER_PTE_USER) &&
          (cap_pte & USER_PTE_WRITE) && (cap_pte & USER_PTE_NOEXEC),
          "cap page has PRESENT|USER|WRITE|NOEXEC");

    /* 7. The kernel's own tables are untouched. */
    CHECK(k_pml4[0] == ((uint64_t)(uintptr_t)k_pdpt | 0x3ULL),
          "kernel PML4[0] unchanged");
    CHECK(k_pd[0] == (0x0ULL | HUGEPAGE), "kernel PD[0] still the huge page");
    CHECK(k_pd[264] == (0x21000000ULL | HUGEPAGE),
          "kernel PD[264] still the huge page");

    /* 8. A second cap in a DIFFERENT chunk also works (registry, chunk 264
     *    covers it too — use a second chunk to exercise the fresh-PT reuse:
     *    another page in the same chunk reuses the PT). */
    uint64_t phys2 = 0x21019000ULL + 2 * 4096;   /* same chunk, 2 pages in */
    int r2 = user_map_identity(child, phys2, 1, USER_PTE_PRESENT | USER_PTE_USER);
    CHECK(r2 == 0, "second identity map in the same chunk succeeds");
    uint64_t* pt2 = (uint64_t*)(uintptr_t)(c_pd[264] & FRAME_MASK);
    uint32_t ti2 = (uint32_t)((phys2 >> 12) & 0x1FF);
    CHECK((pt2[ti2] & USER_PTE_PRESENT) && (pt2[ti2] & USER_PTE_USER),
          "second page user-mapped, same PT reused");

    if (g_fail == 0) printf("\nALL PASS\n");
    else             printf("\n%d FAILURE(S)\n", g_fail);
    return g_fail != 0;
}
