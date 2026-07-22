/*
 * database_host_test.c -- AeroSLS Database Namespace & Access Roadmap
 * Phase 1 verification: a standalone host-buildable test for
 * database_create()/database_drop()/database_list()/database_find_id()
 * (kernel/database.c), the lifecycle + catalog-tagging feature scoped in
 * docs/AeroSLS-Database-Namespace-Roadmap-v0.1.md's own Phase 1 section.
 *
 * Links the REAL, unmodified kernel/database.c -- not a reimplementation.
 * Uses vec_index_host_test.c's own lighter scaffold (host-declare
 * object_catalog[]/object_catalog_count directly, stub catalog_get_role()
 * behind a controllable global) rather than sql_schema_export_import_
 * host_test.c's heavier "link the real object_catalog.c" one -- justified
 * for the identical reason vec_schema_export_import_host_test.c already
 * gave: database.c's only dependency on object_catalog.c is the plain
 * array + one access-check function, never the full valloc/persist/fnv1a
 * machinery that heavier scaffold exists to exercise.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/database_host_test \
 *       tests/database_host_test.c kernel/database.c
 *   /tmp/database_host_test
 */
#include "kernel/database.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

// Controllable stub -- defaults to ROLE_APP_USER (an ordinary, non-owner,
// non-kernel caller) so every scenario has to explicitly opt into
// ROLE_SYSTEM_KERNEL to exercise that branch of database_drop()'s gate,
// matching vec_schema_export_import_host_test.c's own g_access_force_deny
// "default to the restrictive case" convention.
static SLSRole g_role_override = ROLE_APP_USER;
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return g_role_override; }

