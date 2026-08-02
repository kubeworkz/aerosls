/*
 * qemu_sls_mmu.c — Phase 1 host-MMU shadow page tables for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 1.
 */

#include "qemu_sls_mmu.h"
#include "frame_pool.h"
#include "kernel_io.h"
#include "../arch/x86/user_paging.h"
#include <stddef.h>

#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)

#define HUGE_1G_FLAG (1ULL << 7)
#define HUGE_2M_FLAG (1ULL << 7)

uint64_t qemu_sls_guest_cr3  = 0;
uint64_t qemu_sls_shadow_cr3 = 0;

static uint64_t        *shadow_pml4;
static QemuGuestRegion  regions[QEMU_MAX_REGIONS];
static int              region_count;
static int              initialized;

/* Physical frame address for each guest 4 KiB page; 0 = unallocated. */
static uint64_t guest_ram_frames[QEMU_GUEST_RAM_PAGES];

/* ─── qemu_sls_mmu_init ──────────────────────────────────────────────────── */

int qemu_sls_mmu_init(void) {
    shadow_pml4 = (uint64_t *)allocate_physical_ram_frame();
    if (!shadow_pml4) {
        kernel_serial_print("[QEMU-SLS MMU] init: OOM allocating shadow PML4\n");
        return -1;
    }

    /* Copy kernel PT entries so IDT/syscall handlers stay reachable when
     * TCG code runs with this PML4 loaded as CR3. */
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    const uint64_t *kernel_pml4 =
        (const uint64_t *)(uintptr_t)(current_cr3 & USER_PTE_FRAME_MASK);
    for (int i = 0; i < 512; i++) shadow_pml4[i] = kernel_pml4[i];

    qemu_sls_shadow_cr3 = (uint64_t)(uintptr_t)shadow_pml4;
    initialized = 1;

    kernel_serial_printf(
        "[QEMU-SLS MMU] shadow PML4=0x%016lx (kernel PT inherited)\n",
        qemu_sls_shadow_cr3);
    return 0;
}

/* ─── qemu_sls_mmu_map_guest_ram ─────────────────────────────────────────── */

int qemu_sls_mmu_map_guest_ram(uint64_t gpa_start, uint32_t size_pages) {
    if (!initialized || region_count >= QEMU_MAX_REGIONS) return -1;
    if (size_pages == 0) return -1;

    uint32_t first_idx = (uint32_t)(gpa_start / FRAME_SIZE);
    if ((uint64_t)first_idx + size_pages > QEMU_GUEST_RAM_PAGES) return -1;

    uint64_t hva_base = QEMU_GPA_HOST_BASE + gpa_start;

    for (uint32_t i = 0; i < size_pages; i++) {
        void *frame = allocate_physical_ram_frame();
        if (!frame) {
            kernel_serial_printf(
                "[QEMU-SLS MMU] map_guest_ram: OOM at page %u/%u\n", i, size_pages);
            return -1;
        }
        guest_ram_frames[first_idx + i] = (uint64_t)(uintptr_t)frame;
        /* Step 1.1: direct GPA access path — QEMU_GPA_HOST_BASE + GPA → frame. */
        user_map_page(shadow_pml4,
                      hva_base + (uint64_t)i * FRAME_SIZE,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_WRITE);
    }

    QemuGuestRegion *r = &regions[region_count++];
    r->gpa_start = gpa_start;
    r->gpa_end   = gpa_start + (uint64_t)size_pages * FRAME_SIZE;
    r->hva_base  = hva_base;
    r->type      = QEMU_REGION_RAM;
    r->active    = 1;

    kernel_serial_printf(
        "[QEMU-SLS MMU] guest RAM GPA 0x%016lx..0x%016lx → HVA 0x%016lx (%u pages)\n",
        gpa_start, r->gpa_end, hva_base, size_pages);
    return 0;
}

/* ─── qemu_sls_mmu_find_region ──────────────────────────────────────────── */

const QemuGuestRegion *qemu_sls_mmu_find_region(uint64_t hva) {
    for (int i = 0; i < region_count; i++) {
        if (!regions[i].active) continue;
        uint64_t span = regions[i].gpa_end - regions[i].gpa_start;
        if (hva >= regions[i].hva_base && hva < regions[i].hva_base + span)
            return &regions[i];
    }
    return NULL;
}

/* ─── guest PT walk helpers ─────────────────────────────────────────────── */

/* Translate guest physical address to a readable host pointer via the direct
 * GPA map.  Returns NULL if the GPA is unmapped (OOB or not yet allocated). */
