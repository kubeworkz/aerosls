/*
 * sql_setop_phase4_host_test.c — SQL Query-Surface Roadmap Phase 4
 * verification: a standalone host-buildable test for kernel/sql_exec.c's
 * exec_select_set_op() and kernel/sql_parser.c's UNION/UNION ALL/
 * INTERSECT/EXCEPT grammar, linked against the REAL, unmodified full
 * stack -- sql_exec.c, sql_parser.c, predicate.c, row_index.c, rowstore.c,
 * persist.c, cursor.c, mvcc.c, row_constraint.c, row_journal.c,
 * database.c -- not a reimplementation. Mirrors sql_group_phase1_host_
 * test.c's own fixture/stub scaffolding exactly (same stack, same reasons).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_setop_phase4_host_test tests/sql_setop_phase4_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_setop_phase4_host_test
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

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- sql_exec.c's
// exec_create_table() unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's rowstore_drop_table() unconditionally
// calls sys_sls_vfree(). This test never exercises CREATE/DROP TABLE via SQL
// text at runtime, so failure-code no-ops are safe here. ──────────────────
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
static int has_row2(struct collectN_ctx* ctx, const char* a, const char* b) {
    for (uint32_t i = 0; i < ctx->count; i++)
        if (strcmp(ctx->vals[i][0], a) == 0 && strcmp(ctx->vals[i][1], b) == 0) return 1;
    return 0;
}
static uint32_t count_row2(struct collectN_ctx* ctx, const char* a, const char* b) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ctx->count; i++)
        if (strcmp(ctx->vals[i][0], a) == 0 && strcmp(ctx->vals[i][1], b) == 0) n++;
    return n;
}

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();

    /* ── Fixture ────────────────────────────────────────────────────────────
     * employees_ny(name, dept): alice/eng, bob/sales, carol/eng
     * employees_sf(name, dept): carol/eng (an EXACT duplicate of employees_
     *   ny's carol/eng row -- the deliberate overlap every set operator's
     *   own scenario below depends on), dave/sales, erin/hr */
    {
        const char* cols[2] = {"name", "dept"};
        SLSFieldType types[2] = {FIELD_TYPE_STRING, FIELD_TYPE_STRING};
        make_table("employees_ny", 0xE201, 2, cols, types);
        make_table("employees_sf", 0xE202, 2, cols, types);
    }
    struct SqlResult r;
    sql_execute(1, "INSERT INTO employees_ny (name, dept) VALUES ('alice', 'eng')", &r);
    sql_execute(1, "INSERT INTO employees_ny (name, dept) VALUES ('bob', 'sales')", &r);
    sql_execute(1, "INSERT INTO employees_ny (name, dept) VALUES ('carol', 'eng')", &r);
    sql_execute(1, "INSERT INTO employees_sf (name, dept) VALUES ('carol', 'eng')", &r);
    sql_execute(1, "INSERT INTO employees_sf (name, dept) VALUES ('dave', 'sales')", &r);
    sql_execute(1, "INSERT INTO employees_sf (name, dept) VALUES ('erin', 'hr')", &r);
    CHECK(r.error == SQL_ERR_NONE, "setup: 3 employees_ny rows, 3 employees_sf rows inserted");

    /* ── Scenario 1: UNION dedups the one genuinely overlapping row. ──────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny UNION SELECT name, dept FROM employees_sf", &r) == 0,
          "scenario 1: UNION succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 5, "scenario 1: 5 distinct rows (carol/eng deduped from 6 raw)");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 0; ctx.idx[1] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(has_row2(&ctx, "alice", "eng") && has_row2(&ctx, "bob", "sales") &&
              has_row2(&ctx, "carol", "eng") && has_row2(&ctx, "dave", "sales") && has_row2(&ctx, "erin", "hr"),
              "scenario 1: all 5 distinct rows present");
        CHECK(count_row2(&ctx, "carol", "eng") == 1, "scenario 1: carol/eng appears exactly ONCE (deduped, not twice)");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: UNION ALL keeps the duplicate. ────────────────────────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny UNION ALL SELECT name, dept FROM employees_sf", &r) == 0,
          "scenario 2: UNION ALL succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 6, "scenario 2: all 6 raw rows kept (no dedup)");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 0; ctx.idx[1] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(count_row2(&ctx, "carol", "eng") == 2, "scenario 2: carol/eng appears TWICE (UNION ALL keeps both)");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 3: INTERSECT keeps only the genuinely shared row. ────────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny INTERSECT SELECT name, dept FROM employees_sf", &r) == 0,
          "scenario 3: INTERSECT succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 1, "scenario 3: exactly 1 row shared by both sides");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 0; ctx.idx[1] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "carol") == 0 && strcmp(ctx.vals[0][1], "eng") == 0,
              "scenario 3: the shared row is carol/eng");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 4: EXCEPT keeps left-only rows (removes the overlap). ────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny EXCEPT SELECT name, dept FROM employees_sf", &r) == 0,
          "scenario 4: EXCEPT succeeds");
    CHECK(r.error == SQL_ERR_NONE && r.row_count == 2, "scenario 4: 2 rows left after removing the shared row from employees_ny's own 3");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 0; ctx.idx[1] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(has_row2(&ctx, "alice", "eng") && has_row2(&ctx, "bob", "sales") && !has_row2(&ctx, "carol", "eng"),
              "scenario 4: alice/eng and bob/sales remain, carol/eng is gone");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 5: ORDER BY / LIMIT apply to the MERGED result. ──────────── */
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny UNION SELECT name, dept FROM employees_sf ORDER BY name", &r) == 0,
          "scenario 5: UNION with a trailing ORDER BY succeeds");
    CHECK(r.row_count == 5, "scenario 5: still 5 distinct rows");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 5 &&
              strcmp(ctx.vals[0][0], "alice") == 0 && strcmp(ctx.vals[1][0], "bob") == 0 &&
              strcmp(ctx.vals[2][0], "carol") == 0 && strcmp(ctx.vals[3][0], "dave") == 0 &&
              strcmp(ctx.vals[4][0], "erin") == 0,
              "scenario 5: merged result sorted alphabetically by name");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(1, "SELECT name, dept FROM employees_ny UNION SELECT name, dept FROM employees_sf ORDER BY name LIMIT 3", &r) == 0 &&
          r.row_count == 3,
          "scenario 5: LIMIT after ORDER BY keeps exactly the first 3 of the merged, sorted result");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 3 && strcmp(ctx.vals[0][0], "alice") == 0 &&
              strcmp(ctx.vals[1][0], "bob") == 0 && strcmp(ctx.vals[2][0], "carol") == 0,
              "scenario 5: LIMIT 3 keeps alice, bob, carol");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 6: column-count mismatch between branches is a loud error. */
    CHECK(sql_execute(1, "SELECT name FROM employees_ny UNION SELECT name, dept FROM employees_sf", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_COUNT_MISMATCH,
          "scenario 6: mismatched branch column counts rejected with SQL_ERR_COLUMN_COUNT_MISMATCH");

    /* ── Scenario 7: chained set operators ("A op B op C") are a PARSE
     * error in v1, not silently truncated or misinterpreted. ─────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT name FROM employees_ny UNION SELECT name FROM employees_sf UNION SELECT name FROM employees_ny",
                        &stmt, err, sizeof(err)) == 1,
              "scenario 7: a chained 'A UNION B UNION C' is a PARSE error (one set operator per statement in v1)");
    }

    /* ── Scenario 8: parser-level checks (sql_parse() directly, no exec). ── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT name FROM employees_ny UNION SELECT name FROM employees_sf", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.has_set_op == 1 && stmt.u.select.set_op == SQL_SETOP_UNION,
              "scenario 8: bare UNION parses and sets SQL_SETOP_UNION");
        CHECK(sql_parse("SELECT name FROM employees_ny UNION ALL SELECT name FROM employees_sf", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.set_op == SQL_SETOP_UNION_ALL,
              "scenario 8: UNION ALL parses and sets SQL_SETOP_UNION_ALL");
        CHECK(sql_parse("SELECT name FROM employees_ny INTERSECT SELECT name FROM employees_sf", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.set_op == SQL_SETOP_INTERSECT,
              "scenario 8: INTERSECT parses and sets SQL_SETOP_INTERSECT");
        CHECK(sql_parse("SELECT name FROM employees_ny EXCEPT SELECT name FROM employees_sf", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.set_op == SQL_SETOP_EXCEPT,
              "scenario 8: EXCEPT parses and sets SQL_SETOP_EXCEPT");
        CHECK(sql_parse("SELECT name FROM employees_ny UNION SELECT name FROM employees_sf ORDER BY name DESC LIMIT 2",
                        &stmt, err, sizeof(err)) == 0 &&
              stmt.u.select.has_order_by == 1 && strcmp(stmt.u.select.order_by, "name") == 0 &&
              stmt.u.select.order_desc == 1 && stmt.u.select.has_limit == 1 && stmt.u.select.limit == 2,
              "scenario 8: trailing ORDER BY DESC LIMIT 2 lands on the OUTER statement's own fields, not the right branch's");
        CHECK(sql_parse("SELECT name FROM employees_ny UNION", &stmt, err, sizeof(err)) == 1,
              "scenario 8: UNION with nothing after it is a parse error, not a crash");
    }

    /* ── Scenario 9: a set-op statement's LEFT branch can itself be a JOIN,
     * proving exec_select_set_op() correctly re-enters exec_select()'s own
     * dispatch rather than assuming a plain single-table branch. ─────────── */
    {
        const char* dcols[2] = {"id", "title"};
        SLSFieldType dtypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("departments2", 0xE203, 2, dcols, dtypes);
        sql_execute(1, "INSERT INTO departments2 (id, title) VALUES (1, 'Engineering')", &r);
        const char* ecols[2] = {"id", "dept_id"};
        SLSFieldType etypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_UINT64};
        make_table("emp_ids", 0xE204, 2, ecols, etypes);
        sql_execute(1, "INSERT INTO emp_ids (id, dept_id) VALUES (1, 1)", &r);
    }
    CHECK(sql_execute(1, "SELECT departments2.title, emp_ids.id FROM emp_ids "
                        "JOIN departments2 ON emp_ids.dept_id = departments2.id "
                        "UNION SELECT name, name FROM employees_ny WHERE name = 'zzz-none'", &r) == 0 &&
          r.error == SQL_ERR_NONE && r.row_count == 1,
          "scenario 9: a JOIN as the set-op's LEFT branch executes correctly (right branch legitimately empty)");
    cursor_close(r.cursor_id);

    /* ── Scenario 10: both branches near/over the 256-row cap -- UNION ALL
     * truncates honestly (out.truncated set, row_count capped), never
     * silently drops the cap signal. ──────────────────────────────────────── */
    {
        const char* cols[1] = {"n"};
        SLSFieldType types[1] = {FIELD_TYPE_UINT64};
        make_table("bulk_a", 0xE205, 1, cols, types);
        make_table("bulk_b", 0xE206, 1, cols, types);
        char stmt_buf[64];
        for (int i = 0; i < 150; i++) {
            snprintf(stmt_buf, sizeof(stmt_buf), "INSERT INTO bulk_a (n) VALUES (%d)", i);
            sql_execute(1, stmt_buf, &r);
        }
        for (int i = 1000; i < 1150; i++) {
            snprintf(stmt_buf, sizeof(stmt_buf), "INSERT INTO bulk_b (n) VALUES (%d)", i);
            sql_execute(1, stmt_buf, &r);
        }
    }
    CHECK(sql_execute(1, "SELECT n FROM bulk_a UNION ALL SELECT n FROM bulk_b", &r) == 0,
          "scenario 10: UNION ALL over 300 combined rows (150+150) succeeds");
    CHECK(r.row_count == 256 && r.truncated == 1,
          "scenario 10: merged result honestly capped at 256 with truncated=1, not silently dropped or overflowed");
    cursor_close(r.cursor_id);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
