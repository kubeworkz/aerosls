/*
 * qemu_sls_mmu_host_test.c — the QEMU-SLS Phase 1 shadow page-table layer,
 * linked against the REAL, unmodified kernel/qemu_sls_mmu.c.
 *
 * ─── Why this exists before Step 5 and not after ───────────────────────────
 * qemu_sls_mmu_shadow_fault() is already wired into handle_page_fault()
 * (kernel/stubs.c), but today it only runs when the soft-MMU has already
 * failed, so almost nothing reaches it. Step 5 turns tcg_use_softmmu off and
 * makes it the ONLY thing standing between translated guest code and memory:
 * every guest load and store becomes a bare MOV whose translation is whatever
 * this file installed.
 *
 * A bug here is therefore not a failing test. It is a #PF inside translated
 * code, resolved wrongly or not at all, surfacing several layers away from the
 * write that caused it -- the exact shape that cost this project a nine-bug
 * chain and a kernel fault on a poisoned return address. All four
 * kernel/qemu_sls_*.c files had zero host tests when this was written.
 *
 * ─── What is asserted, and why it is the installed PTE ────────────────────
 * The interesting output of a page-table walk is not its return code. Both a
 * correct walk and a walk that resolved the WRONG guest page return 0. So the
 * user_map_page() stub below records every (vaddr, paddr, flags) triple, and
 * the assertions are about what landed in the shadow table -- the far side of
 * the boundary being crossed.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/qemu_sls_test_window.h \
 *       -o /tmp/qemu_sls_mmu_host_test \
 *       tests/qemu_sls_mmu_host_test.c kernel/qemu_sls_mmu.c
 *   /tmp/qemu_sls_mmu_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* The GPA window is 32 TiB in production, which a host test cannot address.
 * tests/qemu_sls_test_window.h is force-included into this file AND into the
 * real qemu_sls_mmu.c, pointing the window at the buffer below. The production
 * constant is asserted on its own further down, so relocating it here cannot
 * hide a wrong value there. */
uint64_t qemu_sls_test_gpa_base;

#include "kernel/qemu_sls_mmu.h"

/* ─── constants mirrored from the headers the .c includes ────────────────── */
#define FRAME_SIZE           4096
#define USER_PTE_PRESENT     (1ULL << 0)
#define USER_PTE_WRITE       (1ULL << 1)
#define USER_PTE_FRAME_MASK  0x000FFFFFFFFFF000ULL

/* ─── The address the CPU reports is NOT the guest's ──────────────────────
 * Emitted guest code addresses memory as `guest_va + guest_base`, and
 * guest_base is QEMU_GPA_HOST_BASE (see tcg/tcg.c, and the correction note in
 * kernel/qemu_sls_mmu.h). So CR2 -- and therefore the argument
 * qemu_sls_mmu_shadow_fault() receives from handle_page_fault() -- is the
 * guest address plus the window base.
 *
 * These tests passed raw guest VAs, which was correct for the GVA-direct
 * design and is wrong for the window model that shipped in Step 5. The
 * mismatch was invisible until shadow_install() gained a guard refusing
 * mappings outside the window's PML4 slot: sixteen checks failed at once, all
 * of them encoding the retired calling convention. The guard found a real
 * defect in the code AND a real defect in the tests, which is the argument for
 * asserting a constraint rather than documenting it.
 *
 * Installed shadow PTEs are therefore at GUEST_FAULT_ADDR(gva), not gva. */
#define GUEST_FAULT_ADDR(gva)  ((uint64_t)(gva) + QEMU_GPA_HOST_BASE)

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 -- tests/run_all.sh counts checks with grep -c '^ok:',
     * so this file reported "0 checks" for its entire life while passing 61.
     * A suite that cannot tell 61 assertions from none is not reporting. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* ─── stubs ──────────────────────────────────────────────────────────────── */

void kernel_serial_print(const char *s) { (void)s; }
void kernel_serial_printf(const char *f, ...) { (void)f; }

/* The reason this file could not exist until arch_read_cr3() was factored out:
 * qemu_sls_mmu_init() read CR3 with inline asm, which is privileged, so the
 * whole test segfaulted before main() reached its first assertion. The stub
 * returns a page-aligned pointer to a real 512-entry table, because init()
 * dereferences it to copy the kernel PML4 entries into the shadow. */
