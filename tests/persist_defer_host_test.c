/*
 * persist_defer_host_test.c — verification for kernel/persist.c's
 * persist_defer_begin()/persist_defer_end() batching bracket, linked
 * against the REAL, unmodified kernel/persist.c — not a reimplementation.
 *
 * Why this exists: every persist_*() rewrites its whole region. Measured
 * from persist.h's own LBA layout, persist_records() is 323 separate 4 KiB
 * NVMe commands (1 header + 322 data frames = 1.26 MiB) per call, and it is
 * called once per direct insert/update/delete. sys_sls_tx_commit()
 * (kernel/transaction.c) applies its staged WAL entries by calling
 * sys_sls_update() in a loop -- each landing on the direct-write path -- so
 * an N-operation commit paid that cost N times to reach a final array state
 * one write produces identically. The bracket added alongside this test
 * collapses that to one write per touched region. See
 * docs/AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md.
 *
 * The two properties that actually matter, and that this file exists to
 * prove, are (a) the write COUNT collapses N->1, and (b) the resulting disk
 * BYTES are identical to what the un-batched sequence would have written.
 * (b) is the load-bearing one: batching is only safe because persist_*()
 * writes the current whole array rather than a delta, so "write after every
 * step" and "write once at the end" must land on identical bytes. Scenario 3
 * checks that directly rather than arguing it.
 *
 * This test deliberately does NOT link kernel/transaction.c: sys_sls_tx_
 * commit()'s own dependency graph (object_catalog.c + journal.c +
 * lock_mgr.c + mqt.c, transitively most of the DB engine) is far heavier
 * than this mechanism needs, and no existing host test links it either.
 * What is verified here is the bracket itself, against the real persist.c;
 * transaction.c's use of it is a two-line call-site change verified by
 * compile-check and review, named honestly rather than claimed as tested.
 *
 * The stub set below mirrors tests/persist_partition_host_test.c's exactly
 * (same real types, same dummy arrays, same in-memory LBA->frame fake NVMe),
 * since persist.c's dependency surface is identical for both.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/persist_defer_host_test \
 *       tests/persist_defer_host_test.c kernel/persist.c kernel/view.c kernel/partition.c
 *   /tmp/persist_defer_host_test
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
#include "kernel/tenant.h"
#include "kernel/database.h"
#include "kernel/persist.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Dummy globals persist.c references (same set as persist_partition_
 * host_test.c, same reasons -- linker satisfaction only, never asserted on
 * except object_records[], which Scenario 3 genuinely mutates). ────────── */
struct SLSTenantEntry  tenants[TENANT_MAX];
uint32_t               tenant_next_id = 1;
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
void catalog_after_restore(void) { }
struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];
uint32_t              rowstore_next_free_page_id = 0;
uint32_t              rowstore_partition_cursor[PARTITION_MAX] = {0};
struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                row_constraint_count = 0;
struct RowIndex         row_indexes[ROW_INDEX_MAX];
struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count = 0;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count = 0;
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
uint32_t                   vecstore_partition_cursor[PARTITION_MAX] = {0};
struct VecIndex            vec_indexes[VEC_INDEX_MAX];
struct SLSDatabaseEntry    databases[DATABASE_MAX];
struct SLSDatabaseGrant    database_grants[DATABASE_GRANT_MAX];
uint32_t                   database_next_id = 1;
uint32_t                   database_grant_count = 0;
void mvcc_bootstrap_from_rowstore(void) { }
int row_index_create(uint32_t u, const char* a, const char* b, const char* c) {
    (void)u; (void)a; (void)b; (void)c; return 1;
}
int vec_index_create(uint32_t u, const char* a, const char* b, VecMetric m) {
    (void)u; (void)a; (void)b; (void)m; return 1;
}
uint32_t vecstore_collection_scan(uint32_t u, const char* n, VecScanCb cb, void* ctx) {
    (void)u; (void)n; (void)cb; (void)ctx; return 0;
}
void vec_index_notify_insert(uint32_t u, const char* n, struct VecId id,
                             uint64_t eid, const struct VecValues* v) {
    (void)u; (void)n; (void)id; (void)eid; (void)v;
}
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
int stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
uint32_t process_kill_partition(uint32_t p) { (void)p; return 0; }
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int partition_lease_step_down(uint32_t p) { (void)p; return 1; }

