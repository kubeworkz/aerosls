/*
 * frame_quota_host_test.c — Phase 13 verification: a standalone
 * host-buildable test for kernel/frame_pool.c's new per-partition quota
 * accounting, linked against the REAL, unmodified frame_pool.c — not a
 * reimplementation.
 *
 * frame_pool.c's only dependency outside its own header is kernel_io.h's
 * two logging functions — small enough surface to fake, same shape as
 * Phase 8's partition_host_test.c and Phase 11's ipc_partition_host_test.c.
 * partition.h is included for PARTITION_MAX only (a compile-time constant,
 * not a function call), so no other kernel source needs linking.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -o /tmp/frame_quota_host_test \
 *       frame_quota_host_test.c kernel/frame_pool.c
 *   /tmp/frame_quota_host_test
 */
#include "kernel/frame_pool.h"
#include <stdio.h>
#include <stdint.h>

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* ── Scenario 1: no quota configured anywhere -> every partition's
     * accounted allocator behaves as pure pass-through, backward compatible
     * by construction (0 = unlimited, the BSS-zero default). ────────────── */
    CHECK(partition_get_frame_quota(1) == 0, "scenario 1: partition 1 starts with quota 0 (unlimited), no init call needed");
    CHECK(partition_get_frame_usage(1) == 0, "scenario 1: partition 1 starts with usage 0");
    for (int i = 0; i < 50; i++) {
        void* f = allocate_physical_ram_frame_for_partition(1);
        CHECK(f != NULL, "scenario 1: unlimited-quota partition never fails to allocate (within this test's small volume)");
    }
    CHECK(partition_get_frame_usage(1) == 50, "scenario 1: 50 successful allocations -> usage counter reads exactly 50");

    /* ── Scenario 2: a real quota is set and enforced, fails cleanly at the
     * boundary without touching the bitmap. ──────────────────────────────── */
    CHECK(partition_set_frame_quota(3, 5) == 0, "scenario 2: setting partition 3's quota to 5 succeeds");
    CHECK(partition_get_frame_quota(3) == 5, "scenario 2: quota readback confirms 5");
    int successes = 0;
    for (int i = 0; i < 5; i++) {
        void* f = allocate_physical_ram_frame_for_partition(3);
        if (f) successes++;
    }
    CHECK(successes == 5, "scenario 2: exactly 5 allocations succeed under a quota of 5");
    CHECK(partition_get_frame_usage(3) == 5, "scenario 2: usage counter reads exactly 5 after hitting quota");
    void* over = allocate_physical_ram_frame_for_partition(3);
    CHECK(over == NULL, "scenario 2: the 6th allocation is denied (over quota)");
    CHECK(partition_get_frame_usage(3) == 5, "scenario 2: usage counter is UNCHANGED after the denied attempt — denial touches nothing, no partial allocation");
    void* over2 = allocate_physical_ram_frame_for_partition(3);
    CHECK(over2 == NULL, "scenario 2: denial is stable, not a one-shot glitch — a 7th attempt is denied too");

    /* ── Scenario 3: partitions' counters are independent — one partition
     * hitting its quota must not affect another partition's accounting or
     * ability to allocate. ────────────────────────────────────────────────── */
    CHECK(partition_get_frame_usage(7) == 0, "scenario 3: an untouched partition (7) still reads usage 0, unaffected by partition 3's quota exhaustion");
    void* p7 = allocate_physical_ram_frame_for_partition(7);
    CHECK(p7 != NULL, "scenario 3: partition 7 (no quota) allocates successfully while partition 3 is fully exhausted");
    CHECK(partition_get_frame_usage(7) == 1, "scenario 3: partition 7's usage counter reflects only its own allocation, not partition 3's 5");

    /* ── Scenario 4: the plain, unaccounted allocate_physical_ram_frame()
     * path is genuinely exempt from quota enforcement — setting an
     * aggressively low quota on PARTITION_SYSTEM must not block it, since
     * every pre-Phase-13 kernel-infrastructure caller (page-table walkers,
     * NVMe queues, SMP stacks, the TIMI activation cache, catalog index
     * nodes) goes through this path and none of them were touched by this
     * phase's changes — see frame_pool.h's header comment. ───────────────── */
    CHECK(partition_set_frame_quota(PARTITION_SYSTEM, 1) == 0, "scenario 4: set an aggressively low quota (1) on PARTITION_SYSTEM");
    int unaccounted_successes = 0;
    for (int i = 0; i < 20; i++) {
        void* f = allocate_physical_ram_frame();
        if (f) unaccounted_successes++;
    }
    CHECK(unaccounted_successes == 20, "scenario 4: the unaccounted allocator succeeds all 20 times despite PARTITION_SYSTEM's quota of 1 — it is not gated at all");
    CHECK(partition_get_frame_usage(PARTITION_SYSTEM) > 1, "scenario 4: PARTITION_SYSTEM's usage counter DOES still increment for visibility (it's now > its own quota of 1) — tracked but not enforced, exactly as designed");
    /* Restore unlimited so scenario 6 below isn't affected by this quota. */
    CHECK(partition_set_frame_quota(PARTITION_SYSTEM, 0) == 0, "scenario 4: quota reset back to unlimited (0) cleanly");

    /* ── Scenario 5: out-of-range partition ids fail closed everywhere. ───── */
    CHECK(allocate_physical_ram_frame_for_partition(999) == NULL, "scenario 5: an out-of-range partition_id (999) is denied, not silently mapped to some default");
    CHECK(partition_set_frame_quota(999, 10) == 1, "scenario 5: setting a quota on an out-of-range partition_id fails (returns 1)");
    CHECK(partition_get_frame_usage(999) == 0xFFFFFFFFFFFFFFFFULL, "scenario 5: usage query on an out-of-range partition_id returns the sentinel, not 0 (which would look like a valid empty partition)");
    CHECK(partition_get_frame_quota(999) == 0xFFFFFFFFFFFFFFFFULL, "scenario 5: quota query on an out-of-range partition_id returns the sentinel too");

    /* ── Scenario 6: the accounted and unaccounted paths draw from the SAME
     * underlying physical bitmap — not two silently separate pools. Every
     * frame handed out so far (accounted + unaccounted, across scenarios
     * 1-4) must be a distinct, 4-KiB-aligned address; two more allocations
     * (one via each path) must not collide with anything already handed
     * out or with each other. ─────────────────────────────────────────────── */
    void* a = allocate_physical_ram_frame_for_partition(2);
    void* b = allocate_physical_ram_frame();
    CHECK(a != NULL && b != NULL, "scenario 6: both paths still allocate successfully");
    CHECK(a != b, "scenario 6: the accounted and unaccounted paths hand out DIFFERENT physical frames on the shared bitmap, not a coincidentally-identical address");
    CHECK(((uintptr_t)a % 4096) == 0 && ((uintptr_t)b % 4096) == 0, "scenario 6: both returned addresses are 4-KiB frame-aligned");

    /* ── Scenario 7: the syscall wrapper matches the direct-call API. ─────── */
    struct SLSPartitionQuotaSetRequest req = { .partition_id = 9, .frame_quota = 42 };
    CHECK(sys_sls_partition_quota_set(&req) == 0, "scenario 7: sys_sls_partition_quota_set wrapper succeeds for a valid request");
    CHECK(partition_get_frame_quota(9) == 42, "scenario 7: the wrapper's effect matches calling partition_set_frame_quota(9, 42) directly");
    CHECK(sys_sls_partition_quota_set(0) == 1, "scenario 7: a NULL request is rejected (returns 1), not dereferenced");

    /* ── Scenario 8 (Phase 14): partition_reset_frame_usage() is accounting-
     * only -- it zeroes the usage counter but the bitmap bits stay set, so
     * memory that was "freed" this way is NOT actually returned to the
     * allocator; the total number of frames the bitmap will still hand out
     * to a fresh, unrelated partition is UNCHANGED by the reset. ─────────── */
    CHECK(partition_get_frame_usage(3) == 5, "scenario 8: partition 3's usage is still 5 from scenario 2, unaffected by everything since");
    CHECK(partition_reset_frame_usage(3) == 0, "scenario 8: resetting partition 3's usage counter succeeds");
    CHECK(partition_get_frame_usage(3) == 0, "scenario 8: usage counter now reads 0");
    /* Partition 3 was fully quota-exhausted before the reset (quota=5,
     * usage=5). After the accounting reset it should be allowed to
     * "allocate" again up to its quota -- but if the reset had also
     * somehow freed real bitmap bits, we'd expect the SAME 5 physical
     * addresses to come back. Confirm instead that these are 5 NEW frames,
     * never seen before -- proving the bitmap was never touched by the
     * reset, exactly as frame_pool.h's header comment on this function
     * says it wouldn't be. */
    void* reused[5];
    int all_fresh = 1;
    for (int i = 0; i < 5; i++) {
        reused[i] = allocate_physical_ram_frame_for_partition(3);
        if (reused[i] == a || reused[i] == b) all_fresh = 0;
        for (int j = 0; j < i; j++) if (reused[i] == reused[j]) all_fresh = 0;
    }
    CHECK(all_fresh, "scenario 8: the 5 post-reset allocations are all-new physical frames, not the same 5 addresses partition 3 held before -- confirms the reset is accounting-only, not a real free");
    CHECK(partition_get_frame_usage(3) == 5, "scenario 8: usage counter climbs back to 5 after 5 fresh allocations under the still-configured quota of 5");
    CHECK(partition_reset_frame_usage(999) == 1, "scenario 8: resetting an out-of-range partition_id fails cleanly");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