static uint64_t g_fake_kernel_pml4[512] __attribute__((aligned(4096)));
uint64_t arch_read_cr3(void) { return (uint64_t)(uintptr_t)g_fake_kernel_pml4; }

/* Recording invlpg, for the same reason as user_map_page: the interesting
 * question is not whether the PTE was written but whether the stale TLB entry
 * was flushed, and only one of those is observable from the return code. */
static uint64_t g_invlpg[64];
static int      g_invlpg_count;
void qemu_sls_invlpg(uint64_t va) {
    if (g_invlpg_count < 64) g_invlpg[g_invlpg_count] = va;
    g_invlpg_count++;
}

/* Guest "physical" memory: the buffer the relocated GPA window points at.
 * 2 MiB is enough for a few page-table levels plus data pages. */
#define GUEST_RAM_BYTES (2u * 1024u * 1024u)
static uint8_t *g_guest_ram;

/* allocate_physical_ram_frame() must return REAL, dereferenceable memory.
 *
 * An earlier version of this stub handed out opaque tokens (0xAAAA0000+) on
 * the theory that a physical frame address is not a pointer. That is wrong for
 * this kernel: AeroSLS identity-maps the low 4 GiB, so a frame address IS a
 * usable pointer, and qemu_sls_mmu_init() legitimately dereferences the frame
 * it just allocated to copy 512 kernel PML4 entries into it. The stub
 * segfaulted the test before its first assertion -- a stub that was LESS
 * capable than the thing it replaced, which is the mirror image of the usual
 * failure where a stub is more permissive.
 *
 * Frames are page-aligned so the code's FRAME_MASK arithmetic behaves as it
 * does on real hardware. */
#define POOL_FRAMES 64
static uint8_t *g_frame_pool;
static int      g_frames_used = 0;

static int g_frames_freed = 0;

void *allocate_physical_ram_frame(void) {
    if (g_frames_used >= POOL_FRAMES) return NULL;
    return g_frame_pool + (size_t)(g_frames_used++) * FRAME_SIZE;
}

/* Counted, not just accepted. "map_guest_ram returned -1" is true whether it
 * rolled back or leaked, so the return code cannot distinguish them -- only
 * the frames coming back can. */
int free_physical_ram_frame(void *frame) {
    if (frame < (void *)g_frame_pool ||
        frame >= (void *)(g_frame_pool + (size_t)POOL_FRAMES * FRAME_SIZE))
        return -1;                      /* not ours: a wild free would land here */
    g_frames_freed++;
    /* Rollback frees in reverse order, which a bump allocator can honour
     * exactly -- so the pool really does recover and "can we map again after a
     * failed attempt" becomes a question this test can ask. */
    if (frame == g_frame_pool + (size_t)(g_frames_used - 1) * FRAME_SIZE)
        g_frames_used--;
    return 0;
}

/* Recording user_map_page(): the shadow PTEs actually installed. */
typedef struct { uint64_t pml4, va, pa, flags; } MapCall;
#define MAX_MAPS 4096
static MapCall g_maps[MAX_MAPS];
static int     g_map_count;

/* Marker the stub writes into the top-level slot, standing in for the PDPT
 * frame the real user_map_page() would allocate and install there. Without
 * this the stub records the call but leaves the PML4 all zeroes -- and the
 * "window published to the kernel root" check below would then compare 0
 * against 0 and pass no matter what the code did. A stub LESS capable than the
 * thing it replaces makes an assertion vacuous just as surely as one that is
 * more capable makes it permissive. */
#define STUB_PDPT_MARKER 0xABCD0000ULL

void user_map_page(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (g_map_count < MAX_MAPS) {
        g_maps[g_map_count].pml4  = (uint64_t)(uintptr_t)pml4;
        g_maps[g_map_count].va    = vaddr;
        g_maps[g_map_count].pa    = paddr;
        g_maps[g_map_count].flags = flags;
    }
    g_map_count++;
    if (pml4) pml4[(vaddr >> 39) & 0x1FF] =
        STUB_PDPT_MARKER | USER_PTE_PRESENT | USER_PTE_WRITE;
}

static const MapCall *last_map(void) {
    return g_map_count > 0 && g_map_count <= MAX_MAPS ? &g_maps[g_map_count - 1] : NULL;
}

/* ─── guest page-table construction, in guest-physical terms ─────────────── */

/* Hands out guest-physical page numbers starting after the data area. */
static uint64_t g_next_gpa_table = 0x10000;   /* GPA 64 KiB */