/* ─── Fake NVMe: an LBA-indexed frame store that also COUNTS writes ──────
 * The write counter is the whole point -- this test's central claim is
 * about how many commands are issued, so the fake has to record that, not
 * just the bytes. Sized generously: a full persist_records() alone is 323
 * distinct frames. */
#define FAKE_NVME_MAX_FRAMES 1024
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];
static uint32_t g_write_count = 0;   /* 4 KiB PAGES written */
static uint32_t g_cmd_count   = 0;   /* NVMe COMMANDS issued -- the Option C measure */

void* io_sq = (void*)1;   /* non-NULL so persist.c's "is NVMe up" guards pass */
void* io_cq = (void*)1;

static int fake_slot(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
/* Page-level store, shared by both the single-page and multi-page fakes.
 * Counts pages but NOT commands, so the multi-page wrapper can count exactly
 * one command for the whole batch. */
static int nvme_write_sync_page(uint64_t lba, const void* buf) {
    int idx = fake_slot(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    g_write_count++;
    return 0;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    g_cmd_count++;                       /* a genuine single-page command */
    return nvme_write_sync_page(lba, buf);
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    }
    return 1;
}

/* Multi-page NVMe transfers (drivers/nvme_io.h): kernel/persist.c's array
 * helpers now batch full pages into one command instead of issuing one per
 * frame. This test fakes the NVMe layer rather than linking drivers/nvme_io.c,
 * so it needs these two symbols. Deliberately implemented as a faithful loop
 * over the single-page fakes above rather than a no-op stub -- the bytes still
 * land in the fake store exactly where the real driver would put them, so
 * every existing assertion in this file keeps verifying real behaviour through
 * the new code path instead of being silently bypassed. */
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t page_count) {
    if (page_count > 32) return 1;      /* mirrors NVME_MAX_PAGES_PER_XFER */
    g_cmd_count++;                       /* ONE command, however many pages */
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_write_sync_page(slba + (uint64_t)i * 8, (const uint8_t*)buf + (size_t)i * 4096) != 0)
            return 1;
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t page_count) {
    if (page_count > 32) return 1;
    g_cmd_count++;
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_read_sync(slba + (uint64_t)i * 8, (uint8_t*)buf + (size_t)i * 4096) != 0)
            return 1;
    return 0;
}
int nvme_flush_sync(void) {
    /* Durability barrier (drivers/nvme_io.h): kernel/persist.c issues one
     * before and one after each region header so the data reaches media
     * before the header validating it does. This fake store is already
     * synchronous and non-volatile for the test's purposes, so there is
     * nothing to force -- success is the faithful answer. Tests that care
     * about ORDERING record the call instead; see
     * tests/persist_crash_consistency_host_test.c. */
    return 0;
}

/* Snapshot/compare helpers for the byte-identity proof in Scenario 3. */
static uint8_t g_snapshot[FAKE_NVME_MAX_FRAMES][4096];
static uint64_t g_snapshot_lba[FAKE_NVME_MAX_FRAMES];
static int      g_snapshot_used[FAKE_NVME_MAX_FRAMES];
static void snapshot_disk(void) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        g_snapshot_used[i] = g_fake_nvme[i].used;
        g_snapshot_lba[i]  = g_fake_nvme[i].lba;
        if (g_fake_nvme[i].used) memcpy(g_snapshot[i], g_fake_nvme[i].data, 4096);
    }
}
/* Compares by LBA, not slot index, so allocation order can't produce a
 * false mismatch. Returns count of differing frames. */
static int disk_differs_from_snapshot(void) {
    int diff = 0;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (!g_fake_nvme[i].used) continue;
        int found = -1;
        for (int j = 0; j < FAKE_NVME_MAX_FRAMES; j++)
            if (g_snapshot_used[j] && g_snapshot_lba[j] == g_fake_nvme[i].lba) { found = j; break; }
        if (found < 0) { diff++; continue; }
        if (memcmp(g_snapshot[found], g_fake_nvme[i].data, 4096) != 0) diff++;
    }
    return diff;
}
static void reset_disk(void) {
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    g_write_count = 0;
    g_cmd_count   = 0;
    /* Wiping the simulated disk is an out-of-band change to a shadowed
     * region -- persist.c would otherwise still believe the region is clean
     * and correctly skip writing it. Invalidating models a fresh boot, and
     * is exactly what persist.h requires of anyone who changes a shadowed
     * region's on-disk image behind persist_*()'s back. */
    persist_shadow_invalidate();
}

