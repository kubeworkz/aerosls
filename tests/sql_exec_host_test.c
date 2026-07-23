/*
 * sql_exec_host_test.c — Phase 19 (relational layer) verification: a
 * standalone host-buildable test for kernel/sql_exec.c's planner +
 * executor, linked against the REAL, unmodified kernel/sql_exec.c,
 * kernel/sql_parser.c, kernel/predicate.c, kernel/row_index.c,
 * kernel/rowstore.c, kernel/persist.c, AND kernel/cursor.c — the full real
 * stack end to end, not a reimplementation of any layer.
 *
 * This is the "end-to-end execution is a heavier dependency graph" half
 * the roadmap's own verification plan named — real execution anyway,
 * exceeding that plan's own stated ceiling (which allowed for compile-
 * check-plus-logic-review here), matching every prior phase's pattern of
 * meeting or beating its own predicted verification bar.
 *
 * Phase 22 update: sql_exec.c now routes every statement through
 * kernel/mvcc.c (each sql_execute() call is its own autocommit
 * transaction), so this test now also links kernel/mvcc.c and calls
 * mvcc_init() at startup. Every scenario below still passes unchanged --
 * MVCC is a correct, transparent pass-through for single-statement
 * autocommit callers, confirming this phase's rewiring didn't regress
 * Phase 19's own behavior.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_exec_host_test tests/sql_exec_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_exec_host_test
 *
 * Database Namespace & Access Roadmap Phase 2 update: sql_exec.c's
 * exec_create_table()/exec_create_database()/exec_drop_database() now call
 * straight into kernel/database.c (database_find_id()/_create()/_drop()),
 * so this test's link line now includes it and this file adds a minimal
 * catalog_get_role() stub below (database_drop()'s own permission gate
 * dependency) -- none of this test's own scenarios exercise CREATE/DROP
 * DATABASE or IN DATABASE, so the stub's exact return value doesn't affect
 * any existing check, only linkability.
 *
 * Phase 23 update: mvcc.c now calls into kernel/row_constraint.c/
 * kernel/row_journal.c automatically, so this test's link line now
 * includes both. Neither is initialized/populated here, so every call is a
 * guaranteed no-op -- see row_constraint_journal_host_test.c for the test
 * that actually exercises them.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/predicate.h"
#include "kernel/sql_exec.h"
#include "kernel/cursor.h"
#include "kernel/index_mgr.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/persist.h"
#include "user/permissions.h"
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Dummy definitions for subsystems the linked files also touch but
 * this test doesn't exercise (see rowstore_host_test.c/row_index_host_
 * test.c precedent) ──────────────────────────────────────────────────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
struct SLSIndex        index_store[INDEX_MAX];   /* cursor.c's legacy path references this; unused here */
uint32_t               index_count = 0;
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }
// Database Namespace & Access Roadmap Phase 2: kernel/database.c's
// database_drop() calls catalog_get_role() for its permission gate -- this
// test never calls CREATE/DROP DATABASE, so the return value is a pure
// linkability stub, not exercised by any scenario below.
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
// Database Namespace & Access Roadmap Phase 3: kernel/database.c's
// database_check_access() calls group_contains_uid() (group_profile.c) to
// resolve grantee groups -- this test never calls any database-grant
// function either, so this is a pure linkability stub, not exercised by
// any scenario below.
int group_contains_uid(const char* name, uint32_t uid) { (void)name; (void)uid; return 0; }

/* Gap Remediation Phase D: persist.c's new restore blocks 9-10 reference
 * vecstore.c/vec_index.c's own globals/functions, neither of which is
 * linked here (this suite is scoped to the RDBMS layer) -- stubbed purely
 * to satisfy the linker, matching rowstore_host_test.c's own identical
 * precedent; both restore blocks correctly no-op at "no snapshot" before
 * ever touching this content. */
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

static int      g_access_calls = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    g_access_calls++;
    return g_access_force_deny ? 0 : 1;
}

void* allocate_physical_ram_frame(void) { return malloc(4096); }

