/*
 * sql_database_phase2_host_test.c -- Database Namespace & Access Roadmap
 * Phase 2 verification: a standalone host-buildable test for the new
 * CREATE DATABASE / DROP DATABASE / CREATE TABLE ... IN DATABASE SQL
 * grammar (kernel/sql_parser.c) and its executor wiring (kernel/sql_exec.c)
 * into the REAL, unmodified kernel/database.c (Phase 1) and kernel/
 * object_catalog.c (sys_sls_valloc()'s own database_id copy) -- not a
 * reimplementation of either.
 *
 * Scaffold is sql_ddl_phase5_host_test.c's own real-object_catalog.c +
 * real-persist.c-with-fake-NVMe harness, verbatim, plus kernel/database.c
 * added to the link line -- justified for the identical reason that file
 * gave: CREATE TABLE's IN DATABASE tagging only exists as real, reachable
 * behavior through the genuine sys_sls_valloc() entry point (this phase's
 * own database_id field copy lives inside it), so a stubbed catalog would
 * test nothing about whether the tagging actually round-trips.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_database_phase2_host_test tests/sql_database_phase2_host_test.c \
 *       kernel/object_catalog.c kernel/database.c kernel/rowstore.c kernel/storage_quota.c kernel/row_index.c \
 *       kernel/row_constraint.c kernel/row_journal.c kernel/persist.c kernel/view.c \
 *       kernel/cursor.c kernel/mvcc.c kernel/predicate.c \
 *       kernel/sql_parser.c kernel/sql_exec.c kernel/group_profile.c \
 *       kernel/authlist.c kernel/security_audit.c
 *   /tmp/sql_database_phase2_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/database.h"
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
// constraint.c/row_journal.c expect to exist. object_catalog[]/
// object_records[]/object_schemas[]/role_table[]/object_catalog_count/
// catalog_after_restore() are all REAL, defined inside the real
// object_catalog.c linked below -- must NOT be redefined here (matching
// sql_ddl_phase5_host_test.c's own established convention). ────────────────
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
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
// Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4: see
// legacy_rowstore_boundary_host_test.c's identical stub for the full
// rationale -- a permissive fake satisfies group_profile.c/authlist.c's
// new dependency on tenant.c without linking the real tenant.c.
int tenant_caller_may_administer(uint32_t caller_uid, uint32_t partition_id) { (void)caller_uid; (void)partition_id; return 1; }

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
// test.c/sql_ddl_phase5_host_test.c already established for these exact
// symbols). ──────────────────────────────────────────────────────────────────
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
// sql_ddl_phase5_host_test.c already established. ───────────────────────────
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
// convention). ───────────────────────────────────────────────────────────────
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

    /* ── Scenario 1: CREATE DATABASE / DROP DATABASE via SQL text. ────────── */
    CHECK(sql_execute(0, "CREATE DATABASE app", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 1: CREATE DATABASE app succeeds");
    CHECK(database_find_id("app") != 0, "scenario 1b: 'app' is a real database afterward (database_find_id resolves)");
    CHECK(sql_execute(0, "CREATE DATABASE app", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 1c: CREATE DATABASE with a duplicate name fails cleanly");
    CHECK(sql_execute(0, "DROP DATABASE does_not_exist", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 1d: DROP DATABASE on an unknown name fails cleanly");

    /* ── Scenario 2: CREATE TABLE ... IN DATABASE tags the real catalog
     * entry's database_id -- the whole point of Phase 2's executor wiring
     * (sys_sls_valloc()'s own database_id copy, Phase 1's object_catalog.c
     * change). ──────────────────────────────────────────────────────────────── */
    CHECK(sql_execute(0, "CREATE TABLE widgets (id UINT64, name STRING) IN DATABASE app", &r) == 0 &&
          r.error == SQL_ERR_NONE,
          "scenario 2: CREATE TABLE ... IN DATABASE app succeeds");
    {
        uint32_t app_id = database_find_id("app");
        int tidx = -1;
        for (uint32_t i = 0; i < object_catalog_count; i++)
            if (object_catalog[i].active && strcmp(object_catalog[i].name, "widgets") == 0) { tidx = (int)i; break; }
        CHECK(tidx >= 0, "scenario 2b: 'widgets' exists in the real catalog");
        CHECK(tidx >= 0 && object_catalog[tidx].database_id == app_id,
              "scenario 2c: widgets' real catalog entry.database_id matches app's real database_id");
    }
    /* A table created with no IN DATABASE clause is untagged (database_id
     * 0/NONE) -- purely additive, never a forced default. */
    CHECK(sql_execute(0, "CREATE TABLE untagged (id UINT64)", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 2d: a plain CREATE TABLE with no IN DATABASE still succeeds");
    {
        int tidx = -1;
        for (uint32_t i = 0; i < object_catalog_count; i++)
            if (object_catalog[i].active && strcmp(object_catalog[i].name, "untagged") == 0) { tidx = (int)i; break; }
        CHECK(tidx >= 0 && object_catalog[tidx].database_id == 0,
              "scenario 2e: 'untagged' has database_id 0 (NONE) -- IN DATABASE is purely additive");
    }

    /* ── Scenario 3: IN DATABASE naming a database that doesn't exist fails
     * cleanly at EXEC time (not parse time) -- resolution happens inside
     * exec_create_table(), matching REFERENCES' own posture. Nothing should
     * be left partially created. ─────────────────────────────────────────── */
    CHECK(sql_execute(0, "CREATE TABLE ghost (id UINT64) IN DATABASE nosuchdb", &r) == 1 &&
          r.error == SQL_ERR_DDL_FAILED,
          "scenario 3: IN DATABASE naming a nonexistent database fails at exec time");
    {
        int tidx = -1;
        for (uint32_t i = 0; i < object_catalog_count; i++)
            if (object_catalog[i].active && strcmp(object_catalog[i].name, "ghost") == 0) { tidx = (int)i; break; }
        CHECK(tidx < 0, "scenario 3b: 'ghost' was never created -- no partial catalog entry left behind");
    }
    /* Confirm the parser itself still parses this fine (it's an exec-time
     * failure, not a parse-time one). */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("CREATE TABLE ghost (id UINT64) IN DATABASE nosuchdb", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_TABLE && stmt.u.create_table.has_database == 1 &&
              strcmp(stmt.u.create_table.database_name, "nosuchdb") == 0,
              "scenario 3c: the parser itself parses IN DATABASE <name> cleanly regardless of whether it exists");
    }

    /* ── Scenario 4: DROP DATABASE refuses (via the real SQL path) while
     * 'widgets' is still tagged with 'app', matching database_drop()'s own
     * real, already-tested refusal behavior (Phase 1) -- this just confirms
     * the SQL surface reaches the exact same real function. ──────────────── */
    CHECK(sql_execute(0, "DROP DATABASE app", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 4: DROP DATABASE app is refused while 'widgets' is still tagged with it");
    CHECK(database_find_id("app") != 0, "scenario 4b: 'app' is still fully intact after the refused drop");

    CHECK(sql_execute(0, "DROP TABLE widgets", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 4c: DROP TABLE widgets (clears the tag's only remaining reference)");
    CHECK(sql_execute(0, "DROP DATABASE app", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 4d: DROP DATABASE app now succeeds once no table references it");
    CHECK(database_find_id("app") == 0, "scenario 4e: 'app' is genuinely gone");

    /* ── Scenario 5: parser-level checks. ──────────────────────────────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("CREATE DATABASE analytics", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_CREATE_DATABASE &&
              strcmp(stmt.u.create_database.database_name, "analytics") == 0,
              "scenario 5: a plain CREATE DATABASE parses with the right name");
        CHECK(sql_parse("DROP DATABASE analytics", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_DROP_DATABASE &&
              strcmp(stmt.u.drop_database.database_name, "analytics") == 0,
              "scenario 5b: a plain DROP DATABASE parses with the right name");
        CHECK(sql_parse("CREATE DATABASE", &stmt, err, sizeof(err)) == 1,
              "scenario 5c: CREATE DATABASE with no name fails to parse");
        CHECK(sql_parse("DROP DATABASE", &stmt, err, sizeof(err)) == 1,
              "scenario 5d: DROP DATABASE with no name fails to parse");
        CHECK(sql_parse("CREATE TABLE t (a UINT64) IN DATABASE", &stmt, err, sizeof(err)) == 1,
              "scenario 5e: IN DATABASE with no name fails to parse");
        CHECK(sql_parse("CREATE TABLE t (a UINT64) IN nosuchkw", &stmt, err, sizeof(err)) == 1,
              "scenario 5f: IN not followed by DATABASE fails to parse (IN reused, but not standalone)");
        CHECK(sql_parse("CREATE TABLE t (a UINT64)", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.has_database == 0,
              "scenario 5g: a plain CREATE TABLE with no IN DATABASE clause parses with has_database == 0");
    }

    /* ── Scenario 6 (Cascading phase): DROP DATABASE ... CASCADE, end to
     * end through the real SQL path -- the §1.6 "no CASCADE in v1"
     * limitation, closed. ─────────────────────────────────────────────────── */
    CHECK(sql_execute(0, "CREATE DATABASE crm", &r) == 0, "scenario 6: CREATE DATABASE crm succeeds");
    CHECK(sql_execute(0, "CREATE TABLE leads (id UINT64) IN DATABASE crm", &r) == 0,
          "scenario 6b: first tagged table created");
    CHECK(sql_execute(0, "CREATE TABLE deals (id UINT64) IN DATABASE crm", &r) == 0,
          "scenario 6c: second tagged table created");
    CHECK(sql_execute(0, "DROP DATABASE crm", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 6d: a plain DROP DATABASE still refuses while tables are tagged -- the pre-cascading default is unchanged");
    CHECK(sql_execute(0, "DROP DATABASE crm CASCADE", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 6e: DROP DATABASE crm CASCADE succeeds where the plain drop refused");
    CHECK(database_find_id("crm") == 0, "scenario 6f: 'crm' is genuinely gone");
    CHECK(sql_execute(0, "SELECT * FROM leads", &r) == 1,
          "scenario 6g: 'leads' was genuinely dropped by the cascade, not just detached");
    CHECK(sql_execute(0, "SELECT * FROM deals", &r) == 1,
          "scenario 6h: 'deals' was genuinely dropped by the cascade too");
    CHECK(sql_execute(0, "CREATE TABLE leads (id UINT64)", &r) == 0,
          "scenario 6i: the dropped table's name is reusable -- the cascade went through the real DROP TABLE path (catalog slot freed, mvcc ghost-row fix included)");

    /* ── Scenario 7 (Cascading phase): parser-level checks for the new
     * grammar -- DROP DATABASE CASCADE and REFERENCES ... ON DELETE. ────────── */
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("DROP DATABASE analytics CASCADE", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_DROP_DATABASE && stmt.u.drop_database.cascade == 1,
              "scenario 7: DROP DATABASE ... CASCADE parses with cascade == 1");
        CHECK(sql_parse("DROP DATABASE analytics", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.drop_database.cascade == 0,
              "scenario 7b: a plain DROP DATABASE parses with cascade == 0 -- purely additive");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id) ON DELETE CASCADE)", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.columns[1].has_reference == 1 &&
              stmt.u.create_table.columns[1].on_delete_action == 1,
              "scenario 7c: REFERENCES ... ON DELETE CASCADE parses (action 1)");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id) ON DELETE SET NULL)", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.columns[1].on_delete_action == 2,
              "scenario 7d: REFERENCES ... ON DELETE SET NULL parses (action 2)");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id) ON DELETE RESTRICT)", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.columns[1].on_delete_action == 0,
              "scenario 7e: REFERENCES ... ON DELETE RESTRICT parses as the explicit default (action 0)");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id))", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.create_table.columns[1].on_delete_action == 0,
              "scenario 7f: REFERENCES with no ON DELETE clause defaults to RESTRICT (action 0) -- pre-cascading statements unchanged");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id) ON DELETE)", &stmt, err, sizeof(err)) == 1,
              "scenario 7g: ON DELETE with no action fails to parse");
        CHECK(sql_parse("CREATE TABLE c (id UINT64, p_id UINT64 REFERENCES p(id) ON UPDATE CASCADE)", &stmt, err, sizeof(err)) == 1,
              "scenario 7h: ON UPDATE is rejected -- only ON DELETE actions are supported, named in the error not silently ignored");
    }

    /* ── Scenario 8 (Database Gap Analysis §2.3): ALTER TABLE ... SET
     * DATABASE -- reassign and untag, end to end through the real SQL
     * path. Closes §1.6's original "empty a database means dropping its
     * tables outright" friction with a real reassignment path. ────────────── */
    CHECK(sql_execute(0, "CREATE DATABASE src", &r) == 0 &&
          sql_execute(0, "CREATE DATABASE dst", &r) == 0,
          "scenario 8: two databases created");
    CHECK(sql_execute(0, "CREATE TABLE movable (id UINT64) IN DATABASE src", &r) == 0,
          "scenario 8b: table created tagged with 'src'");
    CHECK(sql_execute(0, "DROP DATABASE src", &r) == 1,
          "scenario 8c: 'src' can't be dropped while 'movable' is tagged with it");
    CHECK(sql_execute(0, "ALTER TABLE movable SET DATABASE dst", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 8d: ALTER TABLE ... SET DATABASE reassigns the tag");
    CHECK(sql_execute(0, "DROP DATABASE src", &r) == 0,
          "scenario 8e: 'src' drops cleanly now that its only table moved out -- the §1.6 friction, closed");
    {
        uint32_t dst_id = database_find_id("dst");
        int found = 0;
        for (uint32_t i = 0; i < object_catalog_count; i++)
            if (object_catalog[i].active && strcmp(object_catalog[i].name, "movable") == 0 &&
                object_catalog[i].database_id == dst_id) found = 1;
        CHECK(found, "scenario 8f: 'movable' is genuinely tagged with 'dst''s real database_id now");
    }
    /* Access follows the tag live: grant on 'dst', check a bare-GUEST uid
     * gains access to the moved-in table through the real access chain. */
    CHECK(database_grant_uid("dst", 8000, PERM_READ) == 0 &&
          catalog_check_access(8000, "movable", PERM_READ) == 1,
          "scenario 8g: a 'dst' grantee can read the moved-in table -- access follows the tag with no extra step");
    CHECK(sql_execute(0, "ALTER TABLE movable SET DATABASE NULL", &r) == 0 && r.error == SQL_ERR_NONE,
          "scenario 8h: SET DATABASE NULL untags the table (database_id back to 0/NONE)");
    CHECK(catalog_check_access(8000, "movable", PERM_READ) == 0,
          "scenario 8i: the 'dst' grantee loses access the moment the table is untagged -- live in both directions");
    CHECK(sql_execute(0, "DROP DATABASE dst", &r) == 0,
          "scenario 8j: 'dst' drops cleanly after the untag; the table itself lives on, just untagged");
    CHECK(sql_execute(0, "ALTER TABLE movable SET DATABASE nosuchdb", &r) == 1 && r.error == SQL_ERR_DDL_FAILED,
          "scenario 8k: SET DATABASE naming an unknown database fails cleanly, tag unchanged");
    CHECK(sql_execute(0, "ALTER TABLE nosuchtable SET DATABASE NULL", &r) == 1 && r.error == SQL_ERR_TABLE_NOT_FOUND,
          "scenario 8l: SET DATABASE on an unknown table fails cleanly");
    {
        struct SqlStatement stmt;
        char err[SQL_ERR_MSG_LEN];
        CHECK(sql_parse("ALTER TABLE t SET DATABASE analytics", &stmt, err, sizeof(err)) == 0 &&
              stmt.kind == SQL_STMT_ALTER_TABLE && stmt.u.alter_table.alter_kind == 1 &&
              strcmp(stmt.u.alter_table.database_name, "analytics") == 0 &&
              stmt.u.alter_table.database_none == 0,
              "scenario 8m: parser -- SET DATABASE <name> fills alter_kind=1 + name");
        CHECK(sql_parse("ALTER TABLE t SET DATABASE NULL", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.alter_table.alter_kind == 1 && stmt.u.alter_table.database_none == 1,
              "scenario 8n: parser -- SET DATABASE NULL sets database_none");
        CHECK(sql_parse("ALTER TABLE t ADD COLUMN c UINT64", &stmt, err, sizeof(err)) == 0 &&
              stmt.u.alter_table.alter_kind == 0,
              "scenario 8o: parser -- plain ADD COLUMN still parses with alter_kind 0 (zero-default, pre-gap-2.3 statements unchanged)");
        CHECK(sql_parse("ALTER TABLE t SET DATABASE", &stmt, err, sizeof(err)) == 1,
              "scenario 8p: parser -- SET DATABASE with no name fails to parse");
        CHECK(sql_parse("ALTER TABLE t SET COLUMN x", &stmt, err, sizeof(err)) == 1,
              "scenario 8q: parser -- SET followed by anything but DATABASE fails to parse");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
