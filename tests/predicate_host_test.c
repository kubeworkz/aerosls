/*
 * predicate_host_test.c — Phase 18 (relational layer) verification: a
 * standalone host-buildable test for kernel/predicate.c's new WHERE-clause
 * evaluator, linked against the REAL, unmodified kernel/predicate.c — and,
 * for the table-scan-with-predicate half, REAL kernel/rowstore.c and
 * kernel/persist.c too — not a reimplementation.
 *
 * Two halves, matching predicate.h's own two-part scope:
 *   1. predicate_eval() is a pure function (row + schema + predicate tree
 *      -> bool) with zero I/O and zero kernel dependencies beyond the row/
 *      schema structs themselves — table-driven over every comparison
 *      operator, every SLSFieldType, AND/OR combinators, and every
 *      documented fail-closed edge case, per the roadmap's own verification
 *      plan for this phase.
 *   2. predicate_table_scan() is a thin wrapper over rowstore_table_scan()
 *      (Phase 16) — verified against a real rowstore-backed table, reusing
 *      rowstore_host_test.c's own harness shape (fake NVMe, call-tracked
 *      catalog_check_access(), real per-page frame allocation), since
 *      predicate.c inherits rowstore.c's dependency surface for this half.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/predicate_host_test \
 *       tests/predicate_host_test.c kernel/predicate.c kernel/rowstore.c kernel/storage_quota.c kernel/persist.c kernel/view.c
 *   /tmp/predicate_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/predicate.h"
#include "kernel/persist.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Dummy definitions for subsystems persist.c/rowstore.c also touch but
 * this test doesn't exercise (see rowstore_host_test.c precedent) ──────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
void catalog_after_restore(void) { /* no-op for this test */ }

void kernel_serial_print(const char* s) { (void)s; }
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

/* Phase 17 (relational layer): rowstore.c's insert/update/delete
 * unconditionally call these three row_index.c hooks. This test has no
 * interest in exercising indexing -- see row_index_host_test.c for that --
 * so they're stubbed as no-ops, matching rowstore_host_test.c's own fix
 * for the same reason. */
void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}
void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values, const struct RowValues* new_values,
                             const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)old_values; (void)new_values; (void)layout;
}
void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}

static int      g_access_calls = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    g_access_calls++;
    return g_access_force_deny ? 0 : 1;
}

/* Pre-existing gap fixed in passing (post-roadmap, found while moving host
 * tests into tests/ and reverifying every documented build line): this
 * file's own link line never linked kernel/mvcc.c/row_index.c/row_
 * constraint.c/row_journal.c/vecstore.c/vec_index.c, but persist.c's
 * restore blocks 6b-11 reference their globals/functions unconditionally
 * -- the same "persist.c grew cross-subsystem calls, one host test's own
 * stub set was never updated to match" class already found and fixed
 * several times over (Phase D, Phase F addenda) in OTHER files; this
 * particular file was apparently missed by every prior sweep. Confirmed
 * via the original, unmodified git history version of this file (before
 * this session's own unrelated tests/ move) that the link failure is
 * pre-existing, not something the move caused. Same minimal-stub pattern
 * as persist_partition_host_test.c's own identical block -- every restore
 * block correctly no-ops at "no snapshot" before ever touching this
 * content, so none of it is exercised by this test's own scenarios. */
struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                 row_constraint_count = 0;
struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count = 0;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count = 0;
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
uint32_t                   vecstore_partition_cursor[PARTITION_MAX] = {0};
struct RowIndex          row_indexes[ROW_INDEX_MAX];
struct VecIndex             vec_indexes[VEC_INDEX_MAX];
void mvcc_bootstrap_from_rowstore(void) { }
void vec_index_notify_insert(uint32_t caller_uid, const char* collection_name,
                             struct VecId id, uint64_t external_id,
                             const struct VecValues* values) {
    (void)caller_uid; (void)collection_name; (void)id; (void)external_id; (void)values;
}
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

/* ─── Fixture for the pure predicate_eval() half: a hand-built layout, no
 * rowstore/catalog involvement at all -- this half is testing predicate.c
 * as the pure function its own header promises. ─────────────────────────── */
