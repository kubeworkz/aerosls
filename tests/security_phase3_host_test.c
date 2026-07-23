/*
 * security_phase3_host_test.c — Navigator-Parity Gap Roadmap Phase 3
 * verification: a standalone host-buildable test proving group profiles
 * (kernel/group_profile.c), authorization lists (kernel/authlist.c), and the
 * security audit log (kernel/security_audit.c) all work as real, additive
 * extensions of kernel/object_catalog.c's catalog_check_access()/
 * sys_sls_role_set() -- linked against the REAL, unmodified versions of all
 * four files, not reimplementations. Same "link the real object_catalog.c,
 * stub its heavy dependencies instead" approach tests/legacy_rowstore_
 * boundary_host_test.c already established (see that file's own header
 * comment for the full rationale) -- this test's stub list is a strict
 * subset of that one's, since it never calls sys_sls_select/update/insert/
 * delete (the only functions that need lock_mgr/journal/index_mgr/mqt/
 * constraint at all), only sys_sls_valloc/sys_sls_role_set/
 * catalog_check_access() and the three new Phase 3 modules' own APIs.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/security_phase3_host_test tests/security_phase3_host_test.c \
 *       kernel/object_catalog.c kernel/group_profile.c kernel/authlist.c \
 *       kernel/security_audit.c kernel/database.c
 *   /tmp/security_phase3_host_test
 *
 * Database Namespace & Access Roadmap Phase 3 update: catalog_check_
 * access() now calls straight into kernel/database.c's database_check_
 * access() (a no-op for every object here, since none of this test's own
 * seeded objects ever get a database_id -- object_catalog.h's own
 * SLSObjectEntry.database_id zero-defaults, matching every pre-Phase-3
 * object), so this test's link line now includes kernel/database.c. Every
 * scenario below is run completely unmodified from before this addition,
 * to prove the new database-grant path is genuinely additive and didn't
 * regress role/group/authlist -- see tests/database_grant_phase3_host_
 * test.c for the sibling test that actually exercises database grants.
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
#include "kernel/security_audit.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ─── Real globals object_catalog.c expects to exist (same convention as
// legacy_rowstore_boundary_host_test.c -- object_catalog[]/object_records[]/
// object_schemas[]/role_table[]/object_catalog_count are all REAL, already
// defined inside the real object_catalog.c linked below). ──────────────────

// kernel_tick_counter -- security_audit.c's real dependency, same
// definition tests/auth_host_test.c and tests/legacy_rowstore_boundary_
// host_test.c already use.
volatile uint64_t kernel_tick_counter = 0;

// ─── kernel_io.h stand-ins ──────────────────────────────────────────────────
void kernel_serial_print(const char* s) { (void)s; }
void persist_databases(void) { /* Database Gap Analysis Gap 1 -- database.c now persists after every mutation; no-op here, same as every other persist_* stub in these tests */ }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

// ─── tier_mgr.c stand-in (object_catalog.c's own forward-declared extern). ──
void tier_notify_access(uint64_t object_id) { (void)object_id; }

// ─── partition.c stand-in: every uid this test uses defaults to partition 0,
// matching partition_get_for_uid()'s own documented default -- this test
// never exercises cross-partition denial (that's LPAR-phase territory, not
// Phase 3's). ────────────────────────────────────────────────────────────────
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }

// ─── transaction.c stand-ins (object_catalog.c's own forward-declared
// externs) -- never actually reached since this test doesn't call sys_sls_
// select/update/insert/delete, but must still resolve at link time. ────────
uint64_t tx_get_active(uint32_t thread_id) { (void)thread_id; return 0; }
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id, const char* key,
                   const char* old_value, const char* new_value) {
    (void)thread_id; (void)object_id; (void)key; (void)old_value; (void)new_value;
    return 0;
}
uint32_t kernel_get_current_thread_id(void) { return 1; }

// ─── vecstore.c stand-in (object_catalog.c's own forward-declared extern,
// called unconditionally from sys_sls_vfree()). ────────────────────────────
void vecstore_notify_object_freed(const char* collection_name) { (void)collection_name; }

