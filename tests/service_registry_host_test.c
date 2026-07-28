/*
 * service_registry_host_test.c — Orchestration Plan Phase 4: name →
 * partition/node/endpoint resolution. Links the REAL, unmodified
 * kernel/service_registry.c and kernel/partition.c.
 *
 * ─── The property this file exists to prove ───────────────────────────
 * A registration stores name → PARTITION. It does NOT store a node id;
 * that is derived from partition_owner_table[] on every resolve. The
 * whole argument for that design is one claim:
 *
 *     migrate a partition, and every service in it resolves to the new
 *     node immediately, with nothing notified and nothing invalidated.
 *
 * Scenario 3 is therefore the centrepiece. It runs the REAL
 * partition_migrate() and re-resolves. If the node were ever cached here,
 * that scenario fails -- which is exactly why it is written against the
 * real migration path rather than a hand-set owner table.
 *
 * Scenario 6 is its converse and matters just as much: a registration
 * pointing at a DESTROYED partition must stop resolving, rather than
 * confidently reporting the owner node of a partition that no longer
 * exists.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/service_registry_host_test \
 *       tests/service_registry_host_test.c kernel/service_registry.c kernel/partition.c
 *   /tmp/service_registry_host_test
 */
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Stubs ───────────────────────────────────────────────────────────
 * Faithful where faithfulness is observable, permissive where the
 * subsystem is out of scope and has its own coverage elsewhere. */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* persist_services() is counted, not performed: this test asserts that
 * every mutation persists (a registry that silently forgets across reboot
 * is a real failure mode), while the persistence MECHANISM has its own
 * coverage in persist_lba_layout / persist_crash_consistency. */
static int persist_services_calls = 0;
void persist_services(void)   { persist_services_calls++; }
void persist_partitions(void) { }

/* Settable role, so the RBAC gate can be exercised in both directions.
 * The real catalog_get_role() walks role_table[]; this test is about the
 * registry's use of the answer, not about how the answer is computed. */
static SLSRole g_role = ROLE_SYSTEM_KERNEL;
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return g_role; }

/* Settable node identity -- the same fake tests/partition_host_test.c and
 * the cross-node tests already use. */
static uint32_t g_local_node = 0;
uint32_t cluster_local_node_id(void) { return g_local_node; }

