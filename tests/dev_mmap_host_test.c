/*
 * dev_mmap_host_test.c — Driver SDK ABI v0.1 §4.1: CAP_TYPE_DEV with
 * SYS_DEV_MMAP (309), driven against the REAL kernel/cap.c and
 * kernel/chan.c (not a reimplementation).
 *
 * The test plants a CAP_OBJ_KIND_DEV object (a device MMIO region backed
 * by a fake physical map) and mints CAP_TYPE_DEV words directly into a
 * fake process's cap table (there is no user-facing "create DEV cap"
 * syscall yet — DEV caps come from the manifest `devices:` section at
 * create_sidecar, which is M2 work). It then drives k_dev_mmap through
 * the REAL word validation, rights checks, range checks, window
 * allocation, and map-record bookkeeping. The privileged page-table
 * operations are behind cap.c's weak cap_arch_* hooks; here they are
 * overridden with a fake PTE table keyed by vaddr page, so the test can
 * assert the exact physical frame, perms, and cache bits the kernel
 * chose.
 *
 * Scenarios:
 *   1. map a RW dev cap with a hint: the hint is honored, the PTE is
 *      installed with R|W perms at the right physical frames
 *   2. second call with the same cap returns the SAME vaddr (one live
 *      mapping per cap)
 *   3. overlapping hint (already-mapped window) picks a different window
 *   4. WC / uncached flags reach cap_arch_map_page as CAP_PERM_DEV_WC /
 *      CAP_PERM_DEV_UC (the strong arch override turns those into PTE
 *      PWT/PCD)
 *   5. rights: a cap with no R/W perms is CAP_ERR_RIGHTS
 *   6. range: window exceeding the region, zero-length, off + len beyond
 *      npages — all CAP_ERR_RANGE
 *   7. type: MEM cap in the slot is CAP_ERR_TYPE; revoked slot is
 *      CAP_ERR_REVOKED; bad slot is CAP_ERR_RANGE
 *   8. bad flags are CAP_ERR_RANGE; NULL out is CAP_ERR_PROTO
 *   9. syscall wrapper sys_sls_dev_mmap returns the vaddr via out_vaddr
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/dev_mmap_host_test \
 *       tests/dev_mmap_host_test.c kernel/cap.c kernel/chan.c kernel/frame_pool.c
 *   /tmp/dev_mmap_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"   /* stack_bottom/stack_top (frame_pool_init reservation bounds) */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs ──────────────────────────────────────────────────────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_putchar(char c) { (void)c; }

volatile uint64_t kernel_tick_counter = 0;

/* ─── Link stubs for cap.c's sidecar-spawn path (not exercised here) ────── */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;
uint32_t alloc_pid(void) { return 902; }
uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}
uint64_t user_clone_page_table(void) { return 0x3000; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

static uint64_t g_fake_cr3 = 0x5000;
uint64_t cap_proc_cr3(uint32_t pid) { return pid ? g_fake_cr3 : 0; }

/* ─── Fake page table: one 64-bit entry per (vaddr >> 12) ─────────────────
 * Records (paddr, perms) so the test can assert the kernel's mapping
 * choices. Also counts calls to check rollback/unmap behavior. */
/* Windows sit at/above DEV_MMAP_WINDOW_BASE (0x100000000, 4 GiB — above the
 * kernel's 0-4 GiB identity map), so the fake table must span that range. */
#define FAKE_PT_ENTRIES (1u << 21)
static uint64_t g_fake_pt[FAKE_PT_ENTRIES];
static int g_map_calls = 0;
static int g_unmap_calls = 0;

int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                      uint32_t cap_perms) {
    if (!pml4_phys) return -1;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    g_fake_pt[idx] = paddr | (uint64_t)cap_perms;   /* paddr 4K-aligned in tests */
    g_map_calls++;
    return 0;
}

int cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    if (!pml4_phys) return -1;
    uint64_t idx = vaddr >> 12;
    if (idx < FAKE_PT_ENTRIES) g_fake_pt[idx] = 0;
    g_unmap_calls++;
    return 0;
}

void cap_arch_tlb_flush(void) { }

/* cap_table_index (kernel/cap.c): not declared in cap.h (internal only). */
int cap_table_index(uint32_t pid);

/* ─── Device object + cap word planting ─────────────────────────────────── */
/* Plant a CAP_OBJ_KIND_DEV object: a device MMIO region [phys, phys +
 * npages*4096) that is NOT arena-backed. */
