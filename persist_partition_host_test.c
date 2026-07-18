/*
 * persist_partition_host_test.c — Phase 10 verification: a standalone
 * host-buildable round-trip test for kernel/persist.c's new
 * persist_partitions() writer and persist_restore_all()'s new partition
 * restore block, linked against the REAL, unmodified kernel/persist.c and
 * kernel/partition.c — not a reimplementation.
 *
 * persist.c also touches four other subsystems (object_catalog[]/
 * role_table[], object_records[], object_schemas[], service_binaries[])
 * that this test has no interest in exercising, but persist_restore_all()
 * is one function that reads all five — so this test provides minimal
 * dummy definitions for those five arrays/one function (matching the real
 * extern types declared in object_catalog.h/loader.h) purely so the
 * linker can resolve persist.c's other four blocks; their content is
 * never asserted on. The NVMe driver itself is replaced with a tiny
 * in-memory LBA->4KiB-frame map, since persist.c's only two points of
 * contact with real hardware are nvme_read_sync()/nvme_write_sync() plus
 * the io_sq/io_cq "is NVMe up" guards — small enough to fake convincingly.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers \
 *       -o /tmp/persist_partition_host_test \
 *       persist_partition_host_test.c kernel/persist.c kernel/partition.c
 *   /tmp/persist_partition_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Dummy definitions for the four subsystems persist.c also touches ──
 * Real types (from object_catalog.h/loader.h), fake/empty content — this
 * test never asserts anything about these, they exist purely so the
 * linker can resolve persist_restore_all()'s other four blocks. */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
void catalog_after_restore(void) { /* no-op for this test */ }

/* kernel_io.h's two logging functions — persist.c and partition.c both
 * call these purely for diagnostic serial output, no test-relevant
 * behavior depends on them. */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* Phase 14 (LPAR): partition.c's partition_destroy() (added this phase)
 * calls into process.c/object_catalog.c/frame_pool.c, none of which this
 * persistence-focused test has any interest in linking. This test never
 * calls partition_destroy(), so these three stand in purely so the linker
 * resolves partition.c's new references — see scheduler_fairness_host_
 * test.c and frame_quota_host_test.c for the real, call-tracked coverage
 * of destroy/pause/resume and the frame-usage reset. */
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
int partition_reset_frame_usage(uint32_t partition_id) { (void)partition_id; return 0; }

/* ─── Fake NVMe: an in-memory map from frame-aligned LBA to 4KiB bytes ── */
#define FAKE_NVME_MAX_FRAMES 64
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];

void* io_sq = (void*)1;   /* non-NULL so persist.c's "is NVMe up" guards pass */
void* io_cq = (void*)1;

static int find_or_alloc_frame(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}

int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = find_or_alloc_frame(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    }
    return 1;   /* no such frame written yet — matches real nvme_read_sync's error contract */
}

#include "kernel/persist.h"   /* self-contained (just <stdint.h>), safe to include here */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* ── Cold start: no snapshot written yet ─────────────────────────── */
    partition_init();
    persist_restore_all();   /* should no-op cleanly — nothing written to fake NVMe yet */
    CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM,
          "cold start: uid 0 still PARTITION_SYSTEM after a no-op restore");

    /* ── Build real partition state (each mutation persists automatically) ── */
    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    partition_assign_uid(42, pid_a);
    partition_assign_uid(7,  pid_b);
    CHECK(pid_a != 0xFFFFFFFFu && pid_b != 0xFFFFFFFFu, "two partitions created before 'reboot'");
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42 -> tenant-a before 'reboot'");
    CHECK(partition_get_for_uid(7)  == pid_b, "uid 7 -> tenant-b before 'reboot'");

    /* ── Simulate a reboot: wipe in-memory state exactly like a fresh boot
     * would (BSS zero + partition_init()'s slot-0 setup), WITHOUT wiping
     * the fake NVMe — that's the whole point of persistence. ──────────── */
    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_assign_table, 0, sizeof(partition_assign_table));
    partition_init();
    CHECK(partition_get_for_uid(42) == PARTITION_DEFAULT,
          "post-'reboot', pre-restore: uid 42 back to default (state genuinely wiped)");

    /* ── Restore from the fake NVMe ───────────────────────────────────── */
    persist_restore_all();
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42 -> tenant-a restored after 'reboot'");
    CHECK(partition_get_for_uid(7)  == pid_b, "uid 7 -> tenant-b restored after 'reboot'");
    CHECK(strcmp(partition_table[pid_a].name, "tenant-a") == 0, "tenant-a's name string survived the round trip");
    CHECK(strcmp(partition_table[pid_b].name, "tenant-b") == 0, "tenant-b's name string survived the round trip");
    CHECK(partition_table[pid_a].active == 1, "tenant-a still marked active after restore");

    /* ── Struct-size mismatch guard: corrupt the header's magic and
     * confirm restore treats it as a cold start rather than restoring
     * garbage — same "denial looks like absence" carefulness this
     * project applies elsewhere. ─────────────────────────────────────── */
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == PERSIST_PART_HDR_LBA) {
            memset(g_fake_nvme[i].data, 0xFF, 8);  /* stomp the magic */
        }
    }
    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_assign_table, 0, sizeof(partition_assign_table));
    partition_init();
    persist_restore_all();
    CHECK(partition_get_for_uid(42) == PARTITION_DEFAULT,
          "corrupted magic: restore correctly refuses to load, state stays at cold-start default");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