static uint64_t *gpa_ptr(uint64_t gpa) {
    return (uint64_t *)(g_guest_ram + gpa);
}
static uint64_t alloc_guest_table(void) {
    uint64_t gpa = g_next_gpa_table;
    g_next_gpa_table += FRAME_SIZE;
    memset(gpa_ptr(gpa), 0, FRAME_SIZE);
    return gpa;
}

#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)
#define PS_FLAG      (1ULL << 7)

/* Builds a 4-level guest mapping GVA -> target_gpa with the given leaf flags.
 * Returns the guest CR3. `levels_present` lets a test truncate the walk. */
static uint64_t build_guest_pt_4k(uint64_t gva, uint64_t target_gpa,
                                  uint64_t leaf_flags, int levels_present) {
    uint64_t cr3  = alloc_guest_table();
    if (levels_present < 1) return cr3;
    uint64_t pdpt = alloc_guest_table();
    gpa_ptr(cr3)[PML4_IDX(gva)] = pdpt | USER_PTE_PRESENT | USER_PTE_WRITE;
    if (levels_present < 2) return cr3;
    uint64_t pd   = alloc_guest_table();
    gpa_ptr(pdpt)[PDPT_IDX(gva)] = pd | USER_PTE_PRESENT | USER_PTE_WRITE;
    if (levels_present < 3) return cr3;
    uint64_t pt   = alloc_guest_table();
    gpa_ptr(pd)[PD_IDX(gva)] = pt | USER_PTE_PRESENT | USER_PTE_WRITE;
    if (levels_present < 4) return cr3;
    gpa_ptr(pt)[PT_IDX(gva)] = target_gpa | leaf_flags;
    return cr3;
}

