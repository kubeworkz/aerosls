/*
 * storage_quota_host_test.c — Storage Isolation Roadmap Phase 1
 * verification: a standalone host-buildable test for kernel/storage_
 * quota.c's new per-partition on-disk page quota accounting, linked
 * against the REAL, unmodified storage_quota.c — not a reimplementation.
 *
 * storage_quota.c's only dependency outside its own header is kernel_io.h's
 * two logging functions — same shallow surface frame_quota_host_test.c
 * already established for frame_pool.c, and this file deliberately mirrors
 * that test's scenario structure so the two accounting layers (RAM frames
 * vs. on-disk pages) are easy to compare side by side.
 *
 * This file proves storage_quota.c's own contract in isolation. The
 * combined rowstore+vecstore budget claim (storage_quota.h's own header
 * comment) follows directly from that contract, not from a separate
 * cross-file integration test: storage_page_reserve()/_release() are keyed
 * purely by partition_id, with no knowledge of which subsystem is calling
 * — any two callers passing the same partition_id are, by construction,
 * sharing one counter. rowstore_host_test.c and vecstore_host_test.c each
 * separately verify their OWN real call site wires the right partition_id
 * through to this API (denial-before-side-effect, rollback-on-frame-
 * failure) — see their own new scenarios for that half of the picture.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/storage_quota_host_test \
 *       tests/storage_quota_host_test.c kernel/storage_quota.c
 *   /tmp/storage_quota_host_test
 */
