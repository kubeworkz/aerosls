/*
 * service_mesh_host_test.c — Orchestration Plan Phase 6: circuit breaking
 * and per-service metrics. Links the REAL kernel/service_mesh.c and
 * kernel/service_registry.c.
 *
 * ─── The two things worth proving ─────────────────────────────────────
 * 1. THE BREAKER IS A CORRECT STATE MACHINE. A breaker that opens but
 *    never recovers is a permanent outage dressed up as protection; one
 *    that recovers too eagerly is a thundering herd. Scenario 2 walks the
 *    full CLOSED -> OPEN -> HALF_OPEN -> CLOSED cycle and scenario 3
 *    walks the failing branch back to OPEN.
 *
 * 2. THE WEDGED HANDLER IS FINALLY CAUGHT. Phase 4 closed with: "a
 *    process alive and holding its port but whose handler has wedged
 *    reports up ... that needs an application-level probe, which is not
 *    built." Scenario 5 shows it does not: a saturated IPC queue is a
 *    handler that has stopped draining, the kernel already measures it,
 *    and the breaker trips on it with no cooperation from anyone.
 *    That scenario is the reason this phase closed a gap rather than
 *    adding a layer.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/service_mesh_host_test \
 *       tests/service_mesh_host_test.c kernel/service_mesh.c \
 *       kernel/service_registry.c kernel/partition.c
 *   /tmp/service_mesh_host_test
 */
#include "kernel/service_mesh.h"
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/ipc.h"
#include "kernel/microkernel.h"
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

void kernel_serial_print(const char* s) { (void)s; }
void dspp_partition_announce(uint32_t partition_id, const char* name, uint32_t owner_node_id) { (void)partition_id; (void)name; (void)owner_node_id; }
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_services(void) { }
void persist_partitions(void) { }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
uint32_t cluster_local_node_id(void) { return 1; }
volatile uint64_t kernel_tick_counter = 0;

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
void dspp_service_announce(const char* n, uint32_t p, uint8_t k, uint32_t e,
                           uint32_t u, uint8_t sv) {
    (void)n;(void)p;(void)k;(void)e;(void)u;(void)sv;
}
void dspp_service_withdraw(const char* n) { (void)n; }

/* ─── Settable observation sources ────────────────────────────────────
 * The three signals the breaker is fed from, each drivable so the state
 * machine can be walked deliberately rather than waited on. */
static int g_tcp_listening = 0;
static int g_ipc_state     = -1;
static int g_queue_depth   = 0;
int tcp_port_is_listening(uint16_t port) {
    return g_tcp_listening != 0 && (int)port == g_tcp_listening;
}
int mk_ipc_port_state(uint16_t port) { (void)port; return g_ipc_state; }
int ipc_queue_depth(uint16_t port)   { (void)port; return g_queue_depth; }

