/*
 * legacy_rowstore_boundary_host_test.c — Phase 24 (relational layer)
 * verification: a standalone host-buildable test proving the new
 * `uses_rowstore` boundary guard added to kernel/object_catalog.c's
 * sys_sls_select/insert/update/delete/schema_set actually works -- linked
 * against the REAL, unmodified kernel/object_catalog.c AND kernel/rowstore.c
 * (real rowstore_create_table()), not a reimplementation.
 *
 * Phase 24's design decision (see docs/AeroSLS-RDBMS-Roadmap-v0.1.md §11's
 * findings addendum): a direct audit confirmed `object_records[]` is a
 * per-object attribute/property bag (sys_sls_insert adds ONE named field to
 * the object's single record; "Key already exists, use update" is its own
 * literal rejection text), not a competing multi-row table implementation --
 * migrating it onto rowstore.c (Option A) would be a category error, and a
 * bridging shim (Option C) was named as the exact "cheap flexibility that
 * gets tangled" pattern this project avoids at every fork. Option B was
 * chosen: row-set/SQL is the exclusive path for anything table-shaped going
 * forward, enforced concretely by rejecting the legacy record API once an
 * object is promoted to row-set storage -- closing a real, confirmed gap
 * (object_catalog.c had ZERO reference to `uses_rowstore` before this
 * phase, confirmed by grep), rather than leaving it a silent, accidental
 * hole where the two paths could write to the same object and diverge.
 *
 * This test proves two things with real execution: (1) the legacy record
 * API is completely UNCHANGED for a normal (non-row-set) object -- the
 * compatibility promise every "new parallel capability, not a migration"
 * decision in this roadmap has made and kept; (2) all five legacy entry
 * points cleanly reject (return code 4, no data touched) once
 * rowstore_create_table() has promoted the object.
 *
 * kernel/object_catalog.c's own dependency graph is heavy (transaction/WAL,
 * locking, indexing, MQTs, constraints, journaling) -- the same "as heavy as
 * Phase 9 found process.c's to be" wall Phase 16's own investigation named.
 * Every prior host test that needed object_catalog.c's data model instead
 * STUBBED catalog_check_access() and never linked the real file. This test
 * inverts that: it links the REAL object_catalog.c (the file actually under
 * test) and stubs its dependencies instead -- tx_get_active/wal_stage/
 * kernel_get_current_thread_id (object_catalog.c's own forward-declared
 * externs), lock_acquire, journal_write, index_on_insert/update/delete,
 * mqt_refresh_for_table, constraint_check_insert/update, persist_catalog/
 * records/schemas/rowstore_headers, tier_notify_access, and
 * partition_get_for_uid (transitively required by object_catalog.c's own
 * REAL catalog_check_access(), which this test no longer stubs since it
 * comes for free with the real file).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/legacy_rowstore_boundary_host_test tests/legacy_rowstore_boundary_host_test.c \
 *       kernel/object_catalog.c kernel/rowstore.c kernel/group_profile.c \
 *       kernel/authlist.c kernel/security_audit.c
 *   /tmp/legacy_rowstore_boundary_host_test
 *
 * Navigator-Parity Gap Roadmap Phase 3 added group_profile.c/authlist.c/
 * security_audit.c as real (not stubbed) dependencies of object_catalog.c's
 * catalog_check_access()/sys_sls_role_set() -- linked here for the same
 * reason partition_get_for_uid() already was (see this header's own comment
 * above): they come for free with the real object_catalog.c under test, and
 * are cheap, self-contained leaf modules, not part of the heavy dependency
 * graph (transaction/WAL, locking, indexing, MQTs, constraints, journaling)
 * this test otherwise stubs around.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/journal.h"
#include "kernel/lock_mgr.h"
#include "kernel/index_mgr.h"
#include "kernel/constraint.h"
#include "kernel/mqt.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ─── Globals kernel/object_catalog.c/rowstore.c expect to exist. NOTE:
// object_catalog[]/object_records[]/object_schemas[]/role_table[]/
// object_catalog_count/catalog_after_restore() are all REAL, already
// defined INSIDE the real object_catalog.c linked below -- unlike every
// other host test in this repo (which stub catalog_check_access() and
// therefore must supply these globals themselves), this test must NOT
// redefine them, or the linker rejects it as a duplicate definition. Only
// the globals object_catalog.c/rowstore.c reference but do NOT themselves
// define are declared here. ─────────────────────────────────────────────────
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];

// Navigator-Parity Gap Roadmap Phase 3: kernel/security_audit.c (linked for
// real, see this file's own header comment) needs kernel_tick_counter --
// same real-definition-not-a-stub convention tests/auth_host_test.c already
// established for this exact global (kernel/timer.c itself isn't linked
// here either, for the same "no LAPIC/IDT arch layer on a host build" reason).
volatile uint64_t kernel_tick_counter = 0;

// ─── kernel_io.h stand-ins ──────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// ─── tier_mgr.c stand-in (object_catalog.c's own forward-declared extern). ──
void tier_notify_access(uint64_t object_id) { (void)object_id; }

// ─── row_index.c stand-ins -- rowstore.c's row CRUD calls these
// automatically (Phase 17), but this test never calls row CRUD; still
// required to resolve at link time since row_index.c itself isn't linked. ──
void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}
void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values, const struct RowValues* new_values,
                             const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)old_values; (void)new_values; (void)layout;
}
void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}

// ─── partition.c stand-in: uid 0 (kernel) short-circuits catalog_check_access()
// before this is ever consulted; every other uid this test uses defaults to
// partition 0, matching partition_get_for_uid()'s own documented default. ──
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }

// ─── transaction.c stand-ins (object_catalog.c's own forward-declared
// externs) -- tx_get_active() always reports "no open transaction" so every
// DML call below takes the direct (auto-commit) path, not the WAL-staging
// one; wal_stage() is therefore never actually reached at runtime but must
// still resolve at link time. ────────────────────────────────────────────────
uint64_t tx_get_active(uint32_t thread_id) { (void)thread_id; return 0; }
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id, const char* key,
                   const char* old_value, const char* new_value) {
    (void)thread_id; (void)object_id; (void)key; (void)old_value; (void)new_value;
    return 0;
}
uint32_t kernel_get_current_thread_id(void) { return 1; }

// ─── vecstore.c stand-in (VectorStore Interface Roadmap Phase 1 -- another
// of object_catalog.c's own forward-declared externs, same convention as
// tx_get_active/wal_stage/kernel_get_current_thread_id above). This test's
// own scenarios never valloc a vector-collection object, so this is a true
// no-op -- but it must still resolve at link time now that sys_sls_vfree()/
// catalog_vfree_partition() call it unconditionally. Not linking the real
// vecstore.c here keeps this test's dependency graph exactly as narrow as
// its own header comment already commits to. ─────────────────────────────
void vecstore_notify_object_freed(const char* collection_name) { (void)collection_name; }

// ─── lock_mgr.c / journal.c / index_mgr.c / mqt.c / constraint.c stand-ins --
// call-tracked where useful, no-op otherwise. ──────────────────────────────
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

// ─── persist.c stand-ins -- this test isn't exercising NVMe persistence
// (row_index_host_test.c/rowstore_host_test.c already do, for rowstore.c's
// own real persist.c integration); no-ops here keep this test's dependency
// graph focused on the one thing it's actually verifying: the boundary
// guard. ────────────────────────────────────────────────────────────────────
void persist_catalog(void) {}
void persist_records(void) {}
void persist_schemas(void) {}
void persist_rowstore_headers(void) {}

// ─── rowstore.c's own remaining deps (row CRUD functions reference these
// even though this test only calls rowstore_create_table(), never row
// CRUD -- still required to resolve at link time). ─────────────────────────
void* allocate_physical_ram_frame(void) { return malloc(4096); }
void* io_sq = (void*)1;
void* io_cq = (void*)1;
int nvme_write_sync(uint64_t lba, const void* buf) { (void)lba; (void)buf; return 1; }
int nvme_read_sync(uint64_t lba, void* buf) { (void)lba; (void)buf; return 1; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    rowstore_init();

    // ── Two catalog objects: "config_blob" stays legacy (never promoted),
    // "accounts" gets promoted to a real row-set table partway through. ────
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "config_blob");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xB001;
    object_catalog[0].active = 1;
    object_catalog[0].owner_uid = 0;

    memset(&object_catalog[1], 0, sizeof(object_catalog[1]));
    strcpy(object_catalog[1].name, "accounts");
    object_catalog[1].type = OBJ_TYPE_DB_TABLE;
    object_catalog[1].object_id = 0xB002;
    object_catalog[1].active = 1;
    object_catalog[1].owner_uid = 0;

    object_catalog_count = 2;

    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "note"); object_schemas[0].fields[0].type = FIELD_TYPE_STRING; object_schemas[0].fields[0].active = 1;
    object_schemas[0].field_count = 1;

    memset(&object_schemas[1], 0, sizeof(object_schemas[1]));
    strcpy(object_schemas[1].fields[0].key, "id");   object_schemas[1].fields[0].type = FIELD_TYPE_UINT64; object_schemas[1].fields[0].active = 1;
    strcpy(object_schemas[1].fields[1].key, "name"); object_schemas[1].fields[1].type = FIELD_TYPE_STRING; object_schemas[1].fields[1].active = 1;
    object_schemas[1].field_count = 2;

    /* ── Scenario 1: the legacy record API works completely unchanged for
     * a normal, never-promoted object -- the compatibility promise. ──────── */
    struct SLSRecordRequest ins = { .name = "config_blob", .key = "note", .value = "hello" };
    CHECK(sys_sls_insert(&ins) == 0, "s1: legacy INSERT on a non-row-set object succeeds");

    struct SLSRecordRequest sel = { .name = "config_blob", .key = "note" };
    CHECK(sys_sls_select(&sel) == 0, "s1: legacy SELECT on a non-row-set object succeeds");

    struct SLSRecordRequest upd = { .name = "config_blob", .key = "note", .value = "updated" };
    CHECK(sys_sls_update(&upd) == 0, "s1: legacy UPDATE on a non-row-set object succeeds");

    struct SLSSchemaRequest sch = { .object_name = "config_blob", .key = "extra", .type = FIELD_TYPE_STRING };
    CHECK(sys_sls_schema_set(&sch) == 0, "s1: legacy SCHEMA SET on a non-row-set object succeeds");

    struct SLSRecordRequest del = { .name = "config_blob", .key = "note" };
    CHECK(sys_sls_delete(&del) == 0, "s1: legacy DELETE on a non-row-set object succeeds");

    // put "note" back for a clean baseline before the promoted-object scenarios
    sys_sls_insert(&ins);

    /* ── Scenario 2: promote "accounts" to a real row-set table via the
     * REAL rowstore_create_table() -- confirms uses_rowstore actually
     * flips, the real signal the new guard checks. ───────────────────────── */
    CHECK(rowstore_create_table("accounts") == 0, "s2: rowstore_create_table() promotes 'accounts' successfully");
    CHECK(object_catalog[1].uses_rowstore == 1, "s2: uses_rowstore flag is now set on the catalog entry");

    /* ── Scenario 3: every legacy entry point now cleanly rejects the
     * promoted object with the new boundary-guard return code (4), and
     * makes no change. ────────────────────────────────────────────────────── */
    struct SLSRecordRequest ins2 = { .name = "accounts", .key = "id", .value = "1" };
    CHECK(sys_sls_insert(&ins2) == 4, "s3: legacy INSERT on the row-set-promoted object is rejected (4)");
    CHECK(object_records[1].field_count == 0, "s3: the rejected INSERT left object_records[1] untouched");

    struct SLSRecordRequest sel2 = { .name = "accounts", .key = "id" };
    CHECK(sys_sls_select(&sel2) == 4, "s3: legacy SELECT on the row-set-promoted object is rejected (4)");

    struct SLSRecordRequest upd2 = { .name = "accounts", .key = "id", .value = "2" };
    CHECK(sys_sls_update(&upd2) == 4, "s3: legacy UPDATE on the row-set-promoted object is rejected (4)");

    struct SLSRecordRequest del2 = { .name = "accounts", .key = "id" };
    CHECK(sys_sls_delete(&del2) == 4, "s3: legacy DELETE on the row-set-promoted object is rejected (4)");

    struct SLSSchemaRequest sch2 = { .object_name = "accounts", .key = "extra", .type = FIELD_TYPE_STRING };
    CHECK(sys_sls_schema_set(&sch2) == 4, "s3: legacy SCHEMA SET on the row-set-promoted object is rejected (4)");
    CHECK(object_schemas[1].field_count == 2, "s3: the rejected SCHEMA SET left object_schemas[1] at its original 2 fields");

    /* ── Scenario 4: the OTHER, still-legacy object is completely
     * unaffected by "accounts" having been promoted -- the guard is
     * per-object, not global. ─────────────────────────────────────────────── */
    struct SLSRecordRequest sel3 = { .name = "config_blob", .key = "note" };
    CHECK(sys_sls_select(&sel3) == 0, "s4: config_blob (never promoted) is still fully usable via the legacy API");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
