/*
 * sql_view_phase5_host_test.c — SQL Query-Surface Roadmap Phase 5
 * verification: a standalone host-buildable test for kernel/view.c's
 * CREATE VIEW / DROP VIEW lifecycle, kernel/sql_parser.c's VIEW grammar,
 * and kernel/sql_exec.c's exec_select_view()/exec_create_view()/
 * exec_drop_view()/the JOIN and DML v1 rejections, linked against the
 * REAL, unmodified full stack -- sql_exec.c, sql_parser.c, predicate.c,
 * row_index.c, rowstore.c, persist.c, view.c, cursor.c, mvcc.c,
 * row_constraint.c, row_journal.c, database.c -- not a reimplementation.
 * Mirrors sql_setop_phase4_host_test.c's own fixture/stub scaffolding
 * (same stack, same reasons), plus rowstore_host_test.c's own "wipe
 * globals, call the real persist_restore_all(), confirm the round trip"
 * pattern for the persistence scenario.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_view_phase5_host_test tests/sql_view_phase5_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/storage_quota.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_view_phase5_host_test
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
#include "kernel/mvcc.h"
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
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
struct SLSIndex        index_store[INDEX_MAX];
uint32_t               index_count = 0;
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// Controllable role stub -- ROLE_SYSTEM_KERNEL by default (every scenario
// except the DROP VIEW permission-denied one wants every uid to act as an
// unrestricted caller, matching sql_setop_phase4_host_test.c's own fixed
// stub); scenario 6 below flips this to ROLE_GUEST for one narrow window
// to exercise view_drop()'s real owner-or-kernel gate.
static SLSRole g_test_role = ROLE_SYSTEM_KERNEL;
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return g_test_role; }
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

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- sql_exec.c's
// exec_create_table() unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's rowstore_drop_table() unconditionally
// calls sys_sls_vfree() (its return value is never checked -- confirmed by
// reading rowstore_drop_table() directly, so a stubbed failure return here
// does not block DROP TABLE's own real table_headers[]/row_indexes[]/etc.
// cleanup, which is what scenario 3 below depends on). ──────────────────────
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

    /* ── Fixture ────────────────────────────────────────────────────────────
     * employees(id, name, dept, salary): four rows spanning eng/sales/hr,
     * with one deliberately short (4-digit) salary to demonstrate the
     * documented STRING-comparison simplification in scenario 2c below. */
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

    /* ── Scenario 1: CREATE VIEW / query / DROP VIEW round trip. ──────────── */
    CHECK(sql_execute(1, "CREATE VIEW eng_employees AS SELECT id, name, salary FROM employees WHERE dept = 'eng'", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 1: CREATE VIEW succeeds");
    CHECK(view_find_index("eng_employees") >= 0, "scenario 1: view registered in views[]");

    CHECK(sql_execute(1, "SELECT * FROM eng_employees", &r) == 0, "scenario 1: SELECT * FROM the view succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 2 && r.column_count == 3,
          "scenario 1: 2 rows (alice, carol), 3 columns (id, name, salary) -- the view's own projection, not the table's");
    CHECK(strcmp(r.columns[0], "id") == 0 && strcmp(r.columns[1], "name") == 0 && strcmp(r.columns[2], "salary") == 0,
          "scenario 1: outer SELECT * reports the VIEW's own column list");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 3; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(has_val(&ctx, 1, "alice") && has_val(&ctx, 1, "carol") && !has_val(&ctx, 1, "bob") && !has_val(&ctx, 1, "dave"),
              "scenario 1: only the eng-department rows (alice, carol) came through the view's own WHERE");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "SELECT name FROM eng_employees", &r) == 0 && r.row_count == 2 && r.column_count == 1,
          "scenario 1: a narrower outer projection (SELECT name) over the view also works");
    cursor_close(r.cursor_id);

    CHECK(sql_execute(1, "DROP VIEW eng_employees", &r) == 0 && r.error == SQL_ERR_NONE, "scenario 1: DROP VIEW succeeds");
    CHECK(view_find_index("eng_employees") < 0, "scenario 1: view no longer in views[] after DROP");
    CHECK(sql_execute(1, "SELECT * FROM eng_employees", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 1: querying the dropped view now fails exactly like an unknown table");

    /* ── Scenario 2: outer WHERE/ORDER BY/LIMIT composed over a view. ──────── */
    CHECK(sql_execute(1, "CREATE VIEW all_employees AS SELECT id, name, dept, salary FROM employees", &r) == 0,
          "scenario 2: CREATE VIEW all_employees (no inner WHERE) succeeds");

    CHECK(sql_execute(1, "SELECT * FROM all_employees WHERE dept = 'sales'", &r) == 0 && r.row_count == 1,
          "scenario 2a: outer WHERE over the view narrows to exactly 1 row (bob)");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "bob") == 0, "scenario 2a: the matched row is bob");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "SELECT name FROM all_employees WHERE dept = 'eng' ORDER BY name", &r) == 0 && r.row_count == 2,
          "scenario 2b: outer WHERE + outer ORDER BY over the view together");
    {
        // NOTE: the outer SELECT-list narrows what out->columns[] REPORTS,
        // but (matching this whole codebase's established "column
        // projection is metadata-only" convention -- see sql_exec.h) the
        // materialized rows still carry the VIEW's own full underlying
        // columns (id, name, dept, salary) -- "name" is at index 1 there,
        // not 0.
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 2 && strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[1][0], "carol") == 0,
              "scenario 2b: alice before carol, alphabetically ascending");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "SELECT name FROM all_employees ORDER BY name LIMIT 1", &r) == 0 && r.row_count == 1,
          "scenario 2b: outer LIMIT over the view");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;   // "name" is column index 1 in the view's own underlying row -- see note above
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "alice") == 0, "scenario 2b: LIMIT 1 after ORDER BY keeps just alice");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "SELECT * FROM all_employees WHERE nope_col = 'x'", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer WHERE naming an unknown column is a loud, named error, not a silently-empty result");
    CHECK(sql_execute(1, "SELECT nope_col FROM all_employees", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer SELECT-list naming an unknown column is the same loud error");
    CHECK(sql_execute(1, "SELECT * FROM all_employees ORDER BY nope_col", &r) == 1 && r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer ORDER BY naming an unknown column is the same loud error");

    /* ── Scenario 2c: the outer WHERE/ORDER BY's documented STRING-only
     * simplification (see exec_select_view()'s own header comment) --
     * salary compares as TEXT, not numerically, once it's traveled through
     * a view's materialized result. "7000" vs "55000"/"60000"/"90000"/
     * "95000" sorts lexicographically ('5' < '6' < '7' < '9'), NOT
     * numerically (7000 is actually the smallest salary). Asserting the
     * LEXICOGRAPHIC order here is the honest verification the roadmap
     * calls for -- proving the simplification is real and documented, not
     * silently wrong. ─────────────────────────────────────────────────────── */
    sql_execute(1, "INSERT INTO employees (id, name, dept, salary) VALUES (5, 'frank', 'ops', 7000)", &r);
    CHECK(sql_execute(1, "SELECT salary FROM all_employees ORDER BY salary", &r) == 0 && r.row_count == 5,
          "scenario 2c: ORDER BY salary over the view succeeds (5 rows now)");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 3;   // "salary" is column index 3 in the view's own underlying row (id,name,dept,salary)
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 5 &&
              strcmp(ctx.vals[0][0], "55000") == 0 && strcmp(ctx.vals[1][0], "60000") == 0 &&
              strcmp(ctx.vals[2][0], "7000")  == 0 && strcmp(ctx.vals[3][0], "90000") == 0 &&
              strcmp(ctx.vals[4][0], "95000") == 0,
              "scenario 2c: lexicographic order (55000, 60000, 7000, 90000, 95000) -- the documented v1 simplification, not numeric order");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 3: a view whose underlying table is later dropped fails
     * LOUD at query time (propagating the real underlying error), not
     * silently empty. ──────────────────────────────────────────────────────── */
    {
        const char* cols[1] = {"x"};
        SLSFieldType types[1] = {FIELD_TYPE_STRING};
        make_table("temp_tbl", 0xF002, 1, cols, types);
    }
    sql_execute(1, "INSERT INTO temp_tbl (x) VALUES ('hello')", &r);
    CHECK(sql_execute(1, "CREATE VIEW temp_view AS SELECT x FROM temp_tbl", &r) == 0,
          "scenario 3: CREATE VIEW over temp_tbl succeeds (parse-time validation only checks grammar, not that the table still exists later)");
    CHECK(sql_execute(1, "SELECT * FROM temp_view", &r) == 0 && r.row_count == 1,
          "scenario 3: querying the view works while temp_tbl still exists");
    cursor_close(r.cursor_id);

    CHECK(sql_execute(1, "DROP TABLE temp_tbl", &r) == 0 && r.error == SQL_ERR_NONE, "scenario 3: DROP TABLE temp_tbl succeeds");
    CHECK(sql_execute(1, "SELECT * FROM temp_view", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 3: querying the view NOW fails loud with the REAL underlying error (table not found), not a silent empty result");

    /* ── Scenario 4: v1 rejections -- views in JOINs. ──────────────────────── */
    CHECK(sql_execute(1, "CREATE VIEW v1 AS SELECT id, name FROM employees", &r) == 0, "scenario 4 setup: CREATE VIEW v1 succeeds");
    CHECK(sql_execute(1, "SELECT * FROM v1 JOIN employees ON v1.id = employees.id", &r) == 1 &&
          r.error == SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED,
          "scenario 4a: a view as the FROM source of a JOIN is rejected with SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED");
    CHECK(sql_execute(1, "SELECT * FROM employees JOIN v1 ON employees.id = v1.id", &r) == 1 &&
          r.error == SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED,
          "scenario 4b: a view on the JOIN side is rejected the same way");

    /* ── Scenario 4c: v1 rejection -- views of views. Not a bespoke check --
     * caught for free by the SAME nesting-depth guard Phase 3 built (see
     * exec_select_view()'s own header comment): v2's body needs depth 1 to
     * run, and IT references v1, which needs depth 2 -- refused loud. ────── */
    CHECK(sql_execute(1, "CREATE VIEW v2 AS SELECT id, name FROM v1", &r) == 0,
          "scenario 4c setup: CREATE VIEW v2 AS SELECT ... FROM v1 succeeds (parse time never checks this)");
    CHECK(sql_execute(1, "SELECT * FROM v2", &r) == 1 && r.error == SQL_ERR_NESTING_TOO_DEEP,
          "scenario 4c: querying a view-of-a-view fails loud with SQL_ERR_NESTING_TOO_DEEP (v1 scope cut, not a crash or silent empty result)");

    /* ── Scenario 4d: v1 rejection -- DML through a view. ──────────────────── */
    CHECK(sql_execute(1, "INSERT INTO v1 (id, name) VALUES (99, 'zzz')", &r) == 1 &&
          r.error == SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED,
          "scenario 4d: INSERT INTO a view is rejected with SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED");
    CHECK(sql_execute(1, "UPDATE v1 SET name = 'zzz' WHERE id = 1", &r) == 1 &&
          r.error == SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED,
          "scenario 4d: UPDATE a view is rejected the same way");
    CHECK(sql_execute(1, "DELETE FROM v1 WHERE id = 1", &r) == 1 &&
          r.error == SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED,
          "scenario 4d: DELETE FROM a view is rejected the same way");

    /* ── Scenario 5: CREATE VIEW validation. ────────────────────────────────── */
    CHECK(sql_execute(1, "CREATE VIEW employees AS SELECT id FROM employees", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 5a: CREATE VIEW cannot shadow an existing TABLE name");
    CHECK(sql_execute(1, "CREATE VIEW v1 AS SELECT id FROM employees", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 5b: CREATE VIEW rejects a duplicate view name");
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("CREATE VIEW bad_view AS DELETE FROM employees", &stmt, err, sizeof(err)) == 1,
              "scenario 5c: CREATE VIEW ... AS <non-SELECT> is a PARSE error (the captured text must parse as a SELECT)");
        CHECK(sql_parse("CREATE VIEW bad_view AS", &stmt, err, sizeof(err)) == 1,
              "scenario 5d: CREATE VIEW ... AS with nothing after it is a parse error, not a crash");
        CHECK(sql_parse("CREATE VIEW ok_view AS SELECT id FROM employees", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_VIEW && strcmp(stmt.u.create_view.view_name, "ok_view") == 0 &&
              strcmp(stmt.u.create_view.sql_text, "SELECT id FROM employees") == 0,
              "scenario 5e: parser-level check -- CREATE VIEW captures the view name and the raw AS-clause text verbatim");
        CHECK(sql_parse("DROP VIEW ok_view", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_DROP_VIEW && strcmp(stmt.u.drop_view.view_name, "ok_view") == 0,
              "scenario 5e: parser-level check -- DROP VIEW captures the view name");
    }

    /* ── Scenario 6: DROP VIEW not-found / permission-denied. ──────────────── */
    CHECK(sql_execute(1, "DROP VIEW does_not_exist", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 6a: DROP VIEW on an unknown name fails loud (view not found)");
    {
        CHECK(sql_execute(42, "CREATE VIEW owned_by_42 AS SELECT id FROM employees", &r) == 0,
              "scenario 6b setup: uid 42 creates owned_by_42 (view_create() has no permission gate of its own, matching database_create())");
        g_test_role = ROLE_GUEST;   // flip the role stub: neither caller below is SYSTEM_KERNEL now
        CHECK(sql_execute(7, "DROP VIEW owned_by_42", &r) == 1 && r.error == SQL_ERR_PERMISSION_DENIED,
              "scenario 6b: a non-owner, non-kernel uid cannot DROP VIEW someone else's view");
        CHECK(sql_execute(42, "DROP VIEW owned_by_42", &r) == 0 && r.error == SQL_ERR_NONE,
              "scenario 6b: the OWNER can still drop it even under a non-kernel role (owner_uid == caller_uid short-circuits the gate)");
        g_test_role = ROLE_SYSTEM_KERNEL;   // restore for the remaining scenarios
    }

    /* ── Scenario 7: persistence round trip via the fake-NVMe harness --
     * real view_create() -> real persist_views() -> wipe views[] (simulating
     * a fresh boot's BSS-zero) -> real persist_restore_all() -> confirm the
     * view's name AND its full captured SQL text both survive, matching
     * rowstore_host_test.c's own established "real execution, restart round
     * trip" precedent. ─────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "CREATE VIEW survives_reboot AS SELECT id, name, dept FROM employees WHERE dept = 'eng'", &r) == 0,
          "scenario 7 setup: CREATE VIEW survives_reboot succeeds (persist_views() runs internally)");
    int pre_idx = view_find_index("survives_reboot");
    CHECK(pre_idx >= 0, "scenario 7 setup: view present pre-'reboot'");

    memset(views, 0, sizeof(views));   /* wipe every bit of in-memory view state a fresh boot's BSS-zero would */
    CHECK(view_find_index("survives_reboot") < 0, "scenario 7: post-'reboot', pre-restore: views[] genuinely wiped");

    /* mvcc.h's own header comment on mvcc_bootstrap_from_rowstore() spells
     * this out explicitly: "NOT idempotent -- calling this twice would
     * create duplicate versions of the same physical rows; persist_restore_
     * all() is the only correct call site." persist_restore_all() calls it
     * exactly once internally, but this test's "fixture" employees rows were
     * inserted earlier in this SAME process via sql_execute(), which already
     * registered live MVCC version entries for them. A genuine reboot zeroes
     * mvcc's in-memory arrays too (kernel.c calls mvcc_init() at step 6,
     * strictly before persist_restore_all() at step 7b) -- so the faithful
     * simulation of "fresh boot's BSS-zero" needs mvcc_init() here as well,
     * not just the views[] wipe above. Without it, mvcc_bootstrap_from_
     * rowstore() re-registers a second, redundant version for every already-
     * live row on top of the ones still sitting in mvcc_versions[] from this
     * process's own prior inserts, and every query returns each row twice --
     * confirmed by direct instrumentation: even a plain, view-free `SELECT *
     * FROM employees WHERE dept = 'eng'` doubled up the same way. Not a
     * view-execution bug; a missing-reset in this test's own "reboot". */
    mvcc_init();

    persist_restore_all();   /* real function from persist.c */

    int post_idx = view_find_index("survives_reboot");
    CHECK(post_idx >= 0, "scenario 7: view restored after persist_restore_all()");
    CHECK(post_idx >= 0 && strcmp(views[post_idx].sql_text, "SELECT id, name, dept FROM employees WHERE dept = 'eng'") == 0,
          "scenario 7: the FULL captured SQL text survived the round trip byte-for-byte");
    CHECK(post_idx >= 0 && views[post_idx].owner_uid == 1, "scenario 7: owner_uid survived the round trip");
    CHECK(sql_execute(1, "SELECT * FROM survives_reboot", &r) == 0 && r.row_count == 2,
          "scenario 7: the restored view is genuinely queryable, not just present as inert bytes (alice + carol, dept='eng')");
    cursor_close(r.cursor_id);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
