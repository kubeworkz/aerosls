/*
 * persist_shadow_host_test.c — verification for kernel/persist.c's
 * shadow-compare writes (write only the frames that actually changed),
 * linked against the REAL, unmodified kernel/persist.c.
 *
 * Batching cut how many COMMANDS a snapshot costs; it did not cut the BYTES.
 * persist_records() still rewrote all 1.26 MiB of object_records[] to change
 * as little as one 321-byte field -- ~4,100x write amplification. Shadow
 * comparison writes only the differing 4 KiB frames.
 *
 * ─── What this file is really guarding ──────────────────────────────────
 * The scoping doc named the risk of Option B precisely: writing less than
 * everything means a change can be MISSED, and a missed change lives in RAM,
 * is never written, and silently vanishes on reboot -- invisible until
 * someone notices absent data. That is a data-loss class, not a performance
 * regression, so the tests below are weighted accordingly: nearly every
 * scenario ends by asserting DISK == MEMORY, not merely that the frame count
 * dropped. A version of this feature that is fast but occasionally skips a
 * needed frame would pass a count-only test suite and be worse than not
 * doing the work at all.
 *
 * The implementation derives dirtiness from the bytes (comparing against a
 * shadow copy) rather than from marks supplied by mutation sites, which is
 * what makes "someone forgot to mark this call site" impossible by
 * construction. Scenario 7 additionally proves the verify mode catches the
 * one residual way the invariant can break -- someone writing to a shadowed
 * region behind persist.c's back.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/persist_shadow_host_test \
 *       tests/persist_shadow_host_test.c kernel/persist.c kernel/checkpoint_delta.c kernel/view.c kernel/partition.c
 *   /tmp/persist_shadow_host_test
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
#include "kernel/service_registry.h"
#include "kernel/workload.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "kernel/simi_ctx_migrate.h"   // PEC Phase 3 -- stubbed below

/* ─── Orchestration Plan Phase 5: workloads[] ─────────────────────────
 * kernel/persist.c now snapshots and restores this array too, so every
 * test linking persist.c must define it. Real and zero-initialised (=
 * no declarations), which is exactly these tests' situation; persist.c
 * reads and writes the actual bytes. Reconciler behaviour is covered by
 * tests/workload_reconcile_host_test.c. */
struct SLSWorkloadEntry workloads[WORKLOAD_MAX];


/* ─── Orchestration Plan Phase 4: services_registry[] ─────────────────
 * kernel/persist.c now snapshots and restores this array, so every test
 * linking persist.c must provide it. A real, zero-initialised definition
 * rather than a stub: an empty registry is exactly what these tests have,
 * and persist.c reads and writes the actual bytes. The registry's own
 * behaviour is covered by tests/service_registry_host_test.c. */
struct SLSServiceEntry services_registry[SERVICE_MAX];


/* ─── Orchestration Plan Phase 4 stub: partition_destroy()'s registry
 * cleanup. FAITHFUL, not a no-op: the real
 * service_unregister_partition() drops every registration belonging to
 * the partition and returns how many it dropped; this test registers no
 * services, so the real function would find none and return 0 -- exactly
 * what this returns. Full coverage of the real one, including that
 * partition_destroy() genuinely invokes it, is in
 * tests/service_registry_host_test.c. */
uint32_t service_unregister_partition(uint32_t partition_id) {
    (void)partition_id; return 0;
}


/* ─── PEC Phase 3 stub: kernel/partition.c's context-migration step ────
 * FAITHFUL, not a no-op. The real simi_ctx_migrate_send_partition()
 * walks the live-context registry and sends whatever belongs to the
 * partition; this test registers no contexts, so the real function would
 * walk an empty registry and return 0 -- exactly what this returns. */
uint32_t simi_ctx_migrate_send_partition(uint32_t partition_id, uint32_t dest_node) {
    (void)partition_id; (void)dest_node; return 0;
}


static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Dummy globals persist.c references (same set as the sibling persist
 * tests, same reasons). object_records[] is the one genuinely exercised. ── */
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
void dspp_partition_announce(uint32_t partition_id, const char* name, uint32_t owner_node_id) { (void)partition_id; (void)name; (void)owner_node_id; }
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
void dspp_partition_ownedset_send(uint32_t generation, const uint32_t* partition_ids, uint32_t count) { (void)generation; (void)partition_ids; (void)count; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
int stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
/* Paired with the relocate/send stubs above: a test that stands in "nothing
 * to relocate" must also stand in "nothing to count", or partition_migrate()
 * sees 0 sent against a non-zero expectation and aborts every migration.
 * FAITHFUL -- these tests register no streams, so the real function would
 * also return 0. */
int stream_count_for_partition(uint32_t partition_id) { (void)partition_id; return 0; }

int stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
uint32_t process_kill_partition(uint32_t p) { (void)p; return 0; }
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int partition_lease_step_down(uint32_t p) { (void)p; return 1; }

/* ─── Fake NVMe ─────────────────────────────────────────────────────────── */
#define FAKE_NVME_MAX_FRAMES 1024
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];
static uint32_t g_pages_written = 0;

