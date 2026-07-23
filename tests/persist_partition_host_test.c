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
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/persist_partition_host_test \
 *       tests/persist_partition_host_test.c kernel/persist.c kernel/view.c kernel/partition.c
 *   /tmp/persist_partition_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
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

/* Phase 16 (relational layer): persist.c's restore block 6 (added this
 * phase) touches rowstore.c's table_headers[]/rowstore_next_free_page_id.
 * This persistence-focused test has no interest in row-store data — see
 * rowstore_host_test.c for the real, call-tracked coverage of that. */
struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];
uint32_t               rowstore_next_free_page_id = 0;

uint32_t               rowstore_partition_cursor[PARTITION_MAX] = {0};
/* Gap Remediation Phase D (found while regression-sweeping for Phase F --
 * this file was missed during Phase D's own sweep of every persist.c-
 * linking host test): persist.c's restore blocks 6b-11 reference row_
 * index.c/row_constraint.c/row_journal.c/vecstore.c/vec_index.c/mvcc.c's
 * own globals/functions, none of which this persistence-focused test has
 * any interest in linking -- same minimal-stub pattern every other Phase D-
 * affected host test already uses (see e.g. rowstore_host_test.c's own
 * identical block). Every restore block correctly no-ops at "no snapshot"
 * before ever touching this content. */
struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                 row_constraint_count = 0;
struct RowIndex          row_indexes[ROW_INDEX_MAX];
struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count = 0;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count = 0;
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
uint32_t                   vecstore_partition_cursor[PARTITION_MAX] = {0};
struct VecIndex             vec_indexes[VEC_INDEX_MAX];
void mvcc_bootstrap_from_rowstore(void) { }
int row_index_create(uint32_t caller_uid, const char* index_name,
                     const char* table_name, const char* column_name) {
    (void)caller_uid; (void)index_name; (void)table_name; (void)column_name;
    return 1;
}
int vec_index_create(uint32_t caller_uid, const char* index_name,
                     const char* collection_name, VecMetric metric) {
    (void)caller_uid; (void)index_name; (void)collection_name; (void)metric;
    return 1;
}
uint32_t vecstore_collection_scan(uint32_t caller_uid, const char* collection_name,
                                  VecScanCb cb, void* ctx) {
    (void)caller_uid; (void)collection_name; (void)cb; (void)ctx;
    return 0;
}
void vec_index_notify_insert(uint32_t caller_uid, const char* collection_name,
                             struct VecId id, uint64_t external_id,
                             const struct VecValues* values) {
    (void)caller_uid; (void)collection_name; (void)id; (void)external_id; (void)values;
}

/* kernel_io.h's two logging functions — persist.c and partition.c both
 * call these purely for diagnostic serial output, no test-relevant
 * behavior depends on them. */
void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
// Query-Surface Roadmap Phase 5: kernel/view.c's view_drop() calls catalog_get_role()
// for its owner-or-kernel permission gate (same call view.c's own header comment
// says mirrors database_drop()'s). This test has no interest in role semantics --
// same minimal stub sql_setop_phase4_host_test.c etc. already use.
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
/* Database Gap Analysis Gap 1: persist.c now snapshots/restores the database
 * subsystem globals -- defined here as zero-state dummies rather than linking
 * the real kernel/database.c (whose group/catalog dependency graph this test
 * has no interest in), the same dummy-globals pattern these tests already use
 * for object_catalog[] et al. */
