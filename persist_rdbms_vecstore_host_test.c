/*
 * persist_rdbms_vecstore_host_test.c — Gap Remediation Phase D verification:
 * a standalone host-buildable round-trip test for kernel/persist.c's five
 * new Phase D write functions (persist_row_constraints(), persist_row_
 * index_defs(), persist_vecstore_headers(), persist_vec_index_defs(),
 * persist_row_journal()) and persist_restore_all()'s five new restore
 * blocks (7-11), linked against the REAL, unmodified kernel/persist.c,
 * kernel/rowstore.c, kernel/row_index.c, kernel/mvcc.c, kernel/row_
 * constraint.c, kernel/row_journal.c, kernel/vecstore.c, kernel/vec_
 * index.c — not a reimplementation of any of them.
 *
 * Mirrors persist_partition_host_test.c's own "build real state -> wipe
 * in-memory exactly like a real reboot's BSS-zero would -> restore from
 * the fake NVMe -> verify" shape exactly, extended to cover five
 * subsystems in one pass instead of one, because Phase D's own real
 * question is whether ALL FIVE restore blocks cooperate correctly in the
 * real boot ORDER persist_restore_all() runs them in (catalog -> rowstore
 * -> row_constraint -> row_index -> vecstore -> vec_index -> row_journal)
 * -- a per-subsystem test in isolation couldn't catch an ordering bug
 * (e.g. row_index's rebuild running before rowstore's own row data is
 * back would silently rebuild an empty index).
 *
 * Two subsystems get genuinely different treatment here, on purpose (see
 * docs/AeroSLS-Gap-Remediation-Roadmap-v0.1.md's Phase D findings
 * addendum for the full rationale, only summarized in-line below):
 *   - row_index / vec_index: REBUILD-on-boot. Only definitions persist;
 *     this test's own assertions confirm the ACTUAL B-tree/HNSW graph
 *     content is correct post-restore, not just that the definition
 *     struct round-tripped.
 *   - row_constraint / row_journal / vecstore: DIRECT persistence. This
 *     test confirms the data itself (constraint defs, journal entries,
 *     vector values) survives byte-for-byte, no rebuild step involved.
 *
 * This test does NOT link cursor.c/sql_exec.c/sql_parser.c/predicate.c --
 * confirmed by direct audit of mvcc.c's own #include list that none of
 * the eight files under test need the SQL layer at all, keeping this
 * test's dependency footprint as light as vec_join_host_test.c's own
 * "reuse the real engine, don't drag in what it doesn't need" precedent.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers -I net \
 *       -o /tmp/persist_rdbms_vecstore_host_test \
 *       persist_rdbms_vecstore_host_test.c \
 *       kernel/persist.c kernel/rowstore.c kernel/row_index.c kernel/mvcc.c \
 *       kernel/row_constraint.c kernel/row_journal.c kernel/vecstore.c \
 *       kernel/vec_index.c
 *   /tmp/persist_rdbms_vecstore_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/mvcc.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/persist.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Dummy definitions for the catalog/records/schemas/programs/partition
 * subsystems persist_restore_all() also touches (blocks 1-5) but this test
 * has no interest in exercising beyond catalog/schemas, which it DOES use
 * for real -- matching persist_partition_host_test.c's own precedent of
 * providing real types with fake/unused content purely so the linker can
 * resolve every block, not just the ones under test. ────────────────────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
void catalog_after_restore(void) { /* no-op for this test */ }

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return 1;   // this suite is about the persistence round trip, not permission gating
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;   // unused by this suite -- see vecstore_host_test.c's identical stub/comment
}

/* ─── Fake NVMe: an in-memory map from frame-aligned LBA to 4KiB bytes,
 * matching persist_partition_host_test.c's own exactly. Real io_sq/io_cq
 * "is NVMe up" guards pass (non-NULL), so every persist_*() write this
 * test triggers actually engages, not short-circuits. Sized generously --
 * this test's own real page/row-journal data needs more distinct frames
 * than persist_partition_host_test.c's small struct-only footprint did. ── */
#define FAKE_NVME_MAX_FRAMES 4096
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];
void* io_sq = (void*)1;
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
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* ─── Setup helpers, matching vec_join_host_test.c's own precedent ──────── */
static void make_employees_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE801;
    object_catalog[0].active = 1;
    object_catalog_count = 1;
    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");     object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "name");   object_schemas[0].fields[1].type = FIELD_TYPE_STRING; object_schemas[0].fields[1].active = 1;
    object_schemas[0].field_count = 2;
    rowstore_create_table("employees");
}