// ─── lock_mgr.c / journal.c / index_mgr.c / mqt.c / constraint.c stand-ins --
// never actually reached (this test doesn't call sys_sls_select/update/
// insert/delete), but referenced inside object_catalog.c's compiled object
// file and so must still resolve at link time. ─────────────────────────────
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

// ─── persist.c stand-ins -- this test isn't exercising NVMe persistence. ───
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
// empty perm_mask -- so any non-owner, non-kernel caller's access is decided
// entirely by role/group/authlist logic, not an accidental perm_mask grant.
static void seed_object(int slot, const char* name, SLSObjectType type, uint32_t perm_mask) {
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].type       = type;
    object_catalog[slot].object_id  = 0xC000 + (uint64_t)slot;
    object_catalog[slot].active     = 1;
    object_catalog[slot].owner_uid  = 0;   // kernel-owned -- no uid in this test owns it
    object_catalog[slot].perm_mask  = perm_mask;
}

int main(void) {
    object_catalog_count = 0;

    seed_object(0, "ledger", OBJ_TYPE_DB_TABLE, 0 /* no perm_mask grant at all */);
    object_catalog_count = 1;

    // ── Scenario 1: a plain GUEST-role uid with no group/authlist is denied
    // read access to a DB_TABLE -- confirms catalog_role_grants()'s GUEST
    // rule and the "GUEST never falls through" behavior survived the Phase 3
    // refactor unchanged. ─────────────────────────────────────────────────────
    CHECK(catalog_check_access(5000, "ledger", PERM_READ) == 0,
          "s1: a bare GUEST-role uid (no role_table entry) is denied read on a DB_TABLE");
    CHECK(security_audit_log_count == 1 &&
          strcmp(security_audit_log_buf[0].action, "ACCESS_DENIED") == 0 &&
          security_audit_log_buf[0].uid == 5000,
          "s1: the denial was logged to the real security audit trail");

    // ── Scenario 2: assigning uid 5000 the DB_ADMIN role directly grants
    // access via catalog_role_grants() -- and the role change itself was
    // audited. ────────────────────────────────────────────────────────────────
    struct SLSRoleRequest rreq = { .uid = 5000, .role = ROLE_DB_ADMIN };
    CHECK(sys_sls_role_set(&rreq) == 0, "s2: sys_sls_role_set() assigns DB_ADMIN to uid 5000");
    CHECK(catalog_check_access(5000, "ledger", PERM_READ) == 1,
          "s2: uid 5000 now passes as DB_ADMIN via the individual-role path");
    CHECK(security_audit_log_count == 2 &&
          strcmp(security_audit_log_buf[1].action, "ROLE_CHANGE") == 0 &&
          security_audit_log_buf[1].uid == 5000 && security_audit_log_buf[1].granted == 1,
          "s2: the role change itself was logged as a real audit event");

    // Revert 5000 back to GUEST for the rest of the test (role_table only
    // ever holds one role per uid; sys_sls_role_set() updates in place).
    rreq.role = ROLE_GUEST;
    sys_sls_role_set(&rreq);
    CHECK(catalog_check_access(5000, "ledger", PERM_READ) == 0,
          "s2: reverting to GUEST denies access again -- the grant really came from the role");

    // ── Scenario 3: group profiles. uid 6000 is a bare GUEST with no direct
    // role grant, but membership in a DB_ADMIN-role group should grant the
    // exact same access a real DB_ADMIN would get -- additive, not routed
    // through role_table[] at all. ───────────────────────────────────────────
    CHECK(catalog_check_access(6000, "ledger", PERM_READ) == 0,
          "s3: uid 6000 is denied before joining any group (sanity baseline)");
    CHECK(group_create("finance_admins", ROLE_DB_ADMIN) == 1, "s3: group_create() succeeds");
    CHECK(group_create("finance_admins", ROLE_DB_ADMIN) == 0,
          "s3: creating a duplicate group name fails");
    CHECK(group_add_member("finance_admins", 6000) == 1, "s3: group_add_member() adds uid 6000");
    CHECK(group_contains_uid("finance_admins", 6000) == 1,
          "s3: group_contains_uid() reports uid 6000 as a member");
    CHECK(group_contains_uid("finance_admins", 7000) == 0,
          "s3: group_contains_uid() reports uid 7000 (never added) as NOT a member");
    CHECK(catalog_check_access(6000, "ledger", PERM_READ) == 1,
          "s3: uid 6000 now passes via group-derived DB_ADMIN access -- role_table[] was never touched");
    CHECK(catalog_get_role(6000) == ROLE_GUEST,
          "s3: uid 6000's OWN individual role is still GUEST -- the group only added coverage, didn't change it");

    // ── Scenario 4: authorization lists. uid 8000 is a bare GUEST with no
    // group membership either, but is granted READ on "ledger" directly via
    // an authlist. ───────────────────────────────────────────────────────────
    CHECK(catalog_check_access(8000, "ledger", PERM_READ) == 0,
          "s4: uid 8000 is denied before any authlist grant (sanity baseline)");
    CHECK(authlist_create("ledger_readers") == 1, "s4: authlist_create() succeeds");
    CHECK(authlist_create("ledger_readers") == 0, "s4: creating a duplicate authlist name fails");
    CHECK(authlist_grant_object("ledger_readers", "ledger", PERM_READ) == 1,
          "s4: authlist_grant_object() attaches {ledger, PERM_READ} to the list");
    CHECK(authlist_grant_uid("ledger_readers", 8000) == 1,
          "s4: authlist_grant_uid() adds uid 8000 as a direct grantee");
    CHECK(authlist_check_access(8000, "ledger", PERM_READ) == 1,
          "s4: authlist_check_access() confirms uid 8000 is granted READ on 'ledger'");
    CHECK(authlist_check_access(8000, "ledger", PERM_WRITE) == 0,
          "s4: the same list does NOT grant WRITE -- perm_mask on the list entry is respected");
    CHECK(catalog_check_access(8000, "ledger", PERM_READ) == 1,
          "s4: catalog_check_access() itself now honors the authlist grant as a fallback path");
    CHECK(catalog_check_access(8000, "ledger", PERM_WRITE) == 0,
          "s4: ...but still correctly denies WRITE, which no path grants uid 8000");

    // ── Scenario 5: authlist grantee GROUPS -- uid 9000 is a member of a
    // group that is itself a grantee of an authlist (two levels of
    // indirection: uid -> group -> authlist -> object). ─────────────────────
    CHECK(group_create("auditors", ROLE_GUEST) == 1,
          "s5: group_create() for a GUEST-role group (the group itself grants nothing on its "
          "own -- only the authlist grantee-group link should matter here)");
    CHECK(group_add_member("auditors", 9000) == 1, "s5: uid 9000 joins the 'auditors' group");
    CHECK(catalog_check_access(9000, "ledger", PERM_READ) == 0,
          "s5: uid 9000 is still denied -- a GUEST-role group grants nothing by itself");
    CHECK(authlist_grant_group("ledger_readers", "auditors") == 1,
          "s5: authlist_grant_group() adds 'auditors' as a grantee group of 'ledger_readers'");
    CHECK(catalog_check_access(9000, "ledger", PERM_READ) == 1,
          "s5: uid 9000 now passes -- granted via group membership in an authlist grantee group");

    // ── Scenario 6: a truly unauthorized uid (no role, no group, no
    // authlist) is still denied, and every such denial keeps landing in the
    // real audit log (not just the first one from Scenario 1). ─────────────
    uint32_t audit_count_before_s6 = security_audit_log_count;
    CHECK(catalog_check_access(9999, "ledger", PERM_WRITE) == 0,
          "s6: a totally unrelated uid is denied WRITE on 'ledger'");
    CHECK(security_audit_log_count == audit_count_before_s6 + 1,
          "s6: this denial was also logged -- the audit trail isn't a one-shot fluke from s1");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
