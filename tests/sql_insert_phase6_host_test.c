/*
 * sql_insert_phase6_host_test.c — SQL Feature-Parity Roadmap Phase 6
 * verification: a standalone host-buildable test for kernel/sql_parser.c's
 * new multi-row VALUES grammar and kernel/sql_exec.c's new exec_insert()
 * loop + partial-column NULL-fill -- linked against the REAL, unmodified
 * full stack: sql_exec.c, sql_parser.c, predicate.c, row_index.c,
 * rowstore.c, persist.c, cursor.c, mvcc.c, row_constraint.c, row_journal.c.
 * Mirrors sql_null_phase4_host_test.c's own scaffold (same stub globals,
 * same fake NVMe, same CHECK() macro, same make_table()/collectN_ctx
 * helpers).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_insert_phase6_host_test tests/sql_insert_phase6_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_insert_phase6_host_test
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
// exec_create_table() unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's rowstore_drop_table() unconditionally
// calls sys_sls_vfree() (real object_catalog.c cleanup this test doesn't
// link). This test never exercises CREATE/DROP TABLE via SQL text at
// runtime, so failure-code no-ops are safe here. ───────────────────────────
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
     * widgets(id, name, color, weight): no constraints beyond the table
     * shape itself, except a UNIQUE on `name` (used by the mid-batch
     * constraint-violation rollback scenario). */
    {
        const char* cols[4] = {"id", "name", "color", "weight"};
        SLSFieldType types[4] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_STRING, FIELD_TYPE_STRING};
        make_table("widgets", 0xF601, 4, cols, types);
    }
    CHECK(row_constraint_add_unique("widgets", "name") == ROW_CONSTRAINT_OK, "setup: UNIQUE(widgets.name) registers");

    struct SqlResult r;

    /* ── Scenario 1: multi-row INSERT lands every row, in order, in ONE
     * statement. ──────────────────────────────────────────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO widgets (id, name, color, weight) VALUES "
                        "(1, 'sprocket', 'red', '10'), "
                        "(2, 'cog', 'blue', '20'), "
                        "(3, 'gear', 'green', '30')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 1: 3-row INSERT succeeds");
    CHECK(r.affected_rows == 3, "scenario 1: affected_rows reports all 3 rows");

    CHECK(sql_execute(1, "SELECT id, name, color, weight FROM widgets ORDER BY id", &r) == 0,
          "scenario 1: re-select succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 4; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2; ctx.idx[3] = 3;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 3, "scenario 1: all 3 rows are actually queryable back");
        CHECK(strcmp(ctx.vals[0][1], "sprocket") == 0 && strcmp(ctx.vals[0][2], "red") == 0 && strcmp(ctx.vals[0][3], "10") == 0,
              "scenario 1: row 0 (id=1) landed with the right values, in order");
        CHECK(strcmp(ctx.vals[1][1], "cog") == 0 && strcmp(ctx.vals[1][2], "blue") == 0 && strcmp(ctx.vals[1][3], "20") == 0,
              "scenario 1: row 1 (id=2) landed with the right values, in order");
        CHECK(strcmp(ctx.vals[2][1], "gear") == 0 && strcmp(ctx.vals[2][2], "green") == 0 && strcmp(ctx.vals[2][3], "30") == 0,
              "scenario 1: row 2 (id=3) landed with the right values, in order");
    }

    /* ── Scenario 2: partial-column INSERT -- omitted columns are a real
     * NULL, not zero-filled/corrupted. ────────────────────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO widgets (id, name) VALUES (4, 'washer')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 2: partial-column INSERT (only id, name given) succeeds");
    CHECK(sql_execute(1, "SELECT id, name, color, weight FROM widgets WHERE id = 4", &r) == 0,
          "scenario 2: re-select of the partial row succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 4; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2; ctx.idx[3] = 3;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 1, "scenario 2: the partial row is queryable back");
        CHECK(strcmp(ctx.vals[0][1], "washer") == 0, "scenario 2: named column 'name' has the given value");
        /* full-row layout: 0=id,1=name,2=color,3=weight -- color is column
         * index 2, weight is column index 3. */
        CHECK((ctx.null_mask[0] & (1u << 2)) != 0, "scenario 2: omitted column 'color' is a real NULL");
        CHECK((ctx.null_mask[0] & (1u << 3)) != 0, "scenario 2: omitted column 'weight' is a real NULL");
    }

    /* ── Scenario 3: multi-row INSERT combined with partial columns AND an
     * explicit NULL literal in the same statement. ────────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO widgets (id, name, color) VALUES "
                        "(5, 'bolt', NULL), "
                        "(6, 'nut', 'silver')", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: multi-row + partial-column + explicit NULL together succeeds");
    CHECK(r.affected_rows == 2, "scenario 3: affected_rows reports both rows");
    CHECK(sql_execute(1, "SELECT id, name, color, weight FROM widgets WHERE id = 5", &r) == 0,
          "scenario 3: re-select id=5 succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 4; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2; ctx.idx[3] = 3;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 1 && (ctx.null_mask[0] & (1u << 2)) != 0, "scenario 3: id=5's explicit NULL color really is NULL");
        CHECK((ctx.null_mask[0] & (1u << 3)) != 0, "scenario 3: id=5's omitted weight is also NULL");
    }
    CHECK(sql_execute(1, "SELECT id, name, color, weight FROM widgets WHERE id = 6", &r) == 0,
          "scenario 3: re-select id=6 succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 4; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2; ctx.idx[3] = 3;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][2], "silver") == 0, "scenario 3: id=6's given color is 'silver', not NULL");
    }

    /* ── Scenario 4: a mid-batch constraint violation rolls back the WHOLE
     * statement -- all rows from one multi-row INSERT are atomic together,
     * via sql_execute()'s existing autocommit wrapping (no new transaction
     * plumbing was added for this). ────────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT id FROM widgets", &r) == 0, "scenario 4 setup: count widgets before the failing batch");
    uint32_t count_before;
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        count_before = ctx.count;
    }
    CHECK(sql_execute(1, "INSERT INTO widgets (id, name) VALUES "
                        "(100, 'valid_one'), "
                        "(101, 'sprocket'), "   /* duplicate name -- violates UNIQUE(widgets.name) */
                        "(102, 'valid_two')", &r) != 0 && r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 4: the batch as a whole fails on the UNIQUE violation in row 2");
    CHECK(sql_execute(1, "SELECT id FROM widgets", &r) == 0, "scenario 4: count widgets after the failed batch");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == count_before, "scenario 4: row count is UNCHANGED -- row 1 ('valid_one') was rolled back too, not left behind");
    }
    CHECK(sql_execute(1, "SELECT id FROM widgets WHERE id = 100", &r) == 0, "scenario 4: re-select for id=100");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 0, "scenario 4: id=100 ('valid_one') is genuinely absent, confirming real rollback not just a reported error");
    }

    /* ── Scenario 5: explicit-transaction multi-row INSERT + ROLLBACK --
     * confirms MVCC treats every row from one statement as atomic together
     * even under caller-managed transactions (sql_execute_tx), not just
     * the autocommit path. ────────────────────────────────────────────────── */
    {
        uint64_t txn_id = mvcc_begin();
        CHECK(txn_id != 0, "scenario 5: explicit transaction begins");
        struct SqlResult tr;
        CHECK(sql_execute_tx(txn_id, 1, "INSERT INTO widgets (id, name) VALUES "
                             "(200, 'tx_row_a'), (201, 'tx_row_b'), (202, 'tx_row_c')", &tr) == 0
              && tr.error == SQL_ERR_NONE, "scenario 5: 3-row INSERT succeeds inside the open transaction");
        CHECK(tr.affected_rows == 3, "scenario 5: affected_rows reports all 3 rows pre-rollback");
        CHECK(sql_tx_rollback(txn_id, 1) == 0, "scenario 5: explicit ROLLBACK succeeds");
    }
    CHECK(sql_execute(1, "SELECT id FROM widgets WHERE id = 200", &r) == 0, "scenario 5: re-select for id=200 after rollback");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 0, "scenario 5: id=200 is genuinely absent -- the whole 3-row batch was rolled back together");
    }
    CHECK(sql_execute(1, "SELECT id FROM widgets WHERE id = 202", &r) == 0, "scenario 5: re-select for id=202 after rollback");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 0, "scenario 5: id=202 (the LAST row of the rolled-back batch) is also genuinely absent");
    }

    /* ── Scenario 6: explicit-transaction multi-row INSERT + COMMIT --
     * confirms the happy path of the same caller-managed-transaction flow
     * really persists. ─────────────────────────────────────────────────────── */
    {
        uint64_t txn_id = mvcc_begin();
        struct SqlResult tr;
        CHECK(sql_execute_tx(txn_id, 1, "INSERT INTO widgets (id, name) VALUES (300, 'committed_a'), (301, 'committed_b')", &tr) == 0
              && tr.error == SQL_ERR_NONE, "scenario 6: 2-row INSERT succeeds inside the open transaction");
        CHECK(sql_tx_commit(txn_id) == 0, "scenario 6: explicit COMMIT succeeds");
    }
    CHECK(sql_execute(1, "SELECT id FROM widgets WHERE id = 301", &r) == 0, "scenario 6: re-select for id=301 after commit");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 1, "scenario 6: id=301 really persisted after commit");
    }

    /* ── Scenario 7: parser-level edge cases. ───────────────────────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("INSERT INTO widgets (id, name) VALUES (1,'a'),(2,'b'),(3,'c')", &stmt, err, sizeof(err)) == 0
              && stmt.u.insert.extra_row_count == 2, "scenario 7: 3-tuple INSERT parses with extra_row_count == 2");
        CHECK(strcmp(stmt.u.insert.extra_values[0][0], "2") == 0 && strcmp(stmt.u.insert.extra_values[0][1], "b") == 0,
              "scenario 7: extra_values[0] (tuple 2) captured correctly");
        CHECK(strcmp(stmt.u.insert.extra_values[1][0], "3") == 0 && strcmp(stmt.u.insert.extra_values[1][1], "c") == 0,
              "scenario 7: extra_values[1] (tuple 3) captured correctly");
    }
    {
        /* mismatched arity in a LATER tuple is still a parse error, even
         * though the first tuple was well-formed. */
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("INSERT INTO widgets (id, name) VALUES (1,'a'),(2)", &stmt, err, sizeof(err)) != 0,
              "scenario 7: a later tuple with the wrong arity fails to parse");
    }
    {
        /* naming MORE columns than the table has is still a real exec-time
         * error (Phase 6 only relaxed "fewer", never "more"). */
        struct SqlResult rr;
        CHECK(sql_execute(1, "INSERT INTO widgets (id, name, color, weight, bogus) VALUES (999, 'x', 'y', 'z', 'w')", &rr) != 0
              && rr.error == SQL_ERR_COLUMN_COUNT_MISMATCH, "scenario 7: naming more columns than the table has is still rejected");
    }
    {
        /* naming exactly as many columns as the table has, but with one
         * misspelled/nonexistent name, is still rejected as a distinct
         * error (a same-count-but-wrong-name case, not the "too many"
         * case above). */
        struct SqlResult rr;
        CHECK(sql_execute(1, "INSERT INTO widgets (id, name, color, bogus) VALUES (998, 'x', 'y', 'z')", &rr) != 0
              && rr.error == SQL_ERR_COLUMN_NOT_FOUND, "scenario 7: a nonexistent column name (right count) is still rejected");
    }
    {
        /* single-row INSERT (row 0 only, extra_row_count == 0) is still
         * byte-for-byte parseable/executable exactly as before Phase 6. */
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("INSERT INTO widgets (id, name) VALUES (500, 'solo')", &stmt, err, sizeof(err)) == 0
              && stmt.u.insert.extra_row_count == 0, "scenario 7: an ordinary single-row INSERT still has extra_row_count == 0");
    }

    /* ── Scenario 8: permission denial makes a multi-row INSERT fail
     * cleanly and inserts nothing, not a crash or a partial write. ─────────── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "INSERT INTO widgets (id, name) VALUES (600, 'denied_a'), (601, 'denied_b')", &r) != 0
          && r.error == SQL_ERR_PERMISSION_DENIED, "scenario 8: permission denial makes the multi-row INSERT fail cleanly");
    g_access_force_deny = 0;
    CHECK(sql_execute(1, "SELECT id FROM widgets WHERE id = 600", &r) == 0, "scenario 8: re-select for id=600");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 0;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        cursor_close(r.cursor_id);
        CHECK(ctx.count == 0, "scenario 8: id=600 is genuinely absent -- permission denial didn't leave a partial row behind");
    }

    if (g_fail == 0) printf("\nALL CHECKS PASSED\n");
    else              printf("\nSOME CHECKS FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
