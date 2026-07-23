/*
 * database_grant_phase3_host_test.c -- Database Namespace & Access Roadmap
 * Phase 3 verification: a standalone host-buildable test proving
 * database-scoped grants (kernel/database.c's database_grant_uid()/
 * database_grant_group()/database_check_access()) are wired as a real,
 * additive extension of kernel/object_catalog.c's catalog_check_access() --
 * linked against the REAL, unmodified versions of both files (plus
 * kernel/group_profile.c/kernel/authlist.c/kernel/security_audit.c, since
 * catalog_check_access() calls straight into all of them), not
 * reimplementations. Same "link the real object_catalog.c, stub its heavy
 * dependencies instead" scaffold security_phase3_host_test.c already
 * established -- this file is that one's direct sibling, proving the new
 * Phase 3 database-grant path the same way that file already proved
 * role/group/authlist.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/database_grant_phase3_host_test tests/database_grant_phase3_host_test.c \
 *       kernel/object_catalog.c kernel/database.c kernel/group_profile.c \
 *       kernel/authlist.c kernel/security_audit.c
 *   /tmp/database_grant_phase3_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "kernel/journal.h"
#include "kernel/lock_mgr.h"
#include "kernel/index_mgr.h"
#include "kernel/constraint.h"
#include "kernel/mqt.h"
#include "kernel/group_profile.h"
#include "kernel/authlist.h"
#include "kernel/database.h"
#include "kernel/security_audit.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ─── Real globals object_catalog.c expects to exist. ───────────────────────
volatile uint64_t kernel_tick_counter = 0;

// ─── kernel_io.h stand-ins ──────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void persist_databases(void) { /* Database Gap Analysis Gap 1 -- database.c now persists after every mutation; no-op here, same as every other persist_* stub in these tests */ }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// ─── tier_mgr.c stand-in ────────────────────────────────────────────────────
void tier_notify_access(uint64_t object_id) { (void)object_id; }

// ─── partition.c stand-in ───────────────────────────────────────────────────
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }

// ─── transaction.c stand-ins ────────────────────────────────────────────────
uint64_t tx_get_active(uint32_t thread_id) { (void)thread_id; return 0; }
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id, const char* key,
                   const char* old_value, const char* new_value) {
    (void)thread_id; (void)object_id; (void)key; (void)old_value; (void)new_value;
    return 0;
}
uint32_t kernel_get_current_thread_id(void) { return 1; }

// ─── vecstore.c stand-in ────────────────────────────────────────────────────
void vecstore_notify_object_freed(const char* collection_name) { (void)collection_name; }

// ─── lock_mgr.c / journal.c / index_mgr.c / mqt.c / constraint.c stand-ins ──
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

// ─── persist.c stand-ins ────────────────────────────────────────────────────
void persist_catalog(void) {}
void persist_records(void) {}
void persist_schemas(void) {}
void persist_rowstore_headers(void) {}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

// Seeds one active DB_TABLE catalog object owned by uid 0 (kernel), with an
// empty perm_mask and a given database_id -- so any non-owner, non-kernel
// caller's access is decided entirely by role/group/authlist/database
// logic, not an accidental perm_mask grant.
static void seed_object(int slot, const char* name, uint32_t database_id) {
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].type        = OBJ_TYPE_DB_TABLE;
    object_catalog[slot].object_id   = 0xD000 + (uint64_t)slot;
    object_catalog[slot].active      = 1;
    object_catalog[slot].owner_uid   = 0;   // kernel-owned -- no uid in this test owns it
    object_catalog[slot].perm_mask   = 0;   // no perm_mask grant at all
    object_catalog[slot].database_id = database_id;
}

