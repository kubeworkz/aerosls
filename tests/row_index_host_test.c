/*
 * row_index_host_test.c — Phase 17 (relational layer) verification: a
 * standalone host-buildable test for kernel/row_index.c's new B-tree
 * indexing engine, linked against the REAL, unmodified kernel/row_index.c,
 * kernel/rowstore.c, AND kernel/persist.c — not a reimplementation.
 *
 * Mirrors rowstore_host_test.c's harness (same fake NVMe, same call-tracked
 * catalog_check_access() stub, same real-frame allocator) since row_index.c
 * sits directly on top of rowstore.c and inherits its dependency surface;
 * see that file's own header comment for why each stub is shaped the way
 * it is. This test additionally exercises the real, unmodified
 * rowstore_row_insert/update/delete → row_index_notify_* auto-maintenance
 * wiring (Phase 17's change to rowstore.c), not just row_index.c in
 * isolation.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/row_index_host_test \
 *       tests/row_index_host_test.c kernel/row_index.c kernel/rowstore.c kernel/persist.c
 *   /tmp/row_index_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/persist.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Dummy definitions for subsystems persist.c also touches but this test
 * doesn't exercise (see rowstore_host_test.c precedent) ─────────────────── */
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

/* Gap Remediation Phase D: persist.c's new restore blocks 7, 9-11 reference
 * row_constraint.c/row_journal.c/vecstore.c/vec_index.c's own globals/
 * functions, plus mvcc_bootstrap_from_rowstore() (block 6b), none of which
 * is linked here (this suite is scoped to row_index.c itself) -- stubbed
 * purely to satisfy the linker, matching rowstore_host_test.c's own
 * identical precedent; every restore block correctly no-ops at "no
 * snapshot" before ever touching this content. */
struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                 row_constraint_count = 0;
struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count = 0;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count = 0;
void mvcc_bootstrap_from_rowstore(void) { }
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
struct VecIndex             vec_indexes[VEC_INDEX_MAX];
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

/* ─── catalog_check_access() stub — call-tracked, same shape as
 * rowstore_host_test.c's: lets scenarios confirm exactly when/how many
 * times it's called (row_index_create calls it directly for PERM_WRITE,
 * then again indirectly via rowstore_table_scan's PERM_READ check when
 * building an index from existing rows; row_index_lookup/range_scan call
 * it once for PERM_READ). ─────────────────────────────────────────────── */
static int      g_access_calls = 0;
static uint32_t g_access_last_perm = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name;
    g_access_calls++;
    g_access_last_perm = needed_perm;
    return g_access_force_deny ? 0 : 1;
}

void* allocate_physical_ram_frame(void) { return malloc(4096); }

#define FAKE_NVME_MAX_FRAMES 256
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
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* ─── Test fixture: "employees" table, 4 columns ─────────────────────────
 * id UINT64, name STRING, active BOOL, salary FLOAT ->
 * row_width = 1 + 8 + 64 + 1 + 8 = 82 -> rows_per_page = (4096-4)/82 = 49. */
static void make_employees_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type      = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE401;
    object_catalog[0].active    = 1;
    object_catalog_count = 1;

    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    struct { const char* k; SLSFieldType t; } cols[4] = {
        {"id", FIELD_TYPE_UINT64}, {"name", FIELD_TYPE_STRING},
        {"active", FIELD_TYPE_BOOL}, {"salary", FIELD_TYPE_FLOAT}
    };
    for (int i = 0; i < 4; i++) {
        strcpy(object_schemas[0].fields[i].key, cols[i].k);
        object_schemas[0].fields[i].type   = cols[i].t;
        object_schemas[0].fields[i].active = 1;
    }
    object_schemas[0].field_count = 4;
}

static struct RowValues row_of(uint64_t id, const char* name, int active, double salary) {
    struct RowValues v; memset(&v, 0, sizeof(v));
    v.count = 4;
    snprintf(v.values[0], RECORD_VAL_LEN, "%llu", (unsigned long long)id);
    strncpy(v.values[1], name, RECORD_VAL_LEN - 1);
    strcpy(v.values[2], active ? "true" : "false");
    snprintf(v.values[3], RECORD_VAL_LEN, "%.2f", salary);
    return v;
}

