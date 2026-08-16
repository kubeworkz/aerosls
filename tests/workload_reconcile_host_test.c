/*
 * workload_reconcile_host_test.c — Orchestration Plan Phase 5: declarative
 * workloads and the bounded reconciler. Links the REAL, unmodified
 * kernel/workload.c, kernel/service_registry.c and kernel/partition.c.
 *
 * ─── What makes this one different from the earlier phases ────────────
 * Everything before this was reactive: something happened, code responded.
 * This is the first thing in the system that ACTS ON ITS OWN. The failure
 * modes are therefore different in kind, and the scenarios target them
 * specifically:
 *
 *   - Acting when it should not (scenario 1: OFF by default, and OFF means
 *     genuinely nothing, not "log but do it anyway").
 *   - Never settling (scenario 3: convergence must reach a fixed point and
 *     STAY there -- a reconciler that keeps acting on a converged system
 *     is an infinite work generator, and the symptom in production is
 *     wear, not an error).
 *   - Acting on the wrong core (scenario 5: reconcile_tick() runs on the
 *     AP core and must never call persist_*(). Asserted by counting.)
 *   - Losing work silently under pressure (scenario 6: queue overflow is
 *     counted, and the dropped intent is re-derived rather than lost).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/workload_reconcile_host_test \
 *       tests/workload_reconcile_host_test.c kernel/workload.c \
 *       kernel/service_registry.c kernel/partition.c
 *   /tmp/workload_reconcile_host_test
 */
#include "kernel/workload.h"
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
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


/* kernel/service_registry.c now stamps replicated entries with the
 * kernel tick for TTL purposes. These tests exercise no replication, so a
 * clock frozen at 0 is faithful: every local entry they use is
 * authoritative and never ages. */
volatile uint64_t kernel_tick_counter = 0;


/* ─── Orchestration Phase 4 gap: registry replication transmit ────────
 * FAITHFUL: these tests set no cluster identity (node id stays 0, the
 * "uninitialised" sentinel), and the real dspp_service_announce() returns
 * immediately in that state without transmitting. Doing nothing is
 * exactly what the real one does here. */
void dspp_service_announce(const char* n, uint32_t p, uint8_t k, uint32_t e, uint32_t u, uint8_t sv) {
    (void)n; (void)p; (void)k; (void)e; (void)u; (void)sv;
}
void dspp_service_withdraw(const char* n) { (void)n; }


static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_failed += 0; checks_passed++; } \
} while (0)

void kernel_serial_print(const char* s) { (void)s; }
void dspp_partition_announce(uint32_t partition_id, const char* name, uint32_t owner_node_id) { (void)partition_id; (void)name; (void)owner_node_id; }
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
void dspp_partition_ownedset_send(uint32_t generation, const uint32_t* partition_ids, uint32_t count) { (void)generation; (void)partition_ids; (void)count; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* Persist calls are COUNTED, because "the reconciler must not persist on
 * the AP core" is an invariant this file has to be able to assert, not
 * just describe. */
static int persist_services_calls  = 0;
static int persist_workloads_calls = 0;
void persist_services(void)  { persist_services_calls++; }
void persist_workloads(void) { persist_workloads_calls++; }
void persist_partitions(void) { }

static SLSRole g_role = ROLE_SYSTEM_KERNEL;
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return g_role; }

static uint32_t g_local_node = 0;
uint32_t cluster_local_node_id(void) { return g_local_node; }
uint32_t cluster_leader_id(void) { return 0; }  /* resurrected-owner conflict resolution (partition_sync_upsert): no consensus layer here, so no leader known */

struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t process_kill_partition(uint32_t p)  { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int  partition_lease_step_down(uint32_t p) { (void)p; return 1; }
int  stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
uint32_t simi_ctx_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }

/* ─── Live-context stubs ──────────────────────────────────────────────
 * This file exercises the RECONCILER, not the context producer. No
 * workload here declares a program, so the real wlctx_has() would return
 * 0 for every one of them -- which is exactly what this returns. The
 * producer has its own end-to-end coverage in
 * tests/workload_ctx_host_test.c. */
int         wlctx_has(const char* w) { (void)w; return 0; }
const char* wlctx_status_name(int s) { (void)s; return "OK"; }
int         wlctx_start(const char* w, const unsigned char* i, uint32_t n,
                        const char* e, uint32_t p) {
    (void)w; (void)i; (void)n; (void)e; (void)p; return 0;
}
int         wlctx_stop(const char* w) { (void)w; return 0; }
/* Phase 7 restart seam. FAITHFUL: no workload here declares a program, so
 * there is never a live context to have a status. -1 is exactly what the
 * real one returns for that. */
