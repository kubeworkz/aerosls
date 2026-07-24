/*
 * tenant_host_test.c -- Multitenant Isolation Gap Analysis §5 item 1 / §7
 * item 2 verification: a standalone host-buildable test for
 * tenant_create()/tenant_find_id()/tenant_find_by_partition()/tenant_find_
 * by_database() (kernel/tenant.c), the unified tenant identity feature.
 *
 * Links the REAL, unmodified kernel/tenant.c, kernel/partition.c, and
 * kernel/database.c -- not a reimplementation of any of them. This is
 * exactly the integration tenant_create() exists to orchestrate, so unlike
 * a lighter single-file host test, the real value here is exercising the
 * REAL partition_create()/partition_destroy()/partition_assign_uid() and
 * the REAL database_create()/database_find_id() together, proving the
 * rollback path actually calls the real teardown function, not a
 * hand-rolled stand-in for it.
 *
 * Stub set is the union of partition_host_test.c's own stubs (partition.c's
 * external dependencies) and database_host_test.c's own stubs (database.c's
 * external dependencies) -- object_catalog[]/object_catalog_count declared
 * directly (database.c's only real object_catalog.c dependency is the plain
 * array + catalog_get_role(), never the full valloc/persist/fnv1a machinery,
 * the exact justification database_host_test.c's own header comment already
 * gives), process_kill_partition()/catalog_vfree_partition()/partition_
 * reclaim_all_frames() stubbed as partition_destroy()'s cross-subsystem
 * calls (irrelevant here -- no process or catalog object exists in this
 * test's world), cluster_local_node_id() stubbed to a fixed node id
 * (Multi-Node Phase 2 -- partition ownership stamping isn't this test's
 * concern), persist_partitions()/persist_databases()/persist_tenants() all
 * no-ops (matching every other persist_* stub in these host tests).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/tenant_host_test \
 *       tests/tenant_host_test.c kernel/tenant.c kernel/partition.c kernel/database.c
 *   /tmp/tenant_host_test
 */
#include "kernel/tenant.h"
#include "kernel/partition.h"
#include "kernel/database.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Stubs for partition.c's external dependencies ───────────────────── */
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

/* ─── Stubs for database.c's external dependencies ────────────────────── */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_APP_USER; }
int group_contains_uid(const char* name, uint32_t uid) { (void)name; (void)uid; return 0; }   // database_check_access()'s group-grant path -- not exercised by this test (tenant_create() never calls it)

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static int count_active_partitions(void) {
    int n = 0;
    for (int i = 0; i < PARTITION_MAX; i++) if (partition_table[i].active) n++;
    return n;
}
static int count_active_tenants(void) {
    int n = 0;
    for (int i = 0; i < TENANT_MAX; i++) if (tenants[i].active) n++;
    return n;
}