int main(void) {
    object_catalog_count = 0;

    /* ── Scenario 1: an object with database_id 0 (NONE/untagged) is
     * completely unaffected by database grants -- database_check_access()
     * itself returns 0 immediately for database_id 0, and no grant can
     * change that (there's nothing to grant access TO). ────────────────────── */
    seed_object(0, "untagged_table", 0);
    object_catalog_count = 1;
    CHECK(database_create(1, "reports") == 0, "s1: database_create('reports') by uid 1 succeeds");
    uint32_t reports_id = database_find_id("reports");
    CHECK(reports_id != 0, "s1: 'reports' resolves to a real database_id");
    CHECK(database_grant_uid("reports", 5000, PERM_READ) == 0,
          "s1: database_grant_uid() grants uid 5000 READ on 'reports'");
    CHECK(catalog_check_access(5000, "untagged_table", PERM_READ) == 0,
          "s1: uid 5000's 'reports' grant does NOT leak onto an untagged (database_id=0) object");

    /* ── Scenario 2: a uid with no individual role, no group, no authlist --
     * a bare GUEST -- gains access to a database_id-tagged table PURELY via
     * the database grant already made in s1 above. This is the headline
     * additive-OR contract Phase 3 exists to prove: the grant was made
     * against the DATABASE, before this specific table even existed, and
     * still applies the moment the table is tagged with that database_id
     * (matching how a real grant should cover every current AND future
     * table assigned to that database, not just ones that existed at grant
     * time). ──────────────────────────────────────────────────────────────────── */
    CHECK(catalog_get_role(5000) == ROLE_GUEST,
          "s2: uid 5000's own individual role is still GUEST -- no role_table[] entry exists for it");
    seed_object(1, "sales_2024", reports_id);
    object_catalog_count = 2;
    CHECK(catalog_check_access(5000, "sales_2024", PERM_READ) == 1,
          "s2: uid 5000 passes on the brand-new 'sales_2024' -- purely via the earlier database "
          "grant on 'reports', role_table[]/group_table[]/authlist_table[] were never touched for "
          "this uid");
    CHECK(catalog_check_access(5000, "sales_2024", PERM_WRITE) == 0,
          "s2: ...but WRITE is still denied -- the grant only covered PERM_READ");

    /* ── Scenario 3: a uid OUTSIDE the grant is still denied -- the grant is
     * scoped to its actual grantees, not a blanket unlock for the database. ── */
    CHECK(catalog_check_access(9999, "sales_2024", PERM_READ) == 0,
          "s3: an unrelated uid (never granted anything) is denied on 'sales_2024'");

    /* ── Scenario 4: grantee GROUPS -- uid 6000 is a member of a GUEST-role
     * group (grants nothing on its own), which is then made a grantee GROUP
     * of the 'reports' database grant. Two levels of indirection: uid ->
     * group -> database grant -> object, mirroring authlist's own grantee-
     * group mechanism exactly. ──────────────────────────────────────────────── */
    CHECK(group_create("finance_readers", ROLE_GUEST) == 1,
          "s4: group_create() for a GUEST-role group (grants nothing by itself)");
    CHECK(group_add_member("finance_readers", 6000) == 1, "s4: uid 6000 joins 'finance_readers'");
    CHECK(catalog_check_access(6000, "sales_2024", PERM_READ) == 0,
          "s4: uid 6000 is still denied -- group membership alone (GUEST role) grants nothing");
    CHECK(database_grant_group("reports", "finance_readers", PERM_READ) == 0,
          "s4: database_grant_group() adds 'finance_readers' as a grantee group of 'reports'");
    CHECK(catalog_check_access(6000, "sales_2024", PERM_READ) == 1,
          "s4: uid 6000 NOW passes -- granted via group membership in a database grantee group");

    /* Because perm_mask is one shared field per database grant (database.h's
     * own documented design), granting the group READ overwrote the earlier
     * uid-5000 grant's mask too -- both share the SAME 'reports' grant
     * entry. Confirm uid 5000 still has READ (unaffected, since the group
     * grant call passed the same PERM_READ). */
    CHECK(catalog_check_access(5000, "sales_2024", PERM_READ) == 1,
          "s4b: uid 5000's own earlier grant is unaffected (same perm_mask value regranted)");

    /* ── Scenario 5: database_grant_uid()/_group() on a nonexistent database
     * name fail cleanly. ─────────────────────────────────────────────────────── */
    CHECK(database_grant_uid("no_such_db", 1, PERM_READ) == 1,
          "s5: database_grant_uid() on an unknown database name fails cleanly (rc=1)");
    CHECK(database_grant_group("no_such_db", "finance_readers", PERM_READ) == 1,
          "s5b: database_grant_group() on an unknown database name fails cleanly (rc=1)");

    /* ── Scenario 6: a truly unauthorized uid (no role, no group, no
     * authlist, no database grant) is still denied, and this denial is
     * logged to the real security audit trail -- confirms the new database
     * check didn't disturb the existing audit-logging behavior at the
     * bottom of catalog_check_access(). ──────────────────────────────────────── */
    uint32_t audit_count_before = security_audit_log_count;
    CHECK(catalog_check_access(12345, "sales_2024", PERM_WRITE) == 0,
          "s6: a totally unrelated uid is denied WRITE on 'sales_2024'");
    CHECK(security_audit_log_count == audit_count_before + 1,
          "s6: this denial was logged to the real security audit trail");

    /* ── Scenario 7 (Database Gap Analysis §2.1): REVOKE -- the removal
     * half §1.9 originally deferred, exercised against the exact grant
     * state scenarios 1-4 built up (uid 5000 + group 'finance_readers'
     * both grantees of 'reports'). ─────────────────────────────────────────── */
    CHECK(database_revoke_uid("reports", 5000) == 0,
          "s7: database_revoke_uid() removes uid 5000 from 'reports'");
    CHECK(catalog_check_access(5000, "sales_2024", PERM_READ) == 0,
          "s7: uid 5000 is DENIED on 'sales_2024' immediately after the revoke -- access genuinely gone");
    CHECK(catalog_check_access(6000, "sales_2024", PERM_READ) == 1,
          "s7: uid 6000 (via the grantee group) is completely unaffected by 5000's revoke -- scoped removal, not a grant wipe");
    CHECK(database_revoke_uid("reports", 5000) == 2,
          "s7: revoking the same uid again reports 'nothing to revoke' (rc=2), not silent success");
    CHECK(database_revoke_uid("no_such_db", 5000) == 1,
          "s7: revoking on an unknown database name fails cleanly (rc=1)");
    CHECK(database_revoke_group("reports", "no_such_group") == 2,
          "s7: revoking a group that was never a grantee reports rc=2");

    /* ── Scenario 8 (§2.1): revoking the LAST grantee deactivates the
     * whole grant entry -- so a later re-grant starts fresh instead of
     * silently resurrecting the old entry's stale perm_mask. ──────────────── */
    CHECK(database_revoke_group("reports", "finance_readers") == 0,
          "s8: database_revoke_group() removes the last remaining grantee");
    CHECK(catalog_check_access(6000, "sales_2024", PERM_READ) == 0,
          "s8: uid 6000 is now denied too -- no grantees remain");
    {
        uint32_t reports_dbid = database_find_id("reports");
        int any_active_grant = 0;
        for (uint32_t i = 0; i < DATABASE_GRANT_MAX; i++)
            if (database_grants[i].active && database_grants[i].database_id == reports_dbid)
                any_active_grant = 1;
        CHECK(!any_active_grant,
              "s8: the grant entry itself was deactivated when its last grantee left -- no stale perm_mask lingers");
    }
    CHECK(database_grant_uid("reports", 7000, PERM_WRITE) == 0 &&
          catalog_check_access(7000, "sales_2024", PERM_WRITE) == 1,
          "s8: a fresh grant after full revocation works from a clean slate (WRITE granted and honored)");
    CHECK(catalog_check_access(5000, "sales_2024", PERM_READ) == 0,
          "s8: and the long-revoked uid 5000 did NOT come back with it");

    /* ── Scenario 9 (VectorStore Gap Analysis §3): a plain sys_sls_valloc()
     * call -- the SAME generic entry point a vector collection's underlying
     * catalog object is always created through (vecstore_create_collection()
     * finds it by name in object_catalog[], see kernel/vecstore.c) -- with
     * an explicit database_id tags the new object correctly, and
     * catalog_check_access()'s existing database check applies identically
     * to a REAL valloc'd entry as it does to seed_object()'s hand-built
     * ones above, proving §3's "already free" access-control claim end to
     * end with zero vecstore-specific code involved. Also proves the §3
     * Part A fix: database_id is no longer left as uninitialized stack
     * garbage by this call. ────────────────────────────────────────────────── */
    CHECK(database_create(1, "vectors_db") == 0, "s9: database_create('vectors_db')");
    uint32_t vdb_id = database_find_id("vectors_db");
    CHECK(vdb_id != 0, "s9: 'vectors_db' resolves to a real database_id");
    CHECK(database_grant_uid("vectors_db", 8000, PERM_READ) == 0,
          "s9: uid 8000 granted READ on 'vectors_db'");
    CHECK(catalog_get_role(8000) == ROLE_GUEST, "s9: uid 8000 has no individual role (bare GUEST)");
    struct SLSVallocRequest vreq;
    memset(&vreq, 0, sizeof(vreq));
    strcpy(vreq.name, "vc_underlying");
    vreq.type         = OBJ_TYPE_DB_TABLE;
    vreq.size_pages   = 1;
    vreq.owner_uid    = 0;
    vreq.perm_mask    = 0;
    vreq.partition_id = 0;
    vreq.database_id  = vdb_id;   /* the §3 Part B fix -- real valloc-time database tagging */
    uint64_t new_obj_id = sys_sls_valloc(&vreq);
    CHECK(new_obj_id != 0, "s9: sys_sls_valloc() succeeds for 'vc_underlying'");
    CHECK(catalog_check_access(8000, "vc_underlying", PERM_READ) == 1,
          "s9: uid 8000 -- a bare GUEST -- passes on the brand-new valloc'd object PURELY via "
          "the 'vectors_db' grant, proving a real valloc() call (not just seed_object()) threads "
          "database_id correctly end to end");
    CHECK(catalog_check_access(9999, "vc_underlying", PERM_READ) == 0,
          "s9: an unrelated uid is still denied on the same object");

    /* ── Scenario 10 (§3 Part C): catalog_set_database() -- the missing
     * "retag after creation" primitive, reaching an object created WITHOUT
     * a database tag (mirroring an object created before this feature
     * existed, or a vector collection promoted from an untagged catalog
     * entry). ───────────────────────────────────────────────────────────────── */
    seed_object(2, "untagged_vc", 0);
    object_catalog_count = 3;
    CHECK(catalog_check_access(8000, "untagged_vc", PERM_READ) == 0,
          "s10: uid 8000 is denied on 'untagged_vc' before any retag -- database_id is still 0");
    CHECK(catalog_set_database(0, "untagged_vc", "vectors_db") == 0,
          "s10: catalog_set_database() (called as kernel uid 0) retags 'untagged_vc' into 'vectors_db'");
    CHECK(object_catalog[2].database_id == vdb_id,
          "s10: object_catalog[]'s database_id field was actually updated");
    CHECK(catalog_check_access(8000, "untagged_vc", PERM_READ) == 1,
          "s10: uid 8000 NOW passes -- the retag took effect immediately, matching "
          "ALTER TABLE ... SET DATABASE's own 'reads database_id live' property, with zero "
          "vecstore-specific code involved");

    /* ── Scenario 11 (§3 Part C): permission gate + error codes -- denial
     * must never look like absence or a bad database name. ─────────────────── */
    CHECK(catalog_set_database(9999, "untagged_vc", "vectors_db") == 2,
          "s11: a uid with no PERM_WRITE on the object is denied (rc=2), not silently 'not found'");
    CHECK(catalog_set_database(0, "no_such_object", "vectors_db") == 1,
          "s11: retagging a nonexistent object fails cleanly (rc=1)");
    CHECK(catalog_set_database(0, "untagged_vc", "no_such_database") == 3,
          "s11: retagging into a nonexistent database fails cleanly (rc=3)");
    CHECK(object_catalog[2].database_id == vdb_id,
          "s11: the failed rc=3 attempt left database_id untouched (no partial mutation)");

    /* ── Scenario 12 (§3 Part C): clearing the tag back to NONE. ─────────────── */
    CHECK(catalog_set_database(0, "untagged_vc", "") == 0,
          "s12: an empty database name clears the tag back to 0 (NONE)");
    CHECK(object_catalog[2].database_id == 0, "s12: database_id is genuinely back to 0");
    CHECK(catalog_check_access(8000, "untagged_vc", PERM_READ) == 0,
          "s12: uid 8000 is denied again -- the grant no longer applies once the tag is cleared");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