int main(void) {
    printf("=== Circuit breaking + per-service metrics ===\n\n");
    printf("      (threshold %d consecutive failures, cooldown %u ticks,\n"
           "       IPC saturation at %d/%d of a %d-deep queue)\n\n",
           MESH_FAIL_THRESHOLD, MESH_OPEN_COOLDOWN_TICKS,
           MESH_QUEUE_SATURATED_NUM, MESH_QUEUE_SATURATED_DEN, IPC_QUEUE_DEPTH);

    partition_init();
    service_registry_init();
    mesh_init();
    uint32_t p = partition_create("mesh");
    kernel_tick_counter = 10000;
    uint64_t t = kernel_tick_counter;

    /* ═══ Scenario 1: an unknown service is not penalised ═════════════ */
    printf("-- Scenario 1: no history means no obstruction --\n");
    {
        CHECK(service_breaker_state("never-seen") == CB_CLOSED,
              "a service with no failure history reports CLOSED");
        CHECK(service_call_permitted("never-seen", kernel_tick_counter),
              "*** and calls to it are permitted -- an unknown service is not a broken one ***");
        CHECK(service_metrics("never-seen") == 0,
              "...with no metrics entry conjured up merely by asking");
    }

    /* ═══ Scenario 2: the full recovery cycle ═════════════════════════ */
    printf("\n-- Scenario 2: CLOSED -> OPEN -> HALF_OPEN -> CLOSED --\n");
    {
        for (int i = 0; i < MESH_FAIL_THRESHOLD - 1; i++) service_report_failure("api", t);
        CHECK(service_breaker_state("api") == CB_CLOSED,
              "one failure short of the threshold, the breaker is still closed");
        CHECK(service_call_permitted("api", kernel_tick_counter),
              "...and calls still go through -- a burst of failures is not yet an outage");

        service_report_failure("api", t);
        CHECK(service_breaker_state("api") == CB_OPEN,
              "*** the threshold-th consecutive failure opens it ***");
        CHECK(!service_call_permitted("api", kernel_tick_counter),
              "*** and calls are now refused ***");

        /* The cooldown must actually be waited out. */
        CHECK(!service_call_permitted("api", kernel_tick_counter + MESH_OPEN_COOLDOWN_TICKS - 1),
              "one tick before the cooldown elapses, still refused");

        uint64_t after = kernel_tick_counter + MESH_OPEN_COOLDOWN_TICKS;
        CHECK(service_call_permitted("api", after),
              "*** once it elapses, ONE trial call is admitted ***");
        CHECK(service_breaker_state("api") == CB_HALF_OPEN, "...and the state is HALF_OPEN");
        CHECK(!service_call_permitted("api", after),
              "*** a SECOND caller is refused -- one trial, not a herd ***");
        CHECK(!service_call_permitted("api", after + 100000),
              "...and stays refused however long it waits, until the trial reports");

        service_report_success("api", t);
        CHECK(service_breaker_state("api") == CB_CLOSED,
              "*** the trial succeeds and the breaker closes ***");
        CHECK(service_call_permitted("api", after) && service_call_permitted("api", after),
              "...traffic flows freely again");
    }

    /* ═══ Scenario 3: a trial that fails ══════════════════════════════ */
    printf("\n-- Scenario 3: the trial fails --\n");
    {
        mesh_init();
        t = 50000;
        for (int i = 0; i < MESH_FAIL_THRESHOLD; i++) service_report_failure("db", t);
        CHECK(service_breaker_state("db") == CB_OPEN, "the breaker opens");

        kernel_tick_counter = t;
        CHECK(service_call_permitted("db", t + MESH_OPEN_COOLDOWN_TICKS),
              "a trial is admitted after the cooldown");
        service_report_failure("db", t);
        CHECK(service_breaker_state("db") == CB_OPEN,
              "*** the trial fails and it re-opens IMMEDIATELY -- no second threshold to re-reach ***");

        const struct SLSMeshEntry* m = service_metrics("db");
        CHECK(m && m->trips == 2, "...counted as a second trip, not a continuation of the first");
    }

    /* ═══ Scenario 4: success resets the run ══════════════════════════ */
    printf("\n-- Scenario 4: consecutive means consecutive --\n");
    {
        mesh_init();
        for (int i = 0; i < MESH_FAIL_THRESHOLD - 1; i++) service_report_failure("flappy", t);
        service_report_success("flappy", t);
        for (int i = 0; i < MESH_FAIL_THRESHOLD - 1; i++) service_report_failure("flappy", t);
        CHECK(service_breaker_state("flappy") == CB_CLOSED,
              "*** an intermittently-failing service does not accumulate its way to OPEN ***");
        const struct SLSMeshEntry* m = service_metrics("flappy");
        CHECK(m && m->failures == (uint64_t)(2 * (MESH_FAIL_THRESHOLD - 1)),
              "...though every failure is still counted in the metrics");
        CHECK(m && m->successes == 1, "...alongside the success");
    }

    /* ═══ Scenario 5: THE WEDGED HANDLER ══════════════════════════════
     * The gap Phase 4 named and could not close. */
    printf("\n-- Scenario 5: a wedged handler, caught with no cooperation --\n");
    {
        mesh_init();
        service_registry_init();
        t = 90000;

        /* A perfectly healthy-looking IPC service: port bound, watchdog
         * says ONLINE, queue draining normally. */
        g_ipc_state   = SVC_STATE_ONLINE;
        g_queue_depth = 0;
        CHECK(service_register(0, "wedge", p, SVC_ENDPOINT_IPC, 0x1003) == SVC_REG_OK,
              "an IPC service is registered");
        struct SLSServiceLocation loc;
        service_resolve("wedge", &loc);
        CHECK(loc.serving == SVC_SERVING_UP,
              "*** the endpoint probe says UP -- the process is alive and holding its port ***");

        for (int i = 0; i < 20; i++) mesh_observe_local(t);
        CHECK(service_breaker_state("wedge") == CB_CLOSED,
              "...and a healthy service is left alone across many observations");

        /* Now the handler wedges. The process does not die, the port stays
         * bound, the watchdog stays happy -- messages just stop being
         * consumed and the queue fills. */
        g_queue_depth = IPC_QUEUE_DEPTH;
        service_probe_all_local();
        service_resolve("wedge", &loc);
        CHECK(loc.serving == SVC_SERVING_UP,
              "*** the probe STILL says UP -- this is exactly the case Phase 4 could not see ***");

        uint32_t tripped = 0;
        for (int i = 0; i < MESH_FAIL_THRESHOLD; i++) tripped += mesh_observe_local(t);
        CHECK(tripped == 1, "the observation pass trips exactly one breaker");
        CHECK(service_breaker_state("wedge") == CB_OPEN,
              "*** the breaker OPENS on queue saturation -- the wedged handler is caught ***");
        CHECK(!service_call_permitted("wedge", t),
              "...and traffic stops being sent into it");

        /* And it recovers when the handler starts draining again. */
        g_queue_depth = 0;
        CHECK(service_call_permitted("wedge", t + MESH_OPEN_COOLDOWN_TICKS),
              "after the cooldown a trial is admitted");
        service_report_success("wedge", t);
        CHECK(service_breaker_state("wedge") == CB_CLOSED,
              "*** and a drained queue plus a successful call closes it ***");
    }

    /* ═══ Scenario 6: the other automatic signal ══════════════════════ */
    printf("\n-- Scenario 6: a dead endpoint also trips it --\n");
    {
        mesh_init();
        service_registry_init();
        t = 120000;
        g_tcp_listening = 8080;
        CHECK(service_register(0, "web", p, SVC_ENDPOINT_TCP, 8080) == SVC_REG_OK,
              "a TCP service is registered while listening");

        g_tcp_listening = 0;                /* the listener goes away */
        service_probe_all_local();
        for (int i = 0; i < MESH_FAIL_THRESHOLD; i++) mesh_observe_local(t);
        CHECK(service_breaker_state("web") == CB_OPEN,
              "a DOWN endpoint trips the breaker without any caller reporting anything");

        /* A healthy observation must not close it by itself. */
        g_tcp_listening = 8080;
        service_probe_all_local();
        for (int i = 0; i < 50; i++) mesh_observe_local(t);
        CHECK(service_breaker_state("web") == CB_OPEN,
              "*** observing health does NOT close the breaker -- only a real trial call does ***");
        CHECK(service_call_permitted("web", t + MESH_OPEN_COOLDOWN_TICKS),
              "...the cooldown still governs when that trial happens");
    }

    /* ═══ Scenario 7: metrics and manual reset ════════════════════════ */
    printf("\n-- Scenario 7: metrics and operator override --\n");
    {
        mesh_init();
        t = 200000;
        service_call_permitted("m", t);          /* creates nothing -- no entry yet */
        service_report_success("m", t);
        service_report_success("m", t);
        service_report_failure("m", t);
        const struct SLSMeshEntry* m = service_metrics("m");
        CHECK(m && m->successes == 2 && m->failures == 1, "successes and failures are counted");
        CHECK(m && m->consecutive_failures == 1, "...and the current run is tracked separately");

        for (int i = 0; i < MESH_FAIL_THRESHOLD; i++) service_report_failure("m", t);
        CHECK(service_breaker_state("m") == CB_OPEN, "it opens");
        service_call_permitted("m", t);          /* refused, counted */
        m = service_metrics("m");
        CHECK(m && m->calls_refused >= 1, "refusals are counted, so the cost of the outage is visible");

        CHECK(service_breaker_reset("m") == 0, "an operator can reset it");
        CHECK(service_breaker_state("m") == CB_CLOSED, "...and it closes immediately");
        CHECK(service_call_permitted("m", t), "...without waiting out the cooldown");
        m = service_metrics("m");
        CHECK(m && m->trips >= 1 && m->failures >= 1,
              "*** but the history is NOT erased -- a reset clears the state, not the record ***");
        CHECK(service_breaker_reset("nope") != 0, "resetting an unknown service reports failure");
    }

    /* ═══ Scenario 8: clock skew ══════════════════════════════════════ */
    printf("\n-- Scenario 8: a clock reading behind the stamp --\n");
    {
        mesh_init();
        t = 300000;
        kernel_tick_counter = t;
        for (int i = 0; i < MESH_FAIL_THRESHOLD; i++) service_report_failure("skew", t);
        CHECK(service_breaker_state("skew") == CB_OPEN, "the breaker opens at t");
        /* kernel_tick_counter is incremented by whichever core takes the
         * timer IRQ, so a reading can land behind the stamp. An unsigned
         * subtraction would wrap to an enormous elapsed time and admit a
         * trial instantly. */
        CHECK(!service_call_permitted("skew", t - 1),
              "*** a reading one tick behind does not wrap into 'the cooldown elapsed' ***");
        CHECK(service_breaker_state("skew") == CB_OPEN, "...and the state is unchanged");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
