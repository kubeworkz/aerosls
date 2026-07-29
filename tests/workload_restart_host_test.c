/*
 * workload_restart_host_test.c — Phase 7: self-healing restarts.
 *
 * ─── The distinction this phase turns on ──────────────────────────────
 * A context that HALTED is **not** a context that failed. It returned
 * from its top-level frame — it finished. Restarting it on that basis
 * turns a batch job into an infinite loop, and nothing observable can
 * tell you whether that is wrong, because it depends on what the operator
 * meant. Scenario 2 is the whole phase in one test: the same terminal
 * context, under three different policies, gets three different and
 * correct answers.
 *
 * ─── The hazard being bounded ─────────────────────────────────────────
 * A crash-looping workload must not be able to burn the machine. Scenario
 * 4 runs one for hundreds of sweeps and requires the attempts to be
 * spaced by a growing backoff and to STOP at the limit — a restarter with
 * no give-up is a denial-of-service the operator asked for by accident.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I tools/simi \
 *       -o /tmp/workload_restart_host_test \
 *       tests/workload_restart_host_test.c kernel/workload.c kernel/workload_ctx.c \
 *       kernel/simi_ctx_migrate.c kernel/simi_ckpt.c kernel/simi_interp.c \
 *       kernel/service_registry.c kernel/service_mesh.c kernel/partition.c
 *   /tmp/workload_restart_host_test
 */
#include "kernel/workload.h"
#include "kernel/workload_ctx.h"
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/ipc.h"
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

/* Counts "GIVING UP" announcements. The give-up FLAG is otherwise
 * behaviourally equivalent to the attempt count -- both block the
 * restart -- so mutation testing showed that ignoring the flag survived.
 * What the flag uniquely does is stop the decision being re-announced on
 * every sweep for the rest of uptime, which would bury every other
 * console message. Observing the log is the only way to test that, so
 * this stub does. */
static int giveup_announcements = 0;
static int strstr_lite(const char* h, const char* n) {
    for (const char* a = h; *a; a++) {
        const char* x = a; const char* y = n;
        while (*x && *y && *x == *y) { x++; y++; }
        if (!*y) return 1;
    }
    return 0;
}
void kernel_serial_printf(const char* fmt, ...) {
    if (fmt && strstr_lite(fmt, "GIVING UP")) giveup_announcements++;
}
void persist_services(void) { }
void persist_workloads(void) { }
void persist_partitions(void) { }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
uint32_t cluster_local_node_id(void) { return 1; }
volatile uint64_t kernel_tick_counter = 0;

uint64_t simi_rt_resolve(const char* n) { (void)n; return 0; }
uint64_t simi_rt_objsize(uint64_t v)    { (void)v; return 0; }
uint64_t simi_rt_objtype(uint64_t v)    { (void)v; return 0xFFFFFFFFu; }

struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t process_kill_partition(uint32_t p)  { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int  partition_lease_step_down(uint32_t p) { (void)p; return 1; }
int  stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
void dspp_service_announce(const char* n, uint32_t p, uint8_t k, uint32_t e,
                           uint32_t u, uint8_t sv) {
    (void)n;(void)p;(void)k;(void)e;(void)u;(void)sv;
}
void dspp_service_withdraw(const char* n) { (void)n; }
void dspp_ctx_migrate_send_begin(uint64_t a, uint32_t b, uint32_t c, const char* d,
                                 uint64_t e, uint32_t f, uint64_t g) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
}
void dspp_ctx_migrate_send_chunk(uint64_t a, uint32_t b, uint32_t c, uint32_t d,
                                 const uint8_t* e, uint32_t f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
}
int tcp_port_is_listening(uint16_t port) { (void)port; return 0; }
int mk_ipc_port_state(uint16_t port) { (void)port; return -1; }
int ipc_queue_depth(uint16_t port) { (void)port; return 0; }

static uint8_t  g_image[LOADER_MAX_BINARY_SIZE];
static uint32_t g_image_size = 0;
struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];

static uint32_t cycle(void) {
    uint32_t acted = reconcile_tick();
    reconcile_drain();
    return acted;
}

/* Drives a live context to a chosen terminal state. The corpus program
 * runs to HALTED on its own; a TRAP is produced by corrupting the running
 * context's pc past the end of the instruction stream, which is exactly
 * what SIMI_STATUS_TRAP_PC_RANGE exists to report. */