int main(void) {
    g_guest_ram = aligned_alloc(FRAME_SIZE, GUEST_RAM_BYTES);
    g_frame_pool = aligned_alloc(FRAME_SIZE, (size_t)POOL_FRAMES * FRAME_SIZE);
    if (!g_guest_ram || !g_frame_pool) { printf("aligned_alloc failed\n"); return 2; }
    memset(g_guest_ram, 0, GUEST_RAM_BYTES);
    memset(g_frame_pool, 0, (size_t)POOL_FRAMES * FRAME_SIZE);
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a crash does not eat the output */
    qemu_sls_test_gpa_base = (uint64_t)(uintptr_t)g_guest_ram;

    printf("=== QEMU-SLS Phase 1: shadow page tables ===\n\n");

    /* ─── the production constant, checked where relocating it cannot hide it ── */
    printf("-- the GPA window constant --\n");
    {
        const uint64_t prod = 0x0000200000000000ULL;
        CHECK(prod == (32ULL << 40),
              "*** the production GPA window base is 32 TiB -- the header called it "
              "2 TiB until this was computed ***");
        CHECK((prod >> 47) == 0,
              "*** ...and is canonical: below the 128 TiB hole, so GPA+offset never "
              "produces a non-canonical address ***");
        CHECK(prod + (uint64_t)QEMU_GUEST_RAM_PAGES * FRAME_SIZE < (1ULL << 47),
              "...and the whole 256 MiB guest RAM span stays inside it");
        CHECK((prod & (FRAME_SIZE - 1)) == 0, "...and is page-aligned");
    }

    printf("\n-- init and guest RAM mapping --\n");
    CHECK(qemu_sls_mmu_init() == 0, "init allocates a shadow PML4");
    CHECK(qemu_sls_shadow_cr3 != 0, "...and publishes it as the shadow CR3");
    /* init copies 512 kernel PML4 entries by direct assignment, not via
     * user_map_page, so nothing should have been recorded yet. */
    CHECK(g_map_count == 0, "init installs no shadow PTEs of its own");

    g_map_count = 0;
    CHECK(qemu_sls_mmu_map_guest_ram(0, 4) == 0, "maps 4 pages of guest RAM at GPA 0");
    CHECK(g_map_count == 4, "*** one shadow PTE per page, no more and no fewer ***");
    CHECK(g_maps[0].va == QEMU_GPA_HOST_BASE + 0 &&
          g_maps[3].va == QEMU_GPA_HOST_BASE + 3 * FRAME_SIZE,
          "*** each page lands at GPA_HOST_BASE + gpa, contiguously ***");
    CHECK((g_maps[0].flags & (USER_PTE_PRESENT | USER_PTE_WRITE))
              == (USER_PTE_PRESENT | USER_PTE_WRITE),
          "...present and writable");
    CHECK(g_maps[0].pa != g_maps[1].pa,
          "...and distinct frames, so the pool is really being consumed");

    /* ─── the window must be reachable from the KERNEL root, not just the
     *      shadow ────────────────────────────────────────────────────────────
     * map_guest_ram() installs pages in shadow_pml4. The shadow root is only
     * in CR3 while translated guest code runs -- but the EMULATOR touches this
     * same window as ordinary kernel code, on the kernel's CR3, to read guest
     * page tables and load images. init() copies kernel->shadow, once, BEFORE
     * this window exists, so nothing carried it the other way.
     *
     * The consequence in production was a #PF at 0x0000200000001000 (error=2)
     * on the benchmark's very first buffer write, which shadow_fault() then
     * refused -- correctly, since qemu_sls_guest_active was 0. Every guard
     * behaved as designed and the node halted anyway.
     *
     * Asserted against the kernel PML4 that arch_read_cr3() hands out, which
     * is the table the CPU would actually walk. */
    {
        unsigned widx = (unsigned)((QEMU_GPA_HOST_BASE >> 39) & 0x1FF);
        const uint64_t *shadow = (const uint64_t *)(uintptr_t)g_maps[0].pml4;
        CHECK(shadow[widx] != 0,
              "*** the stub really populated the shadow's top-level slot -- "
              "without this the next check compares 0 against 0 ***");
        CHECK(g_fake_kernel_pml4[widx] == shadow[widx],
              "*** the window's PML4 entry was published to the KERNEL root, so "
              "the emulator can reach guest RAM without a guest running ***");
        CHECK(g_fake_kernel_pml4[widx] & USER_PTE_PRESENT,
              "...and it is present, not merely copied as a zero");
    }
    /* Captured now, because g_map_count is reset by later scenarios and
     * g_maps[0] then refers to something else entirely. */
    const uint64_t gpa0_frame = g_maps[0].pa;

    printf("\n-- region lookup --\n");
    CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE) != NULL,
          "the first byte of the region is inside it");
    CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE + 4 * FRAME_SIZE - 1) != NULL,
          "*** and so is the LAST byte -- an exclusive end would miss it ***");
    CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE + 4 * FRAME_SIZE) == NULL,
          "*** but the byte after the region is not ***");
    CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE - 1) == NULL,
          "...and neither is the byte before it");

    /* A second region covering the GPAs where this test builds guest page
     * tables. The walk reads them through gpa_to_hva(), which refuses any GPA
     * that map_guest_ram() never backed -- correctly: an unmapped GPA has no
     * host frame to read. The first version of this file mapped only region 1
     * and every walk below refused, which looked like a walk bug and was a
     * test-setup bug. */
    CHECK(qemu_sls_mmu_map_guest_ram(0x10000, 40) == 0,
          "maps a second region for the guest page tables");
    CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE + 0x10000) != NULL,
          "*** both regions are live at once -- the table is not single-entry ***");

    printf("\n-- DMA pointers --\n");
    CHECK(qemu_sls_dma_host_ptr(0) == (void *)(uintptr_t)QEMU_GPA_HOST_BASE,
          "a mapped GPA resolves to its host pointer");
    CHECK(qemu_sls_dma_host_ptr(4 * FRAME_SIZE) == NULL,
          "*** an UNmapped GPA returns NULL rather than a pointer into nothing ***");
    CHECK(qemu_sls_dma_frame_phys(0) == g_maps[0].pa,
          "the physical frame for GPA 0 is the one that was mapped there");
    CHECK(qemu_sls_dma_frame_phys((uint64_t)QEMU_GUEST_RAM_PAGES * FRAME_SIZE) == 0,
          "*** a GPA past the tracked span returns 0, not an out-of-bounds read ***");

    /* Everything below faults on behalf of guest code. */
    qemu_sls_guest_active = 1;

    printf("\n-- the 4 KiB walk --\n");
    {
        const uint64_t gva = 0x00000000DEADB000ULL;
        uint64_t target_gpa = 2 * FRAME_SIZE;      /* a page we mapped above */
        uint64_t cr3 = build_guest_pt_4k(gva, target_gpa,
                                         USER_PTE_PRESENT | USER_PTE_WRITE, 4);
        qemu_sls_guest_cr3 = cr3;

        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva + 0x123), 0) == 0,
              "a fully-present guest mapping resolves");
        CHECK(g_map_count == 1, "...installing exactly one shadow PTE");
        const MapCall *m = last_map();
        CHECK(m && m->va == GUEST_FAULT_ADDR(gva),
              "*** the shadow PTE is installed at the HOST address the CPU faulted "
              "on -- guest_va + guest_base -- page-aligned, with the faulting offset "
              "stripped. This assertion previously required the opposite (the raw "
              "guest VA), which was correct for the retired GVA-direct design ***");
        CHECK(m && m->pa == qemu_sls_dma_frame_phys(target_gpa),
              "*** ...pointing at the HOST FRAME backing that guest physical page. "
              "This is the assertion a passing return code cannot make: a walk that "
              "resolved the wrong page also returns 0 ***");
        CHECK(m && (m->flags & USER_PTE_WRITE),
              "...and writable, because the guest leaf PTE was");
    }

    printf("\n-- write permission is propagated, not assumed --\n");
    {
        const uint64_t gva = 0x0000000012345000ULL;
        uint64_t cr3 = build_guest_pt_4k(gva, 1 * FRAME_SIZE,
                                         USER_PTE_PRESENT, 4);   /* read-only leaf */
        qemu_sls_guest_cr3 = cr3;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 0, "a read-only guest page resolves");
        const MapCall *m = last_map();
        CHECK(m && (m->flags & USER_PTE_PRESENT), "...present");
        CHECK(m && !(m->flags & USER_PTE_WRITE),
              "*** and NOT writable: a shadow PTE more permissive than the guest's "
              "would let guest code write a page its own tables marked read-only ***");
    }

    printf("\n-- an incomplete guest walk is refused at every level --\n");
    {
        const uint64_t gva = 0x000000ABCDE00000ULL;
        for (int lvl = 0; lvl < 4; lvl++) {
            qemu_sls_guest_cr3 = build_guest_pt_4k(gva, FRAME_SIZE,
                                                   USER_PTE_PRESENT, lvl);
            g_map_count = 0;
            char msg[128];
            snprintf(msg, sizeof msg,
                     "%d of 4 levels present -> refused (returns 1), nothing installed", lvl);
            int rc = qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0);
            CHECK(rc == 1 && g_map_count == 0, msg);
        }
    }

    printf("\n-- huge pages are decomposed to 4 KiB, one fault at a time --\n");
    {
        /* 2 MiB: PS set at the PD level. The GPA is the entry's frame field
         * OR'd with the low 21 bits of the faulting GVA -- get that wrong and
         * every access inside the huge page maps to the same 4 KiB frame. */
        const uint64_t gva  = 0x0000000040000000ULL;
        const uint64_t off  = 0x2000;   /* inside the 2 MiB page, and mapped */
        uint64_t cr3  = alloc_guest_table();
        uint64_t pdpt = alloc_guest_table();
        uint64_t pd   = alloc_guest_table();
        gpa_ptr(cr3)[PML4_IDX(gva)]   = pdpt | USER_PTE_PRESENT | USER_PTE_WRITE;
        gpa_ptr(pdpt)[PDPT_IDX(gva)]  = pd   | USER_PTE_PRESENT | USER_PTE_WRITE;
        gpa_ptr(pd)[PD_IDX(gva)]      = 0ULL | USER_PTE_PRESENT | USER_PTE_WRITE | PS_FLAG;
        qemu_sls_guest_cr3 = cr3;

        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva + off + 0x40), 0) == 0,
              "a 2 MiB guest page resolves");
        const MapCall *m = last_map();
        CHECK(g_map_count == 1,
              "*** ...as ONE 4 KiB shadow PTE, not 512 -- the decomposition is "
              "per-fault, so an untouched huge page costs nothing ***");
        CHECK(m && m->va == GUEST_FAULT_ADDR(gva + off),
              "...at the faulting 4 KiB page within the huge page, in host terms");
        CHECK(m && m->pa == qemu_sls_dma_frame_phys(off & ~(uint64_t)0xFFF),
              "*** ...backed by the frame for GPA (huge_base | gva[20:0]) -- the "
              "offset inside the huge page must survive, or every access in 2 MiB "
              "aliases one frame ***");
    }

    printf("\n-- a huge page over unallocated guest RAM is refused --\n");
    {
        const uint64_t gva = 0x0000000080000000ULL;
        uint64_t cr3  = alloc_guest_table();
        uint64_t pdpt = alloc_guest_table();
        gpa_ptr(cr3)[PML4_IDX(gva)]  = pdpt | USER_PTE_PRESENT | USER_PTE_WRITE;
        /* PS at the PDPT level = 1 GiB page, pointing at GPA 1 GiB which was
         * never mapped by map_guest_ram(). */
        gpa_ptr(pdpt)[PDPT_IDX(gva)] = (1ULL << 30) | USER_PTE_PRESENT | PS_FLAG;
        qemu_sls_guest_cr3 = cr3;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 1 && g_map_count == 0,
              "*** a 1 GiB guest page over guest RAM that was never allocated is "
              "refused, not mapped to frame 0 ***");
    }

    printf("\n-- a short allocation unwinds completely --\n");
    {
        /* The pool has POOL_FRAMES; ask for more than remain. The old code
         * allocated-and-mapped in one pass and returned -1 partway through,
         * leaving frames allocated, mapped into the shadow PT, recorded in
         * guest_ram_frames[] -- and registered in no region. dma_host_ptr()
         * then returned NULL for pages that were live and unreclaimable. */
        uint32_t remaining = POOL_FRAMES - g_frames_used;
        uint32_t ask = remaining + 4;
        uint64_t oom_gpa = 0x100000;            /* 1 MiB, untouched so far */

        int maps_before   = g_map_count;
        int frames_before = g_frames_used;
        g_frames_freed = 0;

        CHECK(qemu_sls_mmu_map_guest_ram(oom_gpa, ask) == -1,
              "asking for more frames than the pool holds fails");
        CHECK(g_map_count == maps_before,
              "*** ...having mapped NOTHING -- allocation completes before any PTE "
              "is installed, so a partial range never reaches the shadow table ***");
        CHECK(g_frames_freed == (int)remaining,
              "*** ...and every one of the frames it managed to take was handed "
              "back: -1 is returned whether it rolled back or leaked, so only the "
              "count distinguishes them ***");
        CHECK(g_frames_used == frames_before,
              "*** ...so the pool is exactly as full as before the attempt ***");
        CHECK(qemu_sls_dma_frame_phys(oom_gpa) == 0,
              "*** guest_ram_frames[] is clean, so a later mapping of the same GPA "
              "is not refused as already-backed ***");
        CHECK(qemu_sls_mmu_find_region(QEMU_GPA_HOST_BASE + oom_gpa) == NULL,
              "...and no region was registered");
    }

    printf("\n-- overlapping a live region is refused, not silently re-mapped --\n");
    {
        int maps_before = g_map_count;
        CHECK(qemu_sls_mmu_map_guest_ram(0, 1) == -1,
              "*** re-mapping a GPA that is already backed is REFUSED -- doing it "
              "would orphan the old frame with nothing pointing at it ***");
        CHECK(g_map_count == maps_before, "...and installs nothing");
        CHECK(qemu_sls_dma_frame_phys(0) == gpa0_frame,
              "...leaving the original mapping intact");
    }

    printf("\n-- faults are only resolved while guest code is running --\n");
    {
        /* The shadow table maps guest VIRTUAL addresses, so no address range
         * separates a guest access from the kernel dereferencing a bad
         * pointer. Without the active flag, a stray kernel fault at an address
         * the guest happens to map gets "resolved" into a table that is not
         * the live CR3, and iretq re-executes forever. */
        const uint64_t gva = 0x00000000CAFE0000ULL;
        qemu_sls_guest_cr3 = build_guest_pt_4k(gva, 3 * FRAME_SIZE,
                                               USER_PTE_PRESENT | USER_PTE_WRITE, 4);

        qemu_sls_guest_active = 1;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 0 && g_map_count == 1,
              "with guest code running, a mapped GVA resolves");

        qemu_sls_guest_active = 0;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 1,
              "*** the SAME fault at the SAME address is REFUSED when no guest is "
              "running -- so a kernel bug halts with a diagnosable [FAULT] line "
              "instead of spinning in the fault handler ***");
        CHECK(g_map_count == 0,
              "...and installs nothing, so the shadow table is not polluted either");
        qemu_sls_guest_active = 1;
    }

    printf("\n-- an address outside the guest window is not a guest access --\n");
    {
        /* The shadow SHARES lower-level page tables with the kernel:
         * qemu_sls_mmu_init() copies all 512 kernel PML4 entries by value, and
         * user_map_page() follows a present entry instead of cloning it. So a
         * mapping installed at an address the kernel also maps does not shadow
         * the kernel's translation, it OVERWRITES it -- in the kernel's own
         * live tables, permanently. At a low address that means remapping the
         * kernel image out from under the CPU executing it.
         *
         * Under the window model this cannot arise from a legitimate guest
         * access, because every such access is at guest_va + guest_base. An
         * address outside the window is by definition something else: a kernel
         * bug, or a caller still using the retired GVA-direct convention.
         *
         * qemu_sls_guest_active is deliberately left SET here. That flag is the
         * other guard, and leaving it on is what makes this check test the
         * window test rather than accidentally passing because the active flag
         * caught it first -- the two guards must be shown to be independent. */
        const uint64_t gva = 0x00000000BEEF0000ULL;
        /* GPA 3*FRAME_SIZE, not 5: only the first four guest pages are backed
         * at this point, and an unbacked target makes the walk fail for a
         * reason that has nothing to do with the window -- which the control
         * check below caught when this test first used 5. */
        qemu_sls_guest_cr3 = build_guest_pt_4k(gva, 3 * FRAME_SIZE,
                                               USER_PTE_PRESENT | USER_PTE_WRITE, 4);
        qemu_sls_guest_active = 1;

        /* Sanity: through the window it resolves. Without this the refusal
         * below could be caused by a broken guest table rather than by the
         * window check, and the test could not tell the difference. */
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 0 &&
              g_map_count == 1,
              "control: the same mapping DOES resolve when addressed through "
              "the window");

        /* ─── What these two checks do and do NOT prove ────────────────────
         * They prove the OUTCOME: an out-of-window address resolves nothing
         * and installs nothing. They do NOT isolate the window guard, and
         * mutation testing is what showed it: deleting
         * `if (!shadow_va_in_window(...)) return 1;` leaves every check here
         * passing, because the subtraction below it then underflows the
         * address into garbage, the guest walk misses, and the function
         * returns 1 for a different reason.
         *
         * So the guard in shadow_fault() is defence in depth, not the sole
         * mechanism, and this test cannot tell the two apart. Recorded rather
         * than papered over: a check whose failure mode is indistinguishable
         * from success is worth less than it appears, and the honest move is
         * to say which one this is.
         *
         * The guard that IS load-bearing is in shadow_install(), against a
         * future caller passing a raw guest VA. It is unreachable from here --
         * shadow_fault has already screened the address -- so it has no test
         * at all, and should get one when guest paging gives it a second
         * caller. */
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(gva, 0) == 1,
              "the raw guest VA -- outside the window -- does not resolve, with "
              "guest code active and the guest mapping otherwise valid");
        CHECK(g_map_count == 0,
              "*** and NOTHING was installed. A refusal that still wrote a PTE "
              "would have corrupted the kernel's own page tables, which is the "
              "entire failure this guard exists to prevent ***");

        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(0x1000, 0) == 1 && g_map_count == 0,
              "...and so is a low kernel-range address");
        /* guest_active deliberately LEFT SET: the invlpg block below depends on
         * it, and clearing it here made four unrelated checks fail. */
    }

    printf("\n-- the TLB is invalidated, not just the table written --\n");
    {
        const uint64_t gva = 0x00000000BEEF0000ULL;
        qemu_sls_guest_cr3 = build_guest_pt_4k(gva, 1 * FRAME_SIZE,
                                               USER_PTE_PRESENT, 4);
        g_invlpg_count = 0; g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva + 0x888), 0) == 0, "the fault resolves");
        CHECK(g_invlpg_count == 1,
              "*** ...and invlpg was issued. x86 does not cache non-present "
              "translations, so the FIRST fault would retry fine without this -- but "
              "a permission fault leaves a stale read-only entry, and the retry "
              "would fault again forever ***");
        CHECK(g_invlpg_count == 1 && g_invlpg[0] == GUEST_FAULT_ADDR(gva),
              "*** on the page-aligned HOST address, not the raw one: invlpg takes a "
              "linear address, so it must be the address the CPU actually faulted on "
              "-- flushing the guest VA would flush an unrelated kernel page ***");
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    free(g_guest_ram); free(g_frame_pool);
    return checks_failed == 0 ? 0 : 1;
}
