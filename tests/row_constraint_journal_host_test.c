/*
 * row_constraint_journal_host_test.c — Phase 23 (relational layer)
 * verification: a standalone host-buildable test proving row_constraint.c
 * (UNIQUE/NOT NULL/RANGE/REFERENCE enforcement) and row_journal.c (audit
 * trail) actually work once wired into kernel/mvcc.c -- linked against the
 * REAL, unmodified kernel/sql_exec.c, kernel/sql_parser.c,
 * kernel/predicate.c, kernel/row_index.c, kernel/rowstore.c,
 * kernel/persist.c, kernel/cursor.c, kernel/mvcc.c, kernel/row_constraint.c,
 * AND kernel/row_journal.c -- the full real stack, not a reimplementation.
 *
 * Constraint registration is a direct API call (row_constraint_add_*()),
 * not SQL DDL -- see row_constraint.h's header comment for why (matches
 * row_index_create()'s own precedent). Everything downstream of
 * registration (INSERT/UPDATE/DELETE enforcement, journal recording) is
 * exercised through real SQL text via sql_execute()/sql_execute_tx(), the
 * same "real stack, not direct mvcc.c calls" posture sql_tx_host_test.c
 * already established for Phase 22.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/row_constraint_journal_host_test tests/row_constraint_journal_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c
 *   /tmp/row_constraint_journal_host_test
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
#include "kernel/row_journal.h"
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

/* Gap Remediation Phase D: persist.c's new restore blocks 9-10 reference
 * vecstore.c/vec_index.c's own globals/functions, neither of which is
 * linked here (this suite is scoped to the RDBMS layer) -- stubbed purely
 * to satisfy the linker, matching rowstore_host_test.c's own identical
 * precedent; both restore blocks correctly no-op at "no snapshot" before
 * ever touching this content. */
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
    return 1;   // this test isn't exercising permission denial (mvcc_host_test.c/sql_exec_host_test.c already do)
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

struct collect_ctx { char values[64][64]; uint32_t count; uint32_t col; };
static void collect_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collect_ctx* ctx = (struct collect_ctx*)ctxp;
    if (ctx->count < 64) { strncpy(ctx->values[ctx->count], v->values[ctx->col], 63); ctx->count++; }
}

// Two tables: accounts(id, name, tag) and orders(id, account_id, amount) --
// orders.account_id REFERENCES accounts.id.
static void make_tables(void) {
    // Both catalog entries must be visible via object_catalog_count BEFORE
    // either rowstore_create_table() call, since it (like row_constraint.c's
    // own rc_find_table_index()) only scans object_catalog[0..count).
    object_catalog_count = 2;

    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "accounts");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xA101;
    object_catalog[0].active = 1;

    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");    object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "name");  object_schemas[0].fields[1].type = FIELD_TYPE_STRING; object_schemas[0].fields[1].active = 1;
    strcpy(object_schemas[0].fields[2].key, "tag");   object_schemas[0].fields[2].type = FIELD_TYPE_STRING; object_schemas[0].fields[2].active = 1;
    object_schemas[0].field_count = 3;
    rowstore_create_table("accounts");

    memset(&object_catalog[1], 0, sizeof(object_catalog[1]));
    strcpy(object_catalog[1].name, "orders");
    object_catalog[1].type = OBJ_TYPE_DB_TABLE;
    object_catalog[1].object_id = 0xA102;
    object_catalog[1].active = 1;

    memset(&object_schemas[1], 0, sizeof(object_schemas[1]));
    strcpy(object_schemas[1].fields[0].key, "id");         object_schemas[1].fields[0].type = FIELD_TYPE_UINT64; object_schemas[1].fields[0].active = 1;
    strcpy(object_schemas[1].fields[1].key, "account_id"); object_schemas[1].fields[1].type = FIELD_TYPE_UINT64; object_schemas[1].fields[1].active = 1;
    strcpy(object_schemas[1].fields[2].key, "amount");     object_schemas[1].fields[2].type = FIELD_TYPE_STRING; object_schemas[1].fields[2].active = 1;
    object_schemas[1].field_count = 3;
    rowstore_create_table("orders");
}

