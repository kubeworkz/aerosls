/*
 * sql_tx_host_test.c — Phase 22 (relational layer) verification: a
 * standalone host-buildable test proving sql_exec.c's real transactional
 * behavior after being wired through kernel/mvcc.c -- linked against the
 * REAL, unmodified kernel/sql_exec.c, kernel/sql_parser.c,
 * kernel/predicate.c, kernel/row_index.c, kernel/rowstore.c,
 * kernel/persist.c, kernel/cursor.c, AND kernel/mvcc.c -- the full real
 * stack, not a reimplementation.
 *
 * Phase 21's own host test (mvcc_host_test.c) already proved mvcc.c's
 * transaction/conflict/visibility logic correct in isolation. This test
 * proves the THING PHASE 22 ACTUALLY ADDED: that sql_exec.c's autocommit
 * wrapper and explicit sql_tx_begin()/sql_execute_tx()/sql_tx_commit()/
 * sql_tx_rollback() API correctly drive that machinery from real SQL text,
 * not direct mvcc.c calls -- "concurrent transactions" simulated the same
 * interleaved-calls way mvcc_host_test.c already established.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers \
 *       -o /tmp/sql_tx_host_test sql_tx_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c
 *   /tmp/sql_tx_host_test
 *
 * Phase 23 update: mvcc.c now calls into kernel/row_constraint.c/
 * kernel/row_journal.c automatically, so this test's link line now
 * includes both. Neither is initialized/populated here, so every call is a
 * guaranteed no-op -- see row_constraint_journal_host_test.c for the test
 * that actually exercises them.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
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

static void make_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "accounts");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xA101;
    object_catalog[0].active = 1;
    object_catalog_count = 1;

    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");    object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "name");  object_schemas[0].fields[1].type = FIELD_TYPE_STRING; object_schemas[0].fields[1].active = 1;
    strcpy(object_schemas[0].fields[2].key, "tag");   object_schemas[0].fields[2].type = FIELD_TYPE_STRING; object_schemas[0].fields[2].active = 1;
    object_schemas[0].field_count = 3;
    rowstore_create_table("accounts");
}

// Fetches the single-column value of one row via a fresh autocommit SELECT.
static void select_one(const char* where_val, char* out, uint32_t outmax) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT name FROM accounts WHERE id = %s", where_val);
    struct SqlResult r;
    sql_execute(1, sql, &r);
    // Column projection is metadata-only (Phase 19), so the materialized
    // row still carries all 3 columns in table order (id=0, name=1, tag=2)
    // regardless of the SELECT list -- col=1 fetches "name".
    struct collect_ctx ctx = { {{0}}, 0, 1 };
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
    make_table();

    struct SqlResult r;

    /* ── Scenario 1: sql_execute()'s autocommit wrapper actually commits --
     * a fresh sql_execute() call sees a prior one's INSERT. ─────────────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (1, 'alice', 'x')", &r) == 0,
          "s1: autocommit INSERT succeeds");
    CHECK(r.inserted_id.logical_id != 0, "s1: INSERT reports a nonzero logical row id");
    CHECK(sql_execute(1, "SELECT name FROM accounts WHERE id = 1", &r) == 0 && r.row_count == 1,
          "s1: a SEPARATE sql_execute() call sees the committed row -- autocommit really committed");

    /* ── Scenario 2: an autocommit statement that ERRORS leaves no partial
     * effect (rolled back, not just reported as an error). ──────────────── */
    CHECK(sql_execute(1, "INSERT INTO accounts (id, name) VALUES (2, 'incomplete')", &r) == 1 &&
          r.error == SQL_ERR_COLUMN_COUNT_MISMATCH,
          "s2: an invalid INSERT (missing a column) is rejected");
    CHECK(sql_execute(1, "SELECT name FROM accounts WHERE id = 2", &r) == 0 && r.row_count == 0,
          "s2: no partial row exists after the rejected INSERT -- confirms the failed autocommit txn rolled back cleanly");

    /* ── Scenario 3: explicit multi-statement transaction -- two
     * statements against the SAME open transaction, only visible to
     * others after an explicit sql_tx_commit(). ───────────────────────── */
    uint64_t tx1 = sql_tx_begin();
    CHECK(tx1 != 0, "s3: sql_tx_begin() returns a nonzero txn_id");
    CHECK(sql_execute_tx(tx1, 1, "INSERT INTO accounts (id, name, tag) VALUES (3, 'bob', 'y')", &r) == 0,
          "s3: first statement in the explicit transaction succeeds");
    CHECK(sql_execute_tx(tx1, 1, "UPDATE accounts SET name = 'bobby' WHERE id = 3", &r) == 0 && r.affected_rows == 1,
          "s3: second statement in the SAME open transaction succeeds and sees the first statement's own write");
    CHECK(sql_execute(1, "SELECT name FROM accounts WHERE id = 3", &r) == 0 && r.row_count == 0,
          "s3: a SEPARATE autocommit SELECT does NOT see tx1's uncommitted work yet");
    CHECK(sql_tx_commit(tx1) == 0, "s3: sql_tx_commit() succeeds");
    {
        char name[32];
        select_one("3", name, sizeof(name));
        CHECK(strcmp(name, "bobby") == 0, "s3: after commit, a fresh SELECT sees BOTH statements' effects (bobby, not bob)");
    }

    /* ── Scenario 4: explicit transaction rollback discards every
     * statement run against it, not just the last one. ──────────────────── */
    uint64_t tx2 = sql_tx_begin();
    CHECK(sql_execute_tx(tx2, 1, "INSERT INTO accounts (id, name, tag) VALUES (4, 'carol', 'z')", &r) == 0,
          "s4: INSERT under tx2 succeeds");
    CHECK(sql_execute_tx(tx2, 1, "UPDATE accounts SET name = 'caroline' WHERE id = 4", &r) == 0,
          "s4: UPDATE under tx2 succeeds");
    CHECK(sql_tx_rollback(tx2, 1) == 0, "s4: sql_tx_rollback() succeeds");
    CHECK(sql_execute(1, "SELECT name FROM accounts WHERE id = 4", &r) == 0 && r.row_count == 0,
          "s4: after rollback, row 4 never existed as far as anyone else is concerned");

    /* ── Scenario 5: a real SQL-level write-write conflict -- two
     * concurrent explicit transactions racing to UPDATE the same row. ──── */
    uint64_t tx3 = sql_tx_begin();
    uint64_t tx4 = sql_tx_begin();
    CHECK(sql_execute_tx(tx3, 1, "UPDATE accounts SET name = 'alice-by-tx3' WHERE id = 1", &r) == 0,
          "s5: tx3 updates id=1 first");
    CHECK(sql_execute_tx(tx4, 1, "UPDATE accounts SET name = 'alice-by-tx4' WHERE id = 1", &r) == 1 &&
          r.error == SQL_ERR_WRITE_CONFLICT,
          "s5: tx4's concurrent UPDATE on the same row surfaces as SQL_ERR_WRITE_CONFLICT");
    CHECK(r.affected_rows == 0, "s5: the conflicting statement reports 0 affected rows, not a partial success");

    /* ── Scenario 6: the conflict clears once the blocker resolves (via
     * sql_tx_rollback()), matching mvcc.c's own already-proven behavior,
     * now demonstrated end to end through real SQL text. ────────────────── */
    CHECK(sql_tx_rollback(tx3, 1) == 0, "s6: tx3 rolls back");
    CHECK(sql_execute_tx(tx4, 1, "UPDATE accounts SET name = 'alice-by-tx4' WHERE id = 1", &r) == 0,
          "s6: tx4's retry now succeeds");
    CHECK(sql_tx_commit(tx4) == 0, "s6: tx4 commits");
    {
        char name[32];
        select_one("1", name, sizeof(name));
        CHECK(strcmp(name, "alice-by-tx4") == 0, "s6: the committed value is tx4's, confirming tx3's rolled-back write never took effect");
    }

    /* ── Scenario 7: statement-level atomicity for a multi-row UPDATE --
     * a conflict on ONE matched row aborts the WHOLE statement, leaving
     * every OTHER matched row untouched too (no partial update). ────────── */
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (10, 'p', 'batch')", &r);
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (11, 'q', 'batch')", &r);
    sql_execute(1, "INSERT INTO accounts (id, name, tag) VALUES (12, 'r', 'batch')", &r);
    uint64_t tx5 = sql_tx_begin();
    CHECK(sql_execute_tx(tx5, 1, "UPDATE accounts SET name = 'p-locked' WHERE id = 10", &r) == 0,
          "s7: tx5 locks row 10 (leaves it pending, uncommitted)");
    uint64_t tx6 = sql_tx_begin();
    CHECK(sql_execute_tx(tx6, 1, "UPDATE accounts SET name = 'batch-updated' WHERE tag = 'batch'", &r) == 1 &&
          r.error == SQL_ERR_WRITE_CONFLICT && r.affected_rows == 0,
          "s7: tx6's batch UPDATE (matching rows 10,11,12) conflicts on row 10 and aborts the WHOLE statement");
    CHECK(sql_tx_commit(tx6) == 0, "s7: tx6 commits (nothing to commit -- the conflicting statement made no changes)");
    {
        char n11[32], n12[32];
        select_one("11", n11, sizeof(n11));
        select_one("12", n12, sizeof(n12));
        CHECK(strcmp(n11, "q") == 0 && strcmp(n12, "r") == 0,
              "s7: rows 11 and 12 (not directly conflicting) were NOT partially updated -- true statement atomicity");
    }
    sql_tx_rollback(tx5, 1);

    /* ── Scenario 8: error paths. ─────────────────────────────────────────── */
    CHECK(sql_execute_tx(999999999ULL, 1, "SELECT * FROM accounts", &r) == 1 && r.error == SQL_ERR_TXN_NOT_ACTIVE,
          "s8: sql_execute_tx() with a bogus txn_id fails cleanly");
    CHECK(sql_tx_commit(999999999ULL) == 1, "s8: sql_tx_commit() with a bogus txn_id fails cleanly");
    CHECK(sql_tx_rollback(999999999ULL, 1) == 1, "s8: sql_tx_rollback() with a bogus txn_id fails cleanly");
    uint64_t tx7 = sql_tx_begin();
    sql_tx_commit(tx7);
    CHECK(sql_execute_tx(tx7, 1, "SELECT * FROM accounts", &r) == 1 && r.error == SQL_ERR_TXN_NOT_ACTIVE,
          "s8: reusing an already-committed txn_id fails cleanly");

    {
        uint64_t fill[128];
        uint32_t n = 0;
        uint64_t x;
        while ((x = sql_tx_begin()) != 0 && n < 128) fill[n++] = x;
        CHECK(sql_tx_begin() == 0, "s8: MVCC_MAX_TXNS concurrently active transactions -- the next sql_tx_begin() fails cleanly");
        CHECK(sql_execute(1, "SELECT * FROM accounts", &r) == 1 && r.error == SQL_ERR_TXN_UNAVAILABLE,
              "s8: sql_execute()'s own autocommit wrapper reports SQL_ERR_TXN_UNAVAILABLE the same way under exhaustion");
        for (uint32_t i = 0; i < n; i++) sql_tx_rollback(fill[i], 1);
    }
    CHECK(sql_execute(1, "SELECT * FROM accounts WHERE id = 1", &r) == 0 && r.row_count == 1,
          "s8: normal operation resumes once the transaction table is no longer exhausted");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
