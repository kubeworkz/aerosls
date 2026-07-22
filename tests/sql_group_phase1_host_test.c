/*
 * sql_group_phase1_host_test.c — SQL Feature-Parity Roadmap Phase 1
 * verification: a standalone host-buildable test for kernel/sql_exec.c's
 * exec_select_group() and kernel/sql_parser.c's GROUP BY/HAVING/aggregate
 * grammar, linked against the REAL, unmodified full stack -- sql_exec.c,
 * sql_parser.c, predicate.c, row_index.c, rowstore.c, persist.c, cursor.c,
 * mvcc.c -- not a reimplementation. Mirrors sql_join_host_test.c's own
 * fixture/stub scaffolding exactly (same stack, same reasons).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_group_phase1_host_test tests/sql_group_phase1_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_group_phase1_host_test
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

int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return 1;
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

/* Collects (dept, agg-column) pairs in cursor-fetch order for assertions
 * below -- col indices are into the SYNTHETIC grouped-result layout (the
 * exact SELECT-list order), not the source table's own columns, per
 * exec_select_group()'s own contract. */
struct collect_ctx { char col0[16][64]; char col1[16][64]; uint32_t count; };
static void collect_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collect_ctx* ctx = (struct collect_ctx*)ctxp;
    if (ctx->count < 16) {
        strncpy(ctx->col0[ctx->count], v->values[0], 63);
        if (v->count > 1) strncpy(ctx->col1[ctx->count], v->values[1], 63);
        ctx->count++;
    }
}

