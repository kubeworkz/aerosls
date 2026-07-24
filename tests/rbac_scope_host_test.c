/*
 * rbac_scope_host_test.c -- Multitenant Isolation Gap Analysis §5 item 5 /
 * §7 item 4 verification: a standalone host-buildable test for the new
 * tenant-scoped RBAC administration gate -- kernel/tenant.c's
 * tenant_caller_may_administer(), and its wiring into kernel/group_profile.c's
 * group_create()/group_add_member() and kernel/authlist.c's authlist_create()/
 * authlist_grant_object()/authlist_grant_uid()/authlist_grant_group().
 *
 * Before this phase, group_create()/authlist_create() etc. took no
 * caller_uid at all and had zero permission gate -- any uid could mint a
 * group naming ANY role or add ANY uid as a member, via a single shared,
 * unscoped global namespace. This test proves the real fix: group/authlist
 * scope is now stamped from the creator's own partition at creation time,
 * only that tenant's recorded owner_uid (or a global ROLE_SYSTEM_KERNEL/
 * ROLE_DB_ADMIN caller) may administer it, and every member/grantee/object
 * attached afterward must itself belong to that same partition.
 *
 * Links the REAL, unmodified kernel/tenant.c, kernel/partition.c,
 * kernel/database.c, kernel/group_profile.c, and kernel/authlist.c -- not a
 * reimplementation of any of them. This is exactly the integration
 * tenant_caller_may_administer() exists to gate, so the real value here is
 * exercising the REAL tenant_create()/partition_assign_uid() alongside the
 * REAL group_create()/authlist_create() together, proving the permission
 * gate actually reads the real tenants[] table, not a hand-rolled stand-in.
 *
 * Stubs: kernel_serial_print/printf (logging), persist_partitions/
 * persist_databases/persist_tenants (no-ops, matching every other persist_*
 * stub in these host tests), process_kill_partition/catalog_vfree_partition/
 * partition_reclaim_all_frames (partition.c's cross-subsystem calls,
 * irrelevant here), cluster_local_node_id (fixed node id, Multi-Node Phase 2
 * concern not this test's), partition_lease_step_down (no-op).
 *
 * catalog_get_role() is a SETTABLE FAKE (set_fake_role()), not a stub fixed
 * to one value and not the real object_catalog.c (whose full valloc/
 * persist/fnv1a machinery is irrelevant to whether tenant_caller_may_
 * administer()'s role checks are wired correctly) -- this test's whole
 * point is exercising both the ROLE_SYSTEM_KERNEL/ROLE_DB_ADMIN bypass
 * paths AND the "no role set, defaults to GUEST" path, so it needs real
 * per-uid control the way frame_pool.c's own quota fakes elsewhere in this
 * suite need real per-partition control. uid 0 always resolves to
 * ROLE_SYSTEM_KERNEL, matching catalog_get_role()'s own real, documented
 * "UID 0 is always kernel" rule exactly.
 *
 * object_catalog[]/object_catalog_count are declared as bare storage (not
 * linking the real object_catalog.c), the same "declare the extern array
 * directly" convention database_host_test.c and tenant_host_test.c already
 * established -- authlist_grant_object() only ever reads .active/.name/
 * .partition_id off this array, never any of object_catalog.c's own CRUD
 * logic.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/rbac_scope_host_test tests/rbac_scope_host_test.c \
 *       kernel/tenant.c kernel/partition.c kernel/database.c \
 *       kernel/group_profile.c kernel/authlist.c
 *   /tmp/rbac_scope_host_test
 */
#include "kernel/tenant.h"
#include "kernel/partition.h"
#include "kernel/database.h"
#include "kernel/group_profile.h"
#include "kernel/authlist.h"
#include "kernel/object_catalog.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Stubs for partition.c's/database.c's external dependencies ────────── */
void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
int stream_migrate_send_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 7 addendum (real cross-node data movement) -- not exercised by this test, permissive "nothing to send" stub, sibling of the stream_relocate_partition() stub above (kernel/partition.c now calls whichever of the two applies depending on cluster_local_node_id()) */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { }
void persist_databases(void) { }
void persist_tenants(void) { }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t cluster_local_node_id(void) { return 0; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }

