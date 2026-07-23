/*
 * tier_capacity_phase5b_host_test.c — Navigator-Parity Gap Roadmap Phase 5b
 * verification: a standalone host-buildable test for kernel/tier_mgr.c's new
 * tier_capacity_totals(), executed as the REAL, unmodified function — not a
 * reimplementation.
 *
 * tier_capacity_totals() is a pure function of object_catalog[]/
 * object_catalog_count (no side effects, doesn't touch tier_stats[] or
 * anything IPC-related), so this test defines those two globals directly
 * (same "define the extern globals ourselves instead of linking the real,
 * much heavier object_catalog.c" approach tests/scheduler_fairness_host_
 * test.c already established for process.c) rather than linking the full
 * object_catalog.c dependency chain. kernel/tier_mgr.c is linked for real;
 * its other functions (tier_mgr_tick, sys_sls_tier_promote/demote/list)
 * reference kernel_serial_print/printf and ipc_post, which must still
 * resolve at link time even though this test never calls them — stubbed
 * below, same as every other host test in this suite that links a file with
 * unrelated print/IPC dependencies.
 *
 * Storage Isolation Roadmap Phase 2 added a real call into kernel/storage_
 * quota.c's storage_get_page_usage()/storage_get_page_quota() inside tier_
 * mgr.c's sys_sls_disk_status() (same translation unit as tier_capacity_
 * totals(), so it's a new link-time dependency here even though this test
 * never calls sys_sls_disk_status() itself in most scenarios) -- linked for
 * real below, same posture as every other file in this suite that picked
 * up storage_quota.c as a dependency in Phase 1.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/tier_capacity_phase5b_host_test \
 *       tests/tier_capacity_phase5b_host_test.c kernel/tier_mgr.c kernel/storage_quota.c
 *   /tmp/tier_capacity_phase5b_host_test
 */
#include "kernel/tier_mgr.h"
#include "kernel/object_catalog.h"
#include "kernel/ipc.h"
#include "kernel/storage_quota.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ─── object_catalog.c stand-in: real globals, not the real (much heavier)
// implementation file — tier_capacity_totals() only ever reads these two
// arrays directly. ───────────────────────────────────────────────────────────
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;

// ─── kernel_io.h / ipc.h stand-ins — tier_mgr.c's OTHER functions
// (tier_mgr_tick, sys_sls_tier_promote/demote/list) reference these; never
// actually called by this test, but must resolve at link time. ────────────
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
int ipc_post(uint16_t port, const struct IPCMessage* msg) { (void)port; (void)msg; return 0; }

// ─── drivers/nvme_admin.c stand-in — Navigator-Parity Gap Roadmap Phase 5c
// added a real call to nvme_get_capacity_bytes() inside tier_mgr.c's new
// sys_sls_disk_status() (linked here since it's in the same translation
// unit as tier_capacity_totals()), but this test never calls that function
// -- only tier_capacity_totals() itself, which has no NVMe dependency at
// all. Stubbed purely to satisfy the linker, same "never actually called by
// this test's flow" posture as the other stand-ins above.
uint64_t nvme_get_capacity_bytes(void) { return 0; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static void seed_object(int slot, const char* name, SLSStorageTier tier,
                        uint32_t size_pages, uint8_t active) {
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].storage_tier = tier;
    object_catalog[slot].size_pages   = size_pages;
    object_catalog[slot].active       = active;
}

