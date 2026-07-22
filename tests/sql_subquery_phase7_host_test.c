/*
 * sql_subquery_phase7_host_test.c — SQL Feature-Parity Roadmap Phase 7
 * verification: a standalone host-buildable test for kernel/sql_parser.c's
 * new embedded-subquery capture/validation grammar and kernel/sql_exec.c's
 * new resolve_predicate_subqueries()/exec_subquery_column() -- linked
 * against the REAL, unmodified full stack: sql_exec.c, sql_parser.c,
 * predicate.c, row_index.c, rowstore.c, persist.c, cursor.c, mvcc.c,
 * row_constraint.c, row_journal.c. Mirrors sql_insert_phase6_host_test.c's
 * own scaffold (same stub globals, same fake NVMe, same CHECK() macro,
 * same make_table()/collectN_ctx helpers).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_subquery_phase7_host_test tests/sql_subquery_phase7_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_subquery_phase7_host_test
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
#include "kernel/row_constraint.h"
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

/* Same vecstore/vec_index link-satisfying stubs the other sql_*_host_test.c
 * files use (this suite is scoped to the RDBMS layer, not vector storage). */
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

#define COLLECT_MAX_COLS 5
#define COLLECT_MAX_ROWS 64
struct collectN_ctx {
    uint32_t idx[COLLECT_MAX_COLS];
    int      ncols;
    char     vals[COLLECT_MAX_ROWS][COLLECT_MAX_COLS][64];
    uint16_t null_mask[COLLECT_MAX_ROWS];
    uint32_t count;
};
static void collectN_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collectN_ctx* ctx = (struct collectN_ctx*)ctxp;
    if (ctx->count >= COLLECT_MAX_ROWS) { ctx->count++; return; }
    ctx->null_mask[ctx->count] = v->null_mask;
    for (int c = 0; c < ctx->ncols; c++) {
        if (v->count > ctx->idx[c]) { strncpy(ctx->vals[ctx->count][c], v->values[ctx->idx[c]], 63); ctx->vals[ctx->count][c][63] = '\0'; }
        else ctx->vals[ctx->count][c][0] = '\0';
    }
    ctx->count++;
}
// cursor rows always hold the FULL underlying table row, in the table's own
// column order -- NOT the SELECT list's projected order (see exec_select()'s
// own g_select_scratch handling in sql_exec.c) -- so callers must pass the
// target column's actual index within employees' full layout (id=0, name=1,
// dept_id=2), not its position in whatever SELECT list was written.
static int collect_names(uint32_t cursor_id, char names[][64], int max, uint32_t col_idx) {
    struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.ncols = 1; ctx.idx[0] = col_idx;
    cursor_fetch_rows(cursor_id, 100, collectN_cb, &ctx);
    cursor_close(cursor_id);
    int n = (int)ctx.count < max ? (int)ctx.count : max;
    for (int i = 0; i < n; i++) strcpy(names[i], ctx.vals[i][0]);
    return n;
}
static int names_contains(char names[][64], int n, const char* target) {
    for (int i = 0; i < n; i++) if (strcmp(names[i], target) == 0) return 1;
    return 0;
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

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- see
// sql_insert_phase6_host_test.c's own identical comment for why these are
// safe no-ops here (this test never exercises CREATE/DROP TABLE via SQL
// text at runtime). ─────────────────────────────────────────────────────
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 0; }
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) { (void)req; return 1; }
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    row_constraint_init();

    /* ── Fixture ────────────────────────────────────────────────────────────
     * departments(id, name, active) -- Eng(1, active), Sales(2, active),
     * Legacy(3, inactive).
     * employees(id, name, dept_id) -- alice/Eng, bob/Eng, carol/Sales,
     * dave/Legacy. ─────────────────────────────────────────────────────── */
    {
        const char* cols[3] = {"id", "name", "active"};
        SLSFieldType types[3] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_BOOL};
        make_table("departments", 0xF701, 3, cols, types);
    }
    {
        const char* cols[3] = {"id", "name", "dept_id"};
        SLSFieldType types[3] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_UINT64};
        make_table("employees", 0xF702, 3, cols, types);
    }

    struct SqlResult r;
    CHECK(sql_execute(1, "INSERT INTO departments (id, name, active) VALUES "
                        "(1, 'Eng', TRUE), (2, 'Sales', TRUE), (3, 'Legacy', FALSE)", &r) == 0,
          "setup: departments seeded");
    CHECK(sql_execute(1, "INSERT INTO employees (id, name, dept_id) VALUES "
                        "(1, 'alice', 1), (2, 'bob', 1), (3, 'carol', 2), (4, 'dave', 3)", &r) == 0,
          "setup: employees seeded");

    /* ── Scenario 1: scalar subquery in a plain SELECT's WHERE. ────────────── */
    CHECK(sql_execute(1, "SELECT id, name FROM employees WHERE dept_id = "
                        "(SELECT id FROM departments WHERE name = 'Eng')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 1: scalar subquery SELECT succeeds");
    {
        char names[8][64];
        int n = collect_names(r.cursor_id, names, 8, 1);
        CHECK(n == 2, "scenario 1: exactly 2 employees in Eng (alice, bob)");
        CHECK(names_contains(names, n, "alice") && names_contains(names, n, "bob"),
              "scenario 1: the right two employees (alice, bob) matched");
    }

    /* ── Scenario 2: IN (SELECT ...) in a plain SELECT's WHERE. ─────────────── */
    CHECK(sql_execute(1, "SELECT id, name FROM employees WHERE dept_id IN "
                        "(SELECT id FROM departments WHERE active = TRUE)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 2: IN (SELECT...) SELECT succeeds");
    {
        char names[8][64];
        int n = collect_names(r.cursor_id, names, 8, 1);
        CHECK(n == 3, "scenario 2: exactly 3 employees in an active department (alice, bob, carol)");
        CHECK(names_contains(names, n, "alice") && names_contains(names, n, "bob") && names_contains(names, n, "carol"),
              "scenario 2: the right three employees matched");
        CHECK(!names_contains(names, n, "dave"), "scenario 2: dave (Legacy, inactive) correctly excluded");
    }

    /* ── Scenario 3: scalar subquery in UPDATE's WHERE actually mutates the
     * right rows and no others. ─────────────────────────────────────────────── */
    CHECK(sql_execute(1, "UPDATE employees SET name = 'ALICE-PROMOTED' WHERE dept_id = "
                        "(SELECT id FROM departments WHERE name = 'Eng') AND name = 'alice'", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: UPDATE with scalar subquery WHERE succeeds");
    CHECK(r.affected_rows == 1, "scenario 3: exactly 1 row updated");
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE id = 1", &r) == 0, "scenario 3: re-select id=1");
    {
        char names[4][64];
        int n = collect_names(r.cursor_id, names, 4, 1);
        CHECK(n == 1 && strcmp(names[0], "ALICE-PROMOTED") == 0, "scenario 3: alice's row actually changed");
    }
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE id = 2", &r) == 0, "scenario 3: re-select id=2 (bob, untouched)");
    {
        char names[4][64];
        int n = collect_names(r.cursor_id, names, 4, 1);
        CHECK(n == 1 && strcmp(names[0], "bob") == 0, "scenario 3: bob's row is untouched");
    }

    /* ── Scenario 4: IN (SELECT ...) in DELETE's WHERE actually removes the
     * right rows and no others. ─────────────────────────────────────────────── */
    CHECK(sql_execute(1, "DELETE FROM employees WHERE dept_id IN "
                        "(SELECT id FROM departments WHERE active = FALSE)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 4: DELETE with IN (SELECT...) WHERE succeeds");
    CHECK(r.affected_rows == 1, "scenario 4: exactly 1 row deleted (dave, Legacy)");
    CHECK(sql_execute(1, "SELECT name FROM employees", &r) == 0, "scenario 4: re-select all employees");
    {
        char names[8][64];
        int n = collect_names(r.cursor_id, names, 8, 1);
        CHECK(n == 3, "scenario 4: exactly 3 employees remain");
        CHECK(!names_contains(names, n, "dave"), "scenario 4: dave is really gone, not just filtered");
    }

    /* ── Scenario 5: IN (SELECT ...) whose subquery matches zero rows is
     * always false (standard SQL IN-with-empty-set semantics), not an
     * error and not "matches everything". ──────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept_id IN "
                        "(SELECT id FROM departments WHERE name = 'Nonexistent')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 5: zero-row IN (SELECT...) succeeds (not an error)");
    CHECK(r.row_count == 0, "scenario 5: zero-row IN (SELECT...) matches nothing");

    /* ── Scenario 6: a scalar subquery whose result is zero rows is treated
     * as an always-false comparison (UNKNOWN-as-false), not an error. ──────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept_id = "
                        "(SELECT id FROM departments WHERE name = 'Nonexistent')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 6: zero-row scalar subquery succeeds (not an error)");
    CHECK(r.row_count == 0, "scenario 6: zero-row scalar subquery matches nothing");

    /* ── Scenario 7: a scalar subquery whose result is MORE than one row is
     * a genuine error -- the statement is rejected, not silently truncated
     * to the first row. ──────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept_id = "
                        "(SELECT id FROM departments)", &r) != 0 && r.error == SQL_ERR_VALUE_INVALID,
          "scenario 7: >1-row scalar subquery is rejected with SQL_ERR_VALUE_INVALID");

    /* ── Scenario 8: unsupported subquery shapes are rejected at PARSE
     * time, before any execution is even attempted. ────────────────────────── */
    {
        char err[SQL_ERR_MSG_LEN];
        struct SqlStatement st;
        CHECK(sql_parse("SELECT * FROM employees WHERE dept_id = (SELECT id, name FROM departments)", &st, err, sizeof(err)) != 0,
              "scenario 8: multi-column subquery rejected at parse time");
        CHECK(sql_parse("SELECT * FROM employees WHERE dept_id = (SELECT COUNT(*) FROM departments)", &st, err, sizeof(err)) != 0,
              "scenario 8: aggregate subquery rejected at parse time");
        CHECK(sql_parse("SELECT * FROM employees WHERE dept_id = "
                        "(SELECT departments.id FROM departments JOIN employees e2 ON departments.id = e2.dept_id)", &st, err, sizeof(err)) != 0,
              "scenario 8: JOIN subquery rejected at parse time");
        CHECK(sql_parse("SELECT * FROM employees WHERE dept_id = "
                        "(SELECT id FROM departments WHERE id = (SELECT id FROM departments))", &st, err, sizeof(err)) != 0,
              "scenario 8: nested subquery rejected at parse time");
    }

    /* ── Scenario 9: a subquery written inside a JOIN's WHERE parses fine
     * (the same parse_comparison_tail() path handles it) but is never
     * resolved (exec_select_join() deliberately doesn't call
     * resolve_predicate_subqueries()) -- it fails CLOSED (matches nothing),
     * not open, and not a crash. ─────────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT e.name FROM employees e JOIN departments d ON e.dept_id = d.id "
                        "WHERE e.dept_id = (SELECT id FROM departments WHERE name = 'Eng')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 9: subquery inside a JOIN's WHERE parses and runs without crashing");
    CHECK(r.row_count == 0, "scenario 9: unresolved subquery marker inside a JOIN's WHERE fails closed (0 rows, not all rows)");

    /* ── Scenario 10: a subquery combined with an ordinary AND'd literal
     * condition in the same WHERE -- proves the "copy the OR-chain root's
     * content back into the marker node's own slot index" resolution
     * doesn't corrupt the parent AND node's left/right references. ────────── */
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept_id IN "
                        "(SELECT id FROM departments WHERE active = TRUE) AND name = 'carol'", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 10: IN (SELECT...) AND'd with a literal condition succeeds");
    {
        char names[4][64];
        int n = collect_names(r.cursor_id, names, 4, 1);
        CHECK(n == 1 && strcmp(names[0], "carol") == 0, "scenario 10: exactly carol matched (AND parent references stayed valid)");
    }

    /* ── Scenario 11: permission denial on the subquery's own table makes
     * it resolve as 0 rows (not a crash, not a false grant) -- same
     * fail-safe posture sql_join_host_test's own scenario 7 already proves
     * for JOINs. ──────────────────────────────────────────────────────────────── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT name FROM employees WHERE dept_id = "
                        "(SELECT id FROM departments WHERE name = 'Eng')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 11: permission-denied subquery table resolves cleanly, not a crash");
    CHECK(r.row_count == 0, "scenario 11: permission denial makes the subquery act as 0 rows (comparison false for every row)");
    g_access_force_deny = 0;

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