static void drive_to_halt(const char* wl) {
    struct SimiContext* c = wlctx_get(wl);
    for (int i = 0; i < 2000 && c->status == SIMI_STATUS_OK; i++) wlctx_step_all(64);
}
static void drive_to_trap(const char* wl) {
    struct SimiContext* c = wlctx_get(wl);
    c->pc = c->obj->num_instr + 1000;   /* off the end */
    wlctx_step_all(4);
}

/* Paired with the relocate/send stubs: a test that stands in "nothing to
 * relocate" must also stand in "nothing to count", or partition_migrate()
 * sees 0 sent against a non-zero expectation and aborts every migration.
 * FAITHFUL -- this test registers no streams, so the real function would
 * also return 0. */
int stream_count_for_partition(uint32_t partition_id) { (void)partition_id; return 0; }

int main(void) {
    printf("=== Phase 7: self-healing restarts ===\n\n");
    printf("      (threshold %u attempts, backoff %u..%u ticks, stable reset %u)\n\n",
           WL_RESTART_MAX_ATTEMPTS, WL_BACKOFF_BASE_TICKS,
           WL_BACKOFF_MAX_TICKS, WL_STABLE_RESET_TICKS);

    FILE* f = fopen("tools/simi/tests/loop_sum.tmo", "rb");
    if (!f) { printf("FAIL: cannot open corpus program\n"); return 1; }
    g_image_size = (uint32_t)fread(g_image, 1, sizeof(g_image), f);
    fclose(f);
    service_binaries[0].active = 1; service_binaries[0].is_simi = 1;
    service_binaries[0].size = g_image_size;
    memcpy(service_binaries[0].object_name, "loopsum", 8);
    memcpy(service_binaries[0].data, g_image, g_image_size);

    partition_init(); service_registry_init(); workload_init(); wlctx_init();
    uint32_t p = partition_create("app");
    reconcile_set_enabled(1);
    kernel_tick_counter = 100000;

    /* ═══ Scenario 1: NEVER is the default, and it means never ════════ */
    printf("-- Scenario 1: the default is to do nothing --\n");
    {
        CHECK(workload_declare(0, "batch", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                               "loopsum", "main", WL_RESTART_NEVER) == WL_OK,
              "a workload is declared with no restart policy");
        for (int i = 0; i < 3 && cycle(); i++) { }
        CHECK(wlctx_has("batch"), "its context starts");

        drive_to_halt("batch");
        CHECK(wlctx_status_of("batch") == (int)SIMI_STATUS_HALTED, "it runs to completion");

        kernel_tick_counter += WL_BACKOFF_MAX_TICKS * 4;
        for (int i = 0; i < 20; i++) cycle();
        struct SLSWorkloadEntry* w = workload_find("batch");
        CHECK(w && w->restarts_total == 0,
              "*** a completed batch job is NOT restarted -- HALTED means finished ***");
        CHECK(wlctx_status_of("batch") == (int)SIMI_STATUS_HALTED, "...and it stays finished");
        CHECK(cycle() == 0, "the sweep settles rather than spinning on it");
    }

    /* ═══ Scenario 2: THE PHASE, in one test ══════════════════════════
     * Same terminal state, three policies, three correct answers. */
    printf("\n-- Scenario 2: HALTED vs TRAPPED, under each policy --\n");
    {
        /* ON_FAILURE + HALTED -> leave it alone. */
        workload_init(); wlctx_init();
        kernel_tick_counter += 100000;
        workload_declare(0, "job", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ON_FAILURE);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("job");
        kernel_tick_counter += WL_BACKOFF_MAX_TICKS * 4;
        for (int i = 0; i < 20; i++) cycle();
        CHECK(workload_find("job")->restarts_total == 0,
              "*** ON_FAILURE + HALTED: not restarted -- completion is not failure ***");

        /* ON_FAILURE + TRAPPED -> restart. */
        workload_init(); wlctx_init();
        kernel_tick_counter += 100000;
        workload_declare(0, "job", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ON_FAILURE);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_trap("job");
        CHECK(wlctx_status_of("job") == (int)SIMI_STATUS_TRAP_PC_RANGE, "the context traps");
        kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
        cycle();
        CHECK(workload_find("job")->restarts_total == 1,
              "*** ON_FAILURE + TRAPPED: restarted ***");
        CHECK(wlctx_status_of("job") == (int)SIMI_STATUS_OK,
              "*** and the context is running again from its entry point ***");
        struct SimiContext* c = wlctx_get("job");
        CHECK(c->steps == 0 && c->pc < c->obj->num_instr,
              "...with its execution state discarded, not resumed from the fault");

        /* ALWAYS + HALTED -> restart. */
        workload_init(); wlctx_init();
        kernel_tick_counter += 100000;
        workload_declare(0, "svc", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("svc");
        kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
        cycle();
        CHECK(workload_find("svc")->restarts_total == 1,
              "*** ALWAYS + HALTED: restarted -- a service is not supposed to return ***");
        CHECK(wlctx_status_of("svc") == (int)SIMI_STATUS_OK, "...and is running again");
    }

    /* ═══ Scenario 3: backoff actually delays ═════════════════════════ */
    printf("\n-- Scenario 3: attempts are spaced, not immediate --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 500000;
        workload_declare(0, "flap", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("flap");

        cycle();
        struct SLSWorkloadEntry* w = workload_find("flap");
        CHECK(w->restarts_total == 1, "the first terminal state restarts immediately");

        drive_to_halt("flap");
        uint32_t before = w->restarts_total;
        for (int i = 0; i < 50; i++) cycle();     /* no time passes */
        CHECK(w->restarts_total == before,
              "*** 50 sweeps with the clock stopped produce NO further restarts -- the backoff holds ***");
        CHECK(cycle() == 0,
              "...and a workload waiting out its backoff does not report the sweep as busy");

        kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
        cycle();
        CHECK(w->restarts_total == before + 1, "once the backoff elapses, one more attempt");

        /* ── The backoff must GROW, not merely exist ──────────────────
         * Mutation testing added this: making wl_backoff() return a
         * constant SURVIVED, because every earlier check advanced the
         * clock by the maximum. Growth is only visible if you advance by
         * the BASE and watch it stop being enough. */
        workload_init(); wlctx_init();
        kernel_tick_counter = 700000;
        workload_declare(0, "grow", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }
        struct SLSWorkloadEntry* g = workload_find("grow");

        drive_to_halt("grow"); cycle();            /* attempt 1, immediate */
        CHECK(g->restarts_total == 1, "attempt 1 fires immediately");

        drive_to_halt("grow");
        kernel_tick_counter += WL_BACKOFF_BASE_TICKS;
        cycle();
        CHECK(g->restarts_total == 2, "attempt 2 needs only the BASE interval");

        drive_to_halt("grow");
        kernel_tick_counter += WL_BACKOFF_BASE_TICKS;
        cycle();
        CHECK(g->restarts_total == 2,
              "*** attempt 3 is NOT granted after only the base interval -- the backoff has grown ***");
        kernel_tick_counter += WL_BACKOFF_BASE_TICKS * 4;
        cycle();
        CHECK(g->restarts_total == 3, "...and fires once the longer interval has passed");
    }

    /* ═══ Scenario 4: THE HAZARD — a crash loop must terminate ════════ */
    printf("\n-- Scenario 4: a crash loop gives up --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 1000000;
        workload_declare(0, "loop", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }

        /* Fail it as fast as the reconciler will let us, for far longer
         * than the attempt limit. */
        giveup_announcements = 0;
        for (int i = 0; i < 400; i++) {
            if (wlctx_status_of("loop") == (int)SIMI_STATUS_OK) drive_to_trap("loop");
            kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
            cycle();
        }
        struct SLSWorkloadEntry* w = workload_find("loop");
        CHECK(w->gave_up, "*** the reconciler GIVES UP rather than restarting forever ***");
        CHECK(w->restart_count == WL_RESTART_MAX_ATTEMPTS,
              "...after exactly the configured number of attempts");
        CHECK(w->restarts_total == WL_RESTART_MAX_ATTEMPTS,
              "*** and 400 sweeps produced no more than that -- the loop is bounded ***");

        /* The flag must also stop the decision being re-announced. Without
         * it the give-up branch is re-entered every sweep and logs again,
         * burying every other console message for the rest of uptime.
         * (Mutation testing: ignoring gave_up otherwise survived, because
         * restart_count >= MAX blocks the restart on its own.) */
        CHECK(w->gave_up == 1, "the give-up is recorded as a flag, not merely implied by the count");
        CHECK(giveup_announcements == 1,
              "*** and announced exactly ONCE across 400 sweeps, not re-logged forever ***");

        uint32_t frozen = w->restarts_total;
        for (int i = 0; i < 100; i++) { kernel_tick_counter += WL_BACKOFF_MAX_TICKS; cycle(); }
        CHECK(w->restarts_total == frozen, "having given up, it stays given up");
        CHECK(cycle() == 0, "...and stops generating work entirely");

        /* An operator can re-arm it. */
        CHECK(workload_clear_giveup("loop") == 0, "an operator can clear the give-up");
        CHECK(!w->gave_up && w->restart_count == 0, "...which re-arms the attempts");
        CHECK(w->restarts_total == frozen,
              "*** but the lifetime total is NOT erased -- clearing resets state, not history ***");
        CHECK(workload_clear_giveup("nope") != 0, "clearing an unknown workload is reported");

        /* Re-declaring also re-arms, since it is an operator intervention. */
        for (int i = 0; i < 200; i++) {
            if (wlctx_status_of("loop") == (int)SIMI_STATUS_OK) drive_to_trap("loop");
            kernel_tick_counter += WL_BACKOFF_MAX_TICKS; cycle();
        }
        CHECK(w->gave_up, "it gives up again");
        workload_declare(0, "loop", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        CHECK(!w->gave_up, "re-declaring re-arms it too -- a corrected declaration gets a chance");
    }

    /* ═══ Scenario 5: a stable run earns its attempts back ════════════ */
    printf("\n-- Scenario 5: the budget is not a lifetime sentence --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 2000000;
        workload_declare(0, "occasional", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }

        /* A few failures, but not enough to give up. */
        for (int i = 0; i < 3; i++) {
            drive_to_trap("occasional");
            kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
            cycle();
        }
        struct SLSWorkloadEntry* w = workload_find("occasional");
        CHECK(w->restart_count == 3, "three attempts used");

        /* Now it runs stably for longer than the reset window. The
         * context is mid-execution the whole time. */
        kernel_tick_counter += WL_STABLE_RESET_TICKS + 1;
        cycle();
        CHECK(w->restart_count == 0,
              "*** a stable run resets the budget -- a workload that fails once a month never gives up ***");
        CHECK(w->restarts_total == 3, "...while the lifetime record is kept");
    }

    /* ═══ Scenario 6: restarts respect the rest of the reconciler ═════ */
    printf("\n-- Scenario 6: interaction with desired state --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 3000000;
        workload_declare(0, "stopme", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("stopme");

        /* Flip to STOPPED before the restart fires. */
        workload_declare(0, "stopme", p, WL_DESIRED_STOPPED, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        kernel_tick_counter += WL_BACKOFF_MAX_TICKS;
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(!wlctx_has("stopme"),
              "*** desired STOPPED tears the context down instead of restarting it ***");
        CHECK(cycle() == 0, "...and settles");

        /* A workload with no program declared is never a restart candidate. */
        workload_declare(0, "noprog", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "", "", WL_RESTART_ALWAYS) ;
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(workload_find("noprog")->restarts_total == 0,
              "a workload declaring no program is never restarted -- there is nothing to restart");
        CHECK(cycle() == 0, "and the sweep settles with both declarations present");
    }

    /* ═══ Scenario 6b: a policy value nobody recognises ═══════════════
     * Mutation testing added this: trusting the caller's value outright
     * SURVIVED, because no test ever passed a bad one. A caller that got
     * this wrong must not thereby opt into autonomous restarts. */
    printf("\n-- Scenario 6b: an unrecognised policy is not an eager one --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 3500000;
        workload_declare(0, "junkpol", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", (SLSWorkloadRestartPolicy)99);
        struct SLSWorkloadEntry* jw = workload_find("junkpol");
        CHECK(jw && jw->restart_policy == WL_RESTART_NEVER,
              "*** an out-of-range policy is clamped to NEVER, not trusted ***");
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("junkpol");
        kernel_tick_counter += WL_BACKOFF_MAX_TICKS * 4;
        for (int i = 0; i < 20; i++) cycle();
        CHECK(jw->restarts_total == 0, "...so it is never restarted");
    }

    /* ═══ Scenario 7: clock skew ══════════════════════════════════════ */
    printf("\n-- Scenario 7: a clock reading behind the stamp --\n");
    {
        workload_init(); wlctx_init();
        kernel_tick_counter = 4000000;
        workload_declare(0, "skew", p, WL_DESIRED_RUNNING, "", SVC_ENDPOINT_TCP, 0,
                         "loopsum", "main", WL_RESTART_ALWAYS);
        for (int i = 0; i < 3 && cycle(); i++) { }
        drive_to_halt("skew");
        cycle();
        struct SLSWorkloadEntry* w = workload_find("skew");
        uint32_t after_first = w->restarts_total;

        drive_to_halt("skew");
        kernel_tick_counter -= 1;      /* a reading one tick behind the stamp */
        cycle();
        CHECK(w->restarts_total == after_first,
              "*** a backwards clock does not wrap into 'the backoff elapsed' ***");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