int main(void) {
    /* ── Scenario 1: empty catalog -- everything zeroed, not garbage. ─────── */
    object_catalog_count = 0;
    uint64_t bytes[TIER_MGR_TIER_COUNT];
    uint32_t counts[TIER_MGR_TIER_COUNT];
    // Poison both arrays first so a callee that forgets to zero them would
    // be caught, not accidentally masked by them already being zero.
    for (int t = 0; t < TIER_MGR_TIER_COUNT; t++) { bytes[t] = 0xDEADBEEFULL; counts[t] = 0xDEADBEEFu; }
    tier_capacity_totals(bytes, counts);
    int all_zero1 = 1;
    for (int t = 0; t < TIER_MGR_TIER_COUNT; t++) if (bytes[t] != 0 || counts[t] != 0) all_zero1 = 0;
    CHECK(all_zero1, "s1: empty catalog -- every tier's bytes/count is zeroed, not left as poison or garbage");

    /* ── Scenario 2: one object per tier, simple byte math. ──────────────── */
    seed_object(0, "l1_obj", STORAGE_TIER_L1_CACHE, 2, 1);   // 2 pages = 8192 bytes
    seed_object(1, "l2_obj", STORAGE_TIER_L2_DRAM,  10, 1);  // 10 pages = 40960 bytes
    seed_object(2, "l3_obj", STORAGE_TIER_L3_SSD,   1, 1);   // 1 page = 4096 bytes
    object_catalog_count = 3;
    tier_capacity_totals(bytes, counts);
    CHECK(bytes[STORAGE_TIER_L1_CACHE] == 2ULL * 4096, "s2: L1_CACHE bytes_used = 2 pages * 4096");
    CHECK(bytes[STORAGE_TIER_L2_DRAM]  == 10ULL * 4096, "s2: L2_DRAM bytes_used = 10 pages * 4096");
    CHECK(bytes[STORAGE_TIER_L3_SSD]   == 1ULL * 4096, "s2: L3_SSD bytes_used = 1 page * 4096");
    CHECK(counts[STORAGE_TIER_L1_CACHE] == 1 && counts[STORAGE_TIER_L2_DRAM] == 1 && counts[STORAGE_TIER_L3_SSD] == 1,
          "s2: each tier's object_count is exactly 1");

    /* ── Scenario 3: multiple objects in the same tier sum correctly, and an
     * inactive object is excluded entirely (not counted, not summed). ────── */
    seed_object(3, "l1_obj2", STORAGE_TIER_L1_CACHE, 5, 1);   // another L1 object, +5 pages
    seed_object(4, "l1_ghost", STORAGE_TIER_L1_CACHE, 999, 0); // inactive -- must be excluded
    object_catalog_count = 5;
    tier_capacity_totals(bytes, counts);
    CHECK(bytes[STORAGE_TIER_L1_CACHE] == (2ULL + 5ULL) * 4096,
          "s3: L1_CACHE sums both active objects (2+5 pages), ignoring the inactive 999-page ghost");
    CHECK(counts[STORAGE_TIER_L1_CACHE] == 2, "s3: L1_CACHE object_count is 2 -- the inactive slot doesn't count");
    CHECK(bytes[STORAGE_TIER_L2_DRAM] == 10ULL * 4096 && counts[STORAGE_TIER_L2_DRAM] == 1,
          "s3: L2_DRAM totals are completely unaffected by L1's changes");

    /* ── Scenario 4: re-running after object_catalog_count shrinks back down
     * (simulating objects having been freed) -- totals reflect ONLY what's
     * currently active/in-range, proving this is a live recomputation every
     * call, not an accumulating counter that would double-count over time. ── */
    object_catalog_count = 3;   // back to the scenario-2 seed only
    tier_capacity_totals(bytes, counts);
    CHECK(bytes[STORAGE_TIER_L1_CACHE] == 2ULL * 4096 && counts[STORAGE_TIER_L1_CACHE] == 1,
          "s4: shrinking object_catalog_count back down drops the scenario-3 addition -- no stale accumulation");

    /* ── Scenario 5: Storage Isolation Roadmap Phase 2 -- sys_sls_disk_status()
     * now also walks storage_quota.c's real per-partition page counters and
     * prints a byte-level breakdown. kernel_serial_printf is stubbed to a
     * no-op above (same as every other host test in this suite -- there's no
     * serial-capture mechanism to assert against), so this is a smoke test:
     * it proves the new code path links and runs to completion against a mix
     * of zero-usage, real-usage, and quota-configured partitions without
     * crashing, mirroring storage_quota_host_test.c's own Scenario 9 for
     * sys_sls_partition_storage_quota_list(). ─────────────────────────────── */
    CHECK(storage_set_page_quota(2, 10) == 0, "s5: partition 2 gets a page quota of 10 (bytes = 40960)");
    CHECK(storage_page_reserve(2) == 0, "s5: partition 2 reserves 1 page (bytes_used = 4096)");
    CHECK(storage_page_reserve(6) == 0, "s5: partition 6 reserves 1 page with no quota configured (unlimited)");
    sys_sls_disk_status();
    CHECK(1, "s5: sys_sls_disk_status() runs to completion with real storage_quota.c data linked in, no crash");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
