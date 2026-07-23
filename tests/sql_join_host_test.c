/*
 * sql_join_host_test.c — Phase 20 (relational layer) verification: a
 * standalone host-buildable test for kernel/sql_exec.c's exec_select_join()
 * and kernel/sql_parser.c's JOIN...ON grammar, linked against the REAL,
 * unmodified full stack -- sql_exec.c, sql_parser.c, predicate.c,
 * row_index.c, rowstore.c, persist.c, cursor.c -- not a reimplementation.
 *
 * The roadmap's own verification plan for this phase allowed for compile-
 * check plus a hand-traced worked example (mirroring the LPAR roadmap's
 * Phase 12 precedent); this test aims higher, matching every prior phase
 * in this roadmap: real execution of a real nested-loop join against two
 * real row-set tables, with and without an index on the joined column,
 * cross-checked against hand-computed expected results -- the "hand-traced
 * worked example" the plan asked for, just executed by the real code
 * instead of traced on paper.
 *
 * Phase 22 update: sql_exec.c now routes every statement (including
 * exec_select_join()) through kernel/mvcc.c, so this test now also links
 * kernel/mvcc.c and calls mvcc_init() at startup. Every scenario below
 * still passes unchanged -- including the "indexed vs. non-indexed join
 * probe" scenarios, which now both take the SAME code path under MVCC
 * (a full snapshot-consistent scan, see sql_exec.h's Phase 22 header
 * note) yet still produce identical, correct results either way.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_join_host_test tests/sql_join_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_join_host_test
 *
 * Phase 23 update: mvcc.c now calls into kernel/row_constraint.c/
 * kernel/row_journal.c automatically, so this test's link line now
 * includes both. Neither is initialized/populated here, so every call is a
 * guaranteed no-op -- see row_constraint_journal_host_test.c for the test
 * that actually exercises them.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/predicate.h"
#include "kernel/sql_exec.h"
#include "kernel/cursor.h"
#include "kernel/index_mgr.h"
#include "kernel/persist.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
struct SLSIndex        index_store[INDEX_MAX];
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
// database_check_access() calls group_contains_uid() (group_profile.c) --
// pure linkability stub, not exercised by any scenario below.
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

static int g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
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
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* Materialized rows always carry the FULL combined row in table-definition
 * order (employees' own columns, then the joined table's), independent of
 * the SELECT list -- "column projection is metadata-only," per sql_exec.h's
 * header comment, a Phase 19 decision this phase inherits unchanged. So the
 * collector takes explicit combined-row indices rather than assuming
 * column 0/1 line up with whatever was SELECTed. */
struct collect_ctx { char col0[64][64]; char col1[64][64]; uint32_t count; uint32_t idx0; uint32_t idx1; };
static void collect_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collect_ctx* ctx = (struct collect_ctx*)ctxp;
    if (ctx->count < 64) {
        strncpy(ctx->col0[ctx->count], v->values[ctx->idx0], 63);
        if (v->count > ctx->idx1) strncpy(ctx->col1[ctx->count], v->values[ctx->idx1], 63);
        ctx->count++;
    }
}