/* partition.c dependencies outside this test's scope. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t process_kill_partition(uint32_t p)  { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int  partition_lease_step_down(uint32_t p) { (void)p; return 1; }
int  stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
uint32_t simi_ctx_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }

int main(void) {
    printf("=== Service registry (name -> partition/node/endpoint) ===\n\n");

    partition_init();
    service_registry_init();

    uint32_t pweb = partition_create("web");
    uint32_t pdb  = partition_create("db");
    CHECK(pweb != 0xFFFFFFFFu && pdb != 0xFFFFFFFFu && pweb != pdb,
          "setup: two distinct partitions exist");

    /* ═══ Scenario 1: register and resolve ════════════════════════════ */
    printf("\n-- Scenario 1: register and resolve --\n");
    {
        persist_services_calls = 0;
        CHECK(service_register(0, "api", pweb, SVC_ENDPOINT_TCP, 8080) == SVC_REG_OK,
              "a service registers into a partition");
        CHECK(persist_services_calls == 1, "registration persists immediately");
        CHECK(service_registry_count() == 1, "the registry counts it");

        struct SLSServiceLocation loc;
        CHECK(service_resolve("api", &loc) == SVC_REG_OK, "the name resolves");
        CHECK(strcmp(loc.name, "api") == 0,          "...to the right name");
        CHECK(loc.partition_id == pweb,              "...the right partition");
        CHECK(loc.endpoint_kind == SVC_ENDPOINT_TCP, "...the right endpoint kind");
        CHECK(loc.endpoint_port == 8080,             "...the right port");
        CHECK(loc.node_id == partition_get_owner_node(pweb),
              "...and the node its partition currently belongs to");

        CHECK(service_resolve("nope", &loc) == SVC_REG_ERR_NOT_FOUND,
              "an unregistered name does not resolve");
    }

    /* ═══ Scenario 2: is_local tracks this node's identity ════════════ */
    printf("\n-- Scenario 2: local vs remote --\n");
    {
        struct SLSServiceLocation loc;
        g_local_node = 0;   /* partitions were created while node id was 0 */
        service_resolve("api", &loc);
        CHECK(loc.is_local, "a service on this node reports is_local");

        g_local_node = 5;   /* same registry, different machine's point of view */
        service_resolve("api", &loc);
        CHECK(!loc.is_local, "the same registration is remote from another node");
        CHECK(loc.node_id == 0, "...and still names the partition's real owner");
        g_local_node = 0;
    }

    /* ═══ Scenario 3: THE PROPERTY ════════════════════════════════════
     * Resolution follows partition migration with nothing to invalidate. */
    printf("\n-- Scenario 3: resolution follows partition_migrate() --\n");
    {
        /* partition_migrate() refuses while node identity is the 0
         * sentinel, so give this node a real one first. */
        g_local_node = 1;
        partition_owner_table[pweb].node_id = 1;

        struct SLSServiceLocation before;
        service_resolve("api", &before);
        CHECK(before.node_id == 1 && before.is_local,
              "before: 'api' is on node 1, and node 1 is us");

        persist_services_calls = 0;
        int rc = partition_migrate(pweb, 7);
        CHECK(rc == 0, "the real partition_migrate() moves the partition to node 7");

        struct SLSServiceLocation after;
        CHECK(service_resolve("api", &after) == SVC_REG_OK,
              "the service still resolves after its partition migrated");
        CHECK(after.node_id == 7,
              "*** resolution reports the NEW node -- the node was never stored ***");
        CHECK(!after.is_local, "...and is correctly no longer local to this node");
        CHECK(after.partition_id == pweb && after.endpoint_port == 8080,
              "...with partition and endpoint unchanged");
        CHECK(persist_services_calls == 0,
              "*** nothing in the registry was written -- no reconciliation happened ***");
        CHECK(service_registry_count() == 1,
              "...and the registration itself was not touched");
    }

    /* ═══ Scenario 4: re-registration updates in place ════════════════ */
    printf("\n-- Scenario 4: re-registration --\n");
    {
        CHECK(service_register(0, "api", pdb, SVC_ENDPOINT_IPC, 0x1003) == SVC_REG_OK,
              "re-registering an existing name succeeds");
        CHECK(service_registry_count() == 1,
              "...updating in place rather than creating a duplicate");

        struct SLSServiceLocation loc;
        service_resolve("api", &loc);
        CHECK(loc.partition_id == pdb && loc.endpoint_kind == SVC_ENDPOINT_IPC
              && loc.endpoint_port == 0x1003,
              "...and the new partition/endpoint is what now resolves");
    }

    /* ═══ Scenario 5: refusals ════════════════════════════════════════ */
    printf("\n-- Scenario 5: refusals --\n");
    {
        CHECK(service_register(0, "", pweb, SVC_ENDPOINT_TCP, 80) == SVC_REG_ERR_NAME,
              "an empty name is refused");

        char toolong[SERVICE_NAME_LEN + 8];
        memset(toolong, 'x', sizeof(toolong) - 1); toolong[sizeof(toolong)-1] = '\0';
        CHECK(service_register(0, toolong, pweb, SVC_ENDPOINT_TCP, 80) == SVC_REG_ERR_NAME,
              "an over-long name is refused rather than truncated into a different name");

        CHECK(service_register(0, "ghost", 200, SVC_ENDPOINT_TCP, 80) == SVC_REG_ERR_PARTITION,
              "registering into an undefined partition is refused -- it would resolve to a confidently wrong node");
        CHECK(service_register(0, "zero", pweb, SVC_ENDPOINT_TCP, 0) == SVC_REG_ERR_ENDPOINT,
              "port 0 is refused");
        CHECK(service_register(0, "weird", pweb, (SLSServiceEndpointKind)9, 80) == SVC_REG_ERR_ENDPOINT,
              "an unknown endpoint kind is refused");

        /* RBAC, both directions. */
        g_role = ROLE_APP_USER;
        CHECK(service_register(3, "sneaky", pweb, SVC_ENDPOINT_TCP, 80) == SVC_REG_ERR_PERM,
              "APP_USER cannot register a service");
        CHECK(service_unregister(3, "api") == SVC_REG_ERR_PERM,
              "APP_USER cannot unregister one either");
        struct SLSServiceLocation loc;
        CHECK(service_resolve("api", &loc) == SVC_REG_OK,
              "...but resolution is NOT role-gated -- reading where something lives is not privileged");
        g_role = ROLE_DB_ADMIN;
        CHECK(service_register(2, "admin-ok", pweb, SVC_ENDPOINT_TCP, 90) == SVC_REG_OK,
              "DB_ADMIN can register");
        g_role = ROLE_SYSTEM_KERNEL;

        CHECK(service_unregister(0, "not-there") == SVC_REG_ERR_NOT_FOUND,
              "unregistering an unknown name is reported, not silently ignored");
    }

    /* ═══ Scenario 6: a destroyed partition takes its services ════════ */
    printf("\n-- Scenario 6: partition_destroy() drops registrations --\n");
    {
        uint32_t ptmp = partition_create("doomed");
        CHECK(service_register(0, "doomed-svc-a", ptmp, SVC_ENDPOINT_TCP, 1) == SVC_REG_OK
           && service_register(0, "doomed-svc-b", ptmp, SVC_ENDPOINT_TCP, 2) == SVC_REG_OK,
              "two services register into a partition");
        CHECK(service_count_for_partition(ptmp) == 2, "both are counted against it");

        uint32_t others = service_registry_count() - 2;
        CHECK(partition_destroy(ptmp) == 0, "the real partition_destroy() runs");

        struct SLSServiceLocation loc;
        CHECK(service_resolve("doomed-svc-a", &loc) == SVC_REG_ERR_NOT_FOUND
           && service_resolve("doomed-svc-b", &loc) == SVC_REG_ERR_NOT_FOUND,
              "its services stop resolving -- rather than naming a partition that no longer exists");
        CHECK(service_count_for_partition(ptmp) == 0, "nothing remains registered to it");
        CHECK(service_registry_count() == others,
              "and services in OTHER partitions are untouched");
    }

    /* ═══ Scenario 7: capacity ════════════════════════════════════════ */
    printf("\n-- Scenario 7: capacity --\n");
    {
        service_registry_init();
        char nm[32];
        int registered = 0;
        for (int i = 0; i < SERVICE_MAX + 4; i++) {
            /* distinct names; a duplicate would update rather than fill a slot */
            nm[0]='s'; nm[1]='v'; nm[2]='c';
            nm[3]=(char)('0'+(i/100)%10); nm[4]=(char)('0'+(i/10)%10); nm[5]=(char)('0'+i%10);
            nm[6]='\0';
            if (service_register(0, nm, pweb, SVC_ENDPOINT_TCP, (uint32_t)(i+1)) == SVC_REG_OK)
                registered++;
        }
        CHECK(registered == SERVICE_MAX, "the registry fills to exactly SERVICE_MAX");
        CHECK(service_registry_count() == SERVICE_MAX, "...and holds that many");
        CHECK(service_register(0, "overflow", pweb, SVC_ENDPOINT_TCP, 999) == SVC_REG_ERR_FULL,
              "one more is refused with a distinct status, not silently dropped");

        CHECK(service_unregister(0, "svc000") == SVC_REG_OK, "freeing a slot works");
        CHECK(service_register(0, "overflow", pweb, SVC_ENDPOINT_TCP, 999) == SVC_REG_OK,
              "...and the freed slot is reusable");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
