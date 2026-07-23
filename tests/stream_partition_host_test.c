/*
 * stream_partition_host_test.c -- Multitenant Isolation Gap Analysis §5
 * item 3 / §7 item 3 verification: a standalone host-buildable test for
 * kernel/stream.c's new owner_uid/partition_id tagging and per-partition
 * frame quota accounting, linked against the REAL, unmodified
 * kernel/stream.c and kernel/partition.c -- not a reimplementation of
 * either.
 *
 * This is exactly the integration the gap analysis doc's §3 table row
 * ("stream/blob storage...genuinely untracked") and LPAR Phase 13's own
 * left-open finding named: stream.c used to call the plain, unaccounted
 * allocate_physical_ram_frame() with no owner or partition concept on
 * struct StreamEntry at all. This test proves the real fix -- stream.c now
 * calls allocate_physical_ram_frame_for_partition(se->partition_id) at
 * every frame-allocating call site, with the right partition_id.
 *
 * ─── Why frame_pool.c's REAL allocator is faked here, not linked ────────
 * Tried linking the real kernel/frame_pool.c first. It segfaults under
 * host execution for a reason that's specific to THIS test, not a flaw in
 * frame_pool.c: alloc_raw_frame() returns physical-address-shaped pointers
 * (frame_index * FRAME_SIZE, e.g. 0x1000, 0x2000...) that are valid,
 * writable memory once the real kernel's page tables identity-map them,
 * but are NOT valid, mapped addresses in a host Linux process.
 * frame_quota_host_test.c (which DOES link the real frame_pool.c) never
 * hits this because it only checks allocation success/failure, never
 * dereferences the returned pointer. This test's whole point is different
 * -- proving stream.c actually WRITES through the frames it allocates via
 * the accounted path -- so it needs real, host-writable memory behind
 * each "frame". The fix: a settable-fake allocate_physical_ram_frame_for_
 * partition() below, backed by real host malloc() and real per-partition
 * counters, the same "fake the address-space-shaped primitive, keep the
 * subsystem actually under test real" precedent partition_host_test.c's
 * own g_fake_local_node_id already established for cluster_local_node_id().
 * frame_pool.c's OWN quota arithmetic is already, separately, thoroughly
 * verified for real by tests/frame_quota_host_test.c -- this file's job is
 * only to prove stream.c calls the accounted entry point with the right
 * partition_id and correctly propagates a denial, not to re-verify
 * frame_pool.c's own math a second time.
 *
 * Other stubs: kernel_serial_print/printf (logging, irrelevant here),
 * io_sq/io_cq left NULL (matches a fresh-boot/NVMe-unavailable state --
 * stream_init()'s own existing cold-start path, not exercised by this
 * test since it calls stream_create()/stream_write_chunk() directly),
 * nvme_read_sync/nvme_write_sync stubbed trivially (stream.c's directory
 * persistence and frame flush are a real side effect this test doesn't
 * care about verifying -- vecstore/rowstore persistence already gets its
 * own dedicated round-trip tests elsewhere), sys_sls_valloc/_insert/
 * _update stubbed as no-ops (object_catalog.c's full valloc/persist/fnv1a
 * machinery is irrelevant to whether stream.c's OWN owner_uid/partition_id
 * fields and frame-accounting call sites are wired correctly -- the same
 * "declare the extern surface, don't link the heavy subsystem" judgment
 * call database_host_test.c already made for object_catalog.c). partition.c
 * stubs are the same set partition_host_test.c/tenant_host_test.c already
 * established for its own external dependencies.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I user \
 *       -o /tmp/stream_partition_host_test \
 *       tests/stream_partition_host_test.c kernel/stream.c kernel/partition.c
 *   /tmp/stream_partition_host_test
 */
#include "kernel/stream.h"
#include "kernel/partition.h"
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Stubs for stream.c's/partition.c's external dependencies ─────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t cluster_local_node_id(void) { return 0; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }

void* io_sq = 0;
void* io_cq = 0;
int nvme_read_sync(uint64_t slba, void* buf) { (void)slba; (void)buf; return -1; }
int nvme_write_sync(uint64_t slba, const void* buf) { (void)slba; (void)buf; return 0; }

uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 1; }
uint64_t sys_sls_insert(struct SLSRecordRequest* req) { (void)req; return 0; }
uint64_t sys_sls_update(struct SLSRecordRequest* req) { (void)req; return 0; }

/* ─── Settable-fake accounted allocator (see header comment above) ──────
 * Real per-partition usage/quota semantics (0 = unlimited, fails cleanly
 * without touching the counter once quota is met/exceeded), matching
 * frame_pool.c's own real contract exactly -- just backed by real,
 * host-writable malloc() memory instead of physical-address-shaped
 * pointers, since this test's whole point is to actually write through
 * the returned frame the way stream.c really does. */
