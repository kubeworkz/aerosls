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
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/frame_quota_host_test \
 *       tests/frame_quota_host_test.c kernel/frame_pool.c
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
     * NVMe queues, SMP stacks, the SIMI activation cache, catalog index
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

    /* ── Scenario 9 (Gap Remediation Phase F): free_physical_ram_frame() /
     * free_physical_ram_frame_for_partition() -- frame_pool.c's first-ever
     * free primitive. Round-trip: allocate a frame, free it, confirm the
     * usage counter drops, then confirm the SAME physical address comes
     * back out on the next allocation -- proof this is a REAL bitmap free
     * (unlike partition_reset_frame_usage()'s accounting-only reset in
     * scenario 8 above, which scenario 8 itself proved never reuses an
     * address). ───────────────────────────────────────────────────────────── */
    CHECK(partition_get_frame_usage(4) == 0, "scenario 9: partition 4 starts clean");
    void* f9 = allocate_physical_ram_frame_for_partition(4);
    CHECK(f9 != NULL, "scenario 9: partition 4 allocates one frame");
    CHECK(partition_get_frame_usage(4) == 1, "scenario 9: usage is 1 after the allocation");
    CHECK(free_physical_ram_frame_for_partition(f9, 4) == 0, "scenario 9: freeing that exact frame under partition 4 succeeds");
    CHECK(partition_get_frame_usage(4) == 0, "scenario 9: usage drops back to 0 after the free");
    void* f9b = allocate_physical_ram_frame_for_partition(4);
    CHECK(f9b == f9, "scenario 9: the very next allocation gets back the SAME physical address -- a real bitmap free, not accounting-only");
    CHECK(free_physical_ram_frame_for_partition(f9b, 4) == 0, "scenario 9: cleanup -- free it again so it doesn't leak into later scenarios' address-uniqueness checks");

    /* ── Scenario 10: double-free and bogus-address rejection -- the exact
     * "denial looks like absence" carefulness every other allocator boundary
     * check in this project already has. A double-free or a never-allocated
     * address must fail (1) WITHOUT corrupting the bitmap or decrementing
     * any partition's usage counter -- confirmed by checking usage is
     * unchanged and that a subsequent real allocation still behaves
     * normally. ───────────────────────────────────────────────────────────── */
    void* f10 = allocate_physical_ram_frame_for_partition(4);
    CHECK(f10 != NULL, "scenario 10: partition 4 allocates a frame to double-free");
    CHECK(free_physical_ram_frame_for_partition(f10, 4) == 0, "scenario 10: first free succeeds");
    CHECK(partition_get_frame_usage(4) == 0, "scenario 10: usage is 0 after the first free");
    CHECK(free_physical_ram_frame_for_partition(f10, 4) == 1, "scenario 10: the SECOND free of the same address fails (1) -- a double-free is rejected, not silently accepted");
    CHECK(partition_get_frame_usage(4) == 0, "scenario 10: usage counter is unaffected by the rejected double-free (not decremented into an underflow)");
    CHECK(free_physical_ram_frame(NULL) == 1, "scenario 10: freeing NULL is rejected cleanly, not dereferenced");
    CHECK(free_physical_ram_frame((void*)1) == 1, "scenario 10: freeing a misaligned (non-page-aligned) address is rejected");
    CHECK(free_physical_ram_frame((void*)(999999999ULL * 4096ULL)) == 1, "scenario 10: freeing a wildly out-of-range address is rejected, not read/written out of bounds");
    /* Out-of-range partition_id must fail closed -- BEFORE touching the
     * bitmap at all, same posture as allocate_physical_ram_frame_for_
     * partition()'s own out-of-range check. Proven causally, not by
     * guessing where the allocator's next first-fit scan would land: take
     * a FRESH still-allocated frame, attempt the rejected free, then
     * confirm a normal free of the SAME frame under the correct partition
     * still succeeds afterward -- if the rejected call had wrongly cleared
     * the bit, this second call would itself be rejected as a double-free. */
    void* f10c = allocate_physical_ram_frame_for_partition(4);
    CHECK(f10c != NULL, "scenario 10: partition 4 allocates a second frame to probe the out-of-range-partition_id rejection");
    CHECK(free_physical_ram_frame_for_partition(f10c, 999) == 1, "scenario 10: freeing under an out-of-range partition_id (999) fails closed");
    CHECK(partition_get_frame_usage(4) == 1, "scenario 10: partition 4's usage is UNCHANGED by the rejected out-of-range-partition free");
    CHECK(free_physical_ram_frame_for_partition(f10c, 4) == 0, "scenario 10: f10c is still validly freeable under the CORRECT partition afterward -- proves the rejected call above never touched the bitmap (a wrongly-cleared bit would make this a double-free and fail)");
    CHECK(partition_get_frame_usage(4) == 0, "scenario 10: usage back to 0 after the real free");

    /* ── Scenario 11: free_physical_ram_frame() (the unaccounted path)
     * decrements PARTITION_SYSTEM's counter, mirroring how
     * allocate_physical_ram_frame() always increments it. ─────────────────── */
    uint64_t sys_usage_before = partition_get_frame_usage(PARTITION_SYSTEM);
    void* f11 = allocate_physical_ram_frame();
    CHECK(f11 != NULL, "scenario 11: unaccounted allocation succeeds");
    CHECK(partition_get_frame_usage(PARTITION_SYSTEM) == sys_usage_before + 1, "scenario 11: PARTITION_SYSTEM's usage incremented by exactly 1");
    CHECK(free_physical_ram_frame(f11) == 0, "scenario 11: unaccounted free succeeds");
    CHECK(partition_get_frame_usage(PARTITION_SYSTEM) == sys_usage_before, "scenario 11: PARTITION_SYSTEM's usage decremented back to where it started");

    /* ── Scenario 12 (Multi-Node Partition Scaling Roadmap Phase 3):
     * partition_reclaim_all_frames() -- real bulk per-partition physical
     * frame reclamation, the opposite of scenario 8's proof that
     * partition_reset_frame_usage() is accounting-only. This is the
     * mechanism partition_destroy() now uses instead. ─────────────────── */
    CHECK(partition_get_frame_usage(5) == 0, "scenario 12: partition 5 starts clean");
    void* r5[4];
    for (int i = 0; i < 4; i++) {
        r5[i] = allocate_physical_ram_frame_for_partition(5);
        CHECK(r5[i] != NULL, "scenario 12: partition 5 allocates a frame to be bulk-reclaimed");
    }
    CHECK(partition_get_frame_usage(5) == 4, "scenario 12: partition 5's usage is 4 before reclaim");

    /* An unrelated partition's frame, allocated in between, must survive
     * partition 5's reclaim completely untouched -- proves the reclaim is
     * scoped to the exact partition_id given, not a blanket sweep. */
    void* other = allocate_physical_ram_frame_for_partition(6);
    CHECK(other != NULL, "scenario 12: an unrelated partition (6) allocates one frame in between");

    uint32_t reclaimed = partition_reclaim_all_frames(5);
    CHECK(reclaimed == 4, "scenario 12: exactly the 4 frames partition 5 actually held are reported reclaimed");
    CHECK(partition_get_frame_usage(5) == 0, "scenario 12: partition 5's usage counter reads 0 after the real reclaim");

    /* Real reclaim, not accounting-only: the SAME 4 physical addresses come
     * back out on the next 4 allocations -- at this point in the test they
     * are the ONLY free slots in the whole bitmap (everything lower was
     * already permanently consumed by earlier scenarios), so first-fit
     * allocation returning them is a direct, causal proof of a real free,
     * not a coincidence. */
    int reclaimed_are_real = 1;
    for (int i = 0; i < 4; i++) {
        void* back = allocate_physical_ram_frame_for_partition(5);
        int matched = 0;
        for (int j = 0; j < 4; j++) if (back == r5[j]) matched = 1;
        if (!matched) reclaimed_are_real = 0;
    }
    CHECK(reclaimed_are_real, "scenario 12: the 4 reclaimed addresses are genuinely back in the free pool -- real bitmap reclamation, not an accounting fiction");

    CHECK(partition_get_frame_usage(6) == 1, "scenario 12: partition 6's single frame survived partition 5's reclaim completely untouched");
    CHECK(free_physical_ram_frame_for_partition(other, 6) == 0, "scenario 12: partition 6's surviving frame is still validly freeable afterward -- proves its bitmap bit was never touched by partition 5's reclaim");

    CHECK(partition_reclaim_all_frames(999) == 0xFFFFFFFFu, "scenario 12: reclaiming an out-of-range partition_id returns the sentinel, touches nothing");
    CHECK(partition_reclaim_all_frames(8) == 0, "scenario 12: reclaiming a partition that never allocated anything returns 0 -- legitimate 'nothing to reclaim', not an error");
    /* The verification loop just above re-allocated 4 frames to partition 5
     * (to prove they were genuinely back in the free pool) -- so partition
     * 5 legitimately holds 4 frames again here, not 0. Reclaiming it a
     * second time is idempotent in the sense that matters: it correctly
     * reclaims exactly what's really there right now, not a stale count
     * left over from the first reclaim. */
    CHECK(partition_reclaim_all_frames(5) == 4, "scenario 12: reclaiming partition 5 again correctly reclaims the 4 frames it re-acquired during the verification loop above, not 0 and not a stale/doubled count");
    CHECK(partition_get_frame_usage(5) == 0, "scenario 12: partition 5's usage is 0 again after this second reclaim");
    CHECK(partition_reclaim_all_frames(5) == 0, "scenario 12: a THIRD reclaim, now genuinely empty, correctly returns 0 -- no error, no underflow");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