#include "kernel/storage_quota.h"
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
    /* ── Scenario 1: no quota configured -> pure pass-through, backward
     * compatible by construction (0 = unlimited, the BSS-zero default). ──── */
    CHECK(storage_get_page_quota(1) == 0, "scenario 1: partition 1 starts with quota 0 (unlimited), no init call needed");
    CHECK(storage_get_page_usage(1) == 0, "scenario 1: partition 1 starts with usage 0");
    for (int i = 0; i < 50; i++) {
        CHECK(storage_page_reserve(1) == 0, "scenario 1: unlimited-quota partition never denied (within this test's small volume)");
    }
    CHECK(storage_get_page_usage(1) == 50, "scenario 1: 50 successful reservations -> usage counter reads exactly 50");

    /* ── Scenario 2: a real quota is set and enforced, fails cleanly at the
     * boundary without mutating usage further. ───────────────────────────── */
    CHECK(storage_set_page_quota(3, 5) == 0, "scenario 2: setting partition 3's page quota to 5 succeeds");
    CHECK(storage_get_page_quota(3) == 5, "scenario 2: quota readback confirms 5");
    int successes = 0;
    for (int i = 0; i < 5; i++) {
        if (storage_page_reserve(3) == 0) successes++;
    }
    CHECK(successes == 5, "scenario 2: exactly 5 reservations succeed under a quota of 5");
    CHECK(storage_get_page_usage(3) == 5, "scenario 2: usage counter reads exactly 5 after hitting quota");
    CHECK(storage_page_reserve(3) == 1, "scenario 2: the 6th reservation is denied (over quota)");
    CHECK(storage_get_page_usage(3) == 5, "scenario 2: usage counter is UNCHANGED after the denied attempt — denial touches nothing");
    CHECK(storage_page_reserve(3) == 1, "scenario 2: denial is stable, not a one-shot glitch — a 7th attempt is denied too");

    /* ── Scenario 3: partitions' counters are independent. ────────────────── */
    CHECK(storage_get_page_usage(7) == 0, "scenario 3: an untouched partition (7) still reads usage 0, unaffected by partition 3's quota exhaustion");
    CHECK(storage_page_reserve(7) == 0, "scenario 3: partition 7 (no quota) reserves successfully while partition 3 is fully exhausted");
    CHECK(storage_get_page_usage(7) == 1, "scenario 3: partition 7's usage counter reflects only its own reservation, not partition 3's 5");

    /* ── Scenario 4: out-of-range partition ids fail closed everywhere. ───── */
    CHECK(storage_page_reserve(999) == 1, "scenario 4: an out-of-range partition_id (999) is denied, not silently mapped to some default");
    CHECK(storage_page_release(999, 1) == 1, "scenario 4: releasing against an out-of-range partition_id fails (1)");
    CHECK(storage_set_page_quota(999, 10) == 1, "scenario 4: setting a quota on an out-of-range partition_id fails (returns 1)");
    CHECK(storage_get_page_usage(999) == 0xFFFFFFFFFFFFFFFFULL, "scenario 4: usage query on an out-of-range partition_id returns the sentinel, not 0 (which would look like a valid empty partition)");
    CHECK(storage_get_page_quota(999) == 0xFFFFFFFFFFFFFFFFULL, "scenario 4: quota query on an out-of-range partition_id returns the sentinel too");

    /* ── Scenario 5: storage_page_release() — accounting-only decrement,
     * floored at 0, never underflows. ─────────────────────────────────────── */
    CHECK(storage_get_page_usage(4) == 0, "scenario 5: partition 4 starts clean");
    for (int i = 0; i < 3; i++) CHECK(storage_page_reserve(4) == 0, "scenario 5: partition 4 reserves 3 pages");
    CHECK(storage_get_page_usage(4) == 3, "scenario 5: usage is 3 after 3 reservations");
    CHECK(storage_page_release(4, 1) == 0, "scenario 5: releasing 1 page succeeds");
    CHECK(storage_get_page_usage(4) == 2, "scenario 5: usage drops to 2 after releasing 1");
    CHECK(storage_page_release(4, 100) == 0, "scenario 5: releasing MORE than the current usage still succeeds (floors, doesn't reject)");
    CHECK(storage_get_page_usage(4) == 0, "scenario 5: usage floors cleanly at 0, no underflow into a huge unsigned value");

    /* ── Scenario 6: reservation rollback pattern (what rowstore.c/vecstore.c
     * call on a subsequent RAM-frame allocation failure) — reserve then
     * immediately release 1, net effect is a no-op on the counter. ───────── */
    CHECK(storage_get_page_usage(5) == 0, "scenario 6: partition 5 starts clean");
    CHECK(storage_page_reserve(5) == 0, "scenario 6: partition 5 reserves a page");
    CHECK(storage_get_page_usage(5) == 1, "scenario 6: usage is 1 right after the reservation");
    CHECK(storage_page_release(5, 1) == 0, "scenario 6: rolling back that exact reservation succeeds");
    CHECK(storage_get_page_usage(5) == 0, "scenario 6: usage is back to 0 -- the rollback pattern is a true no-op, matching rowstore_alloc_page()/vecstore_alloc_page()'s own failure path");

    /* ── Scenario 7: the syscall wrapper matches the direct-call API. ─────── */
    struct SLSPartitionStorageQuotaSetRequest req = { .partition_id = 9, .page_quota = 42 };
    CHECK(sys_sls_partition_storage_quota_set(&req) == 0, "scenario 7: sys_sls_partition_storage_quota_set wrapper succeeds for a valid request");
    CHECK(storage_get_page_quota(9) == 42, "scenario 7: the wrapper's effect matches calling storage_set_page_quota(9, 42) directly");
    CHECK(sys_sls_partition_storage_quota_set(0) == 1, "scenario 7: a NULL request is rejected (returns 1), not dereferenced");

    /* ── Scenario 8: 0 explicitly re-uncaps a previously-limited partition
     * (not just an omission default), same convention frame_pool.h's own
     * frame_quota re-uncap documents. ──────────────────────────────────────── */
    CHECK(storage_page_reserve(3) == 1, "scenario 8: partition 3 (quota 5, usage 5 from scenario 2) is still denied before the re-uncap");
    CHECK(storage_set_page_quota(3, 0) == 0, "scenario 8: explicitly setting partition 3's quota to 0 succeeds");
    CHECK(storage_get_page_quota(3) == 0, "scenario 8: quota readback now reads 0 (unlimited)");
    CHECK(storage_page_reserve(3) == 0, "scenario 8: partition 3 can reserve again now that it's genuinely uncapped, not just re-reading a stale denial");

    /* ── Scenario 9: the debug/introspection listing runs without crashing
     * against a mix of zero, nonzero-usage, and nonzero-quota partitions --
     * mirrors sys_sls_partition_quota_list()'s own smoke-test coverage. ──── */
    sys_sls_partition_storage_quota_list();
    CHECK(1, "scenario 9: sys_sls_partition_storage_quota_list() runs to completion without crashing");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