static uint32_t plant_dev_object(uint64_t phys, uint32_t npages) {
    static uint32_t next_id = 100;   /* past kernel-internal objects */
    uint32_t id = next_id++;
    struct CapObject* o = &cap_objects[id];
    memset(o, 0, sizeof(*o));
    o->id = id;
    o->kind = CAP_OBJ_KIND_DEV;
    o->active = 1;
    o->phys_base = phys;
    o->npages = npages;
    return id;
}

static uint64_t dev_word(uint32_t obj_id, uint16_t off_bytes, uint16_t pages,
                         uint8_t perm) {
    return ((uint64_t)CAP_TYPE_DEV << CAP_TYPE_SHIFT) |
           ((uint64_t)obj_id  << CAP_OBJ_SHIFT) |
           ((uint64_t)perm    << CAP_PERM_SHIFT) |
           ((uint64_t)off_bytes << CAP_OFF_SHIFT) |
           ((uint64_t)pages   << CAP_LEN_SHIFT);
}

static void mint_dev(uint32_t pid, uint16_t slot, uint64_t word) {
    int ti = cap_table_index(pid);
    if (ti < 0) { fprintf(stderr, "FATAL: no cap table for pid %u\n", pid); }
    cap_tables[ti].slots[slot].word = word;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
    printf("ok: %s\n", msg); \
} while (0)