/* Wider 5-column collector for scenario 2 (dept, SUM, AVG, MIN, MAX). */
struct wide_ctx { char v[3][5][64]; int n; };
static void collect5(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct wide_ctx* c = (struct wide_ctx*)ctxp;
    if (c->n < 3) {
        for (int k = 0; k < 5; k++) strncpy(c->v[c->n][k], v->values[k], 63);
        c->n++;
    }
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
    mvcc_init();

    /* ── Fixture: employees(id, name, dept, salary). ─────────────────────── */
    {
        const char* cols[4] = {"id", "name", "dept", "salary"};
        SLSFieldType types[4] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_STRING, FIELD_TYPE_UINT64};
        make_table("employees", 0xF001, 4, cols, types);
    }
    struct SqlResult r;
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (1, 'alice', 'Eng', 90000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (2, 'bob', 'Eng', 85000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (3, 'carol', 'Sales', 70000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (4, 'dave', 'Sales', 72000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (5, 'erin', 'Sales', 68000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (6, 'frank', 'Marketing', 60000)", &r);
    CHECK(r.kind == SQL_STMT_INSERT && r.error == SQL_ERR_NONE, "setup: 6 rows inserted across 3 departments");

    /* ── Scenario 1: GROUP BY + COUNT(*). ─────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees GROUP BY dept ORDER BY dept", &r) == 0,
          "scenario 1: GROUP BY + COUNT(*) succeeds");
    CHECK(r.row_count == 3, "scenario 1: exactly 3 groups (Eng, Marketing, Sales)");
    CHECK(r.column_count == 2 && strcmp(r.columns[0], "dept") == 0 && strcmp(r.columns[1], "COUNT(*)") == 0,
          "scenario 1: output columns are 'dept' and the canonical label 'COUNT(*)'");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 3 &&
              strcmp(ctx.col0[0], "Eng") == 0 && strcmp(ctx.col1[0], "2") == 0 &&
              strcmp(ctx.col0[1], "Marketing") == 0 && strcmp(ctx.col1[1], "1") == 0 &&
              strcmp(ctx.col0[2], "Sales") == 0 && strcmp(ctx.col1[2], "3") == 0,
              "scenario 1: Eng=2, Marketing=1, Sales=3, in ORDER BY dept order");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: SUM/AVG/MIN/MAX together, numeric column. ────────────── */
    CHECK(sql_execute(1, "SELECT dept, SUM(salary), AVG(salary), MIN(salary), MAX(salary) "
                        "FROM employees GROUP BY dept ORDER BY dept", &r) == 0,
          "scenario 2: SUM/AVG/MIN/MAX together succeeds");
    CHECK(r.row_count == 3 && r.column_count == 5, "scenario 2: 3 groups, 5 output columns");
    {
        struct wide_ctx wc; wc.n = 0;
        cursor_fetch_rows(r.cursor_id, 100, collect5, &wc);
        CHECK(wc.n == 3, "scenario 2: 3 rows fetched");
        /* order: Eng, Marketing, Sales (ORDER BY dept) */
        CHECK(strcmp(wc.v[0][0], "Eng") == 0 && strcmp(wc.v[0][1], "175000") == 0 &&
              strcmp(wc.v[0][2], "87500") == 0 && strcmp(wc.v[0][3], "85000") == 0 && strcmp(wc.v[0][4], "90000") == 0,
              "scenario 2: Eng sum=175000 avg=87500 min=85000 max=90000");
        CHECK(strcmp(wc.v[1][0], "Marketing") == 0 && strcmp(wc.v[1][1], "60000") == 0 &&
              strcmp(wc.v[1][2], "60000") == 0 && strcmp(wc.v[1][3], "60000") == 0 && strcmp(wc.v[1][4], "60000") == 0,
              "scenario 2: Marketing (single row) sum=avg=min=max=60000");
        CHECK(strcmp(wc.v[2][0], "Sales") == 0 && strcmp(wc.v[2][1], "210000") == 0 &&
              strcmp(wc.v[2][2], "70000") == 0 && strcmp(wc.v[2][3], "68000") == 0 && strcmp(wc.v[2][4], "72000") == 0,
              "scenario 2: Sales sum=210000 avg=70000 min=68000 max=72000");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 3: MIN/MAX on a STRING column -- type-aware, not just
     * numeric (lexicographic comparison, real per-column type used). ────── */
    CHECK(sql_execute(1, "SELECT dept, MIN(name), MAX(name) FROM employees GROUP BY dept ORDER BY dept", &r) == 0,
          "scenario 3: MIN/MAX on a STRING column succeeds");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        /* col1 here only captures MIN(name); MAX(name) is a 3rd column we
         * don't need for this assertion -- min alone is enough to prove
         * type-aware string comparison is really happening. */
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 3 &&
              strcmp(ctx.col1[0], "alice") == 0 &&    /* Eng: alice < bob */
              strcmp(ctx.col1[1], "frank") == 0 &&    /* Marketing: only frank */
              strcmp(ctx.col1[2], "carol") == 0,      /* Sales: carol < dave < erin */
              "scenario 3: MIN(name) is lexicographically correct per department");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 4: HAVING COUNT(*) > 1 excludes single-row groups. ──────── */
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 1 ORDER BY dept", &r) == 0,
          "scenario 4: HAVING COUNT(*) > 1 succeeds");
    CHECK(r.row_count == 2, "scenario 4: Marketing (count=1) excluded, Eng and Sales remain");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: HAVING on a different aggregate (AVG) than the one
     * used for filtering in scenario 4 -- proves HAVING's rendered label
     * resolution isn't hardcoded to COUNT. ───────────────────────────────── */
    CHECK(sql_execute(1, "SELECT dept, AVG(salary) FROM employees GROUP BY dept HAVING AVG(salary) > 65000 ORDER BY dept", &r) == 0,
          "scenario 5: HAVING AVG(salary) > 65000 succeeds");
    CHECK(r.row_count == 2, "scenario 5: Eng (87500) and Sales (70000) pass; Marketing (60000) filtered out");
    cursor_close(r.cursor_id);

    /* ── Scenario 6: bare aggregate, no GROUP BY at all -- exactly one
     * result row, including the zero-match case. ─────────────────────────── */
    CHECK(sql_execute(1, "SELECT COUNT(*) FROM employees", &r) == 0 && r.row_count == 1,
          "scenario 6: bare COUNT(*) with no GROUP BY returns exactly one row");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.col0[0], "6") == 0, "scenario 6: COUNT(*) == 6");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(1, "SELECT COUNT(*) FROM employees WHERE salary > 1000000", &r) == 0 && r.row_count == 1,
          "scenario 6b: zero-match bare aggregate STILL returns one row (SQL semantics, not an empty result set)");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.col0[0], "0") == 0, "scenario 6b: COUNT(*) == 0, not an empty cursor");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 7: ORDER BY the aggregate result itself (not the GROUP BY
     * column) + LIMIT on the grouped result. ─────────────────────────────── */
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees GROUP BY dept ORDER BY COUNT(*) DESC LIMIT 1", &r) == 0,
          "scenario 7: ORDER BY COUNT(*) DESC LIMIT 1 succeeds");
    CHECK(r.row_count == 1, "scenario 7: LIMIT 1 keeps exactly 1 group");
    {
        struct collect_ctx ctx = {{{0}}, {{0}}, 0};
        cursor_fetch_rows(r.cursor_id, 100, collect_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.col0[0], "Sales") == 0 && strcmp(ctx.col1[0], "3") == 0,
              "scenario 7: Sales (count=3) is the highest-count department");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 8: error paths ────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name, COUNT(*) FROM employees GROUP BY dept", &r) == 1 &&
          r.error == SQL_ERR_GROUP_BY_COLUMN_INVALID,
          "scenario 8: a non-grouped, non-aggregated column in the SELECT list fails cleanly");
    CHECK(sql_execute(1, "SELECT dept, SUM(name) FROM employees GROUP BY dept", &r) == 1 &&
          r.error == SQL_ERR_VALUE_INVALID,
          "scenario 8: SUM() on a STRING column fails cleanly (SQL_ERR_VALUE_INVALID)");
    CHECK(sql_execute(1, "SELECT bogus, COUNT(*) FROM employees GROUP BY bogus", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 8: unknown GROUP BY column fails cleanly");
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING SUM(salary) > 100", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 8: HAVING referencing an aggregate not in the SELECT list fails cleanly");
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees GROUP BY dept ORDER BY bogus", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 8: ORDER BY an unknown column against the grouped result fails cleanly");
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees JOIN nope ON employees.id = nope.id GROUP BY dept", &r) == 1 &&
          r.error == SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED,
          "scenario 8: GROUP BY combined with JOIN fails cleanly (named scope cut, not a crash)");

    /* ── Scenario 9: parser-level checks (sql_parse() directly, no exec). ── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 1", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_SELECT && stmt.u.select.has_aggregates && stmt.u.select.has_group_by && stmt.u.select.has_having,
              "scenario 9: full GROUP BY/HAVING statement parses with all three flags set");
        CHECK(sql_parse("SELECT dept FROM employees HAVING dept = 'Eng'", &stmt, err, sizeof(err)) == 1,
              "scenario 9: HAVING with no aggregate in the SELECT list is a PARSE error, not a runtime one");
        CHECK(sql_parse("SELECT SUM(*) FROM employees", &stmt, err, sizeof(err)) == 1,
              "scenario 9: SUM(*) is a parse error -- only COUNT may take '*'");
    }
    /* A column literally named "count" must still parse as an ordinary
     * column_ref (COUNT/SUM/AVG/MIN/MAX are NOT reserved keywords) -- the
     * one real regression risk this design named up front. */
    {
        const char* cols[1] = {"count"};
        SLSFieldType types[1] = {FIELD_TYPE_UINT64};
        make_table("counters", 0xF002, 1, cols, types);
    }
    sql_execute(1, "INSERT INTO counters (count) VALUES (42)", &r);
    CHECK(sql_execute(1, "SELECT count FROM counters", &r) == 0 && r.row_count == 1,
          "scenario 9b: a column literally named 'count' still parses as a plain column_ref, not a broken aggregate call");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
