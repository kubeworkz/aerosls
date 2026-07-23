/*
 * sql_cte_phase6_host_test.c — SQL Query-Surface Roadmap Phase 6
 * verification: a standalone host-buildable test for kernel/sql_parser.c's
 * WITH <name> AS (<select...>) grammar and kernel/sql_exec.c's
 * exec_select_cte()/the has_cte resolution precedence/the JOIN and
 * aggregate/set-op v1 rejections, linked against the REAL, unmodified full
 * stack -- sql_exec.c, sql_parser.c, predicate.c, row_index.c, rowstore.c,
 * persist.c, view.c, cursor.c, mvcc.c, row_constraint.c, row_journal.c,
 * database.c -- not a reimplementation. Mirrors sql_view_phase5_host_test.c's
 * own fixture/stub scaffolding (same stack, same reasons -- view.c/persist.c
 * are still linked here because scenario 5 below exercises a CTE whose own
 * body queries a view, proving the shared depth-budget guard).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_cte_phase6_host_test tests/sql_cte_phase6_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_cte_phase6_host_test
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

SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
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

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- see
// sql_view_phase5_host_test.c's identical comment for why these three
// stubbed returns are safe here too. ─────────────────────────────────────
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
     * employees(id, name, dept, salary), four rows spanning eng/sales/hr. */
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

    /* ── Scenario 1: basic WITH ... AS (...) SELECT round trip. ────────────── */
    CHECK(sql_execute(1, "WITH eng AS (SELECT id, name, salary FROM employees WHERE dept = 'eng') SELECT * FROM eng", &r) == 0,
          "scenario 1: WITH ... SELECT * FROM <cte-name> succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 2 && r.column_count == 3,
          "scenario 1: 2 rows (alice, carol), 3 columns (id, name, salary) -- the CTE's own projection");
    CHECK(strcmp(r.columns[0], "id") == 0 && strcmp(r.columns[1], "name") == 0 && strcmp(r.columns[2], "salary") == 0,
          "scenario 1: outer SELECT * reports the CTE's own column list");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(has_val(&ctx, 0, "alice") && has_val(&ctx, 0, "carol") && !has_val(&ctx, 0, "bob") && !has_val(&ctx, 0, "dave"),
              "scenario 1: only the eng-department rows (alice, carol) came through the CTE's own WHERE");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "WITH eng AS (SELECT id, name, salary FROM employees WHERE dept = 'eng') SELECT name FROM eng", &r) == 0 &&
          r.row_count == 2 && r.column_count == 1,
          "scenario 1: a narrower outer projection over the CTE also works");
    cursor_close(r.cursor_id);

    /* ── Scenario 2: outer WHERE/ORDER BY/LIMIT composed over a no-WHERE
     * CTE, plus the same loud-error-on-unknown-column checks views got. ──── */
    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT * FROM allemp WHERE dept = 'sales'", &r) == 0 &&
          r.row_count == 1, "scenario 2a: outer WHERE over the CTE narrows to exactly 1 row (bob)");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "bob") == 0, "scenario 2a: the matched row is bob");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT name FROM allemp WHERE dept = 'eng' ORDER BY name", &r) == 0 &&
          r.row_count == 2, "scenario 2b: outer WHERE + outer ORDER BY over the CTE together");
    {
        // Same "column projection is metadata-only" convention as views
        // (see sql_view_phase5_host_test.c's scenario 2b note) -- the
        // materialized rows carry the CTE's own full underlying row
        // (id, name, dept, salary), so "name" is at index 1, not 0.
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 2 && strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[1][0], "carol") == 0,
              "scenario 2b: alice before carol, alphabetically ascending");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT name FROM allemp ORDER BY name LIMIT 1", &r) == 0 &&
          r.row_count == 1, "scenario 2b: outer LIMIT over the CTE");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "alice") == 0, "scenario 2b: LIMIT 1 after ORDER BY keeps just alice");
        cursor_close(r.cursor_id);
    }

    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT * FROM allemp WHERE nope_col = 'x'", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer WHERE naming an unknown column is a loud, named error, not a silently-empty result");
    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT nope_col FROM allemp", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer SELECT-list naming an unknown column is the same loud error");
    CHECK(sql_execute(1, "WITH allemp AS (SELECT id, name, dept, salary FROM employees) SELECT * FROM allemp ORDER BY nope_col", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_NOT_FOUND,
          "scenario 2: an outer ORDER BY naming an unknown column is the same loud error");

    /* ── Scenario 3: a CTE SHADOWS a same-named real TABLE -- standard SQL
     * scoping, the OPPOSITE precedence from views (which only win as a
     * fallback). The CTE's own body legitimately queries the REAL
     * "employees" table (its nested re-parse has no memory of being
     * "inside" a same-named CTE), but the OUTER "FROM employees" resolves
     * to the CTE, not the real table. ──────────────────────────────────────── */
    CHECK(sql_execute(1, "WITH employees AS (SELECT id, name FROM employees WHERE dept = 'hr') SELECT * FROM employees", &r) == 0 &&
          r.row_count == 1 && r.column_count == 2,
          "scenario 3: CTE named 'employees' shadows the real table -- only 1 row (dave, dept=hr) and 2 columns, the CTE's own shape");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "dave") == 0, "scenario 3: the CTE's own row (dave), not the real table's 4 rows");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(1, "SELECT * FROM employees", &r) == 0 && r.row_count == 4,
          "scenario 3: a later, ordinary (non-WITH) query against 'employees' still sees the real table's all 4 rows -- the CTE's shadow was scoped to its OWN statement only");

    /* ── Scenario 4: v1 rejections -- CTE combined with JOIN, aggregates, or
     * a set operator, ALL caught by exec_select()'s single top-of-function
     * check (see sql_exec.c's own comment for why this is one choke point,
     * unlike views' own two-separate-gaps posture). ───────────────────────── */
    CHECK(sql_execute(1, "WITH x AS (SELECT id, name FROM employees WHERE dept = 'eng') SELECT * FROM x JOIN employees ON x.id = employees.id", &r) == 1 &&
          r.error == SQL_ERR_CTE_SCOPE_UNSUPPORTED,
          "scenario 4a: a CTE as the FROM source of a JOIN is rejected with SQL_ERR_CTE_SCOPE_UNSUPPORTED");
    CHECK(sql_execute(1, "WITH x AS (SELECT id, name FROM employees WHERE dept = 'eng') SELECT * FROM employees JOIN x ON employees.id = x.id", &r) == 1 &&
          r.error == SQL_ERR_CTE_SCOPE_UNSUPPORTED,
          "scenario 4b: a CTE on the JOIN side (own FROM source is a real table) is rejected the same way");
    CHECK(sql_execute(1, "WITH x AS (SELECT id, dept FROM employees) SELECT COUNT(*) FROM x", &r) == 1 &&
          r.error == SQL_ERR_CTE_SCOPE_UNSUPPORTED,
          "scenario 4c: a CTE combined with an aggregate is rejected the same way (no silent 'table not found' gap, unlike views' own GROUP BY case)");
    CHECK(sql_execute(1, "WITH x AS (SELECT id, name FROM employees WHERE dept = 'eng') SELECT id, name FROM x UNION SELECT id, name FROM employees WHERE dept = 'hr'", &r) == 1 &&
          r.error == SQL_ERR_CTE_SCOPE_UNSUPPORTED,
          "scenario 4d: a CTE combined with a set operator is rejected the same way");

    /* ── Scenario 5: a CTE whose OWN body queries a VIEW hits the SAME
     * SQL_EXEC_MAX_DEPTH=2 guard views-of-views uses -- not a bespoke
     * check, the exact "CTE over a view" verification the roadmap doc
     * calls for. ────────────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "CREATE VIEW eng_view AS SELECT id, name FROM employees WHERE dept = 'eng'", &r) == 0,
          "scenario 5 setup: CREATE VIEW eng_view succeeds");
    CHECK(sql_execute(1, "WITH x AS (SELECT * FROM eng_view) SELECT * FROM x", &r) == 1 &&
          r.error == SQL_ERR_NESTING_TOO_DEEP,
          "scenario 5: a CTE over a view fails loud with SQL_ERR_NESTING_TOO_DEEP (the same depth-budget guard views-of-views uses), not a crash or silent empty result");

    /* ── Scenario 6: non-recursive by construction -- a CTE body naming its
     * OWN cte_name resolves via an entirely fresh, independent nested
     * parse/exec that has no memory of "being inside" that CTE, so it just
     * fails as an ordinary unknown table, not an infinite loop. ───────────── */
    CHECK(sql_execute(1, "WITH x AS (SELECT * FROM x) SELECT * FROM x", &r) == 1 &&
          r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 6: a CTE body referencing its own name fails loud as an ordinary unknown table, not a hang or crash");

    /* ── Scenario 7: a CTE body containing ANOTHER WITH clause (nested or
     * self-referential) is refused at PARSE time by the same reentrancy
     * guard CREATE VIEW/the set operator's own eager validation uses. ─────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("WITH x AS (WITH y AS (SELECT id FROM employees) SELECT id FROM y) SELECT * FROM x", &stmt, err, sizeof(err)) == 1,
              "scenario 7: a WITH clause whose own body contains another WITH clause is a parse error, not silently accepted");
    }

    /* ── Scenario 8: parser-level checks -- has_cte/cte_name/cte_text
     * capture, empty AS-body, non-SELECT AS-body, missing SELECT after. ───── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("WITH x AS (SELECT id FROM employees) SELECT * FROM x", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_SELECT && stmt.u.select.has_cte == 1 &&
              strcmp(stmt.u.select.cte_name, "x") == 0 &&
              strcmp(stmt.u.select.cte_text, "SELECT id FROM employees") == 0 &&
              strcmp(stmt.u.select.table_name, "x") == 0,
              "scenario 8a: parser-level check -- WITH captures the CTE name and the raw AS-clause body verbatim, and the outer SELECT parses normally with has_cte set");
        CHECK(sql_parse("WITH x AS () SELECT * FROM x", &stmt, err, sizeof(err)) == 1,
              "scenario 8b: WITH x AS () with an empty body is a parse error, not a crash");
        CHECK(sql_parse("WITH x AS (DELETE FROM employees) SELECT * FROM x", &stmt, err, sizeof(err)) == 1,
              "scenario 8c: WITH x AS (<non-SELECT>) is a parse error (the captured body must parse as a SELECT)");
        CHECK(sql_parse("WITH x AS (SELECT id FROM employees)", &stmt, err, sizeof(err)) == 1,
              "scenario 8d: WITH ... AS (...) with no SELECT following is a parse error, not a crash");
        CHECK(sql_parse("SELECT * FROM employees", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_SELECT && stmt.u.select.has_cte == 0,
              "scenario 8e: an ordinary SELECT with no WITH prefix still parses with has_cte==0 (zero-default, byte-for-byte pre-Phase-6 behavior)");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