/* ─── Settable-fake catalog_get_role() (see header comment above) ───────── */
#define FAKE_ROLE_MAX 64
static uint32_t fake_role_uid[FAKE_ROLE_MAX];
static SLSRole  fake_role_val[FAKE_ROLE_MAX];
static int      fake_role_count = 0;
static void set_fake_role(uint32_t uid, SLSRole role) {
    fake_role_uid[fake_role_count] = uid;
    fake_role_val[fake_role_count] = role;
    fake_role_count++;
}
SLSRole catalog_get_role(uint32_t uid) {
    if (uid == 0) return ROLE_SYSTEM_KERNEL;   // matches the real function's own documented rule
    for (int i = 0; i < fake_role_count; i++) {
        if (fake_role_uid[i] == uid) return fake_role_val[i];
    }
    return ROLE_GUEST;   // matches the real function's own documented default
}

/* ─── Bare storage for object_catalog.c's extern arrays (see header) ────── */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
static void add_fake_object(const char* name, uint32_t partition_id) {
    struct SLSObjectEntry* e = &object_catalog[object_catalog_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, OBJECT_NAME_LEN - 1);
    e->active = 1;
    e->partition_id = partition_id;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    partition_init();   // pre-populates partition_table[0] as PARTITION_SYSTEM

    /* ── Setup: two independent tenants, mirroring tenant_host_test.c's own
     * scenario shape. uid 100 owns "acme" (partition P1), uid 300 owns
     * "beta" (partition P2). uid 101 is a second member of acme's own
     * partition (simulates a co-worker of the tenant owner). uid 200 is
     * never assigned to any tenant at all (stays PARTITION_DEFAULT/0). ──── */
    uint32_t acme_tenant_id = 0, beta_tenant_id = 0;
    CHECK(tenant_create(100, "acme", &acme_tenant_id) == 0, "setup: tenant 'acme' created (owner uid 100)");
    CHECK(tenant_create(300, "beta", &beta_tenant_id) == 0, "setup: tenant 'beta' created (owner uid 300)");
    uint32_t p1 = tenant_find_by_partition(0);   // unused; real lookup is by tenant id below
    (void)p1;
    uint32_t P1 = tenants[0].partition_id;   // acme's partition (first tenant slot)
    uint32_t P2 = tenants[1].partition_id;   // beta's partition (second tenant slot)
    CHECK(P1 != P2, "setup: acme and beta really got two different partitions");
    partition_assign_uid(101, P1);   // uid 101 is another user inside acme's own partition

    /* ── Scenario A: the tenant owner can administer their own tenant's
     * RBAC -- group_create() succeeds when caller_uid is the recorded
     * tenants[].owner_uid, with zero global role granted. ────────────────── */
    CHECK(group_create(100, "acme_admins", ROLE_DB_ADMIN) == 1,
          "sA: tenant owner (uid 100) creates a group scoped to their own tenant");
    uint32_t acme_admins_partition = 0xFFFFFFFF;
    CHECK(group_get_partition_id("acme_admins", &acme_admins_partition) == 1 &&
          acme_admins_partition == P1,
          "sA: the new group was really stamped with acme's own partition_id, not 0");

    /* ── Scenario B: a same-tenant member can be added; a stranger from
     * outside the tenant cannot -- cross-tenant membership refused
     * outright, not silently scoped. ─────────────────────────────────────── */
    CHECK(group_add_member(100, "acme_admins", 101) == 1,
          "sB: uid 101 (already inside acme's partition) joins 'acme_admins'");
    CHECK(group_add_member(100, "acme_admins", 200) == 0,
          "sB: uid 200 (never assigned to acme's partition) is refused as a member");
    CHECK(group_contains_uid("acme_admins", 200) == 0,
          "sB: the refused add really left no trace -- uid 200 is not a member");

    /* ── Scenario C: a caller who is NOT the tenant owner (and holds no
     * global admin role) cannot administer that tenant's RBAC at all --
     * not even to create a brand new group naming themselves, and not to
     * touch an existing group by name. ────────────────────────────────────── */
    CHECK(group_add_member(300, "acme_admins", 101) == 0,
          "sC: uid 300 (beta's own owner, not acme's) cannot administer acme's 'acme_admins' group");
    CHECK(group_create(101, "rogue_group", ROLE_DB_ADMIN) == 0,
          "sC: uid 101 (a mere member of acme, not its recorded owner) cannot create groups for acme");

    /* ── Scenario D: a caller correctly administers only their OWN tenant --
     * beta's owner can freely manage beta's own groups, fully independent
     * of acme's. ──────────────────────────────────────────────────────────── */
    CHECK(group_create(300, "beta_group", ROLE_GUEST) == 1,
          "sD: beta's own owner (uid 300) creates a group scoped to beta");
    uint32_t beta_group_partition = 0xFFFFFFFF;
    CHECK(group_get_partition_id("beta_group", &beta_group_partition) == 1 &&
          beta_group_partition == P2,
          "sD: 'beta_group' was stamped with beta's own partition_id, distinct from acme's");

    /* ── Scenario E: the PARTITION_SYSTEM special case -- no single tenant
     * owns it, so only a global ROLE_DB_ADMIN/ROLE_SYSTEM_KERNEL caller may
     * administer RBAC scoped there, matching every uid that was never
     * assigned to any tenant (partition_get_for_uid() defaults to 0). ────── */
    set_fake_role(9000, ROLE_GUEST);      // never assigned to a tenant, no global role either
    set_fake_role(9001, ROLE_DB_ADMIN);   // never assigned to a tenant, but a global admin
    CHECK(group_create(9000, "guest_group", ROLE_GUEST) == 0,
          "sE: a bare GUEST with no tenant and no global admin role cannot administer PARTITION_SYSTEM");
    CHECK(group_create(9001, "admin_group", ROLE_DB_ADMIN) == 1,
          "sE: a global ROLE_DB_ADMIN caller CAN administer PARTITION_SYSTEM even with no tenant of their own");
    CHECK(group_create(0, "kernel_group", ROLE_SYSTEM_KERNEL) == 1,
          "sE: uid 0 (kernel) can always administer any partition, including PARTITION_SYSTEM");

    /* ── Scenario F: authlist_create() gets the identical scoping treatment
     * as group_create() -- same owner gate, same partition stamping. ─────── */
    CHECK(authlist_create(100, "acme_secrets") == 1,
          "sF: tenant owner (uid 100) creates an authlist scoped to acme");
    CHECK(authlist_create(300, "acme_secrets_take2") == 1,
          "sF: (sanity) beta's owner can create their own, differently-named list");

    /* ── Scenario G: authlist_grant_object() refuses to attach a grant on
     * an object belonging to a DIFFERENT tenant's partition, even though
     * the list itself and the caller are both legitimately acme's own. ───── */
    add_fake_object("acme_doc", P1);
    add_fake_object("beta_doc", P2);
    CHECK(authlist_grant_object(100, "acme_secrets", "acme_doc", PERM_READ) == 1,
          "sG: granting on acme's OWN object succeeds");
    CHECK(authlist_grant_object(100, "acme_secrets", "beta_doc", PERM_READ) == 0,
          "sG: granting on beta's object via acme's list is refused -- cross-tenant grant blocked");
    CHECK(authlist_grant_object(100, "acme_secrets", "no_such_object", PERM_READ) == 0,
          "sG: granting on a nonexistent object name fails cleanly (not a crash, not a silent success)");

    /* ── Scenario H: authlist_grant_uid()/authlist_grant_group() apply the
     * same same-partition invariant to grantees that group_add_member()
     * already enforces for group members. ────────────────────────────────── */
    CHECK(authlist_grant_uid(100, "acme_secrets", 101) == 1,
          "sH: uid 101 (inside acme's partition) can be added as a direct grantee");
    CHECK(authlist_grant_uid(100, "acme_secrets", 200) == 0,
          "sH: uid 200 (outside acme's partition) is refused as a grantee");
    CHECK(authlist_grant_group(100, "acme_secrets", "acme_admins") == 1,
          "sH: 'acme_admins' (a real acme-scoped group) can be added as a grantee group");
    CHECK(authlist_grant_group(100, "acme_secrets", "beta_group") == 0,
          "sH: 'beta_group' (a different tenant's group) is refused as a grantee group");
    CHECK(authlist_grant_group(100, "acme_secrets", "no_such_group") == 0,
          "sH: granting a nonexistent group name fails cleanly");

    /* ── Scenario I: end-to-end sanity -- the whole point of all this
     * scoping is that catalog_check_access() still works exactly as
     * before for a legitimately-scoped grant chain (uid -> group ->
     * object access), now proven to only ever assemble from
     * same-tenant pieces. ─────────────────────────────────────────────────── */
    CHECK(authlist_check_access(101, "acme_doc", PERM_READ) == 1,
          "sI: uid 101 (direct grantee, added in sH) is really granted READ on 'acme_doc'");
    CHECK(authlist_check_access(200, "acme_doc", PERM_READ) == 0,
          "sI: uid 200 (never a grantee anywhere) is correctly denied");

    printf("\n%s\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return g_fail ? 1 : 0;
}
