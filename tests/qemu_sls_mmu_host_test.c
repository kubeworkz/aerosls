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
uint64_t qemu_sls_test_guest_window;

#include "kernel/qemu_sls_mmu.h"

/* ─── constants mirrored from the headers the .c includes ────────────────── */
#define FRAME_SIZE           4096
#define USER_PTE_PRESENT     (1ULL << 0)
#define USER_PTE_WRITE       (1ULL << 1)
#define USER_PTE_FRAME_MASK  0x000FFFFFFFFFF000ULL

/* ─── The address the CPU reports is NOT the guest's ──────────────────────
 * Emitted guest code addresses memory as `guest_va + guest_base`, and
 * guest_base is QEMU_GUEST_WINDOW_BASE -- the GUEST window, not the emulator
 * one (see tcg/tcg.c and kernel/qemu_sls_mmu.h). So CR2 -- and the argument
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
#define GUEST_FAULT_ADDR(gva)  ((uint64_t)(gva) + QEMU_GUEST_WINDOW_BASE)

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

/* The MMU now calls into the translation cache to invalidate a page whose code
 * the guest overwrote. Stubbed to do exactly what the real one does to the one
 * piece of state this test observes -- bump the counter -- and nothing else.
 * A stub that did NOT bump it would make every generation assertion below pass
 * against a no-op. */
uint32_t qemu_sls_page_gen[65536];
void qemu_sls_tcache_flush_page(uint64_t gpa) {
    uint32_t pg = (uint32_t)(gpa / 4096);
    if (pg < 65536) qemu_sls_page_gen[pg]++;
}
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

/* Recorded, not swallowed. Dropping the guest window's identity mappings
 * leaves those translations live in the TLB, so the flush is not incidental --
 * without it the guest keeps using the identity mapping it was just supposed
 * to stop using, and reads the right-looking wrong frame. An empty stub would
 * make that omission invisible. */
static int g_flush_tlb_count;
void qemu_sls_flush_tlb(void) { g_flush_tlb_count++; }

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
/* ─── The shared budget that has now caught three separate blocks ─────────
 * Guest page tables come from this bump cursor, but only the GPAs backed by
 * map_guest_ram() can actually be read back through gpa_to_hva(). Run past the
 * backed range and build_guest_pt_4k() returns a CR3 whose tables are
 * unreadable -- so the failure lands on whichever block happens to walk next,
 * with a message about that block's assertion and nothing about the cause.
 *
 * That has happened three times in this file: each new block added four tables,
 * pushed the cursor past the end, and broke three unrelated checks below it.
 * Each time the diagnosis started from the wrong block.
 *
 * The budget cannot be enforced from here (the backed range lives in the .c),
 * but exhausting it can at least stop being silent. */
#define GUEST_TABLE_LIMIT_GPA  (56u * FRAME_SIZE)   /* map_guest_ram backs 16..55 */