static struct RowTableLayout make_layout(void) {
    struct RowTableLayout L; memset(&L, 0, sizeof(L));
    L.column_count = 4;
    strcpy(L.column_names[0], "id");     L.column_types[0] = FIELD_TYPE_UINT64;
    strcpy(L.column_names[1], "name");   L.column_types[1] = FIELD_TYPE_STRING;
    strcpy(L.column_names[2], "active"); L.column_types[2] = FIELD_TYPE_BOOL;
    strcpy(L.column_names[3], "salary"); L.column_types[3] = FIELD_TYPE_FLOAT;
    return L;
}
static struct RowValues make_row(void) {
    struct RowValues v; memset(&v, 0, sizeof(v));
    v.count = 4;
    strcpy(v.values[0], "25");
    strcpy(v.values[1], "carol");
    strcpy(v.values[2], "true");
    strcpy(v.values[3], "42.5");
    return v;
}

static int one_cmp(const struct RowTableLayout* L, const struct RowValues* row,
                   const char* col, PredicateCompareOp op, const char* lit) {
    struct Predicate p; predicate_init(&p);
    p.root = predicate_add_comparison(&p, col, op, lit);
    return predicate_eval(&p, L, row);
}

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- rowstore.c's
// new rowstore_drop_table() now unconditionally calls sys_sls_vfree() (real
// object_catalog.c cleanup this test doesn't link) and row_index_drop()
// (not linked here since this test doesn't link the real row_index.c).
// This test never exercises DROP TABLE, so failure-code no-ops are safe --
// see tests/sql_ddl_phase5_host_test.c for real coverage of these paths. ──
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }
int row_index_drop(uint32_t caller_uid, const char* index_name) {
    (void)caller_uid; (void)index_name; return 1;
}