#define FAKE_NVME_MAX_FRAMES 512
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
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

struct collect_ctx { char ids_seen[64][32]; char names_seen[64][32]; uint32_t count; };
static void collect_id_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collect_ctx* ctx = (struct collect_ctx*)ctxp;
    if (ctx->count < 64) {
        strncpy(ctx->ids_seen[ctx->count], v->values[0], 31);
        strncpy(ctx->names_seen[ctx->count], v->values[1], 31);
        ctx->count++;
    }
}

static void make_employees_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE701;
    object_catalog[0].active = 1;
    object_catalog_count = 1;
    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");     object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "name");   object_schemas[0].fields[1].type = FIELD_TYPE_STRING; object_schemas[0].fields[1].active = 1;
    strcpy(object_schemas[0].fields[2].key, "active"); object_schemas[0].fields[2].type = FIELD_TYPE_BOOL;   object_schemas[0].fields[2].active = 1;
    object_schemas[0].field_count = 3;
    rowstore_create_table("employees");
}

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- sql_exec.c's
// new exec_create_table() now unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's new rowstore_drop_table() now
// unconditionally calls sys_sls_vfree() (real object_catalog.c cleanup
// this test doesn't link). This test never exercises CREATE/DROP TABLE
// via SQL text at runtime, so failure-code no-ops are safe here -- see
// tests/sql_ddl_phase5_host_test.c for the real coverage of these paths. ──
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 0; }
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) { (void)req; return 1; }
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();   // Phase 22: sql_exec.c now routes every statement through mvcc.c
    make_employees_table();
    CHECK(row_index_create(1, "idx_id", "employees", "id") == 0, "setup: idx_id created on 'id'");

    /* ── Scenario 1: INSERT via SQL, 20 rows, id 0..19 ────────────────────── */
    int inserted = 0;
    struct SqlResult r;
    for (int i = 0; i < 20; i++) {
        char q[256];
        snprintf(q, sizeof(q), "INSERT INTO employees (id, name, active) VALUES (%d, 'p%d', %s)",
                 i, i, (i % 3 == 0) ? "TRUE" : "FALSE");
        if (sql_execute(1, q, &r) == 0) inserted++;
    }
    CHECK(inserted == 20, "scenario 1: all 20 INSERTs succeed");
    CHECK(r.affected_rows == 1 && r.kind == SQL_STMT_INSERT, "scenario 1: last INSERT reports affected_rows=1");

    /* ── Scenario 2: INSERT error paths ───────────────────────────────────── */
    // SQL Feature-Parity Roadmap Phase 6: INSERT omitting a column is no
    // longer an error -- the omitted column is filled with a real NULL
    // (partial-column INSERT). This check now exercises the opposite,
    // still-real error: naming MORE columns than the table has.
    CHECK(sql_execute(1, "INSERT INTO employees (id, name, active, bogus) VALUES (99, 'x', TRUE, 'y')", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_COUNT_MISMATCH,
          "scenario 2: INSERT naming more columns than the table has fails cleanly (column count mismatch)");
    CHECK(sql_execute(1, "INSERT INTO employees (id, name, no_such_col) VALUES (99, 'x', 'y')", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: INSERT naming an unknown column fails cleanly");
    CHECK(sql_execute(1, "INSERT INTO no_such_table (a) VALUES (1)", &r) == 1 &&
          r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 2: INSERT into an unknown table fails cleanly");
    CHECK(sql_execute(1, "GARBAGE", &r) == 1 && r.error == SQL_ERR_PARSE,
          "scenario 2: a parse error propagates as SQL_ERR_PARSE with a message");
    CHECK(r.error_msg[0] != '\0', "scenario 2: parse error carries a human-readable message");

    /* ── Scenario 3: SELECT using the INDEX path (single top-level EQ on an
     * indexed column) -- exact match. ────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id = 7", &r) == 0, "scenario 3: indexed exact-match SELECT succeeds");
    CHECK(r.row_count == 1, "scenario 3: exactly 1 row matched id=7");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_id_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.ids_seen[0], "7") == 0, "scenario 3: fetched row is actually id=7");
        cursor_close(r.cursor_id);   // CURSOR_MAX (cursor.h) is only 8 slots -- close each cursor once done, matching real usage
    }

    /* ── Scenario 4: a range comparison on an indexed column. Gap Remediation
     * Phase B's restored index-assisted planner deliberately does NOT
     * accelerate ranges (see sql_exec.c's header comment on
     * try_index_assisted_eq() for why -- the B-tree duplicate-cap
     * completeness proof this phase relies on doesn't extend safely to a
     * range scan), so this always runs the full mvcc_table_scan() +
     * predicate_eval() path -- still exercised here to confirm that path
     * remains correct on its own, independent of the EQ fast path. ──────── */
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id > 15", &r) == 0, "scenario 4: range SELECT (id > 15, full-scan path) succeeds");
    CHECK(r.row_count == 4, "scenario 4: id > 15 matches exactly 4 rows (16,17,18,19)");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: SELECT falling back to a full scan -- AND-wrapped WHERE
     * (planner only uses the index for a single top-level comparison) and a
     * WHERE on a column with no index at all. Both must still be correct. ── */
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id >= 3 AND id <= 7", &r) == 0, "scenario 5: AND-wrapped WHERE (forces full-scan fallback) succeeds");
    CHECK(r.row_count == 5, "scenario 5: id BETWEEN 3 and 7 inclusive matches exactly 5 rows");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE active = TRUE", &r) == 0, "scenario 5: WHERE on a non-indexed column succeeds (full scan)");
    CHECK(r.row_count == 7, "scenario 5: active=TRUE (i%3==0 for i in 0..19) matches exactly 7 rows (0,3,6,9,12,15,18)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id != 5", &r) == 0, "scenario 5: != always falls back to full scan (never index-assisted) and is still correct");
    CHECK(r.row_count == 19, "scenario 5: id != 5 matches exactly 19 of 20 rows");
    cursor_close(r.cursor_id);

    /* ── Scenario 6: ORDER BY ASC/DESC, both directions, verified against the
     * REAL underlying column, not just a count. ──────────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id < 5 ORDER BY id DESC", &r) == 0, "scenario 6: ORDER BY DESC succeeds");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_id_cb, &ctx);
        CHECK(ctx.count == 5 &&
              strcmp(ctx.ids_seen[0], "4") == 0 && strcmp(ctx.ids_seen[1], "3") == 0 &&
              strcmp(ctx.ids_seen[2], "2") == 0 && strcmp(ctx.ids_seen[3], "1") == 0 &&
              strcmp(ctx.ids_seen[4], "0") == 0,
              "scenario 6: rows come back in strict descending id order (4,3,2,1,0)");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id < 5 ORDER BY id ASC", &r) == 0, "scenario 6: ORDER BY ASC (default direction) succeeds");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_id_cb, &ctx);
        CHECK(ctx.count == 5 && strcmp(ctx.ids_seen[0], "0") == 0 && strcmp(ctx.ids_seen[4], "4") == 0,
              "scenario 6: rows come back in strict ascending id order (0..4)");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 7: LIMIT truncates an ordered result set correctly. ─────── */
    CHECK(sql_execute(1, "SELECT * FROM employees ORDER BY id ASC LIMIT 3", &r) == 0, "scenario 7: LIMIT 3 succeeds");
    CHECK(r.row_count == 3, "scenario 7: exactly 3 rows returned despite 20 matching");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_id_cb, &ctx);
        CHECK(ctx.count == 3 && strcmp(ctx.ids_seen[0], "0") == 0 && strcmp(ctx.ids_seen[2], "2") == 0,
              "scenario 7: LIMIT keeps the first 3 rows post-ORDER-BY, not an arbitrary 3");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 8: cursor pagination across multiple fetch calls, same
     * position/done shape the legacy cursor path already used. ──────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees ORDER BY id ASC", &r) == 0, "scenario 8: full ordered SELECT for pagination test");
    CHECK(r.row_count == 20, "scenario 8: all 20 rows in the cursor");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        uint32_t got1 = cursor_fetch_rows(r.cursor_id, 7, collect_id_cb, &ctx);
        uint32_t got2 = cursor_fetch_rows(r.cursor_id, 7, collect_id_cb, &ctx);
        uint32_t got3 = cursor_fetch_rows(r.cursor_id, 7, collect_id_cb, &ctx);
        uint32_t got4 = cursor_fetch_rows(r.cursor_id, 7, collect_id_cb, &ctx);
        CHECK(got1 == 7 && got2 == 7 && got3 == 6 && got4 == 0,
              "scenario 8: fetch-7-at-a-time across 20 rows yields 7,7,6,0 -- position/done pagination works");
        CHECK(ctx.count == 20 && strcmp(ctx.ids_seen[0], "0") == 0 && strcmp(ctx.ids_seen[19], "19") == 0,
              "scenario 8: all 20 rows delivered across calls, in order, none skipped or duplicated");
        cursor_close(r.cursor_id);
    }
    CHECK(cursor_fetch_rows(999999, 10, NULL, NULL) == 0, "scenario 8: fetching an unknown cursor_id returns 0 cleanly");

    /* ── Scenario 9: UPDATE via SQL, with and without WHERE. ──────────────── */
    CHECK(sql_execute(1, "UPDATE employees SET name = 'renamed' WHERE id = 5", &r) == 0, "scenario 9: UPDATE with WHERE succeeds");
    CHECK(r.affected_rows == 1, "scenario 9: exactly 1 row affected");
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id = 5", &r) == 0, "scenario 9: re-SELECT the updated row succeeds");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 10, collect_id_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.names_seen[0], "renamed") == 0,
              "scenario 9: the UPDATE actually changed the stored name to 'renamed'");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(1, "UPDATE employees SET active = TRUE WHERE no_such_col = 1", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 9: UPDATE's WHERE referencing an unknown column is rejected up front (SQL_ERR_COLUMN_NOT_FOUND), not silently matching zero rows");

    /* ── Scenario 10: DELETE via SQL. ─────────────────────────────────────── */
    CHECK(sql_execute(1, "DELETE FROM employees WHERE id < 3", &r) == 0, "scenario 10: DELETE with WHERE succeeds");
    CHECK(r.affected_rows == 3, "scenario 10: exactly 3 rows deleted (ids 0,1,2)");
    CHECK(sql_execute(1, "SELECT * FROM employees", &r) == 0 && r.row_count == 17, "scenario 10: 17 rows remain (20 - 3)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id = 1", &r) == 0 && r.row_count == 0, "scenario 10: deleted row id=1 no longer matches any SELECT");
    cursor_close(r.cursor_id);

    /* ── Scenario 11: permission denial propagates from the reused
     * rowstore.c/predicate.c/row_index.c gates -- no new gate was added. ──── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT * FROM employees", &r) == 0 && r.row_count == 0,
          "scenario 11: permission denial makes a SELECT return 0 rows cleanly (not an error -- matches predicate_table_scan's own denial contract)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "INSERT INTO employees (id, name, active) VALUES (500, 'x', 'true')", &r) == 1 &&
          r.error == SQL_ERR_PERMISSION_DENIED,
          "scenario 11: permission denial makes an INSERT fail with SQL_ERR_PERMISSION_DENIED");
    g_access_force_deny = 0;
    CHECK(g_access_calls > 0, "scenario 11: catalog_check_access was genuinely exercised across this whole test");

    /* ── Scenario 12: unknown-table errors for SELECT/UPDATE/DELETE too. ──── */
    CHECK(sql_execute(1, "SELECT * FROM no_such_table", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND, "scenario 12: SELECT on unknown table");
    CHECK(sql_execute(1, "UPDATE no_such_table SET a = 1", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND, "scenario 12: UPDATE on unknown table");
    CHECK(sql_execute(1, "DELETE FROM no_such_table", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND, "scenario 12: DELETE on unknown table");
    CHECK(sql_execute(1, "SELECT bogus_col FROM employees", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND, "scenario 12: SELECT of an unknown column");
    CHECK(sql_execute(1, "SELECT * FROM employees ORDER BY bogus_col", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND, "scenario 12: ORDER BY an unknown column");

    /* ── Scenario 13 (Gap Remediation Phase B): the correctness-critical case
     * try_index_assisted_eq()'s completeness check exists for -- a value
     * with MORE duplicate rows than a B-tree leaf's BTREE_MAX_DUPES_PER_KEY
     * (16) cap. If the planner trusted an incomplete index result here, it
     * would silently UNDER-report matches; it must instead detect the cap
     * and fall back to a full scan, returning the true, complete count. ──── */
    memset(&object_catalog[1], 0, sizeof(object_catalog[1]));
    strcpy(object_catalog[1].name, "wide_status");
    object_catalog[1].type = OBJ_TYPE_DB_TABLE;
    object_catalog[1].object_id = 0xE702;
    object_catalog[1].active = 1;
    object_catalog_count = 2;
    memset(&object_schemas[1], 0, sizeof(object_schemas[1]));
    strcpy(object_schemas[1].fields[0].key, "sid");    object_schemas[1].fields[0].type = FIELD_TYPE_UINT64; object_schemas[1].fields[0].active = 1;
    strcpy(object_schemas[1].fields[1].key, "status"); object_schemas[1].fields[1].type = FIELD_TYPE_STRING; object_schemas[1].fields[1].active = 1;
    object_schemas[1].field_count = 2;
    CHECK(rowstore_create_table("wide_status") == 0, "scenario 13: wide_status table created");
    CHECK(row_index_create(1, "idx_wstatus", "wide_status", "status") == 0, "scenario 13: idx_wstatus created on 'status'");

    int wide_inserted = 0;
    for (int i = 0; i < 25; i++) {   // 25 > BTREE_MAX_DUPES_PER_KEY(16) -- every row shares status='dup'
        char q[256];
        snprintf(q, sizeof(q), "INSERT INTO wide_status (sid, status) VALUES (%d, 'dup')", i);
        if (sql_execute(1, q, &r) == 0) wide_inserted++;
    }
    CHECK(wide_inserted == 25, "scenario 13: all 25 duplicate-value INSERTs succeed (rows themselves are never capped)");

    CHECK(sql_execute(1, "SELECT * FROM wide_status WHERE status = 'dup'", &r) == 0,
          "scenario 13: EQ SELECT on the over-capacity indexed value succeeds");
    CHECK(r.row_count == 25,
          "scenario 13: all 25 rows reported -- the incomplete index was detected and the planner fell back to a full scan, not silently under-counting at 16");
    cursor_close(r.cursor_id);

    /* ── Scenario 14 (Gap Remediation Phase B): the index-assisted EQ path
     * stays correct across an MVCC UPDATE -- the OLD value must stop
     * matching and the NEW value must start, even though the B-tree index
     * still remembers the OLD physical row (mvcc_resolve_physical() is what
     * filters it out as no-longer-visible). idx_id (on 'id', scenario 3's
     * index) never approaches the duplicate cap, so this exercises the real
     * index-assisted fast path, not the scenario 13 fallback. ────────────── */
    CHECK(sql_execute(1, "UPDATE employees SET id = 777 WHERE id = 6", &r) == 0 && r.affected_rows == 1,
          "scenario 14: UPDATE changes id 6 -> 777");
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id = 6", &r) == 0 && r.row_count == 0,
          "scenario 14: EQ lookup on the OLD id (6) now correctly matches nothing");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT * FROM employees WHERE id = 777", &r) == 0 && r.row_count == 1,
          "scenario 14: EQ lookup on the NEW id (777) correctly finds exactly the updated row");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 10, collect_id_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.ids_seen[0], "777") == 0,
              "scenario 14: the row returned via the index-assisted path really is id=777");
        cursor_close(r.cursor_id);
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