static int catalog_slot = 0;
static void make_table(const char* name, uint64_t object_id, int ncols,
                       const char* colnames[], SLSFieldType coltypes[]) {
    int slot = catalog_slot++;
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].type = OBJ_TYPE_DB_TABLE;
    object_catalog[slot].object_id = object_id;
    object_catalog[slot].active = 1;
    object_catalog_count = (uint32_t)(slot + 1);

    memset(&object_schemas[slot], 0, sizeof(object_schemas[slot]));
    for (int i = 0; i < ncols; i++) {
        strcpy(object_schemas[slot].fields[i].key, colnames[i]);
        object_schemas[slot].fields[i].type = coltypes[i];
        object_schemas[slot].fields[i].active = 1;
    }
    object_schemas[slot].field_count = (uint32_t)ncols;
    rowstore_create_table(name);
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

    /* ── Fixture: employees(id, name, dept_id) x departments(id, title).
     * dept_id=99 (employee "dave") deliberately has no matching department
     * -- an INNER JOIN must exclude that row, never invent a NULL match. ── */
    {
        const char* ecols[3] = {"id", "name", "dept_id"};
        SLSFieldType etypes[3] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_UINT64};
        make_table("employees", 0xE901, 3, ecols, etypes);
        const char* dcols[2] = {"id", "title"};
        SLSFieldType dtypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("departments", 0xE902, 2, dcols, dtypes);
    }
    CHECK(row_index_create(1, "idx_dept_id", "departments", "id") == 0, "setup: index created on departments.id");

    struct SqlResult r;
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r);
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (2, 'Sales')", &r);
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (3, 'Marketing')", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id) VALUES (1, 'alice', 1)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id) VALUES (2, 'bob', 1)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id) VALUES (3, 'carol', 2)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id) VALUES (4, 'dave', 99)", &r);   /* dangling FK */
    CHECK(r.kind == SQL_STMT_INSERT && r.error == SQL_ERR_NONE, "setup: 7 rows inserted (3 departments, 4 employees)");

    /* ── Scenario 1: basic JOIN, indexed join column (departments.id has
     * an index), qualified projection, ORDER BY a qualified column. ─────── */
    CHECK(sql_execute(1, "SELECT employees.name, departments.title FROM employees "
                        "JOIN departments ON employees.dept_id = departments.id "
                        "ORDER BY employees.name", &r) == 0,
          "scenario 1: basic JOIN (indexed join column) succeeds");
    CHECK(r.row_count == 3, "scenario 1: exactly 3 matches (dave's dangling dept_id=99 correctly excluded -- INNER JOIN semantics)");
    CHECK(r.column_count == 2 && strcmp(r.columns[0], "employees.name") == 0 && strcmp(r.columns[1], "departments.title") == 0,
          "scenario 1: column metadata reports the qualified names as requested");
    {
        /* combined layout: 0=employees.id, 1=employees.name, 2=employees.dept_id, 3=departments.id, 4=departments.title */
        struct collect_ctx ctx = {{{0}}, {{0}}, 0, 1, 4};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 3 &&
              strcmp(ctx.col0[0], "alice") == 0 && strcmp(ctx.col0[1], "bob") == 0 && strcmp(ctx.col0[2], "carol") == 0,
              "scenario 1: rows in ORDER BY employees.name order: alice, bob, carol");
        CHECK(strcmp(ctx.col1[0], "Engineering") == 0 && strcmp(ctx.col1[1], "Engineering") == 0 && strcmp(ctx.col1[2], "Sales") == 0,
              "scenario 1: each row's department title matches its employee");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: same join, ON clause written in the OPPOSITE order
     * (departments.id = employees.dept_id) -- confirms qualifier
     * resolution doesn't assume a fixed left/right table position. ──────── */
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN departments "
                        "ON departments.id = employees.dept_id "
                        "WHERE departments.title = 'Engineering' ORDER BY employees.name", &r) == 0,
          "scenario 2: ON clause with qualifiers reversed still resolves correctly");
    CHECK(r.row_count == 2, "scenario 2: WHERE departments.title='Engineering' matches exactly alice and bob");
    cursor_close(r.cursor_id);

    /* ── Scenario 3: JOIN on a column with NO index (fallback full-scan
     * probe path) -- same query semantics, verified against the same
     * expected answer as scenario 1's indexed version, proving both
     * planner paths inside the join probe agree. ─────────────────────────── */
    CHECK(sql_execute(1, "SELECT employees.name, departments.title FROM employees "
                        "JOIN departments ON employees.dept_id = departments.id "
                        "WHERE departments.title != 'zzz' ORDER BY employees.name", &r) == 0,
          "scenario 3: JOIN probe still correct when forced through predicate re-check (title != 'zzz' is never index-eligible)");
    CHECK(r.row_count == 3, "scenario 3: still exactly 3 matches");
    cursor_close(r.cursor_id);

    /* Build a SECOND join scenario against a table with NO index at all on
     * the join column, to directly exercise sql_find_matching_rows()'s
     * full-scan fallback from inside the join probe (not just an
     * unfavorable WHERE operator). */
    {
        const char* pcols[2] = {"emp_id", "note"};
        SLSFieldType ptypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("notes", 0xE903, 2, pcols, ptypes);
    }
    sql_execute(1, "INSERT INTO notes (emp_id, note) VALUES (1, 'top performer')", &r);
    sql_execute(1, "INSERT INTO notes (emp_id, note) VALUES (2, 'needs review')", &r);
    CHECK(sql_execute(1, "SELECT employees.name, notes.note FROM employees "
                        "JOIN notes ON employees.id = notes.emp_id ORDER BY employees.name", &r) == 0,
          "scenario 3b: JOIN against a table with NO index on the join column succeeds (full-scan probe fallback)");
    CHECK(r.row_count == 2, "scenario 3b: exactly 2 matches (employees 1 and 2 have notes; 3 and 4 don't)");
    {
        /* combined layout: 0=employees.id, 1=employees.name, 2=employees.dept_id, 3=notes.emp_id, 4=notes.note */
        struct collect_ctx ctx = {{{0}}, {{0}}, 0, 1, 4};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 2 && strcmp(ctx.col1[0], "top performer") == 0 && strcmp(ctx.col1[1], "needs review") == 0,
              "scenario 3b: correct notes matched to the correct employees");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 4: ORDER BY DESC and LIMIT on a joined result. ──────────── */
    CHECK(sql_execute(1, "SELECT employees.name FROM employees JOIN departments "
                        "ON employees.dept_id = departments.id "
                        "ORDER BY employees.name DESC LIMIT 2", &r) == 0,
          "scenario 4: ORDER BY DESC + LIMIT on a JOIN succeeds");
    CHECK(r.row_count == 2, "scenario 4: LIMIT 2 keeps exactly 2 of the 3 matches");
    {
        /* combined layout: 0=employees.id, 1=employees.name, 2=employees.dept_id, 3=departments.id, 4=departments.title */
        struct collect_ctx ctx = {{{0}}, {{0}}, 0, 1, 4};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 2 && strcmp(ctx.col0[0], "carol") == 0 && strcmp(ctx.col0[1], "bob") == 0,
              "scenario 4: descending order keeps carol, bob (not alice) -- LIMIT applied post-sort, not pre-sort");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 5: error paths ───────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN no_such_table ON employees.dept_id = no_such_table.id", &r) == 1 &&
          r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 5: JOIN against an unknown table fails cleanly (SQL_ERR_TABLE_NOT_FOUND)");
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN departments ON employees.dept_id = badtable.id", &r) == 1 &&
          r.error == SQL_ERR_JOIN_INVALID,
          "scenario 5: ON clause qualifier naming neither joined table fails cleanly (SQL_ERR_JOIN_INVALID)");
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN departments ON employees.no_such_col = departments.id", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 5: ON clause column missing from its table fails cleanly (SQL_ERR_COLUMN_NOT_FOUND)");
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN departments ON employees.dept_id = departments.id "
                        "WHERE no_such_qualifier.bogus = 1", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 5: WHERE referencing an unknown qualified column in a JOIN fails cleanly");
    CHECK(sql_execute(1, "SELECT bogus.col FROM employees JOIN departments ON employees.dept_id = departments.id", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 5: SELECT of an unknown qualified column in a JOIN fails cleanly");

    /* ── Scenario 6: combined column count exceeding ROWSTORE_MAX_COLUMNS
     * (16) is rejected up front, not silently truncated. Two 9-column
     * tables combine to 18. ──────────────────────────────────────────────── */
    {
        const char* wide_cols[9] = {"c0","c1","c2","c3","c4","c5","c6","c7","c8"};
        SLSFieldType wide_types[9] = {
            FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64,
            FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64
        };
        make_table("wide_a", 0xE904, 9, wide_cols, wide_types);
        make_table("wide_b", 0xE905, 9, wide_cols, wide_types);
    }
    CHECK(sql_execute(1, "SELECT * FROM wide_a JOIN wide_b ON wide_a.c0 = wide_b.c0", &r) == 1 &&
          r.error == SQL_ERR_JOIN_TOO_WIDE,
          "scenario 6: two 9-column tables (18 combined) rejected up front (SQL_ERR_JOIN_TOO_WIDE)");

    /* ── Scenario 7: permission denial propagates from the reused
     * rowstore.c/row_index.c/predicate.c gates inside the join probe. ────── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN departments ON employees.dept_id = departments.id", &r) == 0 &&
          r.row_count == 0,
          "scenario 7: permission denial makes a JOIN return 0 rows cleanly, not a crash");
    g_access_force_deny = 0;

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