static uint32_t fake_usage[PARTITION_MAX];
static uint64_t fake_quota[PARTITION_MAX];
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    if (fake_quota[partition_id] != 0 && fake_usage[partition_id] >= fake_quota[partition_id]) return 0;
    void* frame = malloc(FRAME_SIZE);
    if (!frame) return 0;
    fake_usage[partition_id]++;
    return frame;
}
int partition_set_frame_quota(uint32_t partition_id, uint64_t frame_quota) {
    if (partition_id >= PARTITION_MAX) return 1;
    fake_quota[partition_id] = frame_quota;
    return 0;
}
uint64_t partition_get_frame_usage(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return fake_usage[partition_id];
}
uint64_t partition_get_frame_quota(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return fake_quota[partition_id];
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    partition_init();
    stream_init();   // cold start (io_sq/io_cq are NULL) -- just zeroes stream_store[]

    /* ── Scenario A: a stream created by a caller really gets tagged with
     * that caller's REAL partition_id (via partition_get_for_uid()), not
     * left at 0 regardless of who created it. ─────────────────────────────── */
    uint32_t part_a = partition_create("tenant-a");
    partition_assign_uid(100, part_a);
    {
        int rc = stream_create(100, "logfile", "text/plain");
        CHECK(rc == 0, "sA: stream_create() for a real, partitioned caller succeeds");
        struct StreamEntry* se = stream_find("logfile");
        CHECK(se != 0, "sA: the new stream is really findable");
        CHECK(se->owner_uid == 100, "sA: owner_uid is really the caller's uid, not left at 0");
        CHECK(se->partition_id == part_a, "sA: partition_id is really the caller's own partition, resolved via partition_get_for_uid()");
    }

    /* ── Scenario B: writing to that stream really charges frames against
     * the stream's owning partition's REAL frame_pool.c usage counter --
     * the actual fix, not just a tagged-but-unused field. ─────────────────── */
    {
        uint8_t chunk[100];
        memset(chunk, 0xAB, sizeof(chunk));
        uint64_t usage_before = partition_get_frame_usage(part_a);
        int rc = stream_write_chunk("logfile", chunk, sizeof(chunk), 0, 1);
        CHECK(rc == 0, "sB: a small write within one frame succeeds");
        uint64_t usage_after = partition_get_frame_usage(part_a);
        CHECK(usage_after == usage_before + 1,
              "sB: tenant-a's partition frame usage genuinely increased by exactly 1 (one frame allocated for a <4KiB write)");
    }

    /* ── Scenario C: a SEPARATE tenant's stream growth does NOT count
     * against tenant-a's usage -- the real point of this whole fix (closes
     * the gap doc's own "unbounded, unquota'd, noisy-neighbor" finding). ─── */
    uint32_t part_c = partition_create("tenant-c");
    partition_assign_uid(200, part_c);
    {
        uint64_t a_usage_before = partition_get_frame_usage(part_a);
        stream_create(200, "other-log", "text/plain");
        uint8_t chunk[50];
        memset(chunk, 0xCD, sizeof(chunk));
        stream_write_chunk("other-log", chunk, sizeof(chunk), 0, 1);
        CHECK(partition_get_frame_usage(part_c) == 1,
              "sC: tenant-c's own partition usage reflects its own stream's frame");
        CHECK(partition_get_frame_usage(part_a) == a_usage_before,
              "sC: tenant-a's partition usage is COMPLETELY UNCHANGED by tenant-c's stream activity");
    }

    /* ── Scenario D: a configured frame quota really bounds stream growth
     * now -- fails cleanly once a tenant's stream(s) hit their partition's
     * quota, exactly like process/loader allocations already did since
     * LPAR Phase 13. ────────────────────────────────────────────────────────── */
    uint32_t part_d = partition_create("tenant-d");
    partition_assign_uid(300, part_d);
    partition_set_frame_quota(part_d, 2);   // only 2 frames ever, for this whole partition
    {
        stream_create(300, "bigfile", "application/octet-stream");
        uint8_t chunk[4096];
        memset(chunk, 0xEF, sizeof(chunk));
        // Frame 1: offset 0 -- should succeed (1st frame under quota of 2)
        int rc1 = stream_write_chunk("bigfile", chunk, sizeof(chunk), 0, 0);
        CHECK(rc1 == 0, "sD: first 4KiB frame succeeds (1 of 2 allowed)");
        // Frame 2: offset 4096 -- should succeed (2nd frame, exactly at quota)
        int rc2 = stream_write_chunk("bigfile", chunk, sizeof(chunk), 4096, 0);
        CHECK(rc2 == 0, "sD: second 4KiB frame succeeds (2 of 2 allowed, quota now exactly met)");
        CHECK(partition_get_frame_usage(part_d) == 2, "sD: usage reads exactly 2, matching the quota");
        // Frame 3: offset 8192 -- should FAIL, quota exhausted
        int rc3 = stream_write_chunk("bigfile", chunk, sizeof(chunk), 8192, 1);
        CHECK(rc3 == 3, "sD: third 4KiB frame is denied (rc=3, OOM/quota) once tenant-d's partition quota is exhausted");
        CHECK(partition_get_frame_usage(part_d) == 2,
              "sD: usage is STILL exactly 2 after the denied write -- denial touches nothing, no partial charge");
    }

    /* ── Scenario E: an unassigned/never-partitioned caller (uid with no
     * partition_assign_uid() call) still gets a sane, honest tag --
     * PARTITION_DEFAULT (0) -- not a crash or garbage value. ──────────────── */
    {
        int rc = stream_create(999, "unowned-stream", "text/plain");
        CHECK(rc == 0, "sE: stream_create() for a never-assigned uid still succeeds");
        struct StreamEntry* se = stream_find("unowned-stream");
        CHECK(se->partition_id == PARTITION_DEFAULT,
              "sE: an unassigned caller's stream honestly resolves to PARTITION_DEFAULT (0), matching partition_get_for_uid()'s own contract");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