static void make_embeddings_collection(uint32_t idx, uint32_t dimension) {
    memset(&object_catalog[idx], 0, sizeof(object_catalog[idx]));
    strcpy(object_catalog[idx].name, "embeddings");
    object_catalog[idx].type = OBJ_TYPE_DB_TABLE;
    object_catalog[idx].object_id = 0xE802;
    object_catalog[idx].active = 1;
    vecstore_create_collection("embeddings", dimension);
}

int main(void) {
    /* ── Cold boot, first time ever: matches kernel.c's real init order
     * exactly (rowstore -> row_index -> mvcc -> row_constraint ->
     * row_journal -> vecstore -> vec_index), then persist_restore_all()
     * should no-op cleanly -- nothing written to the fake NVMe yet. ────── */
    rowstore_init();
    row_index_init();
    mvcc_init();
    row_constraint_init();
    row_journal_init();
    vecstore_init();
    vec_index_init();
    persist_restore_all();
    CHECK(object_catalog_count == 0, "cold start: no snapshot yet, catalog stays empty after a no-op restore");

    /* ── Build real state, pre-'reboot' ──────────────────────────────────── */
    make_employees_table();                  // object_catalog[0], persists via persist_catalog()/persist_rowstore_headers()
    CHECK(object_catalog[0].uses_rowstore == 1, "setup: employees promoted to a row-set table");

    int r1 = row_constraint_add_unique("employees", "id") == ROW_CONSTRAINT_OK;
    int r2 = row_constraint_add_not_null("employees", "name") == ROW_CONSTRAINT_OK;
    CHECK(r1 && r2, "setup: two constraints registered on employees (UNIQUE id, NOT NULL name)");
    CHECK(row_constraint_count == 2, "setup: row_constraint_count reflects both registrations");

    CHECK(row_journal_attach("audit1", "employees") == 0, "setup: employees attached to journal 'audit1'");

    uint64_t txn1 = mvcc_begin();
    CHECK(txn1 != 0, "setup: transaction begun");
    struct MvccRowId row_ids[5];
    for (int i = 0; i < 5; i++) {
        struct RowValues v;
        v.count = 2;
        snprintf(v.values[0], RECORD_VAL_LEN, "%d", i + 1);
        snprintf(v.values[1], RECORD_VAL_LEN, "employee%d", i + 1);
        MvccError e = mvcc_row_insert(txn1, 1, "employees", &v, &row_ids[i]);
        if (e != MVCC_OK) { printf("FAIL: setup insert %d failed with %d\n", i, (int)e); g_fail++; }
    }
    CHECK(mvcc_commit(txn1) == MVCC_OK, "setup: transaction committed (journal entries move pending -> committed)");
    CHECK(row_journal_entry_count == 5, "setup: 5 journal entries recorded (one PT per insert)");

    CHECK(row_index_create(0, "idx_emp_id", "employees", "id") == 0, "setup: B-tree index created on employees.id");
    struct RowId found[4];
    uint32_t n = row_index_lookup(0, "idx_emp_id", "3", found, 4);
    CHECK(n == 1, "setup: pre-reboot index lookup for id=3 finds exactly one row");

    object_catalog_count = 2;
    make_embeddings_collection(1, 3);
    // A real vecstore collection is normally registered into the catalog via
    // sys_sls_valloc(), which persists object_catalog[]/object_catalog_count
    // itself as a side effect -- this test pokes object_catalog[1] directly
    // (matching vec_join_host_test.c's own setup-helper precedent, since
    // there's no valloc() linked here), so it must replicate that one real
    // side effect explicitly, or the "embeddings" entry would never survive
    // a restore even though vector_collections[1] itself does.
    persist_catalog();
    struct VecId vid[5];
    float xs[5] = {0.0f, 1.0f, 3.0f, 6.0f, 10.0f};
    for (int i = 0; i < 5; i++) {
        struct VecValues v = { 3, { xs[i], 1.0f, 0.0f } };
        CHECK(vecstore_insert(1, "embeddings", (uint64_t)(i + 1), &v, &vid[i]) == 0,
              "setup: embedding inserted");
    }
    CHECK(vec_index_create(0, "idx_emb", "embeddings", VEC_METRIC_L2) == 0,
          "setup: HNSW index created over embeddings (empty -- created after the 5 inserts above, per vec_index_create()'s own no-backfill contract)");
    // Post-index inserts DO get auto-indexed (vec_index_notify_insert()) --
    // these are the ones this test expects the rebuilt graph to find.
    struct VecId vid_post[2];
    float post_xs[2] = { 100.0f, 102.0f };
    for (int i = 0; i < 2; i++) {
        struct VecValues v = { 3, { post_xs[i], 1.0f, 0.0f } };
        CHECK(vecstore_insert(1, "embeddings", (uint64_t)(900 + i), &v, &vid_post[i]) == 0,
              "setup: post-index embedding inserted (auto-indexed)");
    }

    /* ── Simulate a reboot: wipe every in-memory structure exactly like a
     * fresh boot's BSS-zero + each subsystem's own _init() would, WITHOUT
     * wiping the fake NVMe -- that's the whole point of persistence. Then
     * re-run every _init() in the SAME order kernel.c's real boot does,
     * exactly as it would before persist_restore_all() runs. ──────────── */
    memset(object_catalog, 0, sizeof(object_catalog));
    memset(object_schemas, 0, sizeof(object_schemas));
    object_catalog_count = 0;
    rowstore_init();
    row_index_init();
    mvcc_init();
    row_constraint_init();
    row_journal_init();
    vecstore_init();
    vec_index_init();

    CHECK(object_catalog_count == 0, "post-'reboot', pre-restore: catalog genuinely wiped");
    CHECK(row_constraint_count == 0, "post-'reboot', pre-restore: constraints genuinely wiped");
    CHECK(row_journal_entry_count == 0, "post-'reboot', pre-restore: journal genuinely wiped");
    n = row_index_lookup(0, "idx_emp_id", "3", found, 4);
    CHECK(n == 0, "post-'reboot', pre-restore: index lookup finds nothing (index genuinely gone)");

    /* ── Restore from the fake NVMe ───────────────────────────────────────── */
    persist_restore_all();

    /* ── 1/6: catalog + rowstore (pre-existing blocks, sanity-checked here
     * only to confirm THIS test's own setup reached them correctly, not
     * re-verifying Phase 16/B's own already-established behavior) ───────── */
    CHECK(object_catalog_count == 2, "restore: catalog count back to 2 (employees + embeddings)");
    CHECK(object_catalog[0].uses_rowstore == 1, "restore: employees still marked as a row-set table");

    /* ── 7: row constraints -- direct restore, pure definitions ─────────── */
    CHECK(row_constraint_count == 2, "restore: both constraint definitions survived");
    // A real, active transaction snapshot -- row_constraint_check_write()'s
    // own UNIQUE/REFERENCE checks run mvcc_table_scan() under txn_id, and
    // txn_id=0 is mvcc_begin()'s own "failed to allocate a txn" sentinel,
    // not a usable snapshot (matches mvcc.c's own real call sites, which
    // always pass the live transaction's own txn_id, never a bare 0).
    uint64_t txn2 = mvcc_begin();
    CHECK(txn2 != 0, "restore: a fresh transaction can begin post-restore (mvcc_init() state is sane)");

    // The core of this phase's own MVCC-bootstrap fix, checked directly:
    // mvcc_table_scan() is what sql_exec.c's entire live query path has run
    // through since Phase 22 -- without mvcc_bootstrap_from_rowstore(), this
    // would visit 0 rows here even though the raw data and B-tree index are
    // both provably intact (checked separately below).
    uint32_t mv_visited = mvcc_table_scan(txn2, 1, "employees", NULL, NULL);
    CHECK(mv_visited == 5, "restore: mvcc_table_scan() sees all 5 rows post-restore (the query-engine-facing fix)");

    struct RowValues dup;
    dup.count = 2;
    strcpy(dup.values[0], "3");        // id=3 already exists -- should violate UNIQUE
    strcpy(dup.values[1], "someone");
    RowConstraintResult rr = row_constraint_check_write(txn2, 1, object_catalog[0].object_id, &dup, 0);
    CHECK(rr == ROW_CONSTRAINT_VIOLATION_UNIQUE,
          "restore: restored UNIQUE constraint still rejects a duplicate id=3 after 'reboot'");
    struct RowValues nullname;
    nullname.count = 2;
    strcpy(nullname.values[0], "999");
    nullname.values[1][0] = '\0';
    rr = row_constraint_check_write(txn2, 1, object_catalog[0].object_id, &nullname, 0);
    CHECK(rr == ROW_CONSTRAINT_VIOLATION_NOT_NULL,
          "restore: restored NOT NULL constraint still rejects an empty name after 'reboot'");

    /* ── 8: row-set B-tree index -- rebuild-on-boot ──────────────────────── */
    n = row_index_lookup(0, "idx_emp_id", "3", found, 4);
    CHECK(n == 1, "restore: rebuilt B-tree index finds exactly one row for id=3");
    struct RowValues got;
    CHECK(n == 1 && rowstore_row_get(0, "employees", found[0], &got) == 0 &&
          strcmp(got.values[1], "employee3") == 0,
          "restore: the row the rebuilt index points at is the REAL employee3 row, not a stale/wrong one");
    n = row_index_lookup(0, "idx_emp_id", "1", found, 4);
    CHECK(n == 1, "restore: rebuilt index also finds id=1 (not just the one value spot-checked above)");

    /* ── 9: vecstore collection headers + lazily-restored page data ──────── */
    uint64_t ext_id;
    struct VecValues vv;
    CHECK(vecstore_get(1, "embeddings", vid[2], &ext_id, &vv) == 0 && ext_id == 3 &&
          vv.values[0] == xs[2],
          "restore: pre-reboot embedding (external_id=3) round-trips with its exact original values, lazily loaded from NVMe");
    struct VecValues query = { 3, { 0.0f, 1.0f, 0.0f } };
    struct VecMatch matches[3];
    uint32_t sn = vecstore_search(1, "embeddings", &query, VEC_METRIC_L2, 3, matches);
    CHECK(sn == 3 && matches[0].external_id == 1 && matches[1].external_id == 2 && matches[2].external_id == 3,
          "restore: exact vecstore_search() over restored data returns the correct nearest-neighbor order");

    /* ── 10: HNSW index -- rebuild-on-boot, including the backfill this
     * test's setup specifically exercised (index created empty, then
     * populated by 2 post-creation inserts) ─────────────────────────────── */
    struct VecValues hquery = { 3, { 100.0f, 1.0f, 0.0f } };
    struct VecMatch hmatches[2];
    uint32_t hn = vec_index_search(0, "idx_emb", &hquery, 2, 64, hmatches);
    CHECK(hn == 2, "restore: rebuilt HNSW index returns the requested top-2");
    int found_900 = 0;
    for (uint32_t i = 0; i < hn; i++) if (hmatches[i].external_id == 900) found_900 = 1;
    CHECK(found_900, "restore: rebuilt HNSW graph finds the exact post-index-creation point (external_id=900) -- proves the restore path's own backfill scan worked, not just index_create() succeeding");

    /* ── 11: row journal -- direct restore, inherently historical data ──── */
    CHECK(row_journal_entry_count == 5, "restore: all 5 journal entries survived");
    int all_committed = 1;
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES && i < 5; i++)
        if (!row_journal_buffer[i].committed) all_committed = 0;
    CHECK(all_committed, "restore: every restored journal entry is still marked committed=1 (matches pre-reboot commit)");
    CHECK(row_journal_attachment_count == 1, "restore: the journal attachment (employees -> audit1) survived");

    /* ── Struct-size mismatch guard, spot-checked on one representative
     * Phase D region (vecstore) -- same "denial looks like absence"
     * carefulness persist_partition_host_test.c already proved for an
     * earlier phase's region, not re-proving the general mechanism five
     * more times for the other four regions it's already shared code. ──── */
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == PERSIST_VECSTORE_HDR_LBA) {
            memset(g_fake_nvme[i].data, 0xFF, 8);   // stomp the magic
        }
    }
    memset(vector_collections, 0, sizeof(vector_collections));
    vecstore_next_free_page_id = 0;
    vecstore_init();
    persist_restore_all();
    CHECK(vecstore_get(1, "embeddings", vid[2], &ext_id, &vv) != 0,
          "corrupted vecstore magic: restore correctly refuses to load, collection stays cold-start empty");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
