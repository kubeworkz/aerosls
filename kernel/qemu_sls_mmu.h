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
 *
 * Placed at 32 TiB — above the kernel's identity-mapped lower 4 GiB, below the
 * 4-level-paging canonical-address hole that starts at 128 TiB. (This comment
 * said "2 TiB" until it was checked: 0x200000000000 is 2^45, which is 32 TiB,
 * not 2. Both are inside the safe span so nothing was broken, but it is the
 * number the TCG backend patch reasons about.)
 *
 * This window is how the EMULATOR reaches guest memory — reading guest page
 * tables, DMA, image load. It is NOT how translated guest code addresses
 * memory: the shadow PT maps guest virtual addresses directly (see
 * shadow_install() in the .c), so emitted loads and stores use a bare GVA with
 * no base register at all.
 *
 * Overridable so a host test can point the window at a real buffer; the
 * production value is asserted separately in tests/qemu_sls_mmu_host_test.c.
 */
#ifndef QEMU_GPA_HOST_BASE
#define QEMU_GPA_HOST_BASE   0x0000200000000000ULL
#endif

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

/* Nonzero only while translated guest code is executing. The launcher sets it
 * around sls_exec_run(). qemu_sls_mmu_shadow_fault() refuses to resolve
 * anything when it is clear: the shadow table maps guest VIRTUAL addresses, so
 * nothing about a faulting address distinguishes a guest access from a kernel
 * bug, and "resolving" a kernel bug turns a diagnosable halt into an infinite
 * fault loop. */
extern int qemu_sls_guest_active;

/* invlpg on one page. A function, not inline asm at the call site, so a host
 * test can observe the invalidation instead of executing a privileged
 * instruction -- same reason as arch_read_cr3(). */
void qemu_sls_invlpg(uint64_t va);

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