#include "kernel/database.h"
struct SLSDatabaseEntry databases[DATABASE_MAX];
struct SLSDatabaseGrant database_grants[DATABASE_GRANT_MAX];
uint32_t database_next_id = 1;
uint32_t database_grant_count = 0;
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
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }   /* Multi-Node Partition Scaling Roadmap Phase 3 -- replaces partition_reset_frame_usage() at partition_destroy()'s call site */
static uint32_t g_fake_local_node_id = 0;   /* Multi-Node Partition Scaling Roadmap Phase 2 — settable */
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }   /* Multi-Node Partition Scaling Roadmap Phase 6 -- not exercised by this test, safe "nothing to relinquish" stub */

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

    /* ── Multi-Node Partition Scaling Roadmap Phase 2: ownership round trip ──
     * partition_create() above already stamped both tenants' owner rows at
     * node 0 (g_fake_local_node_id's starting value). Pin tenant-a to a
     * different node, then prove that pin survives a simulated reboot the
     * same way name/active/assignment state already does above. ────────── */
    CHECK(partition_get_owner_node(pid_a) == 0, "tenant-a's owner defaults to node 0 pre-pin (post first restore)");
    CHECK(partition_set_owner_node(pid_a, 9) == 0, "pin tenant-a to node 9 (persists automatically via persist_partitions())");
    CHECK(partition_get_owner_node(pid_a) == 9, "tenant-a's owner reads back as node 9 before 'reboot'");
    CHECK(partition_get_owner_node(pid_b) == 0, "tenant-b's owner untouched by tenant-a's pin, still node 0 before 'reboot'");

    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_assign_table, 0, sizeof(partition_assign_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));
    partition_init();
    CHECK(partition_get_owner_node(pid_a) == 0,
          "post-'reboot', pre-restore: tenant-a's owner pin genuinely wiped (reads back 0, the 'no row' default)");

    persist_restore_all();
    CHECK(partition_get_owner_node(pid_a) == 9, "tenant-a's owner pin (node 9) restored after 'reboot'");
    CHECK(partition_get_owner_node(pid_b) == 0, "tenant-b's owner (node 0) also correctly restored after 'reboot'");
    g_fake_local_node_id = 9;
    CHECK(partition_is_local(pid_a) == 1, "tenant-a reads as local post-restore once cluster_local_node_id() also reports node 9");
    CHECK(partition_is_local(pid_b) == 0, "tenant-b correctly reads as remote post-restore (owner 0 != local node 9)");
    g_fake_local_node_id = 0;   /* restore default for the rest of this test */

    /* ── Backward-compat guard: a snapshot written by pre-Phase-2 code always
     * had write_hdr()'s v2 slot (owner_bytes) as 0 -- stomp just that field
     * (header offset +16, matching persist.c's own p_memcpy(&owner_bytes,
     * p_buf + 16, 4) read) while leaving the magic and the other two size
     * fields intact, and confirm restore still loads partition_table[]/
     * partition_assign_table[] normally but leaves the FRESH partition_
     * init() owner stamps alone rather than reading stale/garbage bytes
     * from PERSIST_PART_OWNER_LBA. ───────────────────────────────────────── */
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == PERSIST_PART_HDR_LBA) {
            memset(g_fake_nvme[i].data + 16, 0, 4);   /* owner_bytes -> 0, simulating a pre-Phase-2 snapshot */
        }
    }
    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_assign_table, 0, sizeof(partition_assign_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));
    g_fake_local_node_id = 3;
    partition_init();
    persist_restore_all();
    CHECK(partition_get_for_uid(42) == pid_a,
          "backward-compat snapshot (owner_bytes==0): partition_table[]/assign_table[] still restore normally");
    /* partition_init() only stamps a fresh owner row for slot 0
     * (PARTITION_SYSTEM) -- tenant-a's row (a different slot) is never
     * re-created by anything in this cold-restore path, since restore
     * correctly declines to read it from the mismatched-size snapshot. */
    CHECK(partition_get_owner_node(PARTITION_SYSTEM) == 3,
          "backward-compat snapshot: PARTITION_SYSTEM's owner is partition_init()'s fresh stamp (current cluster_local_node_id(), 3), left alone by the skipped restore");
    CHECK(partition_get_owner_node(pid_a) == 0,
          "backward-compat snapshot: tenant-a's owner is NOT read from the stale/absent PERSIST_PART_OWNER_LBA data -- "
          "it reads back 0 (no row), not tenant-a's old pin of node 9 and not garbage");
    g_fake_local_node_id = 0;   /* restore default for the remaining checks below */

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