int main(void) {
    cap_init();
    const uint32_t P = 901;
    g_cur_pid = P;
    memset(g_fake_pt, 0, sizeof(g_fake_pt));
    g_map_calls = g_unmap_calls = 0;

    /* A 16 MiB device region at 0xF0000000. */
    uint32_t dev_obj = plant_dev_object(0xF0000000ULL, 4096);

    /* ── 1. hint honored, perms and frames right ───────────────────────── */
    uint16_t d = 4;
    mint_dev(P, d, dev_word(dev_obj, 0 /*off*/, 2 /*pages*/, CAP_PERM_R | CAP_PERM_W));
    uint64_t v = 0;
    CHECK(k_dev_mmap(P, d, 0, 0x100000000ULL, &v) == CAP_ERR_OK,
          "map with hint succeeds");
    CHECK(v == 0x100000000ULL, "hint honored");
    CHECK(g_map_calls == 2, "two pages mapped");
    CHECK((g_fake_pt[0x100000] & 0xFFFFFFFFFFF000ULL) == 0xF0000000ULL,
          "page 0 maps physical 0xF0000000");
    CHECK((g_fake_pt[0x100001] & 0xFFFFFFFFFFF000ULL) == 0xF0001000ULL,
          "page 1 maps physical 0xF0001000");
    CHECK((g_fake_pt[0x100000] & (CAP_PERM_R | CAP_PERM_W)) ==
          (CAP_PERM_R | CAP_PERM_W), "PTE carries R|W perms");
    CHECK(!(g_fake_pt[0x100000] & CAP_PERM_X), "PTE is not executable");

    /* ── 2. idempotent: same cap maps once ─────────────────────────────── */
    uint64_t v2 = 0;
    CHECK(k_dev_mmap(P, d, 0, 0x100001000ULL, &v2) == CAP_ERR_OK,
          "second call succeeds");
    CHECK(v2 == 0x100000000ULL, "second call returns the SAME vaddr");
    CHECK(g_map_calls == 2, "no new pages mapped on the second call");

    /* ── 3. overlapping hint picks a different window ──────────────────── */
    uint32_t dev2 = plant_dev_object(0xE0000000ULL, 4096);   /* different region */
    uint16_t d2 = 5;
    mint_dev(P, d2, dev_word(dev2, 0, 2, CAP_PERM_R));
    uint64_t v3 = 0;
    CHECK(k_dev_mmap(P, d2, 0, 0x100000000ULL, &v3) == CAP_ERR_OK,
          "busy hint still maps");
    CHECK(v3 != 0x100000000ULL, "busy hint falls back to another window");
    CHECK((v3 & 0xFFFULL) == 0 && v3 >= 0x100000000ULL && v3 < 0x800000000000ULL,
          "fallback window is aligned, above the identity map, in the user half");
    CHECK((g_fake_pt[v3 >> 12] & 0xFFFFFFFFFFF000ULL) == 0xE0000000ULL,
          "fallback maps the new region's physical base");

    /* ── 4. cache flags reach the arch hook ────────────────────────────── */
    uint32_t dev3 = plant_dev_object(0xD0000000ULL, 4096);
    uint16_t d3 = 6;
    mint_dev(P, d3, dev_word(dev3, 0, 1, CAP_PERM_R | CAP_PERM_W));
    uint64_t v4 = 0;
    CHECK(k_dev_mmap(P, d3, DEV_MMAP_WC, 0x100004000ULL, &v4) == CAP_ERR_OK,
          "WC mapping succeeds");
    CHECK((g_fake_pt[0x100004] & CAP_PERM_DEV_WC) != 0,
          "WC flag reaches cap_arch_map_page (→ PWT in the real arch hook)");
    uint32_t dev4 = plant_dev_object(0xC0000000ULL, 4096);
    uint16_t d4 = 7;
    mint_dev(P, d4, dev_word(dev4, 0, 1, CAP_PERM_R | CAP_PERM_W));
    uint64_t v5 = 0;
    CHECK(k_dev_mmap(P, d4, DEV_MMAP_UNCACHED, 0x100005000ULL, &v5) == CAP_ERR_OK,
          "uncached mapping succeeds");
    CHECK((g_fake_pt[0x100005] & CAP_PERM_DEV_UC) != 0,
          "uncached flag reaches cap_arch_map_page (→ PCD in the real arch hook)");

    /* ── 5. rights ─────────────────────────────────────────────────────── */
    uint16_t d5 = 8;
    mint_dev(P, d5, dev_word(dev_obj, 0, 1, 0));   /* no R/W */
    uint64_t vx = 0;
    CHECK(k_dev_mmap(P, d5, 0, 0, &vx) == CAP_ERR_RIGHTS,
          "cap with no R/W perms is CAP_ERR_RIGHTS");

    /* ── 6. range enforcement ──────────────────────────────────────────── */
    uint16_t d6 = 9;
    mint_dev(P, d6, dev_word(dev_obj, 8192, 4095, CAP_PERM_R)); /* off=2pg, len=4095 */
    CHECK(k_dev_mmap(P, d6, 0, 0, &vx) == CAP_ERR_RANGE,
          "cap window beyond region is CAP_ERR_RANGE");       /* off + len > npages */
    uint16_t d7 = 10;
    mint_dev(P, d7, dev_word(dev_obj, 4096, 4096, CAP_PERM_R)); /* off=1 page, len=4096 */
    CHECK(k_dev_mmap(P, d7, 0, 0, &vx) == CAP_ERR_RANGE,
          "offset + length beyond region is CAP_ERR_RANGE");
    uint16_t d8 = 11;
    mint_dev(P, d8, dev_word(dev_obj, 0, 0, CAP_PERM_R));   /* zero pages */
    CHECK(k_dev_mmap(P, d8, 0, 0, &vx) == CAP_ERR_RANGE,
          "zero-length cap is CAP_ERR_RANGE");

    /* ── 7. type / revoked / slot errors ───────────────────────────────── */
    uint16_t mem_slot = 12;
    mint_dev(P, mem_slot, ((uint64_t)CAP_TYPE_MEM << CAP_TYPE_SHIFT) |
                          ((uint64_t)1 << CAP_OBJ_SHIFT) |
                          ((uint64_t)(CAP_PERM_R | CAP_PERM_W) << CAP_PERM_SHIFT));
    CHECK(k_dev_mmap(P, mem_slot, 0, 0, &vx) == CAP_ERR_TYPE,
          "MEM cap in the slot is CAP_ERR_TYPE");
    uint16_t dead = 13;
    cap_tables[cap_table_index(P)].slots[dead].word = 0;
    CHECK(k_dev_mmap(P, dead, 0, 0, &vx) == CAP_ERR_REVOKED,
          "empty (free) slot is CAP_ERR_REVOKED");
    CHECK(k_dev_mmap(P, 3000, 0, 0, &vx) == CAP_ERR_RANGE,
          "slot beyond CAP_TABLE_ENTRIES is CAP_ERR_RANGE");
    CHECK(k_dev_mmap(P, d, 0, 0, NULL) == CAP_ERR_PROTO,
          "NULL out pointer is CAP_ERR_PROTO");

    /* ── 8. bad flags ──────────────────────────────────────────────────── */
    CHECK(k_dev_mmap(P, d, 0x40, 0, &vx) == CAP_ERR_RANGE,
          "unknown flag bits are CAP_ERR_RANGE");

    /* ── 9. syscall wrapper ────────────────────────────────────────────── */
    struct SLSDevMmapRequest req;
    memset(&req, 0, sizeof(req));
    req.slot = d;
    req.vaddr_hint = 0x100006000ULL;
    CHECK(sys_sls_dev_mmap(&req) == CAP_ERR_OK, "sys_sls_dev_mmap wrapper ok");
    CHECK(req.out_vaddr == 0x100000000ULL,
          "wrapper reports the already-mapped vaddr (same cap)");
    CHECK(sys_sls_dev_mmap(NULL) == CAP_ERR_PROTO, "NULL request is CAP_ERR_PROTO");

    printf("all dev-mmap checks passed\n");
    return 0;
}