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
#include "kernel/timer.h"
#include "kernel/microkernel.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ─── Phase 6 breaker stub ────────────────────────────────────────────
 * FAITHFUL: service_resolve() now reports the breaker state, and a
 * service with no recorded failures is CLOSED -- which is every service
 * in these tests, none of which report outcomes. Full breaker coverage is
 * in tests/service_mesh_host_test.c. */
int service_breaker_state(const char* n) { (void)n; return 0; /* CB_CLOSED */ }


/* ─── Endpoint-liveness probe sources ─────────────────────────────────
 * The real ones read tcp_conns[] (16 MiB) and the microkernel's
 * services[]. Settable stand-ins here so the probe's LOGIC can be driven
 * from both directions -- listening/not, ONLINE/CRASHED -- without
 * linking net/tcp.c or kernel/microkernel.c into a registry test. */
static int g_tcp_listening = 0;   /* port -> listening? (0 = none listening) */
static int g_ipc_state     = -1;  /* SVC_STATE_*, or -1 for "no supervised owner" */
int tcp_port_is_listening(uint16_t port) {
    return (g_tcp_listening != 0) && ((int)port == g_tcp_listening);
}
int mk_ipc_port_state(uint16_t port) { (void)port; return g_ipc_state; }


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
void dspp_partition_announce(uint32_t partition_id, const char* name, uint32_t owner_node_id) { (void)partition_id; (void)name; (void)owner_node_id; }
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
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
/* Paired with the relocate/send stubs: a test that stands in "nothing to
 * relocate" must also stand in "nothing to count", or partition_migrate()
 * sees 0 sent against a non-zero expectation and aborts every migration.
 * FAITHFUL -- this test registers no streams, so the real function would
 * also return 0. */
int stream_count_for_partition(uint32_t partition_id) { (void)partition_id; return 0; }

uint32_t simi_ctx_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }

/* ─── Replication transmit stubs ──────────────────────────────────────
 * COUNTED, not silent: scenario 8 asserts that registering announces and
 * unregistering withdraws, which is the half of replication this file
 * owns. The wire encode/decode and the dispatcher routing have their own
 * coverage in tests/cross_node_migration_host_test.c. */
static int announce_calls = 0, withdraw_calls = 0;
static uint8_t last_announced_serving = 0xFF;
void dspp_service_announce(const char* n, uint32_t p, uint8_t k, uint32_t e, uint32_t u, uint8_t sv) {
    (void)n; (void)p; (void)k; (void)e; (void)u;
    last_announced_serving = sv; announce_calls++;
}
void dspp_service_withdraw(const char* n) { (void)n; withdraw_calls++; }

/* ─── A controllable clock ────────────────────────────────────────────
 * kernel_tick_counter is the real kernel's ~100 Hz tick. Defining it here
 * lets the TTL scenarios move time deliberately instead of sleeping,
 * which would be both slow AND flaky -- a 20-second TTL cannot be waited
 * out in a test, and sleeping "about long enough" is exactly the kind of
 * assertion that fails on a loaded machine. */
