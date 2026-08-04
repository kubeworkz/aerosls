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
int      qemu_sls_guest_active = 0;

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
    uint64_t current_cr3 = arch_read_cr3();
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

    /* Refuse a range that overlaps one already backed. Silently re-mapping
     * would orphan the previous frames -- they would stay marked allocated in
     * the frame pool with nothing left pointing at them, and the guest's old
     * contents would vanish under it. */
    for (uint32_t i = 0; i < size_pages; i++) {
        if (guest_ram_frames[first_idx + i]) {
            kernel_serial_printf(
                "[QEMU-SLS MMU] map_guest_ram REFUSED: GPA 0x%016lx is already backed "
                "by frame 0x%016lx. Re-mapping it would orphan that frame and lose "
                "whatever the guest had there.\n",
                gpa_start + (uint64_t)i * FRAME_SIZE,
                guest_ram_frames[first_idx + i]);
            return -1;
        }
    }

    /* ─── Allocate everything BEFORE mapping anything ──────────────────────
     * The original ordering allocated-and-mapped in one pass and returned -1
     * on OOM partway through. That left the frames it had already taken both
     * allocated and installed in the shadow PT, recorded in
     * guest_ram_frames[], and yet registered in NO region -- so
     * qemu_sls_dma_host_ptr() returned NULL for pages that were mapped and
     * live, the frames were unreclaimable, and the caller saw a clean failure.
     *
     * Two passes make the failure atomic. Nothing is mapped until every frame
     * is in hand, and a short allocation unwinds completely. */
    for (uint32_t i = 0; i < size_pages; i++) {
        void *frame = allocate_physical_ram_frame();
        if (!frame) {
            kernel_serial_printf(
                "[QEMU-SLS MMU] map_guest_ram: OOM at page %u/%u -- rolling back "
                "%u frame(s); nothing mapped, nothing registered.\n",
                i, size_pages, i);
            while (i-- > 0) {
                free_physical_ram_frame(
                    (void *)(uintptr_t)guest_ram_frames[first_idx + i]);
                guest_ram_frames[first_idx + i] = 0;
            }
            return -1;
        }
        guest_ram_frames[first_idx + i] = (uint64_t)(uintptr_t)frame;
    }

    /* Step 1.1: direct GPA access path — QEMU_GPA_HOST_BASE + GPA → frame. */
    for (uint32_t i = 0; i < size_pages; i++) {
        user_map_page(shadow_pml4,
                      hva_base + (uint64_t)i * FRAME_SIZE,
                      guest_ram_frames[first_idx + i],
                      USER_PTE_PRESENT | USER_PTE_WRITE);
    }

    /* ─── The window must also exist in the KERNEL address space ───────────
     * user_map_page() above installed everything in shadow_pml4. But the
     * shadow root is only loaded into CR3 while translated guest code runs --
     * the EMULATOR reaches guest memory through this same window while running
     * as ordinary kernel code on the kernel's CR3, which is exactly what this
     * header promises:
     *
     *     "This window is how the EMULATOR reaches guest memory -- reading
     *      guest page tables, DMA, image load."          (qemu_sls_mmu.h)
     *
     * qemu_sls_mmu_init() copies kernel_pml4 INTO the shadow, and that copy is
     * one-directional and happens BEFORE this function builds the window. So
     * the kernel PML4 has no entry for it, and the first emulator-side touch
     * of the window takes a #PF at 0x0000200000001000 with error=0x2 that
     * qemu_sls_mmu_shadow_fault() then correctly refuses -- correctly, because
     * qemu_sls_guest_active is 0 and no guest is running. Every guard behaved
     * exactly as designed while the node halted on the benchmark's first
     * buffer write.
     *
     * Copying the covering PML4 entries makes both roots share the SAME
     * PDPT/PD/PT below them, so they cannot drift: a later map_guest_ram()
     * that adds pages under an existing entry is visible from both without
     * any further work. It is the same trick init() already uses in the other
     * direction, applied to the one range that init() could not know about.
     *
     * No TLB flush: these entries were not-present, and x86 does not cache
     * non-present translations. */
    {
        uint64_t *kernel_pml4 =
            (uint64_t *)(uintptr_t)(arch_read_cr3() & ~0xFFFULL);
        uint64_t  win_end = hva_base + (uint64_t)size_pages * FRAME_SIZE - 1;
        unsigned  first   = (unsigned)((hva_base >> 39) & 0x1FF);
        unsigned  last    = (unsigned)((win_end  >> 39) & 0x1FF);

        /* A region spanning more than the 512 PML4 slots would wrap the index
         * and copy the WRONG entries -- silently, and only for large guests.
         * QEMU_GUEST_RAM_PAGES is 256 MiB today so this cannot trigger, which
         * is precisely why it is checked rather than assumed. */
        if (last < first) {
            kernel_serial_printf(
                "[QEMU-SLS MMU] window PML4 range wrapped (%u..%u) -- refusing to "
                "publish it to the kernel root.\n", first, last);
            return -1;
        }
        for (unsigned i = first; i <= last; i++) {
            if (kernel_pml4[i] != shadow_pml4[i]) kernel_pml4[i] = shadow_pml4[i];
        }
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

/* ─── The shadow shares page tables with the kernel ────────────────────────
 *
 * qemu_sls_mmu_init() copies all 512 kernel PML4 entries BY VALUE, so every
 * lower-level table is shared, not cloned. user_map_page() follows a present
 * entry rather than cloning it (arch/x86/user_paging.c:110, get_or_alloc).
 *
 * Therefore installing a mapping at an address the KERNEL also maps does not
 * shadow the kernel's translation -- it OVERWRITES it, in the kernel's own
 * live page tables, permanently, whether or not the shadow root is loaded. A
 * mapping at a low address would remap the kernel image (1..221 MiB) out from
 * under the CPU currently executing it.
 *
 * The window at QEMU_GPA_HOST_BASE occupies a PML4 slot nothing else uses,
 * which is why sharing is safe there and why map_guest_ram() can publish that
 * one entry into the kernel root. The constraint has always held; it was
 * never written down, was load-bearing for one caller and fatal to another.
 *
 * See docs/AeroSLS-QEMU-SLS-Guest-Address-Space-Design-v0.1.md.
 */
#define SHADOW_PML4_SLOT(va) (unsigned)(((va) >> 39) & 0x1FF)

static int shadow_va_in_window(uint64_t va) {
    return SHADOW_PML4_SLOT(va) == SHADOW_PML4_SLOT(QEMU_GPA_HOST_BASE);
}

/* Install one shadow PTE: host VA → frame, W-bit propagated from the guest PTE.
 *
 * Returns 0 on success, -1 if refused. The caller must treat a refusal as an
 * unresolved fault -- silently not installing would loop the fault handler
 * forever on the same address. */
static int shadow_install(uint64_t gva, uint64_t frame, uint64_t guest_pte) {
    if (!shadow_va_in_window(gva)) {
        kernel_serial_printf(
            "[QEMU-SLS MMU] shadow_install REFUSED: 0x%016lx is in PML4 slot %u, "
            "outside the guest window's slot %u.\n"
            "[QEMU-SLS MMU] The shadow SHARES lower-level tables with the kernel, so "
            "mapping here would overwrite the kernel's own page tables rather than "
            "shadow them.\n"
            "[QEMU-SLS MMU] Under the window model the faulting address is already "
            "guest_va + QEMU_GPA_HOST_BASE; a raw guest VA reaching here means the "
            "caller still assumes the retired GVA-direct design.\n"
            "[QEMU-SLS MMU] See docs/AeroSLS-QEMU-SLS-Guest-Address-Space-Design-v0.1.md\n",
            gva, SHADOW_PML4_SLOT(gva), SHADOW_PML4_SLOT(QEMU_GPA_HOST_BASE));
        return -1;
    }

    uint64_t flags = USER_PTE_PRESENT;
    if (guest_pte & USER_PTE_WRITE) flags |= USER_PTE_WRITE;
    user_map_page(shadow_pml4, gva & ~(uint64_t)0xFFF, frame, flags);

    /* user_map_page() does not invalidate, and iretq re-executes the faulting
     * instruction immediately. For a NOT-PRESENT fault that is harmless: x86
     * does not cache non-present translations, so the retry walks the table we
     * just wrote. For a PERMISSION fault it is not: the read-only entry IS in
     * the TLB, the new writable PTE is invisible to it, the write faults again,
     * and the handler installs the same PTE forever. A guest page that starts
     * read-only and later becomes writable -- copy-on-write, a loader marking
     * .data, any normal OS behaviour -- hits exactly that.
     *
     * One instruction on a path that has already taken a page fault. */
    qemu_sls_invlpg(gva & ~(uint64_t)0xFFF);
    return 0;
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
int qemu_sls_mmu_shadow_fault(uint64_t faulting_addr, uint32_t error_code) {
    if (!initialized) return 1;
    (void)error_code;

    /* ─── The faulting address is a HOST address, not a guest one ──────────
     * Emitted guest code addresses memory as `guest_va + guest_base`, where
     * guest_base is QEMU_GPA_HOST_BASE (see tcg/tcg.c and the note in
     * qemu_sls_mmu.h). CR2 therefore reports guest_va + the window base, and
     * the guest virtual address has to be recovered before anything can walk
     * the guest's page tables with it.
     *
     * This function previously treated CR2 as a raw guest VA. That was correct
     * for the GVA-direct design and is wrong under the window model that
     * shipped in Step 5 -- it would walk the guest's tables with an address
     * 32 TiB too high, miss, and report an unresolved fault for a page the
     * guest had legitimately mapped.
     *
     * Checking the window FIRST also gives back the address-range test that
     * the comment below says does not exist. It does now: an access outside
     * the window is definitionally not translated guest code touching guest
     * memory. qemu_sls_guest_active remains the primary guard because a kernel
     * bug CAN produce an address inside the window -- the emulator itself uses
     * that range -- but the two together are considerably stronger than either.
     */
    if (!shadow_va_in_window(faulting_addr)) return 1;
    uint64_t faulting_gva = faulting_addr - QEMU_GPA_HOST_BASE;

    /* ─── Only ever resolve faults taken BY guest code ─────────────────────
     * handle_page_fault() calls this for every kernel-mode #PF, at any
     * address. The shadow table maps guest VIRTUAL addresses directly, so
     * there is no address range that separates "a guest access" from "the
     * kernel dereferencing a bad pointer" -- the two spaces overlap by
     * construction.
     *
     * Without this flag, once a guest's RAM and page tables exist, an
     * unrelated kernel fault gets walked against the guest's tables. If the
     * guest happens to map that address, this installs a PTE into the shadow
     * PML4 -- which is NOT the live CR3 when the kernel is running -- and
     * returns 0. iretq then re-executes the faulting instruction, it faults
     * again, and the kernel spins in the fault handler forever instead of
     * halting with the diagnosable [FAULT] line that says what went wrong.
     *
     * The launcher sets this around sls_exec_run() and clears it on exit, so
     * the answer is "is guest code on the stack right now", which is the
     * actual question. */
    if (!qemu_sls_guest_active) return 1;

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
        if (shadow_install(faulting_addr, guest_ram_frames[idx], e2) != 0) return 1;
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
        if (shadow_install(faulting_addr, guest_ram_frames[idx], e1) != 0) return 1;
        return 0;
    }

    const uint64_t *pt = gpa_to_hva(e1 & USER_PTE_FRAME_MASK);
    if (!pt) return 1;
    uint64_t leaf = pt[PT_IDX(faulting_gva)];
    if (!(leaf & USER_PTE_PRESENT)) return 1;

    uint64_t gpa = leaf & USER_PTE_FRAME_MASK;
    uint32_t idx = (uint32_t)(gpa / FRAME_SIZE);
    if (idx >= QEMU_GUEST_RAM_PAGES || !guest_ram_frames[idx]) return 1;

    if (shadow_install(faulting_addr, guest_ram_frames[idx], leaf) != 0) return 1;
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