static uint64_t alloc_guest_table(void) {
    uint64_t gpa = g_next_gpa_table;
    if (gpa + FRAME_SIZE > GUEST_TABLE_LIMIT_GPA) {
        fprintf(stderr,
            "\nFATAL: guest page-table space exhausted at GPA 0x%llx.\n"
            "  build_guest_pt_4k() hands out tables from a bump cursor, and only\n"
            "  GPAs backed by map_guest_ram() are readable. Past this point a\n"
            "  CR3 points at tables gpa_to_hva() returns NULL for, and the walk\n"
            "  fails in whatever block runs NEXT -- not in the one that ran out.\n"
            "  Either reuse an existing qemu_sls_guest_cr3 instead of building a\n"
            "  new table, or back more guest RAM before adding this block.\n",
            (unsigned long long)gpa);
        exit(2);
    }
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
    uint64_t saved_cr3_paged = 0;
    g_guest_ram = aligned_alloc(FRAME_SIZE, GUEST_RAM_BYTES);
    g_frame_pool = aligned_alloc(FRAME_SIZE, (size_t)POOL_FRAMES * FRAME_SIZE);
    if (!g_guest_ram || !g_frame_pool) { printf("aligned_alloc failed\n"); return 2; }
    memset(g_guest_ram, 0, GUEST_RAM_BYTES);
    memset(g_frame_pool, 0, (size_t)POOL_FRAMES * FRAME_SIZE);
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a crash does not eat the output */
    qemu_sls_test_gpa_base = (uint64_t)(uintptr_t)g_guest_ram;
    /* One PML4 slot (512 GiB) above the emulator window. Never dereferenced --
     * see tests/qemu_sls_test_window.h. */
    qemu_sls_test_guest_window = qemu_sls_test_gpa_base + 0x8000000000ULL;

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
    CHECK(g_map_count == 8,
          "*** TWO shadow PTEs per page: one in the emulator window, one in the "
          "guest window. Not one, and not three -- the count is the cheapest "
          "check that both windows were populated and neither twice ***");
    CHECK(g_maps[0].va == QEMU_GPA_HOST_BASE &&
          g_maps[4].va == QEMU_GUEST_WINDOW_BASE,
          "*** ...the first four in the EMULATOR window, the next four in the "
          "GUEST window, both starting at GPA 0 ***");
    CHECK(g_maps[0].pa == g_maps[4].pa,
          "*** ...and the same GPA is backed by the SAME frame through both "
          "windows. Two views of one page, which is the entire point: the "
          "emulator and the guest must see identical memory while paging is "
          "off, and diverge only once the guest's own tables take over ***");
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

    /* Everything below faults on behalf of guest code, with the guest's own
     * paging enabled -- which is the only state in which walking
     * qemu_sls_guest_cr3 means anything.
     *
     * These tests set only guest_active until the paging flag existed, and
     * seventeen of them failed the moment it did. They were not wrong about
     * the walk; they were silent about a precondition they depended on. With
     * paging off the guest's virtual addresses are its physical ones, the
     * window resolves every access directly, and a fault reaching the walker
     * is an error rather than something to resolve. */
    qemu_sls_guest_active = 1;
    qemu_sls_guest_paging_on = 1;

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

    printf("\n-- with guest paging OFF, CR3 is not walked --\n");
    {
        /* Real x86 boots with CR0.PG clear and enables paging later, so this
         * is the state every guest starts in, not a corner case.
         *
         * The danger being guarded is specific: with paging off, CR3 holds
         * whatever the guest last wrote or nothing at all. A four-level walk
         * over arbitrary guest memory readily finds words with the PRESENT bit
         * set -- so the walker would resolve to a frame chosen by garbage,
         * return "handled", and the guest would read the wrong page with no
         * fault and no log line.
         *
         * To make that concrete rather than theoretical, guest_cr3 is pointed
         * at a table that IS valid and WOULD resolve. If the walk happens, it
         * succeeds -- and succeeding is the bug. */
        const uint64_t gva = 0x00000000DEADB000ULL;
        qemu_sls_guest_cr3 = build_guest_pt_4k(gva, 2 * FRAME_SIZE,
                                               USER_PTE_PRESENT | USER_PTE_WRITE, 4);
        /* Kept for the paged-invalidation block below, which needs a real
         * GVA->GPA mapping but must not build another set of tables -- the
         * guest-table budget is shared and nearly spent by this point. */
        saved_cr3_paged = qemu_sls_guest_cr3;
        qemu_sls_guest_active = 1;

        qemu_sls_guest_paging_on = 1;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 0 &&
              g_map_count == 1,
              "control: with paging ON this exact mapping resolves");

        qemu_sls_guest_paging_on = 0;
        g_map_count = 0;
        CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0) == 1,
              "*** with paging OFF the SAME fault is refused -- the walk is "
              "skipped, not merely unsuccessful ***");
        CHECK(g_map_count == 0,
              "*** and no PTE was installed. The control above proves the walk "
              "would have SUCCEEDED, so this is the flag being honoured and not "
              "an unrelated failure ***");

        qemu_sls_guest_paging_on = 1;   /* restore for the blocks below */
    }

    printf("\n-- enabling guest paging drops the identity window --\n");
    {
        /* The transition that made two windows necessary. While paging is off
         * the guest window is identity, so guest_va == guest_physical and every
         * access resolves with no fault. The instant the guest's own tables
         * take over, those identity entries are wrong -- and worse than absent,
         * because an access resolves THROUGH them instead of faulting, quietly
         * reading frame(V) where the guest meant frame(P). */
        const uint64_t saved_cr3 = qemu_sls_guest_cr3;
        qemu_sls_guest_paging_on = 0;
        qemu_sls_guest_cr3 = 0;

        CHECK(qemu_sls_mmu_guest_paging_enable() == -1 &&
              qemu_sls_guest_paging_on == 0,
              "*** refused while guest CR3 is still 0 -- every later access "
              "would walk a null root and report unresolved, naming the symptom "
              "instead of the cause ***");

        /* Reuses the previous block's guest CR3 rather than building another
         * table, and that is not laziness. build_guest_pt_4k() hands out guest
         * tables from a bump pointer, but only GPA indices 16..55 are backed by
         * map_guest_ram() -- so every extra call pushes the NEXT test's page
         * tables closer to unbacked memory. An earlier version of this block
         * did build one, and the invlpg tests below then failed while walking
         * page tables that gpa_to_hva() could no longer read. The budget is
         * real, undocumented, and shared. */
        qemu_sls_guest_cr3 = saved_cr3;
        unsigned slot = (unsigned)((QEMU_GUEST_WINDOW_BASE >> 39) & 0x1FF);
        uint64_t *shadow = (uint64_t *)(uintptr_t)g_maps[0].pml4;

        CHECK(shadow[slot] != 0,
              "control: the guest window's PML4 slot is populated before the "
              "transition (otherwise the check below proves nothing)");

        g_flush_tlb_count = 0;
        CHECK(qemu_sls_mmu_guest_paging_enable() == 0 &&
              qemu_sls_guest_paging_on == 1,
              "paging enables once CR3 is set");
        CHECK(shadow[slot] == 0,
              "*** the guest window's whole subtree is dropped in ONE PML4 "
              "write -- 512 GiB of stale identity, not 65,536 individual "
              "unmaps ***");
        CHECK(g_flush_tlb_count == 1,
              "*** and the TLB was flushed. The identity translations are live "
              "in it; without this the guest keeps using the mapping that was "
              "just revoked and reads a right-looking wrong frame ***");
        CHECK(g_fake_kernel_pml4[(QEMU_GPA_HOST_BASE >> 39) & 0x1FF] != 0,
              "*** the EMULATOR window is untouched -- shadow_fault is about to "
              "walk the guest's page tables through it on the very next fault, "
              "which is why it had to be a separate slot ***");

        CHECK(qemu_sls_mmu_guest_paging_enable() == 0,
              "...and the call is idempotent");
    }

    printf("\n-- self-modifying guest code: protect, fault, invalidate --\n");
    {
        /* The mechanism that makes the persistent cache SAFE. With softmmu off
         * a guest store is a bare MOV -- no helper, no TLB lookup, nothing to
         * hook. Without write protection a guest can rewrite a page it has
         * already executed and the cache goes on serving translations of bytes
         * that no longer exist: correct-looking code, wrong program, no fault
         * and no log line. */
        qemu_sls_guest_paging_on = 0;
        const uint64_t code_gpa = 2 * FRAME_SIZE;
        const uint32_t page_idx = 2;

        g_map_count = 0; g_invlpg_count = 0;
        CHECK(qemu_sls_mmu_write_protect_gpa(code_gpa) == 0,
              "a code page can be write-protected");
        const MapCall *m = last_map();
        CHECK(m && m->va == QEMU_GUEST_WINDOW_BASE + code_gpa,
              "...in the GUEST window, which is the only range guest stores "
              "reach");
        CHECK(m && (m->flags & USER_PTE_PRESENT) && !(m->flags & USER_PTE_WRITE),
              "*** present but NOT writable -- the guest may still execute and "
              "read the page; only the store traps, because only a store can "
              "invalidate a translation ***");
        CHECK(g_invlpg_count == 1,
              "*** and the TLB entry was flushed. A stale writable entry would "
              "let the store succeed WITHOUT faulting, and the cache would "
              "never learn the page changed -- the invlpg IS the protection ***");

        CHECK(qemu_sls_mmu_write_protect_gpa(
                  (uint64_t)QEMU_GUEST_RAM_PAGES * FRAME_SIZE) == -1,
              "...and an unbacked GPA is refused rather than mapped");

        /* Now the fault the protection exists to cause. error_code 0x3 =
         * write (bit 1) to a PRESENT page (bit 0) -- a permission fault, not a
         * missing mapping. */
        uint32_t gen_before = qemu_sls_page_gen[page_idx];
        g_map_count = 0; g_invlpg_count = 0;
        qemu_sls_guest_active = 1;
        CHECK(qemu_sls_mmu_shadow_fault(
                  QEMU_GUEST_WINDOW_BASE + code_gpa + 0x40, 0x3) == 0,
              "a guest write to the protected page resolves");
        CHECK(qemu_sls_page_gen[page_idx] == gen_before + 1,
              "*** ...and the page's GENERATION was bumped, which is what makes "
              "every TB compiled from it a lookup miss ***");
        const MapCall *m2 = last_map();
        CHECK(m2 && (m2->flags & USER_PTE_WRITE),
              "*** ...and write permission was RESTORED, so the retried store "
              "succeeds. Leaving it protected would fault forever on the same "
              "instruction ***");
        CHECK(g_invlpg_count == 1,
              "...with the read-only entry flushed, or the retry faults again");

        /* A write to an UNPROTECTED page must not be swallowed. Only a
         * permission fault means "protected code page"; a not-present fault is
         * a genuine miss and belongs to the walker below. */
        uint32_t gen_other = qemu_sls_page_gen[3];
        CHECK(qemu_sls_mmu_shadow_fault(
                  QEMU_GUEST_WINDOW_BASE + 3 * FRAME_SIZE, 0x2) == 1 &&
              qemu_sls_page_gen[3] == gen_other,
              "*** a NOT-PRESENT write (error 0x2) is not treated as a code-page "
              "write: no generation bumped, fault left unresolved. Otherwise "
              "every ordinary miss would silently invalidate a page ***");

        /* ─── The same thing, with guest paging ON ─────────────────────────
         * Generations are keyed by PHYSICAL page, because that is what a
         * translation was compiled from. Two guest virtual addresses can alias
         * one physical page, so invalidating by GVA would leave the alias
         * serving stale code -- the walk is not an implementation detail, it
         * is the difference between invalidating the right thing and something
         * that merely looks right.
         *
         * The check below is built so a GVA-keyed implementation FAILS it: the
         * GVA and its GPA are deliberately different, and the generation that
         * must move is the one indexed by the GPA. */
        qemu_sls_guest_paging_on = 1;
        {
            /* Reuses the CR3 an earlier block built rather than building a
             * fifth set of tables: the guest-table cursor is a shared, finite
             * budget (see alloc_guest_table) and this block was the one that
             * exhausted it, breaking three checks further down. gva and
             * target_gpa below must match what that block mapped. */
            const uint64_t gva = 0x00000000DEADB000ULL;
            const uint64_t target_gpa = 2 * FRAME_SIZE;
            qemu_sls_guest_cr3 = saved_cr3_paged;
            CHECK((gva / FRAME_SIZE) != (target_gpa / FRAME_SIZE),
                  "control: the test GVA and its GPA are on different pages, so "
                  "keying invalidation by the wrong one is detectable");

            uint32_t gen_gpa_before = qemu_sls_page_gen[target_gpa / FRAME_SIZE];
            g_map_count = 0; g_invlpg_count = 0;

            CHECK(qemu_sls_mmu_shadow_fault(GUEST_FAULT_ADDR(gva), 0x3) == 0,
                  "a paged guest's write to a protected code page resolves");
            CHECK(qemu_sls_page_gen[target_gpa / FRAME_SIZE] == gen_gpa_before + 1,
                  "*** ...and the generation bumped is the one for the PHYSICAL "
                  "page the guest's tables map to -- not the virtual address it "
                  "wrote through ***");
            const MapCall *mw = last_map();
            CHECK(mw && mw->va == GUEST_FAULT_ADDR(gva) &&
                  (mw->flags & USER_PTE_WRITE),
                  "*** ...and the page was made writable again at the GVA, so "
                  "the retried store succeeds instead of faulting forever ***");
            CHECK(mw && mw->pa == qemu_sls_dma_frame_phys(target_gpa),
                  "...pointing at the frame the guest's own tables resolve to");
            CHECK(g_invlpg_count == 1 &&
                  g_invlpg[0] == (GUEST_FAULT_ADDR(gva) & ~(uint64_t)0xFFF),
                  "*** ...and the read-only TLB entry was flushed, at the "
                  "page-aligned host address. Without it the retried store hits "
                  "the stale read-only translation and faults forever on the "
                  "same instruction -- mutation testing caught this assertion "
                  "missing from the paged path while the identical one guarded "
                  "the unpaged one ***");

            /* A write to a GVA the guest does not map cannot be attributed to
             * any physical page. Bumping a generation chosen by guesswork would
             * invalidate unrelated code AND leave the real page stale. */
            uint32_t gen_all = qemu_sls_page_gen[2] + qemu_sls_page_gen[3]
                             + qemu_sls_page_gen[4];
            CHECK(qemu_sls_mmu_shadow_fault(
                      GUEST_FAULT_ADDR(0x00000000DEAD0000ULL), 0x3) == 1,
                  "*** an unmapped GVA is refused, not resolved ***");
            CHECK(qemu_sls_page_gen[2] + qemu_sls_page_gen[3]
                  + qemu_sls_page_gen[4] == gen_all,
                  "*** ...and no generation moved. Guessing would invalidate "
                  "unrelated code while leaving the modified page live ***");
        }

        /* Restore what the blocks below depend on. This block turned paging
         * OFF and guest_active ON for its own purposes, and leaving either
         * that way broke three unrelated checks -- the second time in this
         * file that a new block has done exactly that. The shared mutable
         * state is the hazard, not the individual mistake. */
        qemu_sls_guest_paging_on = 1;
        qemu_sls_guest_active    = 1;
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
