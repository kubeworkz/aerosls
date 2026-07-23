/*
 * sql_null_phase4_host_test.c — SQL Feature-Parity Roadmap Phase 4
 * verification: a standalone host-buildable test for kernel/rowstore.c's
 * new per-column null_mask, kernel/predicate.c's IS NULL/IS NOT NULL and
 * NULL-fails-closed evaluation, kernel/row_constraint.c's real-NULL-aware
 * NOT NULL/UNIQUE/RANGE/REFERENCE checks, and kernel/sql_parser.c's NULL
 * literal + IS [NOT] NULL grammar -- linked against the REAL, unmodified
 * full stack: sql_exec.c, sql_parser.c, predicate.c, row_index.c,
 * rowstore.c, persist.c, cursor.c, mvcc.c, row_constraint.c, row_journal.c.
 * Mirrors sql_expr_phase3_host_test.c's own scaffold (same stub globals,
 * same fake NVMe, same CHECK() macro).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_null_phase4_host_test tests/sql_null_phase4_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_null_phase4_host_test
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
    row_constraint_init();

    /* ── Fixture ────────────────────────────────────────────────────────────
     * people(id, name, nickname, bio, dept_id):
     *   name STRING, nickname STRING (UNIQUE), bio BLOB, dept_id UINT64
     * departments(id, title): 1=Engineering
     * NOT NULL is registered on people.name; UNIQUE on people.nickname;
     * REFERENCE people.dept_id -> departments.id. */
    {
        const char* pcols[5] = {"id", "name", "nickname", "bio", "dept_id"};
        SLSFieldType ptypes[5] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_STRING, FIELD_TYPE_BLOB, FIELD_TYPE_UINT64};
        make_table("people", 0xE301, 5, pcols, ptypes);
        const char* dcols[2] = {"id", "title"};
        SLSFieldType dtypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("departments", 0xE302, 2, dcols, dtypes);
    }
    CHECK(row_constraint_add_not_null("people", "name") == ROW_CONSTRAINT_OK, "setup: NOT NULL(people.name) registers");
    CHECK(row_constraint_add_unique("people", "nickname") == ROW_CONSTRAINT_OK, "setup: UNIQUE(people.nickname) registers");
    CHECK(row_constraint_add_reference("people", "dept_id", "departments", "id") == ROW_CONSTRAINT_OK,
          "setup: REFERENCE(people.dept_id -> departments.id) registers");

    struct SqlResult r;
    sql_execute(1, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r);
    CHECK(r.error == SQL_ERR_NONE, "setup: 1 department inserted");

    /* ── Scenario 1: NULL literal in INSERT round-trips as real NULL, not
     * an empty string -- the core Phase 4 guarantee. ─────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (1, 'Alice', 'ali', 'blobdata', 1)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 1: baseline non-NULL row inserts");
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (2, 'Bob', NULL, NULL, NULL)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 1: row with three NULL columns (nickname/bio/dept_id) inserts");
    CHECK(sql_execute(1, "SELECT id, name, nickname, bio FROM people WHERE id = 2", &r) == 0,
          "scenario 1: re-select of the NULL-bearing row succeeds");
    {
        /* full-row layout: 0=id,1=name,2=nickname,3=bio,4=dept_id */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 4; ctx.idx[0] = 0; ctx.idx[1] = 1; ctx.idx[2] = 2; ctx.idx[3] = 3;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][1], "Bob") == 0,
              "scenario 1: Bob's name (non-NULL) is intact");
        CHECK((ctx.null_mask[0] & (1u << 2)) != 0 && (ctx.null_mask[0] & (1u << 3)) != 0,
              "scenario 1: nickname and bio columns report NULL via null_mask");
        CHECK((ctx.null_mask[0] & (1u << 1)) == 0,
              "scenario 1: name column (real value 'Bob') does NOT report NULL");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 2: NOT NULL constraint rejects a real NULL insert. ──────── */
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (3, NULL, 'someone', 'x', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 2: NULL into people.name (NOT NULL) is rejected with SQL_ERR_CONSTRAINT_VIOLATION");

    /* ── Scenario 3: UNIQUE now correctly treats a real empty STRING as
     * comparable (genuine behavior fix), while still exempting real NULLs. */
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (4, 'Carol', '', 'x', 1)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: first empty-string nickname inserts fine");
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (5, 'Dave', '', 'x', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 3: a SECOND empty-string nickname now correctly violates UNIQUE (was silently allowed pre-Phase-4)");
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (6, 'Eve', NULL, 'x', 1)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: a NULL nickname inserts fine even though Bob (row 2) already has a NULL nickname");
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (7, 'Frank', NULL, 'x', 1)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: a THIRD NULL nickname also inserts fine -- multiple NULLs never conflict under UNIQUE");

    /* ── Scenario 4: REFERENCE constraint still exempts a NULL FK. ─────────── */
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (8, 'Grace', 'grace', 'x', NULL)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 4: a NULL dept_id (FK) inserts fine -- not required to reference anything");
    CHECK(sql_execute(1, "INSERT INTO people (id, name, nickname, bio, dept_id) "
                        "VALUES (9, 'Heidi', 'heidi', 'x', 999)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 4: a non-NULL dept_id referencing nothing still violates REFERENCE");

    /* ── Scenario 5: IS NULL / IS NOT NULL in WHERE. ───────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM people WHERE nickname IS NULL", &r) == 0,
          "scenario 5: IS NULL parses and executes");
    CHECK(r.row_count == 3, "scenario 5: exactly 3 people have a NULL nickname (Bob, Eve, Frank)");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM people WHERE nickname IS NOT NULL", &r) == 0 && r.row_count == 3,
          "scenario 5b: IS NOT NULL matches exactly 3 people (Alice 'ali', Carol '', Grace 'grace') -- "
          "Dave/Heidi never made it in (rejected by UNIQUE/REFERENCE above)");
    cursor_close(r.cursor_id);

    /* ── Scenario 6: NULL fails closed in ordinary comparisons/LIKE/arithmetic
     * (never silently coerced to a matchable value). ─────────────────────── */
    CHECK(sql_execute(1, "SELECT name FROM people WHERE nickname = ''", &r) == 0 && r.row_count == 1,
          "scenario 6: nickname = '' matches only Carol (the real empty string), not the NULL rows");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM people WHERE dept_id + 1 = 2", &r) == 0 && r.row_count >= 1,
          "scenario 6b: arithmetic over dept_id succeeds for non-NULL rows");
    cursor_close(r.cursor_id);
    CHECK(sql_execute(1, "SELECT name FROM people WHERE name LIKE 'B%'", &r) == 0 && r.row_count == 1,
          "scenario 6c: LIKE still matches Bob normally (LIKE is on `name`, never NULL here)");
    cursor_close(r.cursor_id);

    /* ── Scenario 7: UPDATE ... SET col = NULL. ────────────────────────────── */
    CHECK(sql_execute(1, "UPDATE people SET nickname = NULL WHERE id = 1", &r) == 0 && r.affected_rows == 1,
          "scenario 7: UPDATE SET nickname = NULL on Alice succeeds");
    CHECK(sql_execute(1, "SELECT nickname FROM people WHERE id = 1", &r) == 0, "scenario 7: re-select succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 2;   /* nickname */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && (ctx.null_mask[0] & (1u << 2)) != 0,
              "scenario 7: Alice's nickname is now really NULL (not just an empty string)");
        cursor_close(r.cursor_id);
    }
    /* Setting a column back to a real value clears the null bit. */
    CHECK(sql_execute(1, "UPDATE people SET nickname = 'alice2' WHERE id = 1", &r) == 0 && r.affected_rows == 1,
          "scenario 7b: UPDATE SET nickname back to a real value succeeds");
    CHECK(sql_execute(1, "SELECT nickname FROM people WHERE id = 1", &r) == 0, "scenario 7b: re-select succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 2;
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && (ctx.null_mask[0] & (1u << 2)) == 0 && strcmp(ctx.vals[0][0], "alice2") == 0,
              "scenario 7b: Alice's nickname is real again ('alice2'), null bit cleared");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 8: BLOB column round-trips like STRING (Phase 4's
     * deliberately narrow BLOB scope). ────────────────────────────────────── */
    CHECK(sql_execute(1, "SELECT bio FROM people WHERE id = 1", &r) == 0, "scenario 8: SELECT a BLOB column succeeds");
    {
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 1; ctx.idx[0] = 3;   /* bio */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "blobdata") == 0,
              "scenario 8: BLOB column round-trips its text content unchanged");
        cursor_close(r.cursor_id);
    }

    /* ── Scenario 9: parser-level checks. ──────────────────────────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("SELECT * FROM people WHERE nickname IS NULL", &stmt, err, sizeof(err)) == 0,
              "scenario 9: IS NULL parses standalone");
        CHECK(sql_parse("SELECT * FROM people WHERE nickname IS NOT NULL", &stmt, err, sizeof(err)) == 0,
              "scenario 9b: IS NOT NULL parses standalone");
        CHECK(sql_parse("SELECT * FROM people WHERE nickname = NULL", &stmt, err, sizeof(err)) == 1,
              "scenario 9c: a bare '= NULL' comparison is NOT given special meaning -- fails to parse "
              "cleanly (named out-of-scope item, use IS NULL instead)");
        CHECK(sql_parse("UPDATE people SET nickname = NULL WHERE id = 1", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.update.set_is_null[0] == 1,
              "scenario 9d: SET col = NULL parses and sets set_is_null[0]");
        CHECK(sql_parse("INSERT INTO people (id, name, nickname, bio, dept_id) VALUES (99, 'X', NULL, 'y', 1)",
                        &stmt, err, sizeof(err)) == 0 && stmt.u.insert.is_null[2] == 1,
              "scenario 9e: INSERT VALUES (..., NULL, ...) parses and sets is_null[2]");
    }

    /* ── Scenario 10: permission denial still propagates cleanly through the
     * new NULL predicate kind. ────────────────────────────────────────────── */
    g_access_force_deny = 1;
    CHECK(sql_execute(1, "SELECT * FROM people WHERE nickname IS NULL", &r) == 0 && r.row_count == 0,
          "scenario 10: permission denial makes an IS NULL WHERE return 0 rows cleanly, not a crash");
    g_access_force_deny = 0;

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
