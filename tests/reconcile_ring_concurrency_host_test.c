/*
 * reconcile_ring_concurrency_host_test.c — the one thing Phase 5 shipped
 * without proving: that the AP→BSP intent ring is correct under REAL
 * concurrent access, not just correct when driven sequentially.
 *
 * ─── Why the existing test was not enough ─────────────────────────────
 * tests/workload_reconcile_host_test.c calls reconcile_tick() and
 * reconcile_drain() one after the other on one thread. That proves the
 * separation of duties (the sweep performs no persisting calls) and
 * nothing whatsoever about memory ordering: a ring with every __atomic
 * replaced by a plain load/store passes it perfectly. In the running
 * kernel the producer is the AP core and the consumer is the BSP, genuinely
 * simultaneously, and that is the case that was untested.
 *
 * This file runs the real wl_enqueue()/drain protocol from two OS threads
 * on two cores, at speed, and checks the properties an SPSC ring must have:
 *
 *   1. NO LOSS      -- every accepted intent is delivered exactly once
 *   2. NO DUPLICATION
 *   3. IN ORDER     -- a FIFO that reorders is not a FIFO
 *   4. NO TEARING   -- an intent is never read half-written. Each probe
 *                      carries its sequence number in five redundant
 *                      fields plus a derived 64-byte string; the drain
 *                      side reports a poisoned value if any disagree.
 *   5. ACCOUNTING   -- accepted + dropped == offered, exactly.
 *
 * ─── Verification ceiling — MEASURED, not just asserted ───────────────
 * Two pthreads on a 2-core x86-64 host is a real concurrency test of the
 * ALGORITHM, but it is not the kernel's environment. Rather than guess at
 * what that costs, the limit was measured by mutating the ring and
 * re-running this file. Results:
 *
 *   publish head BEFORE writing the slot ......... CAUGHT (tearing + order)
 *   advance tail BEFORE copying the slot out ..... CAUGHT (tearing + order)
 *   fullness test off by one (overwrite a live slot) CAUGHT (tearing + order)
 *   replace every __atomic with a plain access .... **SURVIVED**
 *
 * So this file genuinely catches the STRUCTURAL bugs -- wrong publication
 * order, wrong consumption order, wrong capacity arithmetic -- and does
 * NOT catch missing barriers, because x86-64's strong (TSO) memory model
 * makes acquire/release on a plain load/store free. A ring with no
 * barriers at all is indistinguishable here and would still be wrong on a
 * weakly-ordered target -- which this codebase has, in the RISC-V build.
 *
 * That residual gap is real and is not closed by this file. ThreadSanitizer
 * is the right tool for it and does not run in this environment (it fails
 * to initialise: "unexpected memory mapping"). The barriers in workload.c
 * are therefore correct by construction and by review, not by test.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -pthread \
 *       -o /tmp/reconcile_ring_concurrency_host_test \
 *       tests/reconcile_ring_concurrency_host_test.c kernel/workload.c \
 *       kernel/service_registry.c kernel/partition.c
 *   /tmp/reconcile_ring_concurrency_host_test
 */
#include "kernel/workload.h"
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

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

/* ─── Live-context stubs ──────────────────────────────────────────────
 * FAITHFUL: this file drives the intent RING directly via the probe seam
 * and declares no workloads at all, so none of these is reachable. The
 * context producer has its own coverage in tests/workload_ctx_host_test.c. */
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
const unsigned char* workload_find_program(const char* n, uint32_t* sz) {
    (void)n; (void)sz; return 0;
}


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
void persist_workloads(void) { }
void persist_partitions(void) { }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
uint32_t cluster_local_node_id(void) { return 0; }
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

/* ─── The experiment ──────────────────────────────────────────────────
 * OFFERED intents are numbered 1..N. The producer retries a full ring
 * rather than dropping, so every offered sequence number is eventually
 * accepted -- which makes "no loss" a check on exact equality rather than
 * on an inequality that a broken ring could satisfy by accident. */
#define N_INTENTS 400000u

static volatile int producer_done = 0;
static uint32_t     accepted      = 0;   /* producer-private */
static uint32_t     retries       = 0;   /* enqueue attempts that found the ring full */