int main(void) {
    make_employees_table();
    rowstore_create_table("employees");

    /* ── Scenario 1: row_index_create validation, and creation BEFORE any
     * rows exist (index starts empty, then auto-maintains via the real
     * rowstore_row_insert -> row_index_notify_insert wiring below). ────── */
    CHECK(row_index_create(1, "idx_id", "no_such_table", "id") == 1, "s1: create on unknown table fails (1)");

    g_access_force_deny = 1;
    CHECK(row_index_create(1, "idx_denied", "employees", "id") == 2, "s1: create fails on permission denial (2)");
    g_access_force_deny = 0;

    CHECK(row_index_create(1, "idx_bad_col", "employees", "no_such_column") == 3, "s1: create on unknown column fails (3)");

    CHECK(row_index_create(1, "idx_id", "employees", "id") == 0, "s1: idx_id created on an empty table");
    CHECK(row_indexes[0].entry_count == 0, "s1: idx_id starts with 0 entries (table was empty)");
    CHECK(row_index_create(1, "idx_id", "employees", "name") == 4, "s1: reusing an index name fails (4)");

    /* ── Scenario 2: insert 50 rows with distinct ids 0..49 (forces a
     * multi-level B-tree: BTREE_ORDER=4 means only 3 keys fit per leaf
     * before a split, so 50 distinct keys forces leaf splits AND at least
     * one round of internal-node splits, exercising both split paths and
     * the up-propagation loop, not just a single-level tree). Exercises
     * the real rowstore_row_insert -> row_index_notify_insert wiring for
     * idx_id, which already existed before any row was inserted. ───────── */
    struct RowId ids[50];
    int inserted = 0;
    for (uint64_t i = 0; i < 50; i++) {
        char name[16]; snprintf(name, sizeof(name), "emp%llu", (unsigned long long)i);
        struct RowValues v = row_of(i, name, (i % 2 == 0), (double)i - 25.0);
        if (rowstore_row_insert(1, "employees", &v, &ids[i]) == 0) inserted++;
    }
    CHECK(inserted == 50, "s2: all 50 rows inserted");
    CHECK(row_indexes[0].entry_count == 50, "s2: idx_id auto-maintained to 50 entries via the real insert path");

    /* ── Scenario 3: exact-match lookup on the UINT64 index, every one of
     * the 50 ids, each returning exactly its own row_id — the strongest
     * per-key check that the tree's internal routing is correct after
     * however many splits scenario 2 triggered. ─────────────────────────── */
    int all_hit = 1;
    for (uint64_t i = 0; i < 50 && all_hit; i++) {
        char text[24]; snprintf(text, sizeof(text), "%llu", (unsigned long long)i);
        struct RowId out[4];
        uint32_t n = row_index_lookup(1, "idx_id", text, out, 4);
        if (n != 1 || out[0].page_id != ids[i].page_id || out[0].slot_index != ids[i].slot_index)
            all_hit = 0;
    }
    CHECK(all_hit, "s3: exact lookup for all 50 ids returns exactly the right row_id, individually");
    CHECK(row_index_lookup(1, "idx_id", "9999", NULL, 0) == 0, "s3: lookup for a nonexistent id returns 0");

    /* ── Scenario 4: full unbounded range scan returns all 50 in strictly
     * ascending numeric order — the strongest whole-tree check: if any
     * split or leaf-chain link were wrong, this would show a gap,
     * duplicate, or out-of-order id somewhere in the walk. ─────────────── */
    struct RowId scan_buf[64];
    uint32_t scan_n = row_index_range_scan(1, "idx_id", NULL, NULL, scan_buf, 64);
    CHECK(scan_n == 50, "s4: unbounded range scan returns all 50 entries");
    int strictly_ascending = 1;
    for (uint32_t i = 0; i < scan_n && strictly_ascending; i++) {
        struct RowValues rv;
        if (rowstore_row_get(1, "employees", scan_buf[i], &rv) != 0) { strictly_ascending = 0; break; }
        uint64_t got = strtoull(rv.values[0], NULL, 10);
        if (got != i) strictly_ascending = 0;   /* expect exactly 0,1,2,...,49 in order */
    }
    CHECK(strictly_ascending, "s4: unbounded range scan visits ids 0..49 in strict ascending order, no gaps/dupes");

    /* ── Scenario 5: bounded range scans, both sides and one-sided. ──────── */
    scan_n = row_index_range_scan(1, "idx_id", "10", "20", scan_buf, 64);
    CHECK(scan_n == 11, "s5: range_scan(10,20) on a UINT64 index returns exactly 11 (10..20 inclusive)");

    scan_n = row_index_range_scan(1, "idx_id", NULL, "4", scan_buf, 64);
    CHECK(scan_n == 5, "s5: range_scan(NULL,4) returns exactly 5 (0..4)");

    scan_n = row_index_range_scan(1, "idx_id", "47", NULL, scan_buf, 64);
    CHECK(scan_n == 3, "s5: range_scan(47,NULL) returns exactly 3 (47..49)");

    /* ── Scenario 6: STRING index built via row_index_create's SCAN path
     * (not insert-time auto-maintenance) — exercises rowstore_table_scan's
     * callback wiring into btree_insert directly. Lexicographic (not
     * numeric) ordering deliberately verified: "emp19" < "emp2" as text. ── */
    CHECK(row_index_create(1, "idx_name", "employees", "name") == 0, "s6: idx_name built from 50 existing rows via scan");
    CHECK(row_indexes[1].entry_count == 50, "s6: idx_name has all 50 distinct names");

    scan_n = row_index_range_scan(1, "idx_name", "emp1", "emp19", scan_buf, 64);
    CHECK(scan_n == 11, "s6: lexicographic range [emp1,emp19] contains exactly 11 names (emp1, emp10..emp19)");

    struct RowId emp2_out[2];
    uint32_t emp2_n = row_index_lookup(1, "idx_name", "emp2", emp2_out, 2);
    CHECK(emp2_n == 1 && emp2_out[0].page_id == ids[2].page_id && emp2_out[0].slot_index == ids[2].slot_index,
          "s6: exact lookup on the STRING index finds the right row");

    /* ── Scenario 7: BOOL index built via scan, deliberately exceeding
     * BTREE_MAX_DUPES_PER_KEY (16) — 25 rows share active=true. Verifies
     * the documented "fails loud and narrow" cap: the table itself is
     * unaffected (all 50 rows still there), but the index caps out at 16
     * entries for that one key rather than silently losing track of which
     * 16, or corrupting the tree. ────────────────────────────────────────── */
    CHECK(row_index_create(1, "idx_active", "employees", "active") == 0, "s7: idx_active built from scan");
    struct RowId true_out[32];
    uint32_t true_n = row_index_lookup(1, "idx_active", "true", true_out, 32);
    CHECK(true_n == BTREE_MAX_DUPES_PER_KEY, "s7: duplicate cap enforced -- lookup(true) returns exactly 16, not 25");
    CHECK(table_headers[0].row_count == 50, "s7: the underlying table itself is unaffected by the index's duplicate cap");

    /* ── Scenario 8: FLOAT index (negative/positive/zero), built via scan,
     * verifying the sign-flip memcmp-orderable encoding. ─────────────────── */
    CHECK(row_index_create(1, "idx_salary", "employees", "salary") == 0, "s8: idx_salary built from scan");
    scan_n = row_index_range_scan(1, "idx_salary", "-25", "-20", scan_buf, 64);
    CHECK(scan_n == 6, "s8: FLOAT range scan across an all-negative range (-25..-20) returns exactly 6");
    scan_n = row_index_range_scan(1, "idx_salary", "-2", "2", scan_buf, 64);
    CHECK(scan_n == 5, "s8: FLOAT range scan spanning zero (-2..2) returns exactly 5 (-2,-1,0,1,2)");
    scan_n = row_index_range_scan(1, "idx_salary", NULL, NULL, scan_buf, 64);
    /* Confirm full ascending order across the negative/positive boundary. */
    int salary_ascending = 1; double prev = -1e18;
    for (uint32_t i = 0; i < scan_n; i++) {
        struct RowValues rv;
        rowstore_row_get(1, "employees", scan_buf[i], &rv);
        double v = strtod(rv.values[3], NULL);
        if (v < prev) { salary_ascending = 0; break; }
        prev = v;
    }
    CHECK(scan_n == 50 && salary_ascending, "s8: full FLOAT range scan is in ascending order across the negative/positive boundary");

    /* ── Scenario 9: update re-indexing — change row 25's id from 25 to
     * 9999 via the real rowstore_row_update -> row_index_notify_update
     * path; the OLD key must stop resolving and the NEW key must resolve
     * to the same row_id. Also confirms idx_name (a DIFFERENT index on the
     * same table, built via the scan path) is untouched since "name"
     * wasn't the column that changed. ────────────────────────────────────── */
    struct RowValues updated = row_of(9999, "emp25", 1, 100.0);
    CHECK(rowstore_row_update(1, "employees", ids[25], &updated) == 0, "s9: update on row 25 (id 25 -> 9999) succeeds");
    CHECK(row_index_lookup(1, "idx_id", "25", NULL, 0) == 0, "s9: idx_id no longer resolves the OLD id 25");
    struct RowId out9999[2];
    uint32_t n9999 = row_index_lookup(1, "idx_id", "9999", out9999, 2);
    CHECK(n9999 == 1 && out9999[0].page_id == ids[25].page_id && out9999[0].slot_index == ids[25].slot_index,
          "s9: idx_id resolves the NEW id 9999 to the same row_id");
    CHECK(row_index_lookup(1, "idx_name", "emp25", NULL, 0) == 1, "s9: idx_name (unrelated column) still resolves 'emp25' unaffected");

    /* ── Scenario 10: delete re-indexing — deleting row 30 via the real
     * rowstore_row_delete -> row_index_notify_delete path must remove it
     * from EVERY index defined on the table, not just idx_id. ────────────── */
    CHECK(rowstore_row_delete(1, "employees", ids[30]) == 0, "s10: delete on row 30 succeeds");
    CHECK(row_index_lookup(1, "idx_id", "30", NULL, 0) == 0, "s10: idx_id no longer resolves deleted row 30's id");
    CHECK(row_index_lookup(1, "idx_name", "emp30", NULL, 0) == 0, "s10: idx_name no longer resolves deleted row 30's name either");
    scan_n = row_index_range_scan(1, "idx_id", NULL, NULL, scan_buf, 64);
    CHECK(scan_n == 49, "s10: idx_id's full range scan now shows exactly 49 entries (50 minus the delete, plus the id-25->9999 rename netting zero change)");

    /* ── Scenario 11: permission gating on lookup/range_scan. ─────────────── */
    g_access_force_deny = 1;
    CHECK(row_index_lookup(1, "idx_id", "9999", NULL, 0) == 0, "s11: lookup denied by catalog_check_access returns 0");
    CHECK(row_index_range_scan(1, "idx_id", NULL, NULL, NULL, 0) == 0, "s11: range_scan denied by catalog_check_access returns 0");
    g_access_force_deny = 0;

    /* ── Scenario 12: unencodable query values fail cleanly, not a crash. ── */
    CHECK(row_index_lookup(1, "idx_id", "not-a-number", NULL, 0) == 0, "s12: lookup with an unparseable UINT64 value returns 0, not a crash");
    CHECK(row_index_lookup(1, "no_such_index", "9999", NULL, 0) == 0, "s12: lookup against an unknown index name returns 0");

    /* ── Scenario 13: ROW_INDEX_MAX capacity — 4 indexes already defined
     * (idx_id, idx_name, idx_active, idx_salary); fill the remaining 12
     * slots, then confirm the next one fails cleanly (4). ────────────────── */
    int created = 0;
    for (int i = 0; i < 12; i++) {
        char name[32]; snprintf(name, sizeof(name), "idx_filler_%d", i);
        if (row_index_create(1, name, "employees", "id") == 0) created++;
    }
    CHECK(created == 12, "s13: 12 more indexes fill ROW_INDEX_MAX (4 + 12 = 16)");
    CHECK(row_index_create(1, "idx_overflow", "employees", "id") == 4, "s13: the 17th index creation fails cleanly (4), pool exhausted");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
