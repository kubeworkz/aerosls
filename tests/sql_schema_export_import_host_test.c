/*
 * sql_schema_export_import_host_test.c -- SQL Feature-Parity Roadmap
 * Phase 8 (schema import/export, a post-roadmap follow-on) verification:
 * a standalone host-buildable test for sql_schema_export()/sql_schema_
 * import() (kernel/sql_exec.c) linking the REAL, unmodified kernel/
 * object_catalog.c -- same rationale as sql_ddl_phase5_host_test.c: a
 * genuine export->drop->import->verify-recreated round trip only has real
 * effect through the genuine sys_sls_valloc()/sys_sls_schema_set()/
 * rowstore_create_table() chain, so this links the real object_catalog.c
 * (and its real fake-NVMe-backed persist.c) rather than stubbing it.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_schema_export_import_host_test \
 *       tests/sql_schema_export_import_host_test.c \
 *       kernel/object_catalog.c kernel/rowstore.c kernel/row_index.c \
 *       kernel/row_constraint.c kernel/row_journal.c kernel/persist.c kernel/view.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/predicate.c \
 *       kernel/sql_parser.c kernel/sql_exec.c kernel/group_profile.c \
 *       kernel/authlist.c kernel/security_audit.c kernel/database.c
 *   /tmp/sql_schema_export_import_host_test
 *
 * Database Namespace & Access Roadmap Phase 2 update: sql_exec.c's DDL
 * executors now call straight into kernel/database.c, so this test's link
 * line now includes it. No stub needed -- this file already links the real
 * kernel/object_catalog.c, which provides catalog_get_role().
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
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ─── Globals kernel/object_catalog.c/rowstore.c/row_index.c/row_
// constraint.c/row_journal.c expect to exist -- see sql_ddl_phase5_host_
// test.c for the established rationale (must NOT be redefined inside the
// real object_catalog.c linked below). ──────────────────────────────────────
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
volatile uint64_t kernel_tick_counter = 0;

// ─── kernel_io.h stand-ins ──────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// ─── tier_mgr.c stand-in ────────────────────────────────────────────────────
void tier_notify_access(uint64_t object_id) { (void)object_id; }

// ─── partition.c stand-in ───────────────────────────────────────────────────
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }
// Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4: see
// legacy_rowstore_boundary_host_test.c's identical stub for the full
// rationale -- a permissive fake satisfies group_profile.c/authlist.c's
// new dependency on tenant.c without linking the real tenant.c.
int tenant_caller_may_administer(uint32_t caller_uid, uint32_t partition_id) { (void)caller_uid; (void)partition_id; return 1; }

// ─── transaction.c stand-ins ────────────────────────────────────────────────
uint64_t tx_get_active(uint32_t thread_id) { (void)thread_id; return 0; }
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id, const char* key,
                   const char* old_value, const char* new_value) {
    (void)thread_id; (void)object_id; (void)key; (void)old_value; (void)new_value;
    return 0;
}
uint32_t kernel_get_current_thread_id(void) { return 1; }

// ─── vecstore.c/vec_index.c stand-ins ───────────────────────────────────────
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

// ─── program-loader / legacy-index-manager stand-ins ────────────────────────
struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];
struct SLSIndex       index_store[INDEX_MAX];
uint32_t              index_count = 0;

// ─── lock_mgr.c / legacy journal.c / legacy index_mgr.c / mqt.c / legacy
// constraint.c stand-ins ─────────────────────────────────────────────────────
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

// ─── Real fake-NVMe backing store ────────────────────────────────────────────
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

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    row_constraint_init();
    row_journal_init();

    struct SqlResult r;
    static char out[SQL_SCHEMA_EXPORT_MAX_LEN];
    struct SqlSchemaImportResult ir;

    /* ── Fixture: departments/employees with NOT NULL/UNIQUE/REFERENCES
     * inline constraints + one index, all owned by uid=0 (kernel). ────────── */
    CHECK(sql_execute(0, "CREATE TABLE departments (id UINT64, title STRING)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "fixture: CREATE TABLE departments succeeds");
    CHECK(sql_execute(0, "CREATE TABLE employees (id UINT64, name STRING NOT NULL, "
                        "tag STRING UNIQUE, dept_id UINT64 REFERENCES departments(id))", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "fixture: CREATE TABLE employees with inline NOT NULL/UNIQUE/REFERENCES succeeds");
    CHECK(sql_execute(0, "CREATE INDEX idx_employees_name ON employees (name)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "fixture: CREATE INDEX idx_employees_name succeeds");
    CHECK(sql_execute(0, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "fixture: seed row into departments succeeds");
    CHECK(sql_execute(0, "INSERT INTO employees (id, name, tag, dept_id) VALUES (1, 'Alice', 'al', 1)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "fixture: seed row into employees succeeds");

    /* ── Scenario 1: export reconstructs real, re-parseable CREATE TABLE +
     * CREATE INDEX text, including the inline constraints. ────────────────── */
    uint32_t n = sql_schema_export(0, out, sizeof(out));
    CHECK(n > 0, "scenario 1: sql_schema_export() returns a nonzero byte count");
    CHECK(strstr(out, "CREATE TABLE departments") != NULL,
          "scenario 1b: export includes CREATE TABLE departments");
    CHECK(strstr(out, "CREATE TABLE employees") != NULL,
          "scenario 1c: export includes CREATE TABLE employees");
    CHECK(strstr(out, "NOT NULL") != NULL, "scenario 1d: export preserves the inline NOT NULL constraint");
    CHECK(strstr(out, "UNIQUE") != NULL, "scenario 1e: export preserves the inline UNIQUE constraint");
    CHECK(strstr(out, "REFERENCES") != NULL, "scenario 1f: export preserves the inline REFERENCES constraint");
    CHECK(strstr(out, "CREATE INDEX idx_employees_name") != NULL,
          "scenario 1g: export includes CREATE INDEX idx_employees_name");
    {
        /* The actual bytes sql_schema_export() wrote for employees must
         * themselves re-parse cleanly -- proves the reconstructed text isn't
         * just string-plausible (the strstr() checks above) but genuinely
         * valid SQL per this file's own grammar, extracted verbatim from
         * `out` rather than hand-typed. */
        const char* line_start = strstr(out, "CREATE TABLE employees");
        CHECK(line_start != NULL, "scenario 1h: found the employees CREATE TABLE line inside the export text");
        char line_buf[512];
        const char* nl = line_start ? strchr(line_start, '\n') : NULL;
        uint32_t len = (line_start && nl) ? (uint32_t)(nl - line_start) : 0;
        if (line_start && len > 0 && len < sizeof(line_buf)) {
            memcpy(line_buf, line_start, len);
            line_buf[len] = '\0';
        } else {
            line_buf[0] = '\0';
        }
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(line_buf[0] != '\0' &&
              sql_parse(line_buf, &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_TABLE && stmt.u.create_table.column_count == 4,
              "scenario 1i: the verbatim exported employees CREATE TABLE line parses cleanly with all 4 columns");
    }

    /* ── Scenario 2 (rewritten for Database Gap Analysis §2.7): a table
     * with a RANGE constraint now exports it as a REAL, reconstructable
     * CHECK (col BETWEEN lo AND hi) clause -- the old `-- note:` lossy
     * path this scenario originally asserted is gone. The constraint is
     * created via SQL DDL here (the syntax §2.7 added), and the full
     * round trip is proven: export -> drop -> import -> enforcement. ──────── */
    CHECK(sql_execute(0, "CREATE TABLE metrics (id UINT64, val UINT64 CHECK (val BETWEEN 0 AND 100))", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 2: CREATE TABLE with an inline CHECK (BETWEEN) clause succeeds -- RANGE finally has SQL syntax");
    CHECK(sql_execute(0, "INSERT INTO metrics (id, val) VALUES (1, 50)", &r) == 0,
          "scenario 2b: an in-range INSERT passes the CHECK");
    CHECK(sql_execute(0, "INSERT INTO metrics (id, val) VALUES (2, 500)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 2c: an out-of-range INSERT is rejected -- the DDL-registered RANGE genuinely enforces");
    uint32_t n2 = sql_schema_export(0, out, sizeof(out));
    CHECK(n2 > 0, "scenario 2d: export succeeds with a RANGE-constrained table present");
    CHECK(strstr(out, "CHECK (val BETWEEN 0 AND 100)") != NULL,
          "scenario 2e: export emits the real, reconstructable CHECK clause inline");
    CHECK(strstr(out, "RANGE constraint") == NULL,
          "scenario 2f: the old lossy `-- note:` for RANGE is gone -- nothing left unexportable");
    {
        /* Round trip: drop, import the captured export, re-verify
         * enforcement -- the loss §2.7 named, now provably closed. */
        static char captured[SQL_SCHEMA_EXPORT_MAX_LEN];
        strncpy(captured, out, sizeof(captured) - 1);
        captured[sizeof(captured) - 1] = '\0';
        CHECK(sql_execute(0, "DROP TABLE metrics", &r) == 0, "scenario 2g: metrics dropped for the round trip");
        struct SqlSchemaImportResult ir;
        sql_schema_import(0, captured, &ir);
        /* The captured export also contains scenario 1's departments/
         * employees fixtures, which were NOT dropped -- their duplicate
         * CREATEs correctly fail on re-import. What matters here is that
         * metrics' own CHECK-carrying statement succeeded (proven for real
         * by 2i/2j's enforcement below), so assert progress, not
         * zero-failures. */
        CHECK(ir.succeeded >= 1, "scenario 2h: at least metrics' own CREATE (the dropped table) imported successfully");
        CHECK(sql_execute(0, "INSERT INTO metrics (id, val) VALUES (3, 999)", &r) == 1 &&
              r.error == SQL_ERR_CONSTRAINT_VIOLATION,
              "scenario 2i: the re-imported table REJECTS an out-of-range INSERT -- the RANGE constraint survived the round trip");
        CHECK(sql_execute(0, "INSERT INTO metrics (id, val) VALUES (3, 99)", &r) == 0,
              "scenario 2j: and still accepts an in-range one");
    }

    /* ── Scenario 3: real round trip -- export, DROP both fixture tables,
     * import the exported text, verify both tables exist again with the
     * right columns and the index still finds rows. metrics is dropped
     * first so the captured export text contains only departments/
     * employees (metrics' own export behavior was already verified in
     * scenario 2 and would otherwise fail re-import with a duplicate
     * CREATE TABLE, since metrics itself is never dropped here). ──────────── */
    CHECK(sql_execute(0, "DROP TABLE metrics", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3a: DROP TABLE metrics succeeds (no longer needed post-scenario-2)");
    uint32_t n3 = sql_schema_export(0, out, sizeof(out));
    CHECK(n3 > 0, "scenario 3b: captured export text before dropping the fixture tables");
    CHECK(sql_execute(0, "DROP TABLE employees", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3c: DROP TABLE employees succeeds (pre-round-trip cleanup)");
    CHECK(sql_execute(0, "DROP TABLE departments", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 3d: DROP TABLE departments succeeds");
    CHECK(sql_execute(0, "SELECT * FROM departments", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 3e: departments is genuinely gone before import");

    memset(&ir, 0, sizeof(ir));
    sql_schema_import(0, out, &ir);
    CHECK(ir.total == 3, "scenario 3f: import reports exactly 3 statements found "
                          "(2x CREATE TABLE + 1x CREATE INDEX, metrics excluded via scenario 3a's DROP)");
    CHECK(ir.failed == 0, "scenario 3g: every statement in the round-trip import succeeds "
                          "(departments recreated before employees' REFERENCES needs it)");

    CHECK(sql_execute(0, "SELECT title FROM departments", &r) == 0 && r.row_count == 0,
          "scenario 3h: departments is a real, queryable table again post-import (freshly empty, not carrying over old rows)");
    CHECK(sql_execute(0, "INSERT INTO employees (id, name, tag, dept_id) VALUES (1, 'Bob', 'bb', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 3i: employees' re-imported REFERENCES constraint rejects a dept_id "
          "that doesn't exist in the freshly-recreated (now-empty) departments table");
    CHECK(sql_execute(0, "INSERT INTO departments (id, title) VALUES (1, 'Engineering')", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 3j: re-seeding departments succeeds");
    CHECK(sql_execute(0, "INSERT INTO employees (id, name, tag, dept_id) VALUES (1, NULL, 'bb', 1)", &r) == 1 &&
          r.error == SQL_ERR_CONSTRAINT_VIOLATION,
          "scenario 3k: employees' re-imported NOT NULL on name still rejects a NULL name post-round-trip");
    CHECK(sql_execute(0, "INSERT INTO employees (id, name, tag, dept_id) VALUES (1, 'Bob', 'bb', 1)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 3l: a fully valid INSERT into the re-imported employees succeeds");

    /* ── Scenario 4: multi-statement import where one statement is
     * deliberately invalid -- the others still run, and both counts and
     * per-statement detail are accurate. ─────────────────────────────────────── */
    memset(&ir, 0, sizeof(ir));
    sql_schema_import(0,
        "CREATE TABLE t1 (a UINT64, b STRING); "
        "CREATE TABLE t1 BOGUS SYNTAX HERE; "
        "CREATE TABLE t2 (a UINT64, b STRING);",
        &ir);
    CHECK(ir.total == 3, "scenario 4: import reports exactly 3 statements found");
    CHECK(ir.succeeded == 2 && ir.failed == 1,
          "scenario 4b: 2 succeed and 1 fails, matching the deliberately-malformed middle statement");
    CHECK(ir.stmts[0].ok == 1, "scenario 4c: statement 0 (t1) is reported ok");
    CHECK(ir.stmts[1].ok == 0, "scenario 4d: statement 1 (bogus) is reported failed");
    CHECK(ir.stmts[2].ok == 1, "scenario 4e: statement 2 (t2) still ran and is reported ok "
                               "(the earlier failure did not block it)");
    CHECK(sql_execute(0, "SELECT * FROM t1", &r) == 0, "scenario 4f: t1 genuinely exists post-import");
    CHECK(sql_execute(0, "SELECT * FROM t2", &r) == 0, "scenario 4g: t2 genuinely exists post-import");

    /* ── Scenario 5: quote-aware, comment-aware splitting doesn't break on
     * edge cases -- a ';' inside a string literal, and a `-- ` export-style
     * comment line with no trailing ';', both handled correctly. ───────────── */
    memset(&ir, 0, sizeof(ir));
    sql_schema_import(0,
        "CREATE TABLE t3 (a UINT64, b STRING);\n"
        "-- note: this is a comment line with no trailing semicolon\n"
        "INSERT INTO t3 (a, b) VALUES (1, 'has; a semicolon inside');",
        &ir);
    CHECK(ir.total == 2, "scenario 5: the comment line is skipped, not miscounted as a 3rd statement "
                         "(only the CREATE TABLE and the INSERT are real statements)");
    CHECK(ir.succeeded == 2 && ir.failed == 0,
          "scenario 5b: both real statements succeed -- the comment did not get glued onto the INSERT's "
          "text and corrupt it, and the quoted ';' did not split the INSERT in two");
    CHECK(sql_execute(0, "SELECT b FROM t3 WHERE a = 1", &r) == 0 && r.row_count == 1,
          "scenario 5c: the INSERT's quoted value round-tripped intact through the splitter");

    /* ── Scenario 6: permission-gated export -- a caller without PERM_READ
     * on a table has it silently skipped from their own export (not a
     * crash, not an error, not information leakage). ────────────────────────── */
    CHECK(sql_execute(20, "CREATE TABLE private_data (id UINT64, secret STRING)", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 6: uid=20 creates private_data (becomes its owner)");
    uint32_t n6 = sql_schema_export(99, out, sizeof(out));
    CHECK(n6 == 0 || strstr(out, "private_data") == NULL,
          "scenario 6b: uid=99 (a stranger, no PERM_READ grant) does not see private_data in their own export");
    uint32_t n6_owner = sql_schema_export(20, out, sizeof(out));
    CHECK(n6_owner > 0 && strstr(out, "private_data") != NULL,
          "scenario 6c: uid=20 (the real owner) does see private_data in their own export");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