int main(void) {
    struct RowTableLayout L = make_layout();
    struct RowValues row = make_row();

    /* ── Part 1a: UINT64 column ("id" = 25), every operator ──────────────── */
    CHECK(one_cmp(&L, &row, "id", PRED_OP_EQ, "25")  == 1, "UINT64: 25 = 25 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_EQ, "26")  == 0, "UINT64: 25 = 26 -> false");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_NE, "25")  == 0, "UINT64: 25 != 25 -> false");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_NE, "26")  == 1, "UINT64: 25 != 26 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_LT, "26")  == 1, "UINT64: 25 < 26 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_LT, "25")  == 0, "UINT64: 25 < 25 -> false");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_GT, "24")  == 1, "UINT64: 25 > 24 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_GT, "25")  == 0, "UINT64: 25 > 25 -> false");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_LE, "25")  == 1, "UINT64: 25 <= 25 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_LE, "24")  == 0, "UINT64: 25 <= 24 -> false");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_GE, "25")  == 1, "UINT64: 25 >= 25 -> true");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_GE, "26")  == 0, "UINT64: 25 >= 26 -> false");
    /* Numeric, not lexical: "9" < "25" numerically is false, but "9" > "25" lexically is true -- must NOT be a string compare. */
    CHECK(one_cmp(&L, &row, "id", PRED_OP_GT, "9")   == 1, "UINT64: 25 > 9 numerically -> true (would be false as a string compare)");

    /* ── Part 1b: STRING column ("name" = "carol"), every operator ──────── */
    CHECK(one_cmp(&L, &row, "name", PRED_OP_EQ, "carol") == 1, "STRING: carol = carol -> true");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_EQ, "bob")   == 0, "STRING: carol = bob -> false");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_NE, "bob")   == 1, "STRING: carol != bob -> true");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_LT, "dave")  == 1, "STRING: carol < dave lexically -> true");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_LT, "bob")   == 0, "STRING: carol < bob lexically -> false");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_GT, "bob")   == 1, "STRING: carol > bob lexically -> true");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_LE, "carol") == 1, "STRING: carol <= carol -> true");
    CHECK(one_cmp(&L, &row, "name", PRED_OP_GE, "carol") == 1, "STRING: carol >= carol -> true");

    /* ── Part 1c: BOOL column ("active" = true), every operator ──────────── */
    CHECK(one_cmp(&L, &row, "active", PRED_OP_EQ, "true")  == 1, "BOOL: true = true -> true");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_EQ, "false") == 0, "BOOL: true = false -> false");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_NE, "false") == 1, "BOOL: true != false -> true");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_GT, "false") == 1, "BOOL: true > false -> true (false < true ordering)");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_LT, "true")  == 0, "BOOL: true < true -> false");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_EQ, "1")     == 1, "BOOL: accepts '1' as an alias for true");

    /* ── Part 1d: FLOAT column ("salary" = 42.5), every operator ─────────── */
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_EQ, "42.5")  == 1, "FLOAT: 42.5 = 42.5 -> true");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_LT, "42.6")  == 1, "FLOAT: 42.5 < 42.6 -> true");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_GT, "42.4")  == 1, "FLOAT: 42.5 > 42.4 -> true");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_LE, "42.5")  == 1, "FLOAT: 42.5 <= 42.5 -> true");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_GE, "43")    == 0, "FLOAT: 42.5 >= 43 -> false");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_LT, "-1")    == 0, "FLOAT: 42.5 < -1 -> false (signed comparison correct)");

    /* ── Part 2: AND / OR combinators, including nesting ──────────────────── */
    {
        struct Predicate p; predicate_init(&p);
        uint32_t a = predicate_add_comparison(&p, "id", PRED_OP_EQ, "25");
        uint32_t b = predicate_add_comparison(&p, "active", PRED_OP_EQ, "true");
        p.root = predicate_add_and(&p, a, b);
        CHECK(predicate_eval(&p, &L, &row) == 1, "AND: (id=25 AND active=true) -> true");
    }
    {
        struct Predicate p; predicate_init(&p);
        uint32_t a = predicate_add_comparison(&p, "id", PRED_OP_EQ, "25");
        uint32_t b = predicate_add_comparison(&p, "active", PRED_OP_EQ, "false");
        p.root = predicate_add_and(&p, a, b);
        CHECK(predicate_eval(&p, &L, &row) == 0, "AND: (id=25 AND active=false) -> false");
    }
    {
        struct Predicate p; predicate_init(&p);
        uint32_t a = predicate_add_comparison(&p, "id", PRED_OP_EQ, "999");
        uint32_t b = predicate_add_comparison(&p, "active", PRED_OP_EQ, "true");
        p.root = predicate_add_or(&p, a, b);
        CHECK(predicate_eval(&p, &L, &row) == 1, "OR: (id=999 OR active=true) -> true");
    }
    {
        struct Predicate p; predicate_init(&p);
        uint32_t a = predicate_add_comparison(&p, "id", PRED_OP_EQ, "999");
        uint32_t b = predicate_add_comparison(&p, "active", PRED_OP_EQ, "false");
        p.root = predicate_add_or(&p, a, b);
        CHECK(predicate_eval(&p, &L, &row) == 0, "OR: (id=999 OR active=false) -> false");
    }
    {
        /* id=25 AND (salary>100 OR active=true) -> true (nested OR inside AND) */
        struct Predicate p; predicate_init(&p);
        uint32_t idc  = predicate_add_comparison(&p, "id", PRED_OP_EQ, "25");
        uint32_t sal  = predicate_add_comparison(&p, "salary", PRED_OP_GT, "100");
        uint32_t act  = predicate_add_comparison(&p, "active", PRED_OP_EQ, "true");
        uint32_t orn  = predicate_add_or(&p, sal, act);
        p.root = predicate_add_and(&p, idc, orn);
        CHECK(predicate_eval(&p, &L, &row) == 1, "nested: id=25 AND (salary>100 OR active=true) -> true");
    }

    /* ── Part 3: fail-closed edge cases ───────────────────────────────────── */
    CHECK(predicate_eval(NULL, &L, &row) == 1, "NULL predicate matches every row");
    {
        struct Predicate p; predicate_init(&p);
        CHECK(predicate_eval(&p, &L, &row) == 1, "freshly-initialised (empty, no root set) predicate matches every row");
    }
    CHECK(one_cmp(&L, &row, "no_such_column", PRED_OP_EQ, "x") == 0, "comparison on an unknown column fails closed (false), not a crash");
    CHECK(one_cmp(&L, &row, "id", PRED_OP_EQ, "not-a-number") == 0, "unparseable UINT64 literal fails closed (false)");
    CHECK(one_cmp(&L, &row, "salary", PRED_OP_EQ, "not-a-float") == 0, "unparseable FLOAT literal fails closed (false)");
    CHECK(one_cmp(&L, &row, "active", PRED_OP_EQ, "maybe") == 0, "unparseable BOOL literal fails closed (false)");
    {
        struct Predicate p; predicate_init(&p);
        p.root = 999;   /* malformed: root points past node_count (0) */
        CHECK(predicate_eval(&p, &L, &row) == 0, "malformed tree (root out of range) fails closed (false), not a crash");
    }
    {
        struct Predicate p; predicate_init(&p);
        uint32_t a = predicate_add_comparison(&p, "id", PRED_OP_EQ, "25");
        CHECK(predicate_add_and(&p, a, 999) == PREDICATE_INVALID_NODE, "predicate_add_and rejects an out-of-range child index");
    }
    {
        struct Predicate p; predicate_init(&p);
        int ok_count = 0;
        for (int i = 0; i < PREDICATE_MAX_NODES; i++)
            if (predicate_add_comparison(&p, "id", PRED_OP_EQ, "1") != PREDICATE_INVALID_NODE) ok_count++;
        CHECK(ok_count == PREDICATE_MAX_NODES, "node pool fills to exactly PREDICATE_MAX_NODES");
        CHECK(predicate_add_comparison(&p, "id", PRED_OP_EQ, "1") == PREDICATE_INVALID_NODE,
              "the next comparison after the pool is full fails cleanly (PREDICATE_INVALID_NODE)");
    }

    /* ── Part 4: predicate_table_scan() against a REAL rowstore-backed
     * table -- the wrapper half, not just the pure evaluator. ───────────── */
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE501;
    object_catalog[0].active = 1;
    object_catalog_count = 1;
    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");     object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "active"); object_schemas[0].fields[1].type = FIELD_TYPE_BOOL;   object_schemas[0].fields[1].active = 1;
    object_schemas[0].field_count = 2;
    rowstore_create_table("employees");

    for (uint64_t i = 0; i < 20; i++) {
        struct RowValues v; memset(&v, 0, sizeof(v));
        v.count = 2;
        snprintf(v.values[0], RECORD_VAL_LEN, "%llu", (unsigned long long)i);
        strcpy(v.values[1], (i % 3 == 0) ? "true" : "false");
        struct RowId out;
        rowstore_row_insert(1, "employees", &v, &out);
    }

    struct Predicate scan_pred; predicate_init(&scan_pred);
    scan_pred.root = predicate_add_comparison(&scan_pred, "active", PRED_OP_EQ, "true");
    uint32_t matched = predicate_table_scan(1, "employees", &scan_pred, NULL, NULL);
    CHECK(matched == 7, "predicate_table_scan: 'active=true' matches exactly 7 of 20 rows (0,3,6,9,12,15,18)");

    uint32_t total = predicate_table_scan(1, "employees", NULL, NULL, NULL);
    CHECK(total == 20, "predicate_table_scan: NULL predicate visits all 20 rows, matching rowstore_table_scan directly");

    CHECK(predicate_table_scan(1, "no_such_table", &scan_pred, NULL, NULL) == 0,
          "predicate_table_scan: unknown table returns 0 cleanly");

    g_access_force_deny = 1;
    CHECK(predicate_table_scan(1, "employees", NULL, NULL, NULL) == 0,
          "predicate_table_scan: permission denial (via the reused rowstore_table_scan gate) returns 0");
    g_access_force_deny = 0;
    CHECK(g_access_calls > 0, "predicate_table_scan reuses rowstore_table_scan's own catalog_check_access gate (no duplicate check added)");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
