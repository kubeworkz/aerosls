/*
 * sql_join2_phase2_host_test.c — SQL Feature-Parity Roadmap Phase 2
 * verification: a standalone host-buildable test for kernel/sql_exec.c's
 * generalized exec_select_join() (N-way chained JOIN + aliasing + LEFT
 * JOIN) and kernel/sql_parser.c's extended JOIN grammar, linked against
 * the REAL, unmodified full stack -- sql_exec.c, sql_parser.c, predicate.c,
 * row_index.c, rowstore.c, persist.c, cursor.c, mvcc.c, row_constraint.c,
 * row_journal.c -- not a reimplementation. Mirrors sql_join_host_test.c's
 * own scaffold exactly (same stub globals, same fake NVMe, same CHECK()
 * macro), since that file stays as the Phase 20 backward-compatibility
 * regression test and this one covers only what Phase 2 added.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_join2_phase2_host_test tests/sql_join2_phase2_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_join2_phase2_host_test
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

/* Same vecstore/vec_index link-satisfying stubs sql_join_host_test.c uses
 * (this suite is scoped to the RDBMS layer, not vector storage). */
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

/* Generic N-column row collector -- materialized rows always carry the FULL
 * combined row in chain order (see sql_exec.h), so every scenario below
 * hands this the exact combined-row column indices it cares about. */