// Database Namespace & Access Roadmap Phase 3: database_check_access()
// (not exercised by this Phase 1 test -- see database_grant_phase3_host_
// test.c for that) calls group_contains_uid() unconditionally inside
// database.c's compiled object file, so it must resolve at link time even
// though this test never reaches it. Pure linkability stub.
int group_contains_uid(const char* name, uint32_t uid) { (void)name; (void)uid; return 0; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static int add_fake_table(const char* name, uint32_t database_id) {
    uint32_t idx = object_catalog_count++;
    memset(&object_catalog[idx], 0, sizeof(object_catalog[idx]));
    strcpy(object_catalog[idx].name, name);
    object_catalog[idx].type        = OBJ_TYPE_DB_TABLE;
    object_catalog[idx].active      = 1;
    object_catalog[idx].database_id = database_id;
    return (int)idx;
}

int main(void) {
    /* ── Scenario 1: basic create + find_id. ─────────────────────────────── */
    int rc = database_create(42, "app");
    CHECK(rc == 0, "scenario 1: database_create('app') succeeds");
    uint32_t app_id = database_find_id("app");
    CHECK(app_id != 0, "scenario 1b: database_find_id('app') resolves to a nonzero id");
    CHECK(app_id == 1, "scenario 1c: the very first database gets id 1 (bump allocator starts at 1)");
    CHECK(databases[0].owner_uid == 42, "scenario 1d: owner_uid is stamped from the caller");

    /* ── Scenario 2: duplicate name rejected, id counter untouched. ──────── */
    uint32_t next_id_before = database_next_id;
    rc = database_create(999, "app");
    CHECK(rc == 1, "scenario 2: creating a duplicate name fails");
    CHECK(database_next_id == next_id_before,
          "scenario 2b: a failed duplicate-name create does not burn a database_id");

    /* ── Scenario 3: bad names rejected. ──────────────────────────────────── */
    CHECK(database_create(1, "") == 1, "scenario 3: empty name is rejected");
    char too_long[DATABASE_NAME_LEN + 8];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(database_create(1, too_long) == 1, "scenario 3b: an over-length name is rejected");

    /* ── Scenario 4: DROP DATABASE refuses when a table is still tagged
     * with it, and leaves the database genuinely intact (not partially
     * dropped). ──────────────────────────────────────────────────────────── */
    CHECK(database_create(7, "reports") == 0, "scenario 4: create 'reports'");
    uint32_t reports_id = database_find_id("reports");
    add_fake_table("sales_2024", reports_id);
    rc = database_drop(7, "reports");
    CHECK(rc == 3, "scenario 4b: drop refuses because 'sales_2024' is still tagged with 'reports'");
    CHECK(database_find_id("reports") == reports_id,
          "scenario 4c: 'reports' is still fully intact after the refused drop (no partial drop)");

    /* ── Scenario 5: permission gate on drop -- non-owner, non-kernel is
     * denied; the actual owner can drop their own database even without
     * ROLE_SYSTEM_KERNEL; a non-owner WITH ROLE_SYSTEM_KERNEL can drop
     * anyone's database. ─────────────────────────────────────────────────── */
    CHECK(database_create(100, "secure") == 0, "scenario 5: create 'secure' owned by uid 100");
    g_role_override = ROLE_APP_USER;
    rc = database_drop(999, "secure");
    CHECK(rc == 2, "scenario 5b: a non-owner, non-kernel caller (uid 999) is denied");
    CHECK(database_find_id("secure") != 0, "scenario 5c: 'secure' still exists after the denied attempt");

    rc = database_drop(100, "secure");
    CHECK(rc == 0, "scenario 5d: the actual owner (uid 100) can drop their own database "
                   "even under ROLE_APP_USER");
    CHECK(database_find_id("secure") == 0, "scenario 5e: 'secure' is genuinely gone");

    CHECK(database_create(100, "secure2") == 0, "scenario 5f: create 'secure2' owned by uid 100");
    g_role_override = ROLE_SYSTEM_KERNEL;
    rc = database_drop(999, "secure2");
    CHECK(rc == 0, "scenario 5g: a non-owner (uid 999) WITH ROLE_SYSTEM_KERNEL can still drop it");
    g_role_override = ROLE_APP_USER;

    /* ── Scenario 6: dropping a nonexistent database fails cleanly. ──────── */
    CHECK(database_drop(1, "does_not_exist") == 1, "scenario 6: dropping a nonexistent database fails cleanly");

    /* ── Scenario 7 (the headline §1.2 behavior): recreating a database
     * under a previously-used name gets a genuinely FRESH id, and a table
     * still tagged with the OLD id is a visible orphan, never silently
     * reattached to the new database. This is the whole reason
     * database_id is a bump counter, not fnv1a(name) -- see database.h's
     * own header comment and the roadmap doc's §1.2. ────────────────────── */
    CHECK(database_create(5, "widgets") == 0, "scenario 7: create 'widgets'");
    uint32_t widgets_id_v1 = database_find_id("widgets");
    int old_table_idx = add_fake_table("widget_inventory", widgets_id_v1);
    // Drop it (no tables tagged with THIS check would block it -- wait,
    // 'widget_inventory' IS tagged with widgets_id_v1, so dropping must
    // first deactivate that fake table to simulate "the table was
    // properly dropped/reassigned first", matching §1.6's own real
    // constraint rather than working around it.
    object_catalog[old_table_idx].active = 0;
    CHECK(database_drop(5, "widgets") == 0, "scenario 7b: drop 'widgets' after its one table is gone");
    // Re-activate the OLD table entry as if it had never been cleaned up
    // -- simulating exactly the "forgot to reassign/clear it" scenario
    // this design decision defends against.
    object_catalog[old_table_idx].active = 1;

    CHECK(database_create(5, "widgets") == 0, "scenario 7c: recreate 'widgets' under the same name");
    uint32_t widgets_id_v2 = database_find_id("widgets");
    CHECK(widgets_id_v2 != widgets_id_v1,
          "scenario 7d: the recreated 'widgets' gets a genuinely DIFFERENT database_id "
          "(bump-allocated, not derived from the name)");
    CHECK(object_catalog[old_table_idx].database_id == widgets_id_v1,
          "scenario 7e: the stale 'widget_inventory' entry still carries the OLD id");
    CHECK(object_catalog[old_table_idx].database_id != widgets_id_v2,
          "scenario 7f: ...and that OLD id does NOT match the new 'widgets' -- a visible, "
          "detectable orphan, never a silent resurrection");

    /* ── Scenario 8: table-full boundary. ─────────────────────────────────── */
    // Databases created so far this run: app, reports, secure2, widgets
    // (secure and the two rejected too-long/duplicate attempts never
    // consumed a permanent slot). Fill the rest of the table to DATABASE_MAX.
    int created_so_far = 0;
    for (int i = 0; i < DATABASE_MAX; i++) if (databases[i].active) created_so_far++;
    int to_fill = DATABASE_MAX - created_so_far;
    int fill_ok = 1;
    for (int i = 0; i < to_fill; i++) {
        char name[DATABASE_NAME_LEN];
        snprintf(name, sizeof(name), "filler_%d", i);
        if (database_create(1, name) != 0) fill_ok = 0;
    }
    CHECK(fill_ok, "scenario 8: filling the database table up to DATABASE_MAX all succeed");
    CHECK(database_create(1, "one_too_many") == 1,
          "scenario 8b: the next create, with the table genuinely full, fails cleanly");

    /* ── Scenario 9: database_find_id() on a name that was never created,
     * and on a name that WAS created but then genuinely dropped (not the
     * orphan-simulation case above -- a real, clean drop). ──────────────── */
    CHECK(database_find_id("never_existed") == 0, "scenario 9: an unknown name resolves to 0");
    CHECK(database_find_id("secure") == 0,
          "scenario 9b: 'secure' (genuinely dropped in scenario 5d) resolves to 0, not its old id");

    database_list();   // smoke check only -- kernel_serial_printf is a stubbed no-op in this
                        // host test, so there's no captured output to assert against; this
                        // just confirms the function runs cleanly over a nontrivial table.

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
