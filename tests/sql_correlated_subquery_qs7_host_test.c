/*
 * sql_correlated_subquery_qs7_host_test.c — SQL Query-Surface Roadmap
 * Phase 7 verification: a standalone host-buildable test for kernel/
 * predicate.c's new subquery_is_correlated[]/predicate_add_subquery()
 * detection, and kernel/sql_exec.c's new substitute_outer_refs()/
 * exec_select_correlated()/the resolve_predicate_subqueries() per-row
 * variant -- linked against the REAL, unmodified full stack: sql_exec.c,
 * sql_parser.c, predicate.c, row_index.c, rowstore.c, persist.c, view.c,
 * cursor.c, mvcc.c, row_constraint.c, row_journal.c, database.c. Mirrors
 * sql_subquery_phase7_host_test.c's own scaffold (same stub globals, same
 * fake NVMe, same CHECK() macro, same make_table()/collectN_ctx helpers).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_correlated_subquery_qs7_host_test tests/sql_correlated_subquery_qs7_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/storage_quota.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_correlated_subquery_qs7_host_test
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
#include "kernel/view.h"
#include "user/permissions.h"
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
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
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];
struct SLSIndex        index_store[INDEX_MAX];
uint32_t               index_count = 0;
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
int group_contains_uid(const char* name, uint32_t uid) { (void)name; (void)uid; return 0; }

struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
uint32_t                   vecstore_partition_cursor[PARTITION_MAX] = {0};
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

#define FAKE_NVME_MAX_FRAMES 2048
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

uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 0; }
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) { (void)req; return 1; }
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }

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

#define COLLECT_MAX_COLS 4
#define COLLECT_MAX_ROWS 512
struct collectN_ctx {
    uint32_t idx[COLLECT_MAX_COLS];
    int      ncols;
    char     vals[COLLECT_MAX_ROWS][COLLECT_MAX_COLS][64];
    uint32_t count;
};
static void collectN_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collectN_ctx* ctx = (struct collectN_ctx*)ctxp;
    if (ctx->count >= COLLECT_MAX_ROWS) { ctx->count++; return; }
    for (int c = 0; c < ctx->ncols; c++) {
        if (v->count > ctx->idx[c]) { strncpy(ctx->vals[ctx->count][c], v->values[ctx->idx[c]], 63); ctx->vals[ctx->count][c][63] = '\0'; }
        else ctx->vals[ctx->count][c][0] = '\0';
    }
    ctx->count++;
}
static int has_val(struct collectN_ctx* ctx, int col, const char* v) {
    for (uint32_t i = 0; i < ctx->count; i++)
        if (strcmp(ctx->vals[i][col], v) == 0) return 1;
    return 0;
}

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();

    /* ── Fixture -- same shape as sql_view_phase5_host_test.c's own:
     * employees(id, name, dept, salary), four rows spanning eng/sales/hr.
     * eng has two rows (alice 90000, carol 95000) -- deliberately, so a
     * correlated "anyone else in my dept earns more" subquery has a real,
     * non-trivial answer for at least one row (alice: yes, carol has more;
     * carol: no, she's the top earner) while sales/hr (one person each)
     * never match at all. */
    {
        const char* cols[4]  = {"id", "name", "dept", "salary"};
        SLSFieldType types[4] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_STRING, FIELD_TYPE_UINT64};
        make_table("employees", 0xF001, 4, cols, types);
    }
    struct SqlResult r;
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (1, 'alice', 'eng', 90000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (2, 'bob', 'sales', 60000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (3, 'carol', 'eng', 95000)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (4, 'dave', 'hr', 55000)", &r);
    CHECK(r.error == SQL_ERR_NONE, "setup: 4 employees rows inserted");

    /* ── Scenario 1: correlated IN (SELECT ...) -- "employees whose own
     * department is among the departments of anyone who out-earns them."
     * The subquery ("depts of people earning more than OUTER.salary") is
     * re-run per outer row with a fresh splice; only alice's dept (eng)
     * ends up in that set for her own row (carol, at 95000, out-earns her
     * and is also eng) -- carol has no one above her 95000, and bob/dave's
     * higher-earner sets never include their own dept (sales/hr). ──────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees WHERE dept IN "
                         "(SELECT dept FROM employees WHERE salary > OUTER.salary)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 1: correlated IN (SELECT ...) executes without error");
    CHECK(r.row_count == 1, "scenario 1: exactly one employee (alice) has a higher-paid colleague in their own dept");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;   // name
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(has_val(&ctx, 0, "alice") && !has_val(&ctx, 0, "carol") && !has_val(&ctx, 0, "bob") && !has_val(&ctx, 0, "dave"),
              "scenario 1: the matched row is alice, not carol/bob/dave");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: correlated SCALAR subquery, a deliberate tautology
     * ("this row's own id, found by matching its own dept+salary back to
     * itself") -- proves scalar (not just IN) correlated resolution
     * works, and that splicing TWO separate OUTER.<col> references into
     * the same subquery text in one pass both resolve correctly. Every
     * row's dept+salary combination is unique in this fixture, so the
     * inner subquery always finds exactly the outer row itself -- a
     * always-true comparison, expected to keep all 4 rows. ──────────────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE id = "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept AND salary = OUTER.salary)", &r) == 0 &&
          r.error == SQL_ERR_NONE && r.row_count == 4,
          "scenario 2: correlated scalar subquery (tautological self-match) keeps all 4 rows");
    cursor_close(r.cursor_id);

    /* ── Scenario 3: outer WHERE/ORDER BY/LIMIT still compose normally
     * over a correlated result (exec_select_correlated() applies them
     * exactly like exec_select()'s own plain path). ───────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept IN "
                         "(SELECT dept FROM employees WHERE salary > OUTER.salary) "
                         "ORDER BY name LIMIT 1", &r) == 0 && r.row_count == 1,
          "scenario 3: ORDER BY/LIMIT compose over a correlated result");
    cursor_close(r.cursor_id);

    /* ── Scenario 4: v1 scope cuts -- correlated subquery combined with
     * JOIN/aggregates/a set operator/a CTE, and in UPDATE/DELETE, are all
     * rejected LOUD with SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED, never
     * silently reused as the older "unresolved marker fails closed"
     * fallback. ──────────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees e1 JOIN employees e2 ON e1.id = e2.id WHERE e1.id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept)", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4a: correlated subquery + JOIN is rejected loud");
    CHECK(sql_execute(1, "SELECT dept, COUNT(*) FROM employees WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept) GROUP BY dept", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4b: correlated subquery + aggregates is rejected the same way");
    CHECK(sql_execute(1, "SELECT id FROM employees WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept) "
                         "UNION SELECT id FROM employees WHERE dept = 'hr'", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4c: correlated subquery + a set operator is rejected the same way");
    CHECK(sql_execute(1, "WITH x AS (SELECT id, dept FROM employees) SELECT * FROM x WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept)", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4d: correlated subquery + a CTE is rejected the same way");
    CHECK(sql_execute(1, "UPDATE employees SET salary = 100000 WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept AND salary > OUTER.salary)", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4e: correlated subquery in UPDATE ... WHERE is rejected (v1 scope cut -- SELECT only)");
    CHECK(sql_execute(1, "DELETE FROM employees WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.dept AND salary > OUTER.salary)", &r) == 1 &&
          r.error == SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED,
          "scenario 4f: correlated subquery in DELETE ... WHERE is rejected the same way");
    CHECK(sql_execute(1, "SELECT * FROM employees", &r) == 0 && r.row_count == 4,
          "scenario 4: all 4 rejections above were genuinely no-ops -- the table is untouched");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: an unknown OUTER.<column> reference fails loud with
     * SQL_ERR_COLUMN_NOT_FOUND at EXEC time, not a crash or a silent
     * empty result -- detection is a textual scan (parse time), real
     * column validation only happens once a real outer row exists. ──────── */
    CHECK(sql_execute(1, "SELECT id FROM employees WHERE id IN "
                         "(SELECT id FROM employees WHERE dept = OUTER.nonexistent_col)", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 5: an unknown OUTER.<column> reference fails loud, not silently");

    /* ── Scenario 6: outer-row budget + loud truncation. A dedicated
     * 70-row table, one deliberately tautological correlated IN clause
     * that matches every row it actually tests -- SQL_CORRELATED_MAX_
     * OUTER_ROWS=64 caps how many of the 70 rows get tested at all, and
     * truncated=1 says so rather than silently reporting 64 as if that
     * were the whole story. ─────────────────────────────────────────────── */
    {
        const char* cols[2] = {"id", "grp"};
        SLSFieldType types[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("manyrows", 0xF003, 2, cols, types);
    }
    for (int i = 0; i < 70; i++) {
        char stmt[128];
        snprintf(stmt, sizeof(stmt), "INSERT INTO manyrows (id, grp) VALUES (%d, 'x')", i);
        sql_execute(1, stmt, &r);
    }
    CHECK(sql_execute(1, "SELECT * FROM manyrows", &r) == 0 && r.row_count == 70,
          "scenario 6 setup: 70 rows inserted into manyrows");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT id FROM manyrows WHERE id IN "
                         "(SELECT id FROM manyrows WHERE grp = OUTER.grp AND id = OUTER.id)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 6: correlated query over the 70-row table executes without error");
    CHECK(r.row_count == 64 && r.truncated == 1,
          "scenario 6: exactly 64 rows tested (the budget) and truncated=1 -- not silently reporting 64 as the whole answer");
    cursor_close(r.cursor_id);

    /* ── Scenario 7: parser/predicate-level checks -- subquery_is_
     * correlated[] detection, and confirming an ORDINARY (non-
     * correlated) subquery is completely unaffected (zero behavior
     * change for every pre-Phase-7 query). ──────────────────────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT id FROM employees WHERE id IN (SELECT id FROM employees WHERE dept = OUTER.dept)", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_SELECT && predicate_has_correlated_subquery(&stmt.u.select.where) == 1,
              "scenario 7a: a subquery containing OUTER.<col> is detected as correlated at parse time");
        CHECK(sql_parse("SELECT id FROM employees WHERE id IN (SELECT id FROM employees WHERE dept = 'eng')", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_SELECT && predicate_has_correlated_subquery(&stmt.u.select.where) == 0,
              "scenario 7b: an ordinary, non-correlated subquery is NOT flagged as correlated");
    }
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE id IN (SELECT id FROM employees WHERE dept = 'eng')", &r) == 0 &&
          r.error == SQL_ERR_NONE && r.row_count == 2,
          "scenario 7c: an ordinary non-correlated subquery still resolves via the original once-only path (regression, unchanged behavior)");
    cursor_close(r.cursor_id);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