#define COLLECT_MAX_COLS 8
#define COLLECT_MAX_ROWS 64
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
    mvcc_init();

    /* ── Fixture ────────────────────────────────────────────────────────────
     * departments(id, title): 1=Engineering, 2=Sales, 3=Marketing
     * employees(id, name, dept_id, mgr_id):
     *   1 alice  dept=1(Eng)   mgr=1 (self -- top of the tree, so every
     *                                 employee's mgr_id resolves to a real
     *                                 employee, useful for the self-join
     *                                 scenario)
     *   2 bob    dept=1(Eng)   mgr=1 (reports to alice)
     *   3 carol  dept=2(Sales) mgr=1 (reports to alice, has no projects)
     *   4 dave   dept=99       mgr=2 (reports to bob; dept_id=99 is a
     *                                 dangling FK -- no matching department,
     *                                 exercises both INNER exclusion and
     *                                 LEFT JOIN sentinel-padding)
     * projects(emp_id, title): alice has two, bob has one, carol/dave none. */
    {
        const char* dcols[2] = {"id", "title"};
        SLSFieldType dtypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("departments", 0xF101, 2, dcols, dtypes);
        const char* ecols[4] = {"id", "name", "dept_id", "mgr_id"};
        SLSFieldType etypes[4] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64};
        make_table("employees", 0xF102, 4, ecols, etypes);
        const char* pcols[2] = {"emp_id", "title"};
        SLSFieldType ptypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("projects", 0xF103, 2, pcols, ptypes);
    }

    struct SqlResult r;
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r);
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (2, 'Sales')", &r);
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (3, 'Marketing')", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id, mgr_id) VALUES (1, 'alice', 1, 1)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id, mgr_id) VALUES (2, 'bob', 1, 1)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id, mgr_id) VALUES (3, 'carol', 2, 1)", &r);
    sql_execute(1, "INSERT INTO employees (id, name, dept_id, mgr_id) VALUES (4, 'dave', 99, 2)", &r);
    sql_execute(1, "INSERT INTO projects (emp_id, title) VALUES (1, 'Website Redesign')", &r);
    sql_execute(1, "INSERT INTO projects (emp_id, title) VALUES (1, 'Mobile App')", &r);
    sql_execute(1, "INSERT INTO projects (emp_id, title) VALUES (2, 'Website Redesign')", &r);
    CHECK(r.kind == SQL_STMT_INSERT && r.error == SQL_ERR_NONE, "setup: 3 departments, 4 employees, 3 projects inserted");

    /* ── Scenario 1: 3-table INNER JOIN chain with aliases throughout. ────── */
    CHECK(sql_execute(1, "SELECT e.name, d.title, p.title FROM employees e "
                        "JOIN departments d ON e.dept_id = d.id "
                        "JOIN projects p ON e.id = p.emp_id "
                        "ORDER BY e.name", &r) == 0,
          "scenario 1: 3-table aliased JOIN chain succeeds");
    CHECK(r.row_count == 3, "scenario 1: exactly 3 rows (carol has no projects, dave has no department -- both correctly excluded)");
    {
        /* combined layout: 0=e.id,1=e.name,2=e.dept_id,3=e.mgr_id,4=d.id,5=d.title,6=p.emp_id,7=p.title.
         * Grammar only supports single-column ORDER BY, so ties on e.name
         * (alice's two project rows) fall back to the chain's natural,
         * stable per-step scan/insertion order: alice's projects were
         * inserted 'Website Redesign' then 'Mobile App', so that's the
         * order her two rows come out in. */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 3; ctx.idx[0] = 1; ctx.idx[1] = 5; ctx.idx[2] = 7;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 3 &&
              strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[0][2], "Website Redesign") == 0 &&
              strcmp(ctx.vals[1][0], "alice") == 0 && strcmp(ctx.vals[1][2], "Mobile App") == 0 &&
              strcmp(ctx.vals[2][0], "bob")   == 0 && strcmp(ctx.vals[2][2], "Website Redesign") == 0,
              "scenario 1: rows are alice/Website Redesign, alice/Mobile App, bob/Website Redesign (ORDER BY e.name, stable within ties)");
        CHECK(strcmp(ctx.vals[0][1], "Engineering") == 0 && strcmp(ctx.vals[2][1], "Engineering") == 0,
              "scenario 1: department title resolved correctly through the chain for every row");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: LEFT JOIN pads an unmatched row instead of dropping it. */
    CHECK(sql_execute(1, "SELECT e.name, d.id, d.title FROM employees e "
                        "LEFT JOIN departments d ON e.dept_id = d.id "
                        "ORDER BY e.name", &r) == 0,
          "scenario 2: LEFT JOIN succeeds");
    CHECK(r.row_count == 4, "scenario 2: all 4 employees present, including dave (dangling dept_id=99)");
    {
        /* combined layout: 0=e.id,1=e.name,2=e.dept_id,3=e.mgr_id,4=d.id,5=d.title */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 3; ctx.idx[0] = 1; ctx.idx[1] = 4; ctx.idx[2] = 5;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        /* order: alice, bob, carol, dave */
        CHECK(ctx.count == 4 && strcmp(ctx.vals[3][0], "dave") == 0, "scenario 2: dave is the 4th row (ORDER BY e.name)");
        CHECK(strcmp(ctx.vals[3][1], "0") == 0 && strcmp(ctx.vals[3][2], "") == 0,
              "scenario 2: dave's unmatched department columns are sentinel-filled (\"0\"/\"\"), not dropped or garbage");
        CHECK(strcmp(ctx.vals[0][1], "1") == 0 && strcmp(ctx.vals[0][2], "Engineering") == 0,
              "scenario 2: alice's real department still resolves correctly alongside dave's sentinel row");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 3: self-join with aliases (manager lookup) -- the
     * motivating case for aliasing: the same physical table appears twice,
     * disambiguated purely by e1/e2. ─────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT e1.name, e2.name FROM employees e1 "
                        "JOIN employees e2 ON e1.mgr_id = e2.id "
                        "ORDER BY e1.name", &r) == 0,
          "scenario 3: self-join with aliases succeeds");
    CHECK(r.row_count == 4, "scenario 3: all 4 employees have a resolvable manager");
    {
        /* combined layout: 0=e1.id,1=e1.name,2=e1.dept_id,3=e1.mgr_id,4=e2.id,5=e2.name,6=e2.dept_id,7=e2.mgr_id */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 1; ctx.idx[1] = 5;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 4 &&
              strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[0][1], "alice") == 0 &&
              strcmp(ctx.vals[1][0], "bob")   == 0 && strcmp(ctx.vals[1][1], "alice") == 0 &&
              strcmp(ctx.vals[2][0], "carol") == 0 && strcmp(ctx.vals[2][1], "alice") == 0 &&
              strcmp(ctx.vals[3][0], "dave")  == 0 && strcmp(ctx.vals[3][1], "bob")   == 0,
              "scenario 3: correct manager resolved per employee (alice/bob/carol -> alice, dave -> bob)");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 4: WHERE applied once, after the full 3-table chain. ────── */
    CHECK(sql_execute(1, "SELECT e.name, d.title, p.title FROM employees e "
                        "JOIN departments d ON e.dept_id = d.id "
                        "JOIN projects p ON e.id = p.emp_id "
                        "WHERE e.name != 'alice'", &r) == 0,
          "scenario 4: WHERE on a 3-table chain succeeds");
    CHECK(r.row_count == 1, "scenario 4: WHERE e.name != 'alice' leaves exactly bob's one row");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: ORDER BY DESC + LIMIT on a joined chain result. ───────── */
    CHECK(sql_execute(1, "SELECT e.name, p.title FROM employees e "
                        "JOIN projects p ON e.id = p.emp_id "
                        "ORDER BY p.title DESC LIMIT 2", &r) == 0,
          "scenario 5: ORDER BY DESC + LIMIT on a JOIN chain succeeds");
    CHECK(r.row_count == 2, "scenario 5: LIMIT 2 keeps exactly 2 of the 3 matches");
    {
        /* combined layout: 0=e.id,1=e.name,2=e.dept_id,3=e.mgr_id,4=p.emp_id,5=p.title */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 1; ctx.idx[1] = 5;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 2 &&
              strcmp(ctx.vals[0][1], "Website Redesign") == 0 && strcmp(ctx.vals[1][1], "Website Redesign") == 0 &&
              strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[1][0], "bob") == 0,
              "scenario 5: both kept rows are 'Website Redesign' (DESC beats 'Mobile App'), stable order alice then bob");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 6: Phase 20's original unaliased 2-table syntax still
     * works unchanged -- backward-compatibility regression check (the
     * dedicated Phase 20 test, sql_join_host_test.c, covers this in much
     * more depth; this is a light confirmation it survived the rewrite). ── */
    CHECK(sql_execute(1, "SELECT employees.name, departments.title FROM employees "
                        "JOIN departments ON employees.dept_id = departments.id "
                        "ORDER BY employees.name", &r) == 0,
          "scenario 6: unaliased two-table JOIN (Phase 20 syntax) still parses and executes");
    CHECK(r.row_count == 3, "scenario 6: alice, bob, carol match (dave's dangling dept_id excluded)");
    cursor_close(r.cursor_id);

    /* ── Scenario 7: error paths ────────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM employees e JOIN departments d ON e.dept_id = d.id "
                        "JOIN projects p ON no_such_alias.foo = p.emp_id", &r) == 1 &&
          r.error == SQL_ERR_JOIN_INVALID,
          "scenario 7: ON clause referencing neither the new table nor the chain so far fails cleanly (SQL_ERR_JOIN_INVALID)");
    CHECK(sql_execute(1, "SELECT * FROM employees e JOIN no_such_table t ON e.dept_id = t.id", &r) == 1 &&
          r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 7: JOIN against an unknown table still fails cleanly (SQL_ERR_TABLE_NOT_FOUND)");
    {
        const char* wide_cols[9] = {"c0","c1","c2","c3","c4","c5","c6","c7","c8"};
        SLSFieldType wide_types[9] = {
            FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64,
            FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64, FIELD_TYPE_UINT64
        };
        make_table("wide_a", 0xF104, 9, wide_cols, wide_types);
        make_table("wide_b", 0xF105, 9, wide_cols, wide_types);
    }
    CHECK(sql_execute(1, "SELECT * FROM wide_a JOIN wide_b ON wide_a.c0 = wide_b.c0", &r) == 1 &&
          r.error == SQL_ERR_JOIN_TOO_WIDE,
          "scenario 7: combined column count exceeding ROWSTORE_MAX_COLUMNS still rejected up front (SQL_ERR_JOIN_TOO_WIDE)");
    {
        const char* onecol[1] = {"id"};
        SLSFieldType onetype[1] = {FIELD_TYPE_UINT64};
        make_table("t1", 0xF110, 1, onecol, onetype);
        make_table("t2", 0xF111, 1, onecol, onetype);
        make_table("t3", 0xF112, 1, onecol, onetype);
        make_table("t4", 0xF113, 1, onecol, onetype);
        make_table("t5", 0xF114, 1, onecol, onetype);
    }
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT * FROM t1 JOIN t2 ON t1.id = t2.id JOIN t3 ON t2.id = t3.id "
                        "JOIN t4 ON t3.id = t4.id JOIN t5 ON t4.id = t5.id", &stmt, err, sizeof(err)) == 1,
              "scenario 7: a 5th table (4 JOINs, exceeding SQL_MAX_JOINS=3) is a PARSE error, not silently truncated");
    }

    /* ── Scenario 8: GROUP BY/aggregates combined with a multi-table JOIN
     * remains a named scope cut (Phase 1's rule, unchanged for the N-way
     * case). ──────────────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT e.dept_id, COUNT(*) FROM employees e "
                        "JOIN departments d ON e.dept_id = d.id GROUP BY e.dept_id", &r) == 1 &&
          r.error == SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED,
          "scenario 8: GROUP BY/aggregates combined with a JOIN still fails cleanly (SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED)");

    /* ── Scenario 9: parser-level checks (sql_parse() directly, no exec). ── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT * FROM employees LEFT OUTER JOIN departments ON employees.dept_id = departments.id",
                        &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.join_count == 1 && stmt.u.select.joins[0].type == SQL_JOIN_LEFT,
              "scenario 9: LEFT OUTER JOIN parses (OUTER is a no-op) and sets SQL_JOIN_LEFT");
        CHECK(sql_parse("SELECT * FROM employees e", &stmt, err, sizeof(err)) == 0 &&
              strcmp(stmt.u.select.table_alias, "e") == 0,
              "scenario 9: implicit alias (no AS) on the FROM table parses correctly");
        CHECK(sql_parse("SELECT * FROM employees AS e", &stmt, err, sizeof(err)) == 0 &&
              strcmp(stmt.u.select.table_alias, "e") == 0,
              "scenario 9: explicit 'AS e' alias parses identically to the implicit form");
        CHECK(sql_parse("SELECT e.name, d.title, p.title FROM employees e "
                        "JOIN departments d ON e.dept_id = d.id "
                        "JOIN projects p ON e.id = p.emp_id", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.join_count == 2 &&
              strcmp(stmt.u.select.joins[0].alias, "d") == 0 &&
              strcmp(stmt.u.select.joins[1].alias, "p") == 0,
              "scenario 9: a 3-table chain parses with join_count==2 and both aliases captured in order");
    }

    /* ── Scenario 10: permission denial still propagates cleanly through a
     * chained join (Phase 20's own regression, reconfirmed post-rewrite). ── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT * FROM employees e JOIN departments d ON e.dept_id = d.id "
                        "JOIN projects p ON e.id = p.emp_id", &r) == 0 && r.row_count == 0,
          "scenario 10: permission denial makes a chained JOIN return 0 rows cleanly, not a crash");
    g_access_force_deny = 0;

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
