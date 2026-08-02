/*
 * qemu_sls_mmu.h — Phase 1: host-MMU shadow page tables for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 1.
 *
 * Steps covered:
 *   1.1  Contiguous GPA→HVA mapping  (qemu_sls_mmu_map_guest_ram)
 *   1.2  Shadow page-table build     (qemu_sls_mmu_shadow_fault, guest PT walk)
 *   1.4  Fault-handler hook          (called from kernel/stubs.c handle_page_fault)
 *
 * Step 1.3 (TCG backend: emit direct MOV instead of softmmu helpers) lives in
 * the QEMU-SLS TCG backend patch, not here.
 */
#ifndef QEMU_SLS_MMU_H
#define QEMU_SLS_MMU_H

#include <stdint.h>

/*
 * Guest physical address space is mapped at this fixed offset in the host VA
 * space.  GPA X → host VA (QEMU_GPA_HOST_BASE + X).
 * Placed at 2 TiB — above the kernel's identity-mapped lower 4 GiB, below
 * the 4-level-paging canonical-address hole at 128 TiB.
 */
#define QEMU_GPA_HOST_BASE   0x0000200000000000ULL

/* Maximum guest RAM in 4 KiB pages (256 MiB). */
#define QEMU_GUEST_RAM_PAGES 65536U

#define QEMU_MAX_REGIONS 8

typedef enum { QEMU_REGION_RAM = 0, QEMU_REGION_MMIO = 1 } QemuRegionType;

typedef struct {
    uint64_t       gpa_start;  /* inclusive */
    uint64_t       gpa_end;    /* exclusive */
    uint64_t       hva_base;   /* host VA for gpa == gpa_start */
    QemuRegionType type;
    int            active;
} QemuGuestRegion;

/* Written by TCG vCPU on every emulated MOV to CR3. */
extern uint64_t qemu_sls_guest_cr3;

/* Physical address of the shadow PML4; load into CR3 before running TCG code. */
extern uint64_t qemu_sls_shadow_cr3;

/* One-time boot init: allocates shadow PML4 inheriting all kernel PT entries. */
int qemu_sls_mmu_init(void);

/*
 * Allocate physical frames for [gpa_start, gpa_start + size_pages*4096) and
 * map them in the shadow PT at host VA (QEMU_GPA_HOST_BASE + gpa_start).
 * Returns 0 on success, -1 on OOM or range overflow.
 */
int qemu_sls_mmu_map_guest_ram(uint64_t gpa_start, uint32_t size_pages);

/*
 * Called from handle_page_fault() for kernel-mode faults in the guest VA space.
 * Walks qemu_sls_guest_cr3's page table, installs the shadow PTE, and returns 0
 * (resolved → isr14_stub iretq re-executes the faulting instruction).
 * Returns 1 when the guest itself has no mapping (caller should inject guest #PF).
 */
int qemu_sls_mmu_shadow_fault(uint64_t faulting_gva, uint32_t error_code);

/* Returns the guest-region record whose HVA range contains hva, or NULL. */
const QemuGuestRegion *qemu_sls_mmu_find_region(uint64_t hva);

/* Zero-copy DMA (Phase 4): map guest physical address to host pointers. */
void    *qemu_sls_dma_host_ptr(uint64_t gpa);    /* host VA for direct R/W */
uint64_t qemu_sls_dma_frame_phys(uint64_t gpa);  /* physical frame for DMA hardware */

#endif /* QEMU_SLS_MMU_H */