static const uint64_t *gpa_to_hva(uint64_t gpa) {
    uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
    if (idx >= QEMU_GUEST_RAM_PAGES || !guest_ram_frames[idx]) return NULL;
    return (const uint64_t *)(QEMU_GPA_HOST_BASE + gpa);
}

/* Install one shadow PTE: gva → frame, W-bit propagated from the guest PTE. */
static void shadow_install(uint64_t gva, uint64_t frame, uint64_t guest_pte) {
    uint64_t flags = USER_PTE_PRESENT;
    if (guest_pte & USER_PTE_WRITE) flags |= USER_PTE_WRITE;
    user_map_page(shadow_pml4, gva & ~(uint64_t)0xFFF, frame, flags);
}

/* ─── qemu_sls_mmu_shadow_fault ─────────────────────────────────────────── */

/*
 * Step 1.2 + 1.4: called when TCG code takes a #PF in guest VA space.
 * Walks the guest page table rooted at qemu_sls_guest_cr3, installs the
 * GVA → physical-frame mapping in the shadow PT, and returns 0 so the
 * isr14_stub iretq re-executes the faulting instruction.
 *
 * Huge-page guest mappings (1 GiB, 2 MiB) are decomposed into 4 KiB shadow
 * PTEs — one per fault — so we only pay for pages the guest actually touches.
 */
int qemu_sls_mmu_shadow_fault(uint64_t faulting_gva, uint32_t error_code) {
    if (!initialized) return 1;
    (void)error_code;

    uint64_t cr3_gpa = qemu_sls_guest_cr3 & ~(uint64_t)0xFFF;
    const uint64_t *pml4 = gpa_to_hva(cr3_gpa);
    if (!pml4) return 1;

    uint64_t e3 = pml4[PML4_IDX(faulting_gva)];
    if (!(e3 & USER_PTE_PRESENT)) return 1;

    const uint64_t *pdpt = gpa_to_hva(e3 & USER_PTE_FRAME_MASK);
    if (!pdpt) return 1;
    uint64_t e2 = pdpt[PDPT_IDX(faulting_gva)];
    if (!(e2 & USER_PTE_PRESENT)) return 1;

    if (e2 & HUGE_1G_FLAG) {
        /* 1 GiB page: GPA = upper bits of e2 | lower 30 bits of GVA. */
        uint64_t gpa = (e2 & ~(uint64_t)0x3FFFFFFF) |
                       (faulting_gva & (uint64_t)0x3FFFFFFF);
        uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
        if (idx >= QEMU_GUEST_RAM_PAGES || !guest_ram_frames[idx]) return 1;
        shadow_install(faulting_gva, guest_ram_frames[idx], e2);
        return 0;
    }

    const uint64_t *pd = gpa_to_hva(e2 & USER_PTE_FRAME_MASK);
    if (!pd) return 1;
    uint64_t e1 = pd[PD_IDX(faulting_gva)];
    if (!(e1 & USER_PTE_PRESENT)) return 1;

    if (e1 & HUGE_2M_FLAG) {
        /* 2 MiB page: GPA = upper bits of e1 | lower 21 bits of GVA. */
        uint64_t gpa = (e1 & ~(uint64_t)0x1FFFFF) |
                       (faulting_gva & (uint64_t)0x1FFFFF);
        uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
        if (idx >= QEMU_GUEST_RAM_PAGES || !guest_ram_frames[idx]) return 1;
        shadow_install(faulting_gva, guest_ram_frames[idx], e1);
        return 0;
    }

    const uint64_t *pt = gpa_to_hva(e1 & USER_PTE_FRAME_MASK);
    if (!pt) return 1;
    uint64_t leaf = pt[PT_IDX(faulting_gva)];
    if (!(leaf & USER_PTE_PRESENT)) return 1;

    uint64_t gpa = leaf & USER_PTE_FRAME_MASK;
    uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
    if (idx >= QEMU_GUEST_RAM_PAGES || !guest_ram_frames[idx]) return 1;

    shadow_install(faulting_gva, guest_ram_frames[idx], leaf);
    return 0;
}

/* ─── zero-copy DMA (Phase 4) ─────────────────────────────────────────────── */

void *qemu_sls_dma_host_ptr(uint64_t gpa) {
    if (!qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE + gpa)) return NULL;
    return (void *)(QEMU_GPA_HOST_BASE + gpa);
}

uint64_t qemu_sls_dma_frame_phys(uint64_t gpa) {
    uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
    if (idx >= QEMU_GUEST_RAM_PAGES) return 0;
    return guest_ram_frames[idx];
}