/* Expected frame counts, derived from sizeof() the same way persist.c's own
 * loop does, rather than hardcoded -- but Scenario 1 additionally asserts
 * the records figure equals the 323 documented in persist.h's layout table
 * and the scoping doc, so a future struct change that silently moves the
 * cost shows up as a test failure rather than a stale doc. */
#define FRAMES_FOR(bytes) (((bytes) + 4095u) / 4096u)
static uint32_t expect_records_writes(void) {
    return 1 /* header */ + FRAMES_FOR((uint32_t)sizeof(object_records));
}
static uint32_t expect_catalog_writes(void) {
    return 1 /* header */ + FRAMES_FOR((uint32_t)sizeof(object_catalog))
                          + FRAMES_FOR((uint32_t)sizeof(role_table));
}

int main(void) {
    printf("=== persist deferral / write-amplification batching ===\n\n");

    /* ── Scenario 1: the un-batched baseline really is what the docs claim ── */
    reset_disk();
    persist_records();
    uint32_t one_records = g_write_count;
    CHECK(one_records == expect_records_writes(),
          "baseline: one persist_records() issues exactly (1 header + ceil(sizeof/4096)) writes");
    CHECK(one_records == 323,
          "baseline: that figure is 323 page-writes -- matching persist.h's layout table and the scoping doc, not the stale 57 the old code comment claimed");

    /* Option C: those 323 pages now leave as far fewer NVMe COMMANDS, because
     * persist_write_array() batches full pages through nvme_write_pages_sync()
     * (up to NVME_MAX_PAGES_PER_XFER=32 per command) instead of issuing one
     * per frame. Expected: ceil(321 full pages / 32) = 11 batch commands,
     * + 1 for the 1,536-byte zero-padded tail frame, + 1 for the header. */
    uint32_t one_records_cmds = g_cmd_count;
    CHECK(one_records_cmds == 13,
          "Option C: those 323 page-writes leave as only 13 NVMe commands (11 batched + tail + header), not 323");
    CHECK(one_records_cmds < one_records / 20,
          "Option C: that is a >20x reduction in command count for the identical bytes");
    printf("      (323 page-writes in %u commands = %.1fx fewer round trips)\n",
           one_records_cmds, (double)one_records / (double)one_records_cmds);
    CHECK(persist_defer_active() == 0,
          "baseline: no bracket is active by default -- deferral is opt-in, existing callers are unaffected");

    /* ── Scenario 2: the bracket collapses N calls to one write ─────────── */
    reset_disk();
    persist_defer_begin();
    for (int i = 0; i < 5; i++) persist_records();
    CHECK(g_write_count == 0,
          "inside a bracket, five persist_records() calls issue zero writes");
    persist_defer_end();
    CHECK(g_write_count == one_records,
          "closing the bracket issues exactly ONE region's worth of writes for all five calls (N->1 collapse)");
    printf("      (un-batched would have been %u writes; batched was %u -- %ux fewer)\n",
           5 * one_records, g_write_count, (5 * one_records) / g_write_count);

    /* ── Scenario 3: THE correctness proof -- identical bytes on disk ─────
     * Batching is only safe because persist_*() writes the whole current
     * array, never a delta. Prove it: run a sequence of mutations with a
     * persist after each (the old behavior), snapshot the disk; then reset,
     * replay the identical mutations inside one bracket, and require the
     * resulting disk to be byte-for-byte identical. */
    reset_disk();
    for (int step = 0; step < 4; step++) {
        object_records[step].object_id  = 0xA000 + step;
        object_records[step].field_count = step + 1;
        memset(object_records[step].fields[0].value, 'A' + step, 16);
        /* This scenario isolates the DEFERRAL property, so each un-batched
         * step must be a full-region write for the 4x comparison below to
         * mean what it says. Shadow-compare (a separate feature, covered by
         * tests/persist_shadow_host_test.c) would otherwise make steps 2-4
         * partial and conflate the two effects. */
        persist_shadow_invalidate();
        persist_records();                       /* un-batched: write every step */
    }
    uint32_t unbatched_writes = g_write_count;
    snapshot_disk();

    /* Identical final in-memory state, reached the same way, but batched. */
    reset_disk();
    memset(object_records, 0, sizeof(object_records));
    persist_defer_begin();
    for (int step = 0; step < 4; step++) {
        object_records[step].object_id  = 0xA000 + step;
        object_records[step].field_count = step + 1;
        memset(object_records[step].fields[0].value, 'A' + step, 16);
        persist_records();                       /* batched: marked, not written */
    }
    persist_defer_end();
    uint32_t batched_writes = g_write_count;

    CHECK(disk_differs_from_snapshot() == 0,
          "batched and un-batched sequences leave BYTE-IDENTICAL disk contents -- the property that makes this safe");
    CHECK(batched_writes == one_records,
          "...and the batched run wrote one region's worth");
    CHECK(unbatched_writes == 4 * one_records,
          "...while the un-batched run wrote four times that");
    printf("      (identical bytes; %u writes batched vs %u un-batched)\n",
           batched_writes, unbatched_writes);

    /* ── Scenario 4: distinct regions each flush exactly once ───────────── */
    reset_disk();
    persist_defer_begin();
    for (int i = 0; i < 3; i++) { persist_records(); persist_catalog(); }
    CHECK(g_write_count == 0, "two different regions, three calls each, still zero writes inside the bracket");
    persist_defer_end();
    CHECK(g_write_count == expect_records_writes() + expect_catalog_writes(),
          "closing flushes each touched region exactly once -- records + catalog, not six full rewrites");

    /* ── Scenario 5: brackets nest, inner close does not flush early ────── */
    reset_disk();
    persist_defer_begin();
    persist_defer_begin();
    persist_records();
    CHECK(persist_defer_active() == 1, "nested: bracket still reported active");
    persist_defer_end();                          /* inner */
    CHECK(g_write_count == 0,
          "nested: closing the INNER bracket flushes nothing -- the outer one is still open");
    CHECK(persist_defer_active() == 1, "nested: still active after the inner close");
    persist_defer_end();                          /* outer */
    CHECK(g_write_count == one_records,
          "nested: closing the OUTER bracket performs the single flush");
    CHECK(persist_defer_active() == 0, "nested: inactive once fully unwound");

    /* ── Scenario 6: an unbalanced end is ignored, not an underflow ─────── */
    reset_disk();
    persist_defer_end();                          /* no matching begin */
    CHECK(g_write_count == 0, "unbalanced persist_defer_end() writes nothing");
    CHECK(persist_defer_active() == 0,
          "unbalanced persist_defer_end() does not underflow the depth counter (would have wrapped to ~4 billion and silently swallowed every future write)");
    persist_records();
    CHECK(g_write_count == one_records,
          "...and normal immediate writes still work afterwards -- the underflow would have broken exactly this");

    /* ── Scenario 7: pending-mask introspection ─────────────────────────── */
    reset_disk();
    CHECK(persist_defer_pending_mask() == 0, "pending mask starts clear");
    persist_defer_begin();
    persist_records();
    CHECK(persist_defer_pending_mask() != 0, "pending mask records a deferred region");
    uint32_t mask_with_one = persist_defer_pending_mask();
    persist_catalog();
    CHECK(persist_defer_pending_mask() != mask_with_one,
          "a second distinct region sets an additional bit");
    persist_defer_end();
    CHECK(persist_defer_pending_mask() == 0, "pending mask clears on flush");

    /* ── Scenario 8: an empty bracket is a genuine no-op ────────────────── */
    reset_disk();
    persist_defer_begin();
    persist_defer_end();
    CHECK(g_write_count == 0, "an empty bracket writes nothing at all");

    /* ── Scenario 9: the transaction-shaped case, end to end ─────────────
     * kernel/transaction.c's sys_sls_tx_commit() applies N staged WAL
     * entries by calling sys_sls_update() N times, each of which ends in
     * persist_records(). This models that exact shape (without linking the
     * DB engine) at a realistic transaction size. */
    const int TX_OPS = 20;
    reset_disk();
    persist_defer_begin();
    for (int i = 0; i < TX_OPS; i++) {
        object_records[i % CATALOG_MAX_OBJECTS].field_count = i;
        persist_records();
    }
    persist_defer_end();
    CHECK(g_write_count == one_records,
          "a 20-operation transaction's apply loop costs ONE region write, not twenty");
    printf("      (20-op commit: %u writes batched vs %u before = %.1f MiB saved per commit)\n",
           g_write_count, TX_OPS * one_records,
           (double)((TX_OPS - 1) * one_records) * 4096.0 / (1024.0 * 1024.0));

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