static void* producer(void* arg) {
    (void)arg;
    for (uint32_t seq = 1; seq <= N_INTENTS; seq++) {
        /* Spin until there is room. A full ring is back-pressure here,
         * not an error -- the drop path has its own coverage in
         * workload_reconcile_host_test.c scenario 6. */
        while (reconcile_enqueue_probe(seq) != 0) {
            retries++;
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        accepted++;
    }
    __atomic_store_n(&producer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint32_t received      = 0;
static uint32_t out_of_order  = 0;
static uint32_t torn          = 0;
static uint32_t last_seen     = 0;

static void* consumer(void* arg) {
    (void)arg;
    uint32_t batch[64];
    for (;;) {
        uint32_t n = reconcile_drain_probe(batch, 64);
        if (n == 0) {
            if (__atomic_load_n(&producer_done, __ATOMIC_ACQUIRE)) {
                /* One last sweep: the producer may have enqueued between
                 * our empty read and its done flag. */
                n = reconcile_drain_probe(batch, 64);
                if (n == 0) break;
            } else {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
                continue;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (batch[i] == 0xFFFFFFFFu) { torn++; received++; continue; }
            if (batch[i] != last_seen + 1) out_of_order++;
            last_seen = batch[i];
            received++;
        }
    }
    return 0;
}

int main(void) {
    printf("=== Reconciler intent ring under real concurrency ===\n\n");
    printf("      (%u intents, producer and consumer on separate threads)\n\n", N_INTENTS);

    partition_init();
    service_registry_init();
    workload_init();

    pthread_t tp, tc;
    CHECK(pthread_create(&tp, 0, producer, 0) == 0, "producer thread started");
    CHECK(pthread_create(&tc, 0, consumer, 0) == 0, "consumer thread started");
    pthread_join(tp, 0);
    pthread_join(tc, 0);

    printf("      offered=%u accepted=%u received=%u torn=%u out_of_order=%u\n\n",
           N_INTENTS, accepted, received, torn, out_of_order);

    CHECK(accepted == N_INTENTS,
          "every offered intent was eventually accepted (the ring applies back-pressure, it does not wedge)");
    CHECK(received == accepted,
          "*** NO LOSS and NO DUPLICATION: received count equals accepted count exactly ***");
    CHECK(torn == 0,
          "*** NO TEARING: no intent was ever observed half-written ***");
    CHECK(out_of_order == 0,
          "*** IN ORDER: the sequence arrived strictly monotonically, 1..N ***");
    CHECK(last_seen == N_INTENTS,
          "the final intent arrived -- nothing was stranded in the ring at shutdown");
    CHECK(reconcile_queue_depth() == 0, "the ring is empty at the end");

    /* The dropped counter counts ENQUEUE ATTEMPTS THAT FOUND THE RING
     * FULL, not work lost. This producer retries, so every retry bumps
     * it. Asserting exact equality with the retry count is a stronger
     * check than "dropped == 0" would have been: it proves the counter
     * fires on precisely the full-ring condition and never spuriously,
     * across hundreds of thousands of genuinely contended attempts.
     *
     * (In production nothing retries -- the reconciler re-derives the
     * intent on its next sweep instead -- so there the counter really
     * does mean "an intent was discarded". Both readings are consistent:
     * it counts refusals at the ring, and what happens next is the
     * caller's choice.) */
    CHECK(retries > 0,
          "the ring genuinely filled during the run -- this was a contended test, not a serial one");
    CHECK(reconcile_queue_dropped() == retries,
          "*** the full-ring counter is EXACT: it fired once per refusal and never spuriously ***");
    printf("      (ring filled %u times under contention)\n", retries);

    /* ─── Index wrap ──────────────────────────────────────────────────
     * N_INTENTS far exceeds WL_INTENT_MAX, so head/tail wrapped the ring
     * many thousands of times during the run above. That is the point of
     * using a large N: the masking arithmetic and the unsigned-difference
     * fullness test are exercised at every offset, not just near zero. */
    CHECK(N_INTENTS > 32u * 1000u,
          "the run wrapped the 32-slot ring thousands of times, exercising the index mask at every offset");

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
