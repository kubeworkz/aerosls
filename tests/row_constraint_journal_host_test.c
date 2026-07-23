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
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
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

// Cascading phase: five more tables for the ON DELETE CASCADE / SET NULL
// scenarios (s14-s17). Fresh tables rather than reusing accounts/orders,
// because orders.account_id already carries a RESTRICT REFERENCE from the
// registration block above and row_constraints[] has no removal API
// (bump-allocated by design) -- a second, differently-actioned constraint
// on the SAME column pair would leave both active, with RESTRICT firing
// first and masking the cascade under test.
static void make_cascade_tables(void) {
    static const struct { const char* name; uint64_t oid;
                          const char* c0; const char* c1; const char* c2; } defs[] = {
        { "customers", 0xA103, "id", "name",        NULL   },
        { "invoices",  0xA104, "id", "customer_id", "memo" },
        { "lines",     0xA105, "id", "invoice_id",  "qty"  },
        { "notes",     0xA106, "id", "customer_id", "body" },
        { "strict",    0xA107, "id", "customer_id", "body" },
    };
    for (int t = 0; t < 5; t++) {
        int i = 2 + t;
        memset(&object_catalog[i], 0, sizeof(object_catalog[i]));
        strcpy(object_catalog[i].name, defs[t].name);
        object_catalog[i].type = OBJ_TYPE_DB_TABLE;
        object_catalog[i].object_id = defs[t].oid;
        object_catalog[i].active = 1;

        memset(&object_schemas[i], 0, sizeof(object_schemas[i]));
        strcpy(object_schemas[i].fields[0].key, defs[t].c0);
        object_schemas[i].fields[0].type = FIELD_TYPE_UINT64; object_schemas[i].fields[0].active = 1;
        strcpy(object_schemas[i].fields[1].key, defs[t].c1);
        object_schemas[i].fields[1].type = defs[t].c2 ? FIELD_TYPE_UINT64 : FIELD_TYPE_STRING;
        object_schemas[i].fields[1].active = 1;
        object_schemas[i].field_count = 2;
        if (defs[t].c2) {
            strcpy(object_schemas[i].fields[2].key, defs[t].c2);
            object_schemas[i].fields[2].type = FIELD_TYPE_STRING; object_schemas[i].fields[2].active = 1;
            object_schemas[i].field_count = 3;
        }
        object_catalog_count = (uint32_t)(i + 1);   // visible BEFORE rowstore_create_table(), same as make_tables()
        rowstore_create_table(defs[t].name);
    }
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

    /* ══ Cascading phase: ON DELETE CASCADE / SET NULL (s14-s17) ═══════════ */
    make_cascade_tables();
    // CURSOR_MAX is only 8 and every SELECT allocates one -- the scenarios
    // above leak a few (harmlessly, they stay under the pool), but these
    // new scenarios run enough SELECT checks to exhaust it, so each one
    // closes its cursor via this helper instead of leaking.
    #define SELECT_COUNT(sql_text, rowcount_out) do { \
        struct SqlResult sr_; \
        sql_execute(1, (sql_text), &sr_); \
        (rowcount_out) = sr_.row_count; \
        cursor_close(sr_.cursor_id); \
    } while (0)
    uint32_t nrows;
    CHECK(row_constraint_add_reference_action("invoices", "customer_id", "customers", "id", ROW_ONDELETE_CASCADE) == ROW_CONSTRAINT_OK,
          "reg2: REFERENCE(invoices.customer_id -> customers.id) ON DELETE CASCADE registers");
    CHECK(row_constraint_add_reference_action("lines", "invoice_id", "invoices", "id", ROW_ONDELETE_CASCADE) == ROW_CONSTRAINT_OK,
          "reg2: REFERENCE(lines.invoice_id -> invoices.id) ON DELETE CASCADE registers (second level of the chain)");
    CHECK(row_constraint_add_reference_action("notes", "customer_id", "customers", "id", ROW_ONDELETE_SET_NULL) == ROW_CONSTRAINT_OK,
          "reg2: REFERENCE(notes.customer_id -> customers.id) ON DELETE SET NULL registers");
    CHECK(row_constraint_add_reference_action("strict", "customer_id", "customers", "id", ROW_ONDELETE_SET_NULL) == ROW_CONSTRAINT_OK,
          "reg2: REFERENCE(strict.customer_id -> customers.id) ON DELETE SET NULL registers");
    CHECK(row_constraint_add_not_null("strict", "customer_id") == ROW_CONSTRAINT_OK,
          "reg2: NOT NULL(strict.customer_id) registers -- the deliberate SET NULL/NOT NULL conflict for s17");

    /* ── Scenario 14: multi-level ON DELETE CASCADE through the REAL
     * delete path -- customers -> invoices -> lines, two levels deep, with
     * an unrelated sibling proving the cascade is scoped, not a table wipe. ── */
    {
        sql_execute(1, "INSERT INTO customers (id, name) VALUES (1, 'acme')", &r);
        sql_execute(1, "INSERT INTO customers (id, name) VALUES (2, 'globex')", &r);
        sql_execute(1, "INSERT INTO invoices (id, customer_id, memo) VALUES (10, 1, 'inv-a')", &r);
        sql_execute(1, "INSERT INTO invoices (id, customer_id, memo) VALUES (11, 1, 'inv-b')", &r);
        sql_execute(1, "INSERT INTO invoices (id, customer_id, memo) VALUES (12, 2, 'inv-c')", &r);
        sql_execute(1, "INSERT INTO lines (id, invoice_id, qty) VALUES (100, 10, '3')", &r);
        CHECK(r.error == SQL_ERR_NONE, "s14: setup rows all inserted");

        CHECK(sql_execute(1, "DELETE FROM customers WHERE id = 1", &r) == 0 && r.affected_rows == 1,
              "s14: DELETE of a customer with CASCADE-referencing invoices SUCCEEDS (pre-cascading this was s6's RESTRICT rejection)");
        SELECT_COUNT("SELECT * FROM invoices WHERE customer_id = 1", nrows);
        CHECK(nrows == 0, "s14: both of customer 1's invoices were cascade-deleted");
        SELECT_COUNT("SELECT * FROM lines WHERE id = 100", nrows);
        CHECK(nrows == 0, "s14: the line under invoice 10 was cascade-deleted too -- the chain really recursed a second level");
        SELECT_COUNT("SELECT * FROM invoices WHERE id = 12", nrows);
        CHECK(nrows == 1, "s14: customer 2's invoice is completely untouched -- the cascade was scoped to the deleted parent's children only");
    }

    /* ── Scenario 15: ON DELETE SET NULL -- the child row survives, its FK
     * column becomes a REAL SQL NULL (null_mask, Phase 4 semantics). ──────── */
    {
        sql_execute(1, "INSERT INTO customers (id, name) VALUES (3, 'initech')", &r);
        sql_execute(1, "INSERT INTO notes (id, customer_id, body) VALUES (20, 3, 'call back')", &r);
        CHECK(sql_execute(1, "DELETE FROM customers WHERE id = 3", &r) == 0 && r.affected_rows == 1,
              "s15: DELETE of a customer with a SET NULL-referencing note succeeds");
        SELECT_COUNT("SELECT * FROM notes WHERE id = 20", nrows);
        CHECK(nrows == 1, "s15: the note itself survives the parent's deletion");
        SELECT_COUNT("SELECT * FROM notes WHERE customer_id IS NULL", nrows);
        CHECK(nrows == 1, "s15: its customer_id is now a real SQL NULL (IS NULL matches -- null_mask, not empty-string)");
        {
            char body[64];
            select_col("notes", "20", 2, body, sizeof(body));
            CHECK(strcmp(body, "call back") == 0, "s15: the note's OTHER columns are untouched by the SET NULL rewrite");
        }
    }

    /* ── Scenario 16: a rolled-back transaction undoes the cascade along
     * with the parent delete -- no separate cascade-undo machinery, just
     * MVCC doing its job on the child mutations made under the same txn. ──── */
    {
        sql_execute(1, "INSERT INTO customers (id, name) VALUES (5, 'hooli')", &r);
        sql_execute(1, "INSERT INTO invoices (id, customer_id, memo) VALUES (50, 5, 'inv-x')", &r);
        uint64_t tx = sql_tx_begin();
        CHECK(sql_execute_tx(tx, 1, "DELETE FROM customers WHERE id = 5", &r) == 0,
              "s16: cascading DELETE succeeds inside an explicit transaction");
        CHECK(sql_execute_tx(tx, 1, "SELECT * FROM invoices WHERE id = 50", &r) == 0 && r.row_count == 0,
              "s16: inside the transaction, the cascade-deleted invoice is already gone (read-your-own-writes)");
        cursor_close(r.cursor_id);
        CHECK(sql_tx_rollback(tx, 1) == 0, "s16: rollback succeeds");
        SELECT_COUNT("SELECT * FROM customers WHERE id = 5", nrows);
        CHECK(nrows == 1, "s16: after rollback the customer is back");
        SELECT_COUNT("SELECT * FROM invoices WHERE id = 50", nrows);
        CHECK(nrows == 1, "s16: and the cascade-deleted invoice is back too -- the cascade rolled back with the transaction");
    }

    /* ── Scenario 17: SET NULL against a NOT NULL FK column fails the whole
     * delete -- standard SQL behavior, obtained for free because the child
     * rewrite goes through the real mvcc_row_update() constraint check. ────── */
    {
        sql_execute(1, "INSERT INTO customers (id, name) VALUES (4, 'umbrella')", &r);
        sql_execute(1, "INSERT INTO strict (id, customer_id, body) VALUES (30, 4, 'pinned')", &r);
        CHECK(r.error == SQL_ERR_NONE, "s17: setup rows inserted (strict.customer_id NOT NULL is satisfied)");
        CHECK(sql_execute(1, "DELETE FROM customers WHERE id = 4", &r) == 1 &&
              r.error == SQL_ERR_CONSTRAINT_VIOLATION && r.affected_rows == 0,
              "s17: DELETE whose SET NULL action would violate the child's NOT NULL is rejected whole");
        SELECT_COUNT("SELECT * FROM customers WHERE id = 4", nrows);
        CHECK(nrows == 1, "s17: the parent row still exists after the rejected delete");
        SELECT_COUNT("SELECT * FROM strict WHERE customer_id IS NULL", nrows);
        CHECK(nrows == 0, "s17: the child row was NOT left nulled-out -- statement atomicity held through the failed cascade");
    }

    /* ── Scenario 18 (Database Gap Analysis §3.1): constraint pool slot
     * reuse. rowstore_drop_table() deactivates a dropped table's
     * constraint entries in place (rowstore.c literally sets active=0 on
     * matching rows -- simulated identically here rather than driving a
     * full DROP TABLE, whose sys_sls_vfree dependency this test stubs);
     * registration must then REUSE that slot instead of bump-allocating
     * past it, or repeated create/drop cycles exhaust the 64-slot pool
     * permanently. Before this fix, rc_add() was bump-only and this
     * scenario's slot/count assertions would both fail. ─────────────────── */
    {
        uint32_t count_before = row_constraint_count;
        CHECK(count_before >= 2, "s18: at least two constraints registered by earlier scenarios (precondition)");

        /* Deactivate slot 1 in place -- byte-for-byte what rowstore_drop_
         * table()'s cleanup loop does to a dropped table's entries. */
        row_constraints[1].active = 0;

        CHECK(row_constraint_add_unique("customers", "name") == ROW_CONSTRAINT_OK,
              "s18: a new constraint registers successfully after a slot was deactivated");
        CHECK(row_constraints[1].active == 1 &&
              row_constraints[1].kind == ROW_CONSTRAINT_UNIQUE &&
              strcmp(row_constraints[1].table_name, "customers") == 0,
              "s18: the new constraint REUSED deactivated slot 1 -- not appended past it");
        CHECK(row_constraint_count == count_before,
              "s18: row_constraint_count (the high-water mark) did not grow -- the pool no longer leaks on drop/re-create cycles");
        CHECK(sql_execute(1, "INSERT INTO customers (id, name) VALUES (60, 'globex')", &r) == 1 &&
              r.error == SQL_ERR_CONSTRAINT_VIOLATION,
              "s18: the slot-reused UNIQUE constraint genuinely enforces ('globex' survives from s14 -- 'acme' itself was cascade-deleted there)");
    }

    /* ── Scenario 19 (Database Gap Analysis §2.8): parent-key UPDATE
     * RESTRICT -- the update-side half of REFERENCE enforcement. State
     * entering this scenario: orders row 200 (from s13) has account_id=10,
     * referencing accounts row id=10 ('daniel'); orders.account_id
     * REFERENCES accounts.id was registered back in the reg block. Before
     * this fix, rewriting accounts.id=10 to another value silently
     * orphaned order 200. ─────────────────────────────────────────────────── */
    {
        SELECT_COUNT("SELECT * FROM orders WHERE account_id = 10", nrows);
        CHECK(nrows == 1, "s19: precondition -- order 200 still references account 10");

        CHECK(sql_execute(1, "UPDATE accounts SET id = 999 WHERE id = 10", &r) == 1 &&
              r.error == SQL_ERR_CONSTRAINT_VIOLATION && r.affected_rows == 0,
              "s19: rewriting a referenced parent key is REJECTED while a child still points at it (the silent-orphan hole, closed)");
        SELECT_COUNT("SELECT * FROM accounts WHERE id = 10", nrows);
        CHECK(nrows == 1, "s19: the parent row is unchanged after the rejected update");

        CHECK(sql_execute(1, "UPDATE accounts SET name = 'danny' WHERE id = 10", &r) == 0 && r.affected_rows == 1,
              "s19: updating a NON-key column of the same referenced parent still succeeds -- no child scan blocks it");

        CHECK(sql_execute(1, "UPDATE accounts SET id = 998 WHERE id = 11", &r) == 0 && r.affected_rows == 1,
              "s19: rewriting an UNREFERENCED row's key succeeds -- the check scans for actual children, not just constraint existence");

        CHECK(sql_execute(1, "DELETE FROM orders WHERE id = 200", &r) == 0 && r.affected_rows == 1,
              "s19: the referencing order is deleted");
        CHECK(sql_execute(1, "UPDATE accounts SET id = 999 WHERE id = 10", &r) == 0 && r.affected_rows == 1,
              "s19: the same parent-key rewrite succeeds once nothing references the old value");

        /* Update-side is RESTRICT for EVERY constraint regardless of its
         * ON DELETE action -- customers id=2 ('globex') has invoice 12
         * under an ON DELETE CASCADE constraint, but there is no ON UPDATE
         * CASCADE: the key rewrite must still be blocked, not cascaded. */
        CHECK(sql_execute(1, "UPDATE customers SET id = 97 WHERE id = 2", &r) == 1 &&
              r.error == SQL_ERR_CONSTRAINT_VIOLATION,
              "s19: a CASCADE-on-delete parent's key rewrite is still RESTRICT-blocked -- ON UPDATE actions deliberately don't exist");
        SELECT_COUNT("SELECT * FROM invoices WHERE id = 12", nrows);
        CHECK(nrows == 1, "s19: and the invoice was neither orphaned nor cascade-touched by the rejected update");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