int         wlctx_status_of(const char* w) { (void)w; return -1; }
int         wlctx_restart(const char* w) { (void)w; return 0; }
/* FAITHFUL: no program is uploaded in this test, so the real lookup
 * would find nothing and return 0 -- exactly what this returns. */
const unsigned char* workload_find_program(const char* n, uint32_t* sz) {
    (void)n; (void)sz; return 0;
}


/* One full convergence cycle as the running kernel performs it: the AP
 * core sweeps, then the BSP drains. Kept as one helper so no scenario can
 * accidentally test a half-cycle it did not mean to. */
static uint32_t cycle(void) {
    uint32_t acted = reconcile_tick();   /* AP core */
    reconcile_drain();                   /* BSP */
    return acted;
}

/* Paired with the relocate/send stubs: a test that stands in "nothing to
 * relocate" must also stand in "nothing to count", or partition_migrate()
 * sees 0 sent against a non-zero expectation and aborts every migration.
 * FAITHFUL -- this test registers no streams, so the real function would
 * also return 0. */
int stream_count_for_partition(uint32_t partition_id) { (void)partition_id; return 0; }

int main(void) {
    printf("=== Declarative workloads + bounded reconciler ===\n\n");

    partition_init();
    service_registry_init();
    workload_init();

    uint32_t papi = partition_create("api");
    CHECK(papi != 0xFFFFFFFFu, "setup: a partition exists");

    /* ═══ Scenario 1: OFF by default, and OFF means nothing happens ═══ */
    printf("\n-- Scenario 1: the reconciler is off until asked --\n");
    {
        CHECK(!reconcile_is_enabled(),
              "the reconciler is DISABLED at init -- something autonomous is opt-in");

        CHECK(workload_declare(0, "api", papi, WL_DESIRED_RUNNING, "api-svc",
                               SVC_ENDPOINT_TCP, 8080, "", "", WL_RESTART_NEVER) == WL_OK,
              "a workload is declared");
        partition_pause(papi);

        persist_services_calls = 0;
        uint32_t acted = cycle();
        CHECK(acted == 0, "a disabled reconciler takes no action at all");
        CHECK(partition_is_paused(papi), "...the paused partition stays paused");
        struct SLSServiceLocation loc;
        CHECK(service_resolve("api-svc", &loc) == SVC_REG_ERR_NOT_FOUND,
              "...and the declared service is not registered");
        CHECK(persist_services_calls == 0, "...and nothing was written");
    }

    /* ═══ Scenario 2: it converges ════════════════════════════════════ */
    printf("\n-- Scenario 2: enabled, it converges on the declaration --\n");
    {
        reconcile_set_enabled(1);
        CHECK(reconcile_is_enabled(), "the reconciler can be enabled");

        uint32_t acted = cycle();
        CHECK(acted == 2, "one sweep takes both outstanding actions (resume + register)");
        CHECK(!partition_is_paused(papi),
              "the partition was resumed to match desired=RUNNING");

        struct SLSServiceLocation loc;
        CHECK(service_resolve("api-svc", &loc) == SVC_REG_OK,
              "the declared service was registered");
        CHECK(loc.partition_id == papi && loc.endpoint_port == 8080
              && loc.endpoint_kind == SVC_ENDPOINT_TCP,
              "...with exactly the declared partition and endpoint");
    }

    /* ═══ Scenario 3: it SETTLES ══════════════════════════════════════
     * The failure this catches has no error message: a reconciler that
     * keeps acting on an already-correct system just burns work forever. */
    printf("\n-- Scenario 3: convergence is a fixed point --\n");
    {
        CHECK(cycle() == 0, "a converged system produces no actions");
        int quiet = 1;
        for (int i = 0; i < 50; i++) if (cycle() != 0) { quiet = 0; break; }
        CHECK(quiet, "...and stays quiet across 50 further sweeps -- it settles, it does not churn");

        struct SLSWorkloadEntry* w = workload_find("api");
        CHECK(w && w->converged, "the entry reports itself converged");
        /* 1, not 2: the partition resume is decided per PARTITION (union
         * semantics) and so is not attributed to any single workload --
         * see the actions_taken comment in workload.h. The workload's own
         * action is its service registration. */
        CHECK(w && w->actions_taken == 1,
              "...having taken exactly the 1 action attributable to it, and no more");

        persist_services_calls = 0;
        cycle();
        CHECK(persist_services_calls == 0,
              "a converged sweep writes nothing -- no persist churn either");
    }

    /* ═══ Scenario 4: drift is corrected ══════════════════════════════ */
    printf("\n-- Scenario 4: drift away from the declaration is corrected --\n");
    {
        partition_pause(papi);                  /* someone pauses it out of band */
        CHECK(cycle() == 1, "an out-of-band pause is detected and one action taken");
        CHECK(!partition_is_paused(papi), "...the partition is resumed again");

        service_unregister(0, "api-svc");       /* someone removes the registration */
        CHECK(cycle() == 1, "an out-of-band unregistration is detected");
        struct SLSServiceLocation loc;
        CHECK(service_resolve("api-svc", &loc) == SVC_REG_OK, "...and re-registered");

        /* Endpoint drift, not just presence -- a registration that exists
         * but points somewhere else is just as wrong as a missing one. */
        service_register(0, "api-svc", papi, SVC_ENDPOINT_TCP, 9999);
        CHECK(cycle() == 1, "a service registered on the WRONG port is detected");
        service_resolve("api-svc", &loc);
        CHECK(loc.endpoint_port == 8080, "...and corrected back to the declared port");
        CHECK(cycle() == 0, "...then settles again");
    }

    /* ═══ Scenario 5: THE CONCURRENCY INVARIANT ═══════════════════════
     * reconcile_tick() runs on the AP core. persist_*() runs on the BSP
     * and shares an unlocked DMA staging buffer. The sweep must therefore
     * perform zero persisting calls itself -- all of them must come out of
     * the drain. This is the invariant the plan flagged as the one likely
     * to cause a rare, undebuggable corruption if got wrong. */
    printf("\n-- Scenario 5: the AP-core sweep never persists --\n");
    {
        service_unregister(0, "api-svc");
        partition_pause(papi);

        persist_services_calls = 0;
        persist_workloads_calls = 0;
        uint32_t acted = reconcile_tick();      /* AP core ONLY -- no drain */
        CHECK(acted == 2, "the sweep found both actions");
        CHECK(persist_services_calls == 0,
              "*** reconcile_tick() called persist_services() ZERO times ***");
        CHECK(persist_workloads_calls == 0,
              "*** and persist_workloads() zero times ***");
        CHECK(reconcile_queue_depth() == 1,
              "the persisting action is sitting in the queue, not applied");
        CHECK(!partition_is_paused(papi),
              "the NON-persisting action was applied directly -- that is allowed on the AP core");

        /* The converged flag has to be falsifiable, not just true when
         * things are fine. Mutation testing found this hole: hard-coding
         * converged=1 passed every check until this one existed. */
        struct SLSWorkloadEntry* w = workload_find("api");
        CHECK(w && !w->converged,
              "an entry with outstanding work reports itself NOT converged");

        uint32_t applied = reconcile_drain();   /* BSP */
        CHECK(applied == 1, "the BSP drain applies the queued intent");
        CHECK(persist_services_calls == 1, "...and THAT is where the persist happened");
        CHECK(reconcile_queue_depth() == 0, "the queue is empty again");
        reconcile_tick();
        CHECK(w && w->converged, "...and once satisfied it reports converged again");
    }

    /* ═══ Scenario 6: queue overflow is bounded and recoverable ═══════
     * The ring is deliberately sized so ONE sweep cannot overflow it (see
     * the _Static_assert in workload.c). Overflow therefore means exactly
     * one thing: the BSP stopped draining. That is how it is provoked
     * here -- sweep repeatedly with no drain at all -- rather than by
     * inventing an impossible single-sweep flood. */
    printf("\n-- Scenario 6: queue overflow --\n");
    {
        workload_init();
        service_registry_init();

        int declared = 0;
        char nm[24], sv[24];
        for (int i = 0; i < WORKLOAD_MAX; i++) {
            nm[0]='w'; nm[1]='l';
            nm[2]=(char)('0'+(i/10)%10); nm[3]=(char)('0'+i%10); nm[4]='\0';
            sv[0]='s'; sv[1]='v';
            sv[2]=(char)('0'+(i/10)%10); sv[3]=(char)('0'+i%10); sv[4]='\0';
            if (workload_declare(0, nm, papi, WL_DESIRED_RUNNING, sv, SVC_ENDPOINT_TCP,
                                 (uint32_t)(1000+i), "", "", WL_RESTART_NEVER) == WL_OK) declared++;
        }
        CHECK(declared == WORKLOAD_MAX, "a full table of workloads is declared");

        uint32_t dropped_before = reconcile_queue_dropped();
        reconcile_tick();
        CHECK(reconcile_queue_dropped() == dropped_before,
              "a single sweep does NOT overflow -- the ring is sized to absorb one");

        /* Now stop draining, as a stalled BSP would. */
        for (int i = 0; i < 4; i++) reconcile_tick();
        CHECK(reconcile_queue_dropped() > dropped_before,
              "sweeping without draining DOES overflow, and it is COUNTED, not silent");

        /* Recovery: drain and re-sweep until quiet. Dropped intents must
         * be re-derived, because the sweep reads ACTUAL state rather than
         * trusting that a past enqueue succeeded. */
        int settled = 0;
        for (int i = 0; i < 60; i++) {
            reconcile_drain();
            if (reconcile_tick() == 0) { reconcile_drain(); settled = 1; break; }
        }
        CHECK(settled, "*** it still converges after overflow -- dropped intents are re-derived ***");

        uint32_t unresolved = 0;
        struct SLSServiceLocation loc;
        for (int i = 0; i < WORKLOAD_MAX; i++) {
            sv[0]='s'; sv[1]='v';
            sv[2]=(char)('0'+(i/10)%10); sv[3]=(char)('0'+i%10); sv[4]='\0';
            if (service_resolve(sv, &loc) != SVC_REG_OK) unresolved++;
        }
        CHECK(unresolved == 0, "...and every declared service ended up registered");
    }

    /* ═══ Scenario 7: desired STOPPED, and shared-partition semantics ═══
     * The second half is the one that matters. Deciding a partition's
     * run-state per workload made two declarations in one partition
     * oscillate: each sweep one paused it and the other resumed it, and
     * the reconciler never settled. Union semantics fixed that, and this
     * asserts the fix rather than the symptom. */
    printf("\n-- Scenario 7: desired STOPPED --\n");
    {
        workload_init();
        service_registry_init();
        uint32_t psolo = partition_create("solo");

        CHECK(workload_declare(0, "only", psolo, WL_DESIRED_RUNNING, "only-svc",
                               SVC_ENDPOINT_TCP, 8080, "", "", WL_RESTART_NEVER) == WL_OK, "a lone workload wants RUNNING");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(!partition_is_paused(psolo), "its partition is running");

        CHECK(workload_declare(0, "only", psolo, WL_DESIRED_STOPPED, "only-svc",
                               SVC_ENDPOINT_TCP, 8080, "", "", WL_RESTART_NEVER) == WL_OK, "it is flipped to STOPPED");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(partition_is_paused(psolo), "the partition is paused");
        struct SLSServiceLocation loc;
        CHECK(service_resolve("only-svc", &loc) == SVC_REG_ERR_NOT_FOUND,
              "and the service is withdrawn -- a stopped workload stops advertising");
        CHECK(cycle() == 0, "it settles");

        /* Two workloads, one partition, opposite desires. */
        CHECK(workload_declare(0, "peer", psolo, WL_DESIRED_RUNNING, "peer-svc",
                               SVC_ENDPOINT_TCP, 8081, "", "", WL_RESTART_NEVER) == WL_OK,
              "a SECOND workload in the same partition wants RUNNING");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(!partition_is_paused(psolo),
              "*** the partition runs: one workload wanting it up outvotes one wanting it down ***");

        int quiet = 1;
        for (int i = 0; i < 30; i++) if (cycle() != 0) { quiet = 0; break; }
        CHECK(quiet, "*** and it SETTLES -- contradictory declarations do not oscillate ***");

        CHECK(service_resolve("peer-svc", &loc) == SVC_REG_OK,
              "the running workload's service is advertised");
        CHECK(service_resolve("only-svc", &loc) == SVC_REG_ERR_NOT_FOUND,
              "...while the stopped one's stays withdrawn, in the same live partition");

        /* Order matters, and testing only one order is not enough.
         * Mutation testing showed it: replacing the union with "last
         * workload in table order wins" still PASSED the checks above,
         * because there the STOPPED declaration happened to sit at a
         * lower index than the RUNNING one. Declaring a STOPPED workload
         * AFTER the running one exercises the opposite order, where
         * last-wins gives the wrong answer. */
        CHECK(workload_declare(0, "zlast", psolo, WL_DESIRED_STOPPED, "zlast-svc",
                               SVC_ENDPOINT_TCP, 8082, "", "", WL_RESTART_NEVER) == WL_OK,
              "a THIRD workload, wanting STOPPED, is declared after the running one");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(!partition_is_paused(psolo),
              "*** the partition STILL runs -- union, not last-declaration-wins ***");
        int quiet2 = 1;
        for (int i = 0; i < 30; i++) if (cycle() != 0) { quiet2 = 0; break; }
        CHECK(quiet2, "...and still settles with three competing declarations");
        CHECK(service_resolve("peer-svc", &loc) == SVC_REG_OK,
              "the running workload keeps its service through all of it");
    }

    /* ═══ Scenario 8: a declaration that cannot converge ══════════════ */
    printf("\n-- Scenario 8: unconvergeable declarations do not spin --\n");
    {
        workload_init();
        service_registry_init();
        uint32_t pdoom = partition_create("doomed");
        CHECK(workload_declare(0, "ghost", pdoom, WL_DESIRED_RUNNING, "ghost-svc",
                               SVC_ENDPOINT_TCP, 7000, "", "", WL_RESTART_NEVER) == WL_OK,
              "a workload is declared in a partition");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(cycle() == 0, "it converges");

        partition_destroy(pdoom);
        CHECK(!partition_exists(pdoom), "its partition is then destroyed");
        CHECK(cycle() == 0,
              "the reconciler takes NO action for a partition that no longer exists");
        int quiet = 1;
        for (int i = 0; i < 20; i++) if (cycle() != 0) { quiet = 0; break; }
        CHECK(quiet, "...and does not spin retrying something that can never succeed");
    }

    /* ═══ Scenario 9: declaration validation ══════════════════════════ */
    printf("\n-- Scenario 9: declarations are validated up front --\n");
    {
        CHECK(workload_declare(0, "", papi, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_ERR_NAME, "an empty name is refused");
        CHECK(workload_declare(0, "bad", 250, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_ERR_PARTITION, "an undefined partition is refused");
        CHECK(workload_declare(0, "halfsvc", papi, WL_DESIRED_RUNNING, "s", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_ERR_ENDPOINT,
              "a service name with no port is refused -- it could never converge");
        CHECK(workload_declare(0, "nosvc", papi, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_OK,
              "a workload declaring NO service is legal");

        g_role = ROLE_APP_USER;
        CHECK(workload_declare(3, "sneaky", papi, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_ERR_PERM, "APP_USER cannot declare a workload");
        CHECK(workload_delete(3, "nosvc") == WL_ERR_PERM, "...nor delete one");
        g_role = ROLE_SYSTEM_KERNEL;

        CHECK(workload_delete(0, "nope") == WL_ERR_NOT_FOUND,
              "deleting an unknown workload is reported");

        persist_workloads_calls = 0;
        CHECK(workload_declare(0, "persisted", papi, WL_DESIRED_STOPPED, "",
                               SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_OK, "a declaration succeeds");
        CHECK(persist_workloads_calls == 1,
              "declarations persist immediately -- a declarative spec that vanishes on reboot is not declarative");
        CHECK(workload_delete(0, "persisted") == WL_OK && persist_workloads_calls == 2,
              "and so does deletion");
    }

    /* ═══ Scenario 10: capacity ═══════════════════════════════════════ */
    printf("\n-- Scenario 10: capacity --\n");
    {
        workload_init();
        char nm[24];
        int ok = 0;
        for (int i = 0; i < WORKLOAD_MAX + 4; i++) {
            nm[0]='w';
            nm[1]=(char)('0'+(i/100)%10); nm[2]=(char)('0'+(i/10)%10); nm[3]=(char)('0'+i%10);
            nm[4]='\0';
            if (workload_declare(0, nm, papi, WL_DESIRED_STOPPED, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_OK)
                ok++;
        }
        CHECK(ok == WORKLOAD_MAX, "the table fills to exactly WORKLOAD_MAX");
        CHECK(workload_declare(0, "extra", papi, WL_DESIRED_STOPPED, "", SVC_ENDPOINT_TCP, 0, "", "", WL_RESTART_NEVER) == WL_ERR_FULL, "one more is refused with a distinct status");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