void* io_sq = (void*)1;
void* io_cq = (void*)1;

static int fake_slot(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = fake_slot(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    g_pages_written++;
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    return 1;
}
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_write_sync(slba + (uint64_t)i * 8, (const uint8_t*)buf + (size_t)i * 4096) != 0)
            return 1;
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t page_count) {
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

/* ─── The assertion that actually matters ───────────────────────────────
 * Reconstructs object_records[] from the fake disk image and compares it
 * against memory. Any frame that should have been written but wasn't shows
 * up here as a mismatch. Returns 0 when disk and memory agree. */
#define REC_ENT_LBA 2056ULL
static int disk_matches_memory(void) {
    uint32_t total = (uint32_t)sizeof(object_records);
    uint32_t frames = (total + 4095) / 4096;
    const uint8_t* mem = (const uint8_t*)object_records;
    for (uint32_t f = 0; f < frames; f++) {
        uint8_t frame[4096];
        if (nvme_read_sync(REC_ENT_LBA + (uint64_t)f * 8, frame) != 0) return -1;
        uint32_t off  = f * 4096;
        uint32_t span = (total - off) < 4096 ? (total - off) : 4096;
        if (memcmp(frame, mem + off, span) != 0) return (int)(f + 1);   /* 1-based frame no. */
    }
    return 0;
}

static void reset_all(void) {
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    memset(object_records, 0, sizeof(object_records));
    g_pages_written = 0;
    persist_shadow_invalidate();
}

/* Records are ~10,284 B each, so index 0 and index 200 land in very
 * different frames -- used below to prove two far-apart edits produce two
 * separate small writes rather than one big span. */
static void touch_record(int idx, uint8_t fill) {
    object_records[idx].object_id  = 0xB000u + (uint32_t)idx;
    object_records[idx].field_count = 1;
    memset(object_records[idx].fields[0].value, fill, 32);
}

int main(void) {
    printf("=== persist shadow-compare: write only what changed ===\n\n");

    const uint32_t TOTAL_FRAMES = ((uint32_t)sizeof(object_records) + 4095) / 4096;

    /* ── Scenario 1: cold start must write EVERYTHING ────────────────────
     * The on-disk image is absent/unknown, so nothing may be skipped. */
    reset_all();
    persist_records();
    CHECK(persist_last_frames_written() == TOTAL_FRAMES,
          "cold start (shadow invalid): writes the entire region, skipping nothing");
    CHECK(disk_matches_memory() == 0, "cold start: disk matches memory");
    printf("      (region is %u frames)\n", TOTAL_FRAMES);

    /* ── Scenario 2: an unchanged region writes NOTHING ──────────────── */
    g_pages_written = 0;
    persist_records();
    CHECK(persist_last_frames_written() == 0,
          "re-persisting an unchanged region writes zero frames");
    CHECK(g_pages_written == 1,
          "...and the only page that moved was the header, which is not shadowed");
    CHECK(disk_matches_memory() == 0, "unchanged: disk still matches memory");

    /* ── Scenario 3: THE headline -- one field change writes 1-2 frames ── */
    touch_record(0, 0xAA);
    persist_records();
    uint32_t one_field = persist_last_frames_written();
    CHECK(one_field >= 1 && one_field <= 2,
          "changing ONE field writes 1-2 frames, not 322");
    CHECK(disk_matches_memory() == 0,
          "one-field change: disk matches memory -- the change really landed");
    printf("      (%u frame(s) = %u KiB, vs %u frames = %u KiB before -- %ux less written)\n",
           one_field, one_field * 4, TOTAL_FRAMES, TOTAL_FRAMES * 4,
           TOTAL_FRAMES / one_field);

    /* ── Scenario 4: two far-apart edits stay two small writes ───────────
     * Proves runs are tracked per-region rather than collapsing into one
     * span covering everything between them. */
    touch_record(0,   0xBB);
    touch_record(120, 0xCC);
    persist_records();
    uint32_t two_edits = persist_last_frames_written();
    CHECK(two_edits >= 2 && two_edits <= 6,
          "two records ~120 apart write only their own frames, not the span between them");
    CHECK(disk_matches_memory() == 0, "two far-apart edits: disk matches memory");
    printf("      (%u frames for two edits %u frames apart)\n", two_edits, TOTAL_FRAMES);

    /* ── Scenario 5: granularity is per-FRAME, not per-record ────────────
     * touch_record() changes only ~44 bytes at the start of each 10,284-byte
     * record, and records sit 2.5 frames apart. So 40 consecutive records
     * SPAN ~101 frames but only actually dirty ~40 of them -- the ~61 frames
     * holding the untouched middles of those records are correctly left
     * alone. Asserting the tighter bound is the point: a record-granular
     * implementation would write ~101 here and still pass a loose check. */
    for (int i = 0; i < 40; i++) touch_record(i, 0xD0);
    persist_records();
    uint32_t forty = persist_last_frames_written();
    uint32_t spanned = (uint32_t)((40u * sizeof(struct SLSObjectRecord)) / 4096u);
    CHECK(forty >= 40,
          "changing 40 records dirties at least one frame each");
    CHECK(forty < spanned,
          "...but FEWER frames than those 40 records span -- granularity is per-frame, not per-record");
    CHECK(disk_matches_memory() == 0, "partially-dirty run: disk matches memory");
    printf("      (%u frames written; those 40 records span %u frames)\n", forty, spanned);

    /* ── Scenario 6: exhaustive -- mutate every record, one at a time,
     * verifying disk==memory after EACH. This is the scenario that would
     * catch a frame-index off-by-one that only bites at certain offsets
     * (e.g. records straddling a 4 KiB boundary), which a handful of
     * hand-picked indices could easily miss. ──────────────────────────── */
    int all_match = 1;
    int first_bad = 0;
    for (int i = 0; i < CATALOG_MAX_OBJECTS; i++) {
        touch_record(i, (uint8_t)(0x40 + (i & 0x1F)));
        persist_records();
        int r = disk_matches_memory();
        if (r != 0 && all_match) { all_match = 0; first_bad = i; }
    }
    CHECK(all_match,
          "exhaustive: after mutating each of the 128 records individually, disk matched memory every single time");
    if (!all_match) printf("      (first divergence at record %d)\n", first_bad);

    /* ── Scenario 7: verify mode catches an out-of-band disk change ──────
     * The one residual way the shadow invariant can break is someone writing
     * to a shadowed region behind persist.c's back. Simulate exactly that by
     * corrupting a frame directly in the fake disk, then confirm verify mode
     * notices and self-repairs rather than leaving disk and memory diverged. */
    CHECK(persist_verify_get() == 0, "verify mode is off by default");
    persist_verify_set(1);
    CHECK(persist_verify_get() == 1, "verify mode can be switched on");

    int slot = fake_slot(REC_ENT_LBA + 5 * 8);          /* frame 5, out of band */
    memset(g_fake_nvme[slot].data, 0x99, 4096);
    CHECK(disk_matches_memory() != 0,
          "setup: disk deliberately corrupted out-of-band, so it no longer matches memory");

    touch_record(1, 0xEE);   /* an unrelated change elsewhere */
    persist_records();
    CHECK(disk_matches_memory() == 0,
          "verify mode detected the out-of-band divergence and self-repaired with a full rewrite");
    persist_verify_set(0);

    /* Without verify mode the same corruption would persist, because frame 5
     * is clean as far as the shadow is concerned. Confirm that directly --
     * it documents precisely what verify mode buys and why it exists. */
    slot = fake_slot(REC_ENT_LBA + 7 * 8);
    memset(g_fake_nvme[slot].data, 0x77, 4096);
    touch_record(2, 0xAB);
    persist_records();
    CHECK(disk_matches_memory() != 0,
          "with verify OFF, an out-of-band corruption is NOT noticed -- this is the documented limitation, asserted rather than assumed");

    /* ...and the escape hatch repairs it. */
    persist_shadow_invalidate();
    persist_records();
    CHECK(disk_matches_memory() == 0,
          "persist_shadow_invalidate() forces a full rewrite that repairs the divergence");
    CHECK(persist_last_frames_written() == TOTAL_FRAMES,
          "...and that rewrite really was the full region");

    /* ── Scenario 8: composes with the deferral bracket ─────────────────── */
    reset_all();
    persist_records();                 /* establish the shadow */
    persist_defer_begin();
    for (int i = 0; i < 10; i++) touch_record(i + 30, (uint8_t)(0x10 + i));
    for (int i = 0; i < 10; i++) persist_records();
    CHECK(persist_last_frames_written() == TOTAL_FRAMES,
          "inside a bracket nothing is written yet (count still reflects the earlier full write)");
    persist_defer_end();
    CHECK(persist_last_frames_written() < TOTAL_FRAMES,
          "the single flush at bracket close writes only the dirty frames -- deferral and shadow-compare compose");
    CHECK(disk_matches_memory() == 0, "batched + shadow-compared: disk matches memory");

    /* ── Scenario 9: zero-length and no-op safety ───────────────────────── */
    g_pages_written = 0;
    persist_records();
    CHECK(persist_last_frames_written() == 0,
          "an immediately repeated persist after a flush writes nothing");
    CHECK(disk_matches_memory() == 0, "...and disk still matches memory");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
