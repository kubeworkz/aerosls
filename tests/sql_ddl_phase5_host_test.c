/*
 * sql_ddl_phase5_host_test.c -- SQL Feature-Parity Roadmap Phase 5
 * verification: a standalone host-buildable test for the new
 * CREATE TABLE / ALTER TABLE ADD COLUMN / DROP TABLE / CREATE INDEX /
 * DROP INDEX SQL grammar (kernel/sql_parser.c) and its executor wiring
 * (kernel/sql_exec.c) into the REAL, unmodified kernel/object_catalog.c
 * (sys_sls_valloc/schema_set/vfree), kernel/rowstore.c (rowstore_create_
 * table()/rowstore_drop_table()/rowstore_add_column()), kernel/row_index.c
 * (row_index_create()/row_index_drop()), and kernel/row_constraint.c
 * (row_constraint_add_*()) -- not a reimplementation of any of them.
 *
 * This is the first Phase 5+ host test in this roadmap to link the REAL
 * object_catalog.c rather than stubbing catalog_check_access(): CREATE
 * TABLE's own three-step flow (valloc -> schema_set loop -> rowstore_
 * create_table()) only exists as real, reachable behavior through the
 * genuine sys_sls_valloc()/sys_sls_schema_set() entry points, so a stubbed
 * catalog_check_access() would test nothing about whether CREATE TABLE
 * actually wires those calls correctly. Mirrors legacy_rowstore_boundary_
 * host_test.c's own "link the real object_catalog.c, stub its heavy
 * legacy-KV-path dependency graph (WAL/lock_mgr/legacy journal/legacy
 * index_mgr/mqt/legacy constraint.c/tier_mgr)" pattern, combined with
 * sql_null_phase4_host_test.c's real-fake-NVMe scaffold (needed here
 * because rowstore_drop_table()/rowstore_add_column() genuinely call
 * persist_row_index_defs()/persist_row_constraints()/persist_row_journal()/
 * persist_rowstore_headers(), which this test does NOT stub -- it links
 * the real persist.c so those calls actually round-trip instead of being
 * silently swallowed).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_ddl_phase5_host_test tests/sql_ddl_phase5_host_test.c \
 *       kernel/object_catalog.c kernel/rowstore.c kernel/row_index.c \
 *       kernel/row_constraint.c kernel/row_journal.c kernel/persist.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/predicate.c \
 *       kernel/sql_parser.c kernel/sql_exec.c kernel/group_profile.c \
 *       kernel/authlist.c kernel/security_audit.c
 *   /tmp/sql_ddl_phase5_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/predicate.h"
#include "kernel/sql_parser.h"
#include "kernel/sql_exec.h"
#include "kernel/cursor.h"
#include "kernel/mvcc.h"
#include "kernel/journal.h"
#include "kernel/lock_mgr.h"
#include "kernel/index_mgr.h"
#include "kernel/constraint.h"
#include "kernel/mqt.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ─── Globals kernel/object_catalog.c/rowstore.c/row_index.c/row_
// constraint.c/row_journal.c expect to exist. object_catalog[]/
// object_records[]/object_schemas[]/role_table[]/object_catalog_count/
// catalog_after_restore() are all REAL, defined inside the real
// object_catalog.c linked below -- must NOT be redefined here (matching
// legacy_rowstore_boundary_host_test.c's own established convention). ────
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
volatile uint64_t kernel_tick_counter = 0;   // security_audit.c dependency

// ─── kernel_io.h stand-ins ──────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// ─── tier_mgr.c stand-in ────────────────────────────────────────────────────
void tier_notify_access(uint64_t object_id) { (void)object_id; }

// ─── partition.c stand-in: every uid this test uses defaults to partition 0,
// matching partition_get_for_uid()'s own documented default. ───────────────
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }

// ─── transaction.c stand-ins (object_catalog.c's own forward-declared
// externs) -- tx_get_active() always reports "no open transaction" so every
// DML call takes the direct (auto-commit) path. ─────────────────────────────
uint64_t tx_get_active(uint32_t thread_id) { (void)thread_id; return 0; }
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id, const char* key,
                   const char* old_value, const char* new_value) {
    (void)thread_id; (void)object_id; (void)key; (void)old_value; (void)new_value;
    return 0;
}
uint32_t kernel_get_current_thread_id(void) { return 1; }

// ─── vecstore.c/vec_index.c stand-ins -- sys_sls_vfree() calls
// vecstore_notify_object_freed() unconditionally, and persist.c's own
// write/restore paths reference the rest even though this test never
// creates a vector collection (same convention sql_null_phase4_host_
// test.c already established for these exact symbols). ─────────────────────
void vecstore_notify_object_freed(const char* collection_name) { (void)collection_name; }
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
struct VecIndex            vec_indexes[VEC_INDEX_MAX];
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

// ─── program-loader / legacy-index-manager stand-ins -- persist.c and
// cursor.c reference these arrays even though this test never touches
// programs or the legacy (non-row-set) secondary-index path. ───────────────
struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];
struct SLSIndex       index_store[INDEX_MAX];
uint32_t              index_count = 0;

// ─── lock_mgr.c / legacy journal.c / legacy index_mgr.c / mqt.c / legacy
// constraint.c stand-ins -- the legacy KV path's own dependency graph,
// never exercised by this test (row-set tables only), same convention
// legacy_rowstore_boundary_host_test.c already established. ───────────────
int  lock_acquire(uint64_t tx_id, uint64_t object_id, const char* key, LockType type) {
    (void)tx_id; (void)object_id; (void)key; (void)type; return 0;
}
void journal_write(const char* object_name, const char* key, const char* before_image,
                   const char* after_image, JournalEntryType type, uint64_t tx_id) {
    (void)object_name; (void)key; (void)before_image; (void)after_image; (void)type; (void)tx_id;
}
void index_on_insert(const char* table_name, const char* rec_key, const char* field_name, const char* value) {
    (void)table_name; (void)rec_key; (void)field_name; (void)value;
}
void index_on_update(const char* table_name, const char* rec_key, const char* field_name,
                     const char* old_value, const char* new_value) {
    (void)table_name; (void)rec_key; (void)field_name; (void)old_value; (void)new_value;
}
void index_on_delete(const char* table_name, const char* rec_key) { (void)table_name; (void)rec_key; }
void mqt_refresh_for_table(const char* table_name) { (void)table_name; }
int  constraint_check_insert(const char* table_name, const char* key, const char* value) {
    (void)table_name; (void)key; (void)value; return 0;
}
int  constraint_check_update(const char* table_name, const char* key,
                             const char* old_value, const char* new_value) {
    (void)table_name; (void)key; (void)old_value; (void)new_value; return 0;
}

// ─── Real fake-NVMe backing store (sql_null_phase4_host_test.c's own
// convention) -- persist.c is linked for REAL here (not stubbed), since
// rowstore_drop_table()/rowstore_add_column()/row_index_create()/_drop()
// genuinely call persist_row_index_defs()/persist_row_constraints()/
// persist_row_journal()/persist_rowstore_headers()/persist_catalog()/
// persist_schemas() and this test wants those calls to actually round-trip
// through a backing store rather than being no-op'd away. ──────────────────
void* allocate_physical_ram_frame(void) { return malloc(4096); }
#define FAKE_NVME_MAX_FRAMES 1024
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

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    row_constraint_init();
    row_journal_init();

    struct SqlResult r;

    /* ── Scenario 1: CREATE TABLE via SQL text, with inline NOT NULL/
     * UNIQUE/REFERENCES constraints, run as caller_uid=0 (kernel -- always
     * passes catalog_check_access(), matching every other DDL-adjacent
     * host test's "who owns the fixture" convention). ─────────────────────── */
    CHECK(sql_execute(0, "CREATE TABLE departments (id UINT64, title STRING)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 1: CREATE TABLE departments (no constraints) succeeds");
    CHECK(sql_execute(0, "CREATE TABLE people (id UINT64, name STRING NOT NULL, "
                        "nickname STRING UNIQUE, dept_id UINT64 REFERENCES departments(id))", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 1b: CREATE TABLE people with inline NOT NULL/UNIQUE/REFERENCES succeeds");
    CHECK(object_catalog_count >= 2, "scenario 1: both tables exist in the catalog");

    /* CREATE TABLE is indistinguishable from direct-API creation: the table
     * is immediately queryable via SELECT, same as any pre-existing table. */
    CHECK(sql_execute(0, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 1c: INSERT into a SQL-text-created table succeeds");
    CHECK(sql_execute(0, "SELECT title FROM departments WHERE id = 1", &r) == 0 && r.row_count == 1,
          "scenario 1d: SELECT from a SQL-text-created table finds the inserted row");
    cursor_close(r.cursor_id);

    /* ── Scenario 2: the inline constraints registered by CREATE TABLE
     * actually enforce -- proves parsing AND registration both worked, not
     * just that CREATE TABLE "succeeded". ─────────────────────────────────── */
    CHECK(sql_execute(0, "INSERT INTO people (id, name, nickname, dept_id) VALUES (1, NULL, 'al', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 2: inline NOT NULL on people.name rejects a NULL name");
    CHECK(sql_execute(0, "INSERT INTO people (id, name, nickname, dept_id) VALUES (2, 'Alice', 'al', 1)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 2b: first 'al' nickname inserts fine");
    CHECK(sql_execute(0, "INSERT INTO people (id, name, nickname, dept_id) VALUES (3, 'Bob', 'al', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 2c: inline UNIQUE on people.nickname rejects a duplicate 'al'");
    CHECK(sql_execute(0, "INSERT INTO people (id, name, nickname, dept_id) VALUES (4, 'Carol', 'cc', 999)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 2d: inline REFERENCES on people.dept_id rejects a dept_id that doesn't exist");

    /* ── Scenario 3: CREATE INDEX via SQL text; the index actually finds
     * the row (not just "CREATE INDEX succeeded"). ───────────────────────── */
    CHECK(sql_execute(0, "CREATE INDEX idx_people_name ON people (name)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3: CREATE INDEX succeeds");
    {
        struct RowId found[4];
        uint32_t n = row_index_lookup(0, "idx_people_name", "Alice", found, 4);
        CHECK(n == 1, "scenario 3b: the new index finds exactly one match for 'Alice'");
    }

    /* ── Scenario 4: ALTER TABLE ADD COLUMN -- real migration. Existing
     * rows get a real NULL for the new column; the pre-existing index
     * (idx_people_name) is transparently rebuilt and still works after
     * every row's RowId changes underneath it. ────────────────────────────── */
    CHECK(sql_execute(0, "ALTER TABLE people ADD COLUMN nickname_len UINT64", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 4: ALTER TABLE ADD COLUMN succeeds");
    CHECK(sql_execute(0, "SELECT name, nickname_len FROM people WHERE id = 2", &r) == 0,
          "scenario 4b: SELECT including the new column succeeds post-migration");
    {
        /* cursor_fetch_rows() returns the FULL row (all 5 table columns, in
         * table order), never a projected subset -- SqlResult.columns[]/
         * column_count is metadata-only (see sql_exec.h's own comment on
         * struct SqlResult). people's table-column order after the
         * migration is id=0, name=1, nickname=2, dept_id=3, nickname_len=4
         * (appended), regardless of what order the SELECT list named them
         * in -- so name is index 1 and nickname_len is index 4, not 0/1. */
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 1; ctx.idx[1] = 4;   /* name, nickname_len */
        cursor_fetch_rows(r.cursor_id, 100, collectN_cb, &ctx);
        CHECK(ctx.count == 1 && strcmp(ctx.vals[0][0], "Alice") == 0,
              "scenario 4c: Alice's pre-existing name column survived the migration untouched");
        CHECK((ctx.null_mask[0] & (1u << 4)) != 0,
              "scenario 4d: Alice's new nickname_len column is a real NULL, not corrupted/zeroed-and-mistaken-for-0");
        cursor_close(r.cursor_id);
    }
    CHECK(sql_execute(0, "INSERT INTO people (id, name, nickname, dept_id, nickname_len) VALUES (5, 'Dave', 'dd', 1, 2)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 4e: a fresh INSERT can supply a real value for the migrated-in column");
    {
        /* The index built BEFORE the migration must still find rows correctly
         * afterward -- every row's underlying RowId changed during ADD
         * COLUMN's page rewrite, so this proves the rebuild-not-patch logic
         * actually re-pointed the B-tree at the new pages. */
        struct RowId found[4];
        uint32_t n = row_index_lookup(0, "idx_people_name", "Alice", found, 4);
        CHECK(n == 1, "scenario 4f: idx_people_name still finds Alice after the ADD COLUMN migration");
        n = row_index_lookup(0, "idx_people_name", "Dave", found, 4);
        CHECK(n == 1, "scenario 4g: idx_people_name also finds Dave (inserted after the migration)");
    }

    /* ── Scenario 5: DROP INDEX -- the index stops finding anything. ──────── */
    CHECK(sql_execute(0, "DROP INDEX idx_people_name", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 5: DROP INDEX succeeds");
    {
        struct RowId found[4];
        uint32_t n = row_index_lookup(0, "idx_people_name", "Alice", found, 4);
        CHECK(n == 0, "scenario 5b: the dropped index no longer finds anything");
    }

    /* ── Scenario 6: DROP TABLE -- real cleanup, not just a bare catalog
     * flag flip. The table becomes unqueryable, and no row_constraints[]
     * entry is left active pointing at the freed table_object_id. ────────── */
    {
        uint32_t before_active = 0;
        for (uint32_t i = 0; i < row_constraint_count; i++)
            if (row_constraints[i].active) before_active++;
        CHECK(before_active >= 3, "scenario 6: at least 3 constraints are active on people before DROP TABLE "
                                  "(NOT NULL/UNIQUE/REFERENCES from scenario 1b)");

        CHECK(sql_execute(0, "DROP TABLE people", &r) == 0 && r.error == SQL_ERR_NONE,
              "scenario 6b: DROP TABLE people succeeds");
        CHECK(sql_execute(0, "SELECT * FROM people", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND,
              "scenario 6c: SELECT against the dropped table now fails with SQL_ERR_TABLE_NOT_FOUND");

        uint32_t after_active = 0;
        for (uint32_t i = 0; i < row_constraint_count; i++)
            if (row_constraints[i].active) after_active++;
        CHECK(after_active == 0,
              "scenario 6d: every one of people's row_constraints[] entries was deactivated by DROP TABLE "
              "(no dangling constraint left pointing at the freed table_object_id)");
    }

    /* ── Scenario 7: permission denial -- real enforcement, not a stubbed
     * flag. Table owned by uid=10; an unrelated uid=99 (defaults to
     * ROLE_GUEST, no grant, not the owner) is denied write-gated DDL. ─────── */
    CHECK(sql_execute(10, "CREATE TABLE secrets (id UINT64, val STRING)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 7: uid=10 creates 'secrets' (becomes its owner)");
    CHECK(sql_execute(99, "DROP TABLE secrets", &r) == 1 && r.error == SQL_ERR_PERMISSION_DENIED,
          "scenario 7b: uid=99 (a stranger, ROLE_GUEST, no grant) is denied DROP TABLE on secrets");
    CHECK(sql_execute(99, "ALTER TABLE secrets ADD COLUMN extra STRING", &r) == 1 && r.error == SQL_ERR_PERMISSION_DENIED,
          "scenario 7c: uid=99 is denied ALTER TABLE ADD COLUMN on secrets");
    CHECK(sql_execute(99, "CREATE INDEX idx_secrets_val ON secrets (val)", &r) == 1 && r.error == SQL_ERR_PERMISSION_DENIED,
          "scenario 7d: uid=99 is denied CREATE INDEX on secrets");
    CHECK(sql_execute(10, "DROP TABLE secrets", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 7e: uid=10 (the real owner) can still DROP TABLE secrets");

    /* ── Scenario 8: parser-level checks. ──────────────────────────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("CREATE TABLE t (a UINT64, b STRING)", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_TABLE && stmt.u.create_table.column_count == 2,
              "scenario 8: a plain CREATE TABLE parses and reports 2 columns");
        CHECK(sql_parse("CREATE TABLE t (a UINT64 NOT NULL UNIQUE)", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.columns[0].not_null == 1 && stmt.u.create_table.columns[0].is_unique == 1,
              "scenario 8b: a column can carry both NOT NULL and UNIQUE at once, in either order");
        CHECK(sql_parse("CREATE TABLE t (a NOTATYPE)", &stmt, err, sizeof(err)) == 1,
              "scenario 8c: an unrecognized column type fails to parse cleanly");
        CHECK(sql_parse("ALTER TABLE t ADD extra STRING", &stmt, err, sizeof(err)) == 1,
              "scenario 8d: ALTER TABLE without the COLUMN keyword fails to parse (ADD COLUMN required)");
        CHECK(sql_parse("DROP TABLE", &stmt, err, sizeof(err)) == 1,
              "scenario 8e: DROP TABLE with no table name fails to parse");
        CHECK(sql_parse("CREATE INDEX ix ON t (a)", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_INDEX,
              "scenario 8f: a plain CREATE INDEX parses");
        CHECK(sql_parse("CREATE VIEW v AS SELECT 1", &stmt, err, sizeof(err)) == 1,
              "scenario 8g: CREATE VIEW (never implemented, named out-of-scope) fails to parse cleanly");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