int main(void) {
    partition_init();   // pre-populates partition_table[0] as PARTITION_SYSTEM, mirrors kernel.c's own boot sequence

    /* ── Scenario A: a basic tenant_create() call really links a real
     * partition_id to a real database_id, and the returned tenant_id is
     * queryable both ways. ──────────────────────────────────────────────── */
    uint32_t tenant_a_id = 0;
    {
        int rc = tenant_create(100, "acme", &tenant_a_id);
        CHECK(rc == 0, "sA: tenant_create() for a fresh name succeeds");
        CHECK(tenant_a_id != 0, "sA: a non-zero tenant_id was assigned");

        uint32_t idx = 0xFFFFFFFF;
        for (int i = 0; i < TENANT_MAX; i++)
            if (tenants[i].active && tenants[i].tenant_id == tenant_a_id) { idx = (uint32_t)i; break; }
        CHECK(idx != 0xFFFFFFFF, "sA: the new tenant row is really active in tenants[]");
        CHECK(tenants[idx].partition_id != 0, "sA: the tenant's partition_id is a real, non-system partition");
        CHECK(tenants[idx].database_id != 0, "sA: the tenant's database_id is a real, non-NONE database");
        CHECK(partition_table[tenants[idx].partition_id].active == 1,
              "sA: the partition_id really exists as an active row in partition_table[] (real partition_create(), not a fake id)");
        CHECK(database_find_id("acme") == tenants[idx].database_id,
              "sA: the database_id really matches what database_create()/database_find_id() report (real database_create(), not a fake id)");
    }

    /* ── Scenario B: a duplicate tenant name is rejected, and rejection
     * doesn't leak a partition or database behind it. ─────────────────────── */
    {
        int parts_before = count_active_partitions();
        int rc = tenant_create(101, "acme", 0);
        CHECK(rc == 1, "sB: creating a second tenant with an already-used name fails with rc=1");
        CHECK(count_active_partitions() == parts_before,
              "sB: the duplicate-name rejection created no new partition");
    }

    /* ── Scenario C: caller_uid 0 (kernel) is never assigned into the new
     * partition -- partition_assign_uid()'s own permanent-PARTITION_SYSTEM
     * invariant for uid 0 is respected, not fought against. ──────────────── */
    {
        uint32_t tenant_c_id = 0;
        int rc = tenant_create(0, "kernelspace", &tenant_c_id);
        CHECK(rc == 0, "sC: tenant_create() with caller_uid 0 still succeeds");
        CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM,
              "sC: uid 0 is still resolved to PARTITION_SYSTEM afterward, not reassigned into the new tenant's partition");
    }

    /* ── Scenario D: a non-kernel caller really ends up inside their own
     * new partition -- the whole point of the assignment step. ───────────── */
    {
        uint32_t tenant_d_id = 0;
        int rc = tenant_create(200, "widgetco", &tenant_d_id);
        CHECK(rc == 0, "sD: setup -- tenant_create() for uid 200 succeeds");
        uint32_t idx = 0xFFFFFFFF;
        for (int i = 0; i < TENANT_MAX; i++)
            if (tenants[i].active && tenants[i].tenant_id == tenant_d_id) { idx = (uint32_t)i; break; }
        CHECK(partition_get_for_uid(200) == tenants[idx].partition_id,
              "sD: uid 200 (a real, non-kernel caller) is really assigned into their own new partition afterward");
    }

    /* ── Scenario E: reverse lookups resolve to the right tenant, and to
     * NOTHING for ids no tenant claims. ────────────────────────────────────── */
    {
        uint32_t idx = 0xFFFFFFFF;
        for (int i = 0; i < TENANT_MAX; i++)
            if (tenants[i].active && tenants[i].tenant_id == tenant_a_id) { idx = (uint32_t)i; break; }
        CHECK(tenant_find_by_partition(tenants[idx].partition_id) == tenant_a_id,
              "sE: tenant_find_by_partition() resolves acme's partition_id back to acme's tenant_id");
        CHECK(tenant_find_by_database(tenants[idx].database_id) == tenant_a_id,
              "sE: tenant_find_by_database() resolves acme's database_id back to acme's tenant_id");
        CHECK(tenant_find_by_partition(PARTITION_SYSTEM) == 0,
              "sE: PARTITION_SYSTEM (predates any tenant_create() call) resolves to no tenant, honestly");
        CHECK(tenant_find_by_database(0) == 0,
              "sE: database_id 0 (NONE) resolves to no tenant, honestly");
        CHECK(tenant_find_id("no-such-tenant") == 0,
              "sE: an unknown tenant name resolves to 0, matching database_find_id()'s own convention");
    }

    /* ── Scenario F: when database_create() fails (name collision at the
     * database layer) AFTER partition_create() already succeeded, the
     * just-created partition is really rolled back via a real
     * partition_destroy() call -- not left as an orphan. ──────────────────── */
    {
        int rc0 = database_create(999, "preexisting-db");
        CHECK(rc0 == 0, "sF: setup -- a database named 'preexisting-db' already exists (created directly, no partition consumed)");

        int parts_before  = count_active_partitions();
        int tenants_before = count_active_tenants();
        uint32_t out_id = 12345;   // sentinel -- must NOT be written on failure per tenant_create()'s own contract... actually it IS only set on rc==0, so this stays 12345
        int rc = tenant_create(300, "preexisting-db", &out_id);

        CHECK(rc == 3, "sF: tenant_create() for a name that already exists as a DATABASE (but not a tenant) fails with rc=3");
        CHECK(count_active_partitions() == parts_before,
              "sF: the partition created during this failed attempt was really rolled back (partition count unchanged, not +1)");
        CHECK(count_active_tenants() == tenants_before,
              "sF: no tenant row was left behind by the failed attempt");
        CHECK(tenant_find_id("preexisting-db") == 0,
              "sF: no tenant now claims the name 'preexisting-db'");
        CHECK(out_id == 12345, "sF: out_tenant_id is left untouched on failure, matching tenant_create()'s own documented contract");
    }

    /* ── Scenario G: after that rollback, the freed partition slot is
     * really reusable -- proving partition_destroy() did real teardown,
     * not just a soft/partial one that leaves the slot unusable. ──────────── */
    {
        uint32_t tenant_g_id = 0;
        int rc = tenant_create(400, "freshstart", &tenant_g_id);
        CHECK(rc == 0, "sG: a fresh tenant_create() after the rollback in sF still succeeds (the rolled-back partition slot is reusable)");
    }

    /* ── Scenario H: partition capacity genuinely runs out eventually
     * (PARTITION_MAX=256, minus PARTITION_SYSTEM's own permanent slot 0 =
     * 255 usable), and when it does, tenant_create() reports rc=2 and
     * creates NEITHER a database NOR a tenant row for that final, failed
     * name -- step 1 failing must short-circuit before step 2 ever runs.
     *
     * Multitenant Isolation Gap Analysis §5 item 9 (capacity sizing):
     * TENANT_MAX and DATABASE_MAX were raised to 256 in lockstep with
     * PARTITION_MAX (see kernel/tenant.h's/kernel/database.h's own comments
     * on why), so this scenario still proves what it always proved --
     * partition_create()'s reserved slot 0 makes it, not TENANT_MAX/
     * DATABASE_MAX, the tighter ceiling by exactly one slot (255 vs 256
     * usable), so rc=2 (partition table full) still fires first, not rc=1
     * (tenant table full). If TENANT_MAX/DATABASE_MAX are ever raised
     * without raising PARTITION_MAX in lockstep, or vice versa, this
     * scenario's rc==2 assertion is exactly what would catch that drifting
     * out of sync. ──────────────────────────────────────────────────────── */
    {
        int rc = 0;
        char name[TENANT_NAME_LEN];
        int i;
        for (i = 0; i < PARTITION_MAX + 2; i++) {
            snprintf(name, sizeof(name), "cap%d", i);
            rc = tenant_create(500 + (uint32_t)i, name, 0);
            if (rc == 2) break;
        }
        CHECK(rc == 2, "sH: partition capacity exhaustion is eventually hit and reported as rc=2");
        CHECK(database_find_id(name) == 0,
              "sH: no database was created for the name that hit partition capacity exhaustion (step 2 never ran)");
        CHECK(tenant_find_id(name) == 0,
              "sH: no tenant row exists for that name either");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