volatile uint64_t kernel_tick_counter = 0;

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

    /* ═══ Scenario 8: cross-node replication ══════════════════════════
     * Phase 4's stated limitation: a name registered on node 1 did not
     * resolve on node 2. This drives the receive side directly (the wire
     * encode/decode has its own coverage in the DSPP test) and checks the
     * two properties that make a cache safe: local always wins, and only
     * the announcing node may withdraw. */
    printf("\n-- Scenario 8: a name registered elsewhere resolves here --\n");
    {
        service_registry_init();
        g_local_node = 1;
        uint32_t plocal = partition_create("local");
        partition_owner_table[plocal].node_id = 1;

        struct SLSServiceLocation loc;
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_ERR_NOT_FOUND,
              "*** before replication, a service on another node does NOT resolve ***");

        announce_calls = withdraw_calls = 0;
        service_register(0, "announced", plocal, SVC_ENDPOINT_TCP, 5000);
        CHECK(announce_calls == 1, "registering announces the name to the cluster");
        service_unregister(0, "announced");
        CHECK(withdraw_calls == 1, "unregistering withdraws it");
        CHECK(service_announce_all() == service_registry_count(),
              "announce_all re-announces every local registration (cluster re-sync)");

        service_remote_learn("far-svc", 7, 3, SVC_ENDPOINT_TCP, 9090, 0, SVC_SERVING_UP);
        CHECK(service_remote_count() == 1, "an announcement from node 7 is cached");
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_OK,
              "*** it now resolves on this node ***");
        CHECK(loc.node_id == 7, "...to the announcing node");
        CHECK(loc.partition_id == 3 && loc.endpoint_port == 9090,
              "...with its partition and endpoint");
        CHECK(!loc.is_local && loc.is_remote,
              "...and is marked remote, not local");

        /* Local must win. */
        CHECK(service_register(0, "far-svc", plocal, SVC_ENDPOINT_TCP, 1111) == SVC_REG_OK,
              "this node then registers the SAME name locally");
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_OK && loc.is_local
              && loc.endpoint_port == 1111,
              "*** the LOCAL registration wins -- a remote entry cannot shadow it ***");
        CHECK(service_remote_count() == 1,
              "...and the remote entry is still cached, just outranked");
        service_unregister(0, "far-svc");
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_OK && loc.is_remote,
              "removing the local one falls back to the remote entry");

        /* Only the owner may withdraw. */
        service_remote_forget("far-svc", 99);
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_OK,
              "a withdraw from a node that never announced it is ignored");
        service_remote_forget("far-svc", 7);
        CHECK(service_resolve("far-svc", &loc) == SVC_REG_ERR_NOT_FOUND,
              "...but the announcing node can withdraw it");

        /* Losing a whole node. */
        service_remote_learn("a", 7, 1, SVC_ENDPOINT_TCP, 1, 0, SVC_SERVING_UP);
        service_remote_learn("b", 7, 1, SVC_ENDPOINT_TCP, 2, 0, SVC_SERVING_UP);
        service_remote_learn("c", 8, 1, SVC_ENDPOINT_TCP, 3, 0, SVC_SERVING_UP);
        CHECK(service_remote_count() == 3, "three remote services from two nodes");
        CHECK(service_remote_forget_node(7) == 2, "dropping node 7 forgets exactly its two");
        CHECK(service_resolve("c", &loc) == SVC_REG_OK, "...leaving node 8's alone");

        /* Re-announcement is an update, not a duplicate. */
        service_remote_learn("c", 8, 1, SVC_ENDPOINT_TCP, 4444, 0, SVC_SERVING_UP);
        CHECK(service_remote_count() == 1, "a repeated announcement updates in place");
        service_resolve("c", &loc);
        CHECK(loc.endpoint_port == 4444, "...with the new endpoint");

        /* A node with no cluster identity must not be cached. */
        service_remote_learn("ghost", 0, 1, SVC_ENDPOINT_TCP, 1, 0, SVC_SERVING_UP);
        CHECK(service_resolve("ghost", &loc) == SVC_REG_ERR_NOT_FOUND,
              "an announcement from node 0 (the uninitialised sentinel) is refused");
    }

    /* ═══ Scenario 9: TTL and health on replicated entries ════════════
     * A node that dies silently -- power loss, cable pulled, panic --
     * never withdraws anything, because withdrawal requires it to send.
     * Without expiry its services resolve forever on every other node,
     * routing traffic into a hole. */
    printf("\n-- Scenario 9: cached entries age out --\n");
    {
        service_registry_init();
        g_local_node = 1;
        kernel_tick_counter = 10000;   /* arbitrary non-zero start */

        struct SLSServiceLocation loc;
        service_remote_learn("aged", 5, 2, SVC_ENDPOINT_TCP, 7000, 0, SVC_SERVING_UP);
        CHECK(service_resolve("aged", &loc) == SVC_REG_OK, "a freshly announced service resolves");
        CHECK(loc.health == SVC_HEALTH_FRESH, "...and reports FRESH");
        CHECK(service_remote_health("aged", kernel_tick_counter) == SVC_HEALTH_FRESH,
              "...as does a direct health query");

        /* Just before the stale threshold. */
        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS * 2 - 1;
        CHECK(service_resolve("aged", &loc) == SVC_REG_OK && loc.health == SVC_HEALTH_FRESH,
              "still FRESH just under two heartbeat intervals -- one dropped announcement is normal");

        /* Past stale, still inside TTL. */
        kernel_tick_counter += 2;
        CHECK(service_resolve("aged", &loc) == SVC_REG_OK,
              "*** a STALE entry still RESOLVES -- overdue is a warning, not a deletion ***");
        CHECK(loc.health == SVC_HEALTH_STALE, "...and says so");

        /* Past TTL. */
        kernel_tick_counter = 10000 + SERVICE_REMOTE_TTL_TICKS;
        CHECK(service_resolve("aged", &loc) == SVC_REG_ERR_NOT_FOUND,
              "*** past TTL it stops resolving -- a dead node stops attracting traffic ***");
        CHECK(service_remote_health("aged", kernel_tick_counter) == SVC_HEALTH_EXPIRED,
              "...and reports EXPIRED");

        /* THE property that makes this robust: expiry does not depend on
         * a sweep having run. The slot is still occupied at this point. */
        CHECK(service_remote_count() == 1,
              "the slot is still occupied -- nothing has swept yet");
        CHECK(service_resolve("aged", &loc) == SVC_REG_ERR_NOT_FOUND,
              "*** and it STILL does not resolve -- expiry is checked at lookup, not by the sweep ***");

        CHECK(service_remote_expire(kernel_tick_counter) == 1, "the sweep then reclaims the slot");
        CHECK(service_remote_count() == 0, "...and the cache is empty");
        CHECK(service_remote_expire(kernel_tick_counter) == 0, "a second sweep finds nothing to do");

        /* A heartbeat rescues an entry before it dies. */
        kernel_tick_counter = 20000;
        service_remote_learn("kept", 5, 2, SVC_ENDPOINT_TCP, 7001, 0, SVC_SERVING_UP);
        for (int i = 0; i < 10; i++) {
            kernel_tick_counter += SERVICE_HEARTBEAT_TICKS;
            service_remote_learn("kept", 5, 2, SVC_ENDPOINT_TCP, 7001, 0, SVC_SERVING_UP);   /* the heartbeat */
        }
        CHECK(service_resolve("kept", &loc) == SVC_REG_OK && loc.health == SVC_HEALTH_FRESH,
              "*** a heartbeated service stays FRESH indefinitely, well past one TTL ***");
        CHECK(service_remote_expire(kernel_tick_counter) == 0, "...and is never swept");

        /* Local entries do not age. */
        uint32_t plocal = partition_create("ttl-local");
        CHECK(service_register(0, "mine", plocal, SVC_ENDPOINT_TCP, 1234) == SVC_REG_OK,
              "a LOCAL service is registered");
        kernel_tick_counter += SERVICE_REMOTE_TTL_TICKS * 10;
        CHECK(service_resolve("mine", &loc) == SVC_REG_OK,
              "*** it still resolves after ten TTLs -- this node is authoritative for its own ***");
        CHECK(loc.health == SVC_HEALTH_FRESH, "...and is always FRESH");

        /* An unknown name answers the same as a dead one, on purpose. */
        CHECK(service_remote_health("never-existed", kernel_tick_counter) == SVC_HEALTH_EXPIRED,
              "a name never heard of reports EXPIRED -- same answer to 'should I route there'");

        /* ── A reading BEHIND the stamp ───────────────────────────────
         * kernel_tick_counter is incremented by whichever core takes the
         * timer IRQ, so a read here can occasionally be marginally behind
         * a stamp taken moments earlier. The age arithmetic saturates at
         * zero for exactly that case; an unsigned subtraction would wrap
         * to an astronomical age and instantly expire a healthy entry.
         *
         * Mutation testing added this: removing the saturation SURVIVED
         * the whole suite, because nothing ever moved the clock
         * backwards. */
        service_registry_init();
        kernel_tick_counter = 90000;
        service_remote_learn("skewed", 5, 1, SVC_ENDPOINT_TCP, 1, 0, SVC_SERVING_UP);
        kernel_tick_counter = 89999;            /* the read lands one tick behind */
        CHECK(service_remote_health("skewed", kernel_tick_counter) == SVC_HEALTH_FRESH,
              "*** a clock reading behind the stamp reads as age 0, not as a wrapped enormous age ***");
        CHECK(service_resolve("skewed", &loc) == SVC_REG_OK,
              "...so the entry still resolves rather than vanishing");
        CHECK(service_remote_expire(kernel_tick_counter) == 0,
              "...and the sweep does not reclaim it");
    }

    /* ═══ Scenario 10: the heartbeat itself ═══════════════════════════ */
    printf("\n-- Scenario 10: heartbeat pacing --\n");
    {
        service_registry_init();
        g_local_node = 1;
        uint32_t p = partition_create("beat");
        service_register(0, "s1", p, SVC_ENDPOINT_TCP, 1);
        service_register(0, "s2", p, SVC_ENDPOINT_TCP, 2);

        kernel_tick_counter = 50000;
        announce_calls = 0;
        CHECK(service_heartbeat_tick(kernel_tick_counter) == 2,
              "the first heartbeat announces immediately -- a fresh node is discoverable at once, not after a full interval");
        CHECK(announce_calls == 2, "...one announcement per local registration");

        announce_calls = 0;
        for (int i = 0; i < 20; i++) service_heartbeat_tick(kernel_tick_counter);
        CHECK(announce_calls == 0,
              "calling it repeatedly within the interval announces nothing -- it is paced, not spammed");

        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS;
        announce_calls = 0;
        CHECK(service_heartbeat_tick(kernel_tick_counter) == 2,
              "once the interval elapses it announces again");

        /* The pacing must survive a node with nothing to announce. */
        service_registry_init();
        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS;
        CHECK(service_heartbeat_tick(kernel_tick_counter) == 0,
              "a node with no local registrations announces nothing");
    }

    /* ═══ Scenario 11: ENDPOINT liveness, not just node liveness ══════
     * TTL answers "is the owning node still talking?". This answers the
     * different question "is the endpoint actually accepting?" -- and the
     * two are kept apart on purpose. A node can be perfectly healthy and
     * heartbeating while the service behind one of its ports has died. */
    printf("\n-- Scenario 11: endpoint liveness --\n");
    {
        service_registry_init();
        g_local_node = 1;
        kernel_tick_counter = 100000;
        uint32_t p = partition_create("live");

        /* TCP: a LISTEN socket is the observation. */
        g_tcp_listening = 8080;
        CHECK(service_probe_local(SVC_ENDPOINT_TCP, 8080) == SVC_SERVING_UP,
              "a TCP endpoint with something LISTENing probes UP");
        CHECK(service_probe_local(SVC_ENDPOINT_TCP, 9999) == SVC_SERVING_DOWN,
              "...and a port with nothing listening probes DOWN");

        /* IPC: the microkernel watchdog is the observation, and it is the
         * strongest signal available -- it comes from real crash events. */
        g_ipc_state = SVC_STATE_ONLINE;
        CHECK(service_probe_local(SVC_ENDPOINT_IPC, 0x1003) == SVC_SERVING_UP,
              "an IPC endpoint whose service the watchdog calls ONLINE probes UP");
        g_ipc_state = SVC_STATE_CRASHED;
        CHECK(service_probe_local(SVC_ENDPOINT_IPC, 0x1003) == SVC_SERVING_DOWN,
              "...CRASHED probes DOWN");
        g_ipc_state = SVC_STATE_DEGRADED;
        CHECK(service_probe_local(SVC_ENDPOINT_IPC, 0x1003) == SVC_SERVING_DOWN,
              "*** DEGRADED also probes DOWN -- routing to a degraded service is how it becomes an outage ***");
        g_ipc_state = -1;
        CHECK(service_probe_local(SVC_ENDPOINT_IPC, 0x1003) == SVC_SERVING_UNKNOWN,
              "an IPC port with no supervised owner is UNKNOWN, not guessed");

        /* Registration probes immediately. */
        g_tcp_listening = 8080;
        CHECK(service_register(0, "web", p, SVC_ENDPOINT_TCP, 8080) == SVC_REG_OK,
              "a service is registered while its port is listening");
        struct SLSServiceLocation loc;
        service_resolve("web", &loc);
        CHECK(loc.serving == SVC_SERVING_UP,
              "*** it resolves as UP immediately -- registration probes rather than waiting for a heartbeat ***");
        CHECK(loc.health == SVC_HEALTH_FRESH,
              "...and FRESH, which is a SEPARATE question about the information's age");

        /* The endpoint dies. The node is fine; the service is not. */
        g_tcp_listening = 0;
        CHECK(service_probe_all_local() == 1, "the probe pass notices exactly one change");
        service_resolve("web", &loc);
        CHECK(loc.serving == SVC_SERVING_DOWN,
              "*** the service now reports DOWN ***");
        CHECK(loc.health == SVC_HEALTH_FRESH,
              "*** while STILL reporting FRESH -- 'I know, and it is down' is not 'I have not heard' ***");
        CHECK(service_resolve("web", &loc) == SVC_REG_OK,
              "a down service still RESOLVES -- the registry reports, it does not hide");
        CHECK(service_probe_all_local() == 0, "a second probe pass sees no further change");

        /* It comes back. */
        g_tcp_listening = 8080;
        CHECK(service_probe_all_local() == 1, "recovery is noticed too");
        service_resolve("web", &loc);
        CHECK(loc.serving == SVC_SERVING_UP, "...and it reports UP again");

        /* The verdict travels with the heartbeat. */
        g_tcp_listening = 0;
        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS * 4;
        last_announced_serving = 0xFF;
        CHECK(service_heartbeat_tick(kernel_tick_counter) == 1, "the heartbeat fires");
        CHECK(last_announced_serving == SVC_SERVING_DOWN,
              "*** and announces DOWN -- the owning node probes its own endpoint and tells the cluster ***");

        /* A receiving node records what it was told. */
        service_registry_init();
        service_remote_learn("theirs", 9, 1, SVC_ENDPOINT_TCP, 1234, 0, SVC_SERVING_DOWN);
        CHECK(service_resolve("theirs", &loc) == SVC_REG_OK, "a remote entry resolves");
        CHECK(loc.serving == SVC_SERVING_DOWN,
              "*** carrying the owning node's verdict -- no cross-node probe was needed ***");
        CHECK(loc.is_remote && loc.health == SVC_HEALTH_FRESH,
              "...and is fresh remote information about a down endpoint");

        /* The two axes are genuinely independent: stale AND up. */
        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS * 2 + 1;
        service_registry_init();
        kernel_tick_counter = 200000;
        service_remote_learn("both", 9, 1, SVC_ENDPOINT_TCP, 1, 0, SVC_SERVING_UP);
        kernel_tick_counter += SERVICE_HEARTBEAT_TICKS * 2 + 1;
        service_resolve("both", &loc);
        CHECK(loc.health == SVC_HEALTH_STALE && loc.serving == SVC_SERVING_UP,
              "*** STALE + UP: the last thing we heard was good, but we have not heard lately ***");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