// Fetches one column's value for the (single) row matching an id WHERE
// clause via a fresh autocommit SELECT -- mirrors sql_tx_host_test.c's own
// select_one() helper, including its "column projection is metadata-only"
// gotcha (col index is the table-definition order, not the SELECT list).
static void select_col(const char* table, const char* where_val, uint32_t col, char* out, uint32_t outmax) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE id = %s", table, where_val);
    struct SqlResult r;
    sql_execute(1, sql, &r);
    struct collect_ctx ctx = { {{0}}, 0, col };
    cursor_fetch_rows(r.cursor_id, 10, collect_cb, &ctx);
    cursor_close(r.cursor_id);
    if (ctx.count > 0) strncpy(out, ctx.values[0], outmax - 1);
    else out[0] = '\0';
}

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    row_constraint_init();
    row_journal_init();
    make_tables();

    struct SqlResult r;

    /* ── Registration ─────────────────────────────────────────────────────── */
    CHECK(row_constraint_add_unique("accounts", "name") == ROW_CONSTRAINT_OK, "reg: UNIQUE(accounts.name) registers");
    CHECK(row_constraint_add_not_null("accounts", "tag") == ROW_CONSTRAINT_OK, "reg: NOT NULL(accounts.tag) registers");
    CHECK(row_constraint_add_range("accounts", "id", "1", "1000") == ROW_CONSTRAINT_OK, "reg: RANGE(accounts.id, 1..1000) registers");
    CHECK(row_constraint_add_reference("orders", "account_id", "accounts", "id") == ROW_CONSTRAINT_OK,
          "reg: REFERENCE(orders.account_id -> accounts.id) registers");
    CHECK(row_constraint_add_unique("nosuchtable", "x") == ROW_CONSTRAINT_ERR_TABLE_NOT_FOUND,
          "reg: registering against an unknown table fails cleanly");
    CHECK(row_constraint_add_unique("accounts", "nosuchcol") == ROW_CONSTRAINT_ERR_COLUMN_NOT_FOUND,
          "reg: registering against an unknown column fails cleanly");
    CHECK(row_constraint_add_reference("orders", "account_id", "accounts", "name") == ROW_CONSTRAINT_ERR_TYPE_MISMATCH,
          "reg: REFERENCE with mismatched column types (uint64 -> string) fails cleanly");
    CHECK(row_constraint_add_range("accounts", "id", "not-a-number", "1000") == ROW_CONSTRAINT_ERR_RANGE_INVALID,
          "reg: RANGE with unparseable bounds fails cleanly");

    row_journal_attach("acct_journal", "accounts");
    row_journal_attach("order_journal", "orders");

    /* ── Scenario 1: baseline INSERT succeeds and is enforced-clean. ───────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (1, 'alice', 'x')", &r) == 0,
          "s1: baseline INSERT (satisfies every constraint) succeeds");

    /* ── Scenario 2: UNIQUE violation on INSERT. ────────────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (2, 'alice', 'y')", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "s2: INSERT with a duplicate name violates UNIQUE and is rejected");
    CHECK(sql_execute(1, "SELECT * FROM accounts WHERE id = 2", &r) == 0 && r.row_count == 0,
          "s2: the rejected INSERT left no partial row -- autocommit rolled back cleanly");

    /* ── Scenario 3: NOT NULL violation on INSERT (column present, value
     * NULL -- INSERT still requires every column, per sql_exec.c's own
     * no-partial-row-support rule; NOT NULL is about the VALUE being a real
     * SQL NULL). SQL Feature-Parity Roadmap Phase 4 note: before Phase 4,
     * this codebase had no real NULL representation, so NOT NULL was
     * checked via `strlen(val) == 0` -- an empty STRING and NULL were
     * indistinguishable, and this scenario used an empty-string tag as the
     * closest available stand-in for "NULL". Phase 4 added a real NULL
     * literal and a real null_mask, so this scenario now uses the actual
     * NULL keyword, and a companion check right after confirms the
     * corrected behavior: a real empty STRING is no longer treated as a
     * NOT NULL violation (see rowstore.h's Phase 4 note -- a genuine,
     * intentional behavior fix, not a regression). ─────────────────────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (3, 'bob', NULL)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "s3: INSERT with a NULL tag violates NOT NULL and is rejected");
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (4, 'bobby', '')", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "s3b: Phase 4 correction -- INSERT with a real empty-STRING tag ('') is NOT a NOT NULL "
          "violation (only an actual NULL is), unlike the pre-Phase-4 strlen==0 convention");

    /* ── Scenario 4: RANGE violation on INSERT (id outside 1..1000). ───────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (9999, 'carol', 'z')", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "s4: INSERT with id=9999 violates RANGE(1..1000) and is rejected");

    /* ── Scenario 5: REFERENCE violation on INSERT into the child table. ───── */
    CHECK(sql_execute(1, "INSERT INTO orders (id, account_id, amount) VALUES (100, 999, '50')", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "s5: INSERT into orders with a nonexistent account_id violates REFERENCE and is rejected");
    CHECK(sql_execute(1, "INSERT INTO orders (id, account_id, amount) VALUES (101, 1, '50')", &r) == 0,
          "s5: INSERT into orders with a valid account_id succeeds");

    /* ── Scenario 6: REFERENCED violation on DELETE of the parent row. ─────── */
    CHECK(sql_execute(1, "DELETE FROM accounts WHERE id = 1", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION && r.affected_rows == 0,
          "s6: DELETE of an accounts row still referenced by orders is rejected (RESTRICT)");
    CHECK(sql_execute(1, "SELECT * FROM accounts WHERE id = 1", &r) == 0 && r.row_count == 1,
          "s6: the row still exists after the rejected DELETE");

    /* ── Scenario 7: DELETE succeeds once nothing references the row. ──────── */
    CHECK(sql_execute(1, "DELETE FROM orders WHERE id = 101", &r) == 0 && r.affected_rows == 1,
          "s7: DELETE of the referencing order succeeds");
    CHECK(sql_execute(1, "DELETE FROM accounts WHERE id = 1", &r) == 0 && r.affected_rows == 1,
          "s7: DELETE of the now-unreferenced account succeeds");

    /* ── Scenario 8: UNIQUE on UPDATE, with exclude-self semantics -- a row
     * updated to its OWN current value must not conflict with itself. ────── */
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (10, 'dan', 'a')", &r);
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (11, 'eve', 'b')", &r);
    CHECK(sql_execute(1, "UPDATE accounts SET name = 'eve' WHERE id = 10", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION && r.affected_rows == 0,
          "s8: UPDATE that would duplicate another row's name violates UNIQUE and is rejected");
    CHECK(sql_execute(1, "UPDATE accounts SET name = 'dan' WHERE id = 10", &r) == 0 && r.affected_rows == 1,
          "s8: UPDATE that sets a row to its OWN existing value does NOT self-conflict on UNIQUE");
    {
        char name[32];
        select_col("accounts", "10", 1, name, sizeof(name));
        CHECK(strcmp(name, "dan") == 0, "s8: row 10's name is unchanged after the self-update");
    }

    /* ── Scenario 9: statement-level atomicity carries through constraint
     * violations too (not just write conflicts) -- a multi-row UPDATE that
     * violates a constraint on ONE row aborts the WHOLE statement. ────────── */
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (20, 'p', 'batch')", &r);
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (21, 'q', 'batch')", &r);
    CHECK(sql_execute(1, "UPDATE accounts SET name = 'dan' WHERE tag = 'batch'", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION && r.affected_rows == 0,
          "s9: batch UPDATE that would violate UNIQUE on its first matched row aborts the whole statement");
    {
        char n20[32], n21[32];
        select_col("accounts", "20", 1, n20, sizeof(n20));
        select_col("accounts", "21", 1, n21, sizeof(n21));
        CHECK(strcmp(n20, "p") == 0 && strcmp(n21, "q") == 0,
              "s9: neither row 20 nor 21 was partially updated -- true statement atomicity for constraint violations");
    }

    /* ── Scenario 10: journal recording -- a committed INSERT produces a
     * committed PT entry with the right after-image. ──────────────────────── */
    {
        char buf[4096];
        int n = row_journal_to_json("acct_journal", 0, buf, sizeof(buf));
        CHECK(n > 0 && strstr(buf, "\"type\":\"PT\"") != NULL, "s10: journal contains at least one PT (insert) entry");
        CHECK(strstr(buf, "\"committed\":true") != NULL, "s10: autocommitted INSERTs produce committed=true journal entries");
        CHECK(strstr(buf, "dan") != NULL, "s10: the journal's after-image contains real row data (dan)");
    }

    /* ── Scenario 11: journal recording -- a committed UPDATE produces a
     * before-image (UB) AND an after-image (UP) entry pair. ───────────────── */
    {
        uint32_t before_count = row_journal_entry_count;
        sql_execute(1, "UPDATE accounts SET name = 'daniel' WHERE id = 10", &r);
        CHECK(r.error == SQL_ERR_NONE, "s11: the UPDATE itself succeeds");
        CHECK(row_journal_entry_count == before_count + 2,
              "s11: a single UPDATE writes exactly two journal entries (UB + UP), matching journal.c's own before/after-image shape");
        char buf[4096];
        row_journal_to_json("acct_journal", 0, buf, sizeof(buf));
        CHECK(strstr(buf, "\"type\":\"UB\"") != NULL && strstr(buf, "\"type\":\"UP\"") != NULL,
              "s11: both a UB and a UP entry are present in the journal");
    }

    /* ── Scenario 12: journal recording -- a ROLLED BACK transaction's
     * entries are purged, never surfacing as committed. ───────────────────── */
    {
        uint64_t txr = sql_tx_begin();
        CHECK(sql_execute_tx(txr, 1, "INSERT INTO accounts (id, name, tag) VALUES (30, 'rollme', 'r')", &r) == 0,
              "s12: INSERT under an explicit transaction succeeds (pending)");
        char buf_pending[4096];
        row_journal_to_json("acct_journal", 0, buf_pending, sizeof(buf_pending));
        CHECK(strstr(buf_pending, "rollme") != NULL, "s12: the pending entry is visible in the journal before rollback");
        CHECK(sql_tx_rollback(txr, 1) == 0, "s12: rollback succeeds");
        char buf_after[4096];
        row_journal_to_json("acct_journal", 0, buf_after, sizeof(buf_after));
        CHECK(strstr(buf_after, "rollme") == NULL,
              "s12: after rollback, the entry for the never-committed row is gone from the journal entirely");
        CHECK(sql_execute(1, "SELECT * FROM accounts WHERE id = 30", &r) == 0 && r.row_count == 0,
              "s12: and the row itself never really existed either");
    }

    /* ── Scenario 13: an unattached table's mutations are silently NOT
     * journaled (matching journal_write()'s own "not attached" contract) --
     * orders was attached above, so this instead confirms a table that was
     * NEVER attached produces nothing. ─────────────────────────────────────── */
    {
        row_journal_detach("order_journal", "orders");
        uint32_t before_count = row_journal_entry_count;
        sql_execute(1, "INSERT INTO orders (id, account_id, amount) VALUES (200, 10, '75')", &r);
        CHECK(r.error == SQL_ERR_NONE, "s13: the INSERT itself still succeeds once detached");
        CHECK(row_journal_entry_count == before_count,
              "s13: a detached table's mutation writes NO journal entry");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
