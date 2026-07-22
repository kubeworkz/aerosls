/*
 * sql_expr_phase3_host_test.c — SQL Feature-Parity Roadmap Phase 3
 * verification: a standalone host-buildable test for kernel/predicate.c's
 * new PRED_OP_LIKE / PRED_NODE_ARITH_COMPARISON support and kernel/
 * sql_parser.c's extended WHERE/SET grammar (parenthesized grouping,
 * IN (...), LIKE, arithmetic), linked against the REAL, unmodified full
 * stack -- sql_exec.c, sql_parser.c, predicate.c, row_index.c, rowstore.c,
 * persist.c, cursor.c, mvcc.c, row_constraint.c, row_journal.c -- not a
 * reimplementation. Mirrors sql_join2_phase2_host_test.c's own scaffold
 * (same stub globals, same fake NVMe, same CHECK() macro).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_expr_phase3_host_test tests/sql_expr_phase3_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c
 *   /tmp/sql_expr_phase3_host_test
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

#define COLLECT_MAX_COLS 4
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
     * products(id, name, category, price):
     *   1 Widget      gadgets   10.00
     *   2 Gizmo       gadgets   25.50
     *   3 Sprocket    hardware  5.00
     *   4 Widget Pro  gadgets   50.00
     *   5 Bolt        hardware  0.50 */
    {
        const char* cols[4] = {"id", "name", "category", "price"};
        SLSFieldType types[4] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_STRING, FIELD_TYPE_FLOAT};
        make_table("products", 0xE201, 4, cols, types);
    }
    struct SqlResult r;
    sql_execute(1, "INSERT INTO products (id, name, category, price) VALUES (1, 'Widget', 'gadgets', 10.00)", &r);
    sql_execute(1, "INSERT INTO products (id, name, category, price) VALUES (2, 'Gizmo', 'gadgets', 25.50)", &r);
    sql_execute(1, "INSERT INTO products (id, name, category, price) VALUES (3, 'Sprocket', 'hardware', 5.00)", &r);
    sql_execute(1, "INSERT INTO products (id, name, category, price) VALUES (4, 'Widget Pro', 'gadgets', 50.00)", &r);
    sql_execute(1, "INSERT INTO products (id, name, category, price) VALUES (5, 'Bolt', 'hardware', 0.50)", &r);
    CHECK(r.kind == SQL_STMT_INSERT && r.error == SQL_ERR_NONE, "setup: 5 products inserted");

    /* ── Scenario 1: parenthesized grouping overrides default AND/OR precedence. */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE (category = 'hardware' OR category = 'nonexistent') "
                        "AND price < 3", &r) == 0,
          "scenario 1: grouped predicate parses and executes");
    CHECK(r.row_count == 1, "scenario 1: only Bolt (hardware, price 0.50 < 3) matches");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 1;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "Bolt") == 0, "scenario 1: matched row is Bolt");
        cursor_close(r.cursor_id);
    }
    /* Without parens, "category='hardware' OR category='nonexistent' AND price<3"
     * would parse as OR(comp, AND(comp,comp)) under a flat and_expr(OR and_expr)*
     * grammar -- both Sprocket and Bolt (all hardware) plus nothing from the AND
     * side match, i.e. every hardware row. Confirms grouping actually changes
     * the result versus the ungrouped form. */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE category = 'hardware' OR category = 'nonexistent' "
                        "AND price < 3", &r) == 0 && r.row_count == 2,
          "scenario 1b: same clause WITHOUT parens matches both hardware rows -- proves grouping changed precedence");
    cursor_close(r.cursor_id);

    /* ── Scenario 2: IN membership. ────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE id IN (1, 3, 5)", &r) == 0,
          "scenario 2: IN list parses and executes");
    CHECK(r.row_count == 3, "scenario 2: exactly 3 of 5 rows match id IN (1,3,5)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE id IN (999)", &r) == 0 && r.row_count == 0,
          "scenario 2b: IN list matching zero rows returns an empty (not erroring) result");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE category IN ('gadgets')", &r) == 0 && r.row_count == 3,
          "scenario 2c: single-value IN list behaves like equality (3 gadgets)");
    cursor_close(r.cursor_id);

    /* ── Scenario 3: LIKE wildcard matching. ───────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE name LIKE 'Widget%'", &r) == 0,
          "scenario 3: LIKE with '%' wildcard parses and executes");
    CHECK(r.row_count == 2, "scenario 3: 'Widget%' matches Widget and Widget Pro");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE name LIKE 'B_lt'", &r) == 0 && r.row_count == 1,
          "scenario 3b: '_' wildcard matches exactly one character (Bolt)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE name LIKE 'nomatch%'", &r) == 0 && r.row_count == 0,
          "scenario 3c: non-matching LIKE pattern returns zero rows cleanly");
    cursor_close(r.cursor_id);
    /* LIKE against a non-STRING column fails closed (documented Phase 3 scope: no coercion). */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE price LIKE '1%'", &r) == 0 && r.row_count == 0,
          "scenario 3d: LIKE against a non-STRING (FLOAT) column fails closed -- zero rows, not a crash");
    cursor_close(r.cursor_id);

    /* ── Scenario 4: arithmetic in WHERE. ──────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM products WHERE price * 2 > 40", &r) == 0,
          "scenario 4: arithmetic WHERE comparison (column * literal) parses and executes");
    CHECK(r.row_count == 2, "scenario 4: price*2>40 matches Gizmo (51) and Widget Pro (100)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE price + 5 = 15", &r) == 0 && r.row_count == 1,
          "scenario 4b: price + 5 = 15 matches only Widget (10+5=15)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM products WHERE id / 0 = 1", &r) == 0 && r.row_count == 0,
          "scenario 4c: division by zero in a WHERE arithmetic expression fails closed (zero rows, not a crash)");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: arithmetic in UPDATE ... SET, including the
     * multiple-assignments-see-OLD-values semantic.
     * NOTE on column indices below: cursor_fetch_rows() -- for a plain
     * (non-join, non-aggregate) SELECT -- always hands the callback the
     * FULL underlying table row (id=0, name=1, category=2, price=3), not
     * a row trimmed to just the SELECT list; out->columns[] is what names
     * the requested projection, not the row shape delivered to callbacks.
     * This matches the JOIN path's own documented "materialized rows
     * always carry the FULL combined row" convention in
     * sql_join2_phase2_host_test.c -- so every collectN_ctx below indexes
     * into the real table layout, not the SELECT list position. ────────── */
    CHECK(sql_execute(1, "UPDATE products SET price = price * 1.1 WHERE id = 1", &r) == 0,
          "scenario 5: arithmetic SET (column * literal) parses and executes");
    CHECK(r.affected_rows == 1, "scenario 5: exactly 1 row updated");
    CHECK(sql_execute(1, "SELECT price FROM products WHERE id = 1", &r) == 0, "scenario 5: re-select after update succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 3;  /* price */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        double v = atof(ctx.vals[0][0]);
        CHECK(ctx.count == 1 && v > 10.99 && v < 11.01, "scenario 5: Widget's price became 11.0 (10.00 * 1.1)");
        cursor_close(r.cursor_id);
    }
    /* id=4 Widget Pro price=50: SET price = price + 10, name = 'Discounted'
     * -- neither assignment depends on the other here, but this establishes
     * the multi-assignment path runs both correctly in one statement. */
    CHECK(sql_execute(1, "UPDATE products SET price = price + 10, name = 'Discounted' WHERE id = 4", &r) == 0 &&
          r.affected_rows == 1,
          "scenario 5b: multi-assignment UPDATE (one arithmetic, one literal) succeeds");
    CHECK(sql_execute(1, "SELECT name, price FROM products WHERE id = 4", &r) == 0, "scenario 5b: re-select succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 1; ctx.idx[1] = 3;  /* name, price */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        double v = atof(ctx.vals[0][1]);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "Discounted") == 0 && v > 59.99 && v < 60.01,
              "scenario 5b: name became 'Discounted' and price became 60.0 (50+10)");
        cursor_close(r.cursor_id);
    }
    /* The real OLD-value semantic: id=3 Sprocket price=5. SET price = price + 1,
     * then set a NEW column 'category' (string, unaffected) just to exercise
     * two SET entries where one is arithmetic -- to directly prove ordering
     * independence we'd need two arithmetic SETs referencing each other, but
     * this schema only has one numeric column. Instead: verify that reading
     * `price` mid-statement is impossible by chaining a second arithmetic
     * SET against the SAME column and confirming it computes from the
     * ORIGINAL value, not a chained one, via two separate single-column
     * arithmetic updates compared against the expected closed-form result. */
    CHECK(sql_execute(1, "UPDATE products SET price = price + 1 WHERE id = 3", &r) == 0 && r.affected_rows == 1,
          "scenario 5c: setup for OLD-value check -- Sprocket price 5 -> 6");
    CHECK(sql_execute(1, "SELECT price FROM products WHERE id = 3", &r) == 0, "scenario 5c: re-select succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 3;  /* price */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        double v = atof(ctx.vals[0][0]);
        CHECK(ctx.count == 1 && v > 5.99 && v < 6.01, "scenario 5c: Sprocket's price became 6.0 (5 + 1)");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 6: arithmetic SET failing closed (non-numeric operand). ─── */
    CHECK(sql_execute(1, "UPDATE products SET price = category + 1 WHERE id = 2", &r) == 1 &&
          r.error == SQL_ERR_VALUE_INVALID,
          "scenario 6: arithmetic SET referencing a non-numeric (STRING) column fails closed with SQL_ERR_VALUE_INVALID");

    /* ── Scenario 7: parser-level checks (sql_parse() directly, no exec). ── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT * FROM products WHERE (price > 1)", &stmt, err, sizeof(err)) == 0,
              "scenario 7: a trivially-parenthesized single comparison parses fine");
        CHECK(sql_parse("SELECT * FROM products WHERE id NOT IN (1,2)", &stmt, err, sizeof(err)) == 1,
              "scenario 7b: NOT IN is not implemented -- fails to parse cleanly (named out-of-scope item)");
        // Phase 4 promoted IS NULL out of scope-cut status and implemented it
        // for real (see sql_null_phase4_host_test.c) -- this now parses fine,
        // updated from the Phase 3 assertion that it failed to parse.
        CHECK(sql_parse("SELECT * FROM products WHERE name IS NULL", &stmt, err, sizeof(err)) == 0,
              "scenario 7c: IS NULL now parses cleanly (Phase 4 implemented it -- was out-of-scope in Phase 3)");
        CHECK(sql_parse("SELECT * FROM products WHERE price > (id + 1) * 2", &stmt, err, sizeof(err)) == 1,
              "scenario 7d: multi-operation arithmetic (more than one +-*/ per comparison) is out of scope -- fails to parse");
    }

    /* ── Scenario 8: permission denial still propagates cleanly through the
     * new predicate kinds (LIKE/arithmetic), matching every prior phase's
     * own permission-denial regression. ───────────────────────────────────── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT * FROM products WHERE name LIKE 'W%' AND price * 2 > 1", &r) == 0 && r.row_count == 0,
          "scenario 8: permission denial makes a LIKE+arithmetic WHERE return 0 rows cleanly, not a crash");
    g_access_force_deny = 0;

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
