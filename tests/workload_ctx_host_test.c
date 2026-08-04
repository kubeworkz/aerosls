/*
 * workload_ctx_host_test.c — closes the gap PEC Phase 3 shipped with.
 *
 * ─── The claim being tested ───────────────────────────────────────────
 * Phase 3 built and proved the whole cross-node migration path, then said
 * honestly that it was unreachable from the system: nothing created
 * long-lived SimiContexts, so simi_ctx_migrate_send_partition() walked an
 * empty registry and partition_migrate() moved zero contexts on every
 * boot. The Phase 3 findings recorded that as named, outstanding work.
 *
 * This file proves it is no longer true. It goes all the way from an
 * OPERATOR-STYLE DECLARATION to a running computation resuming on another
 * node:
 *
 *   declare a workload with a program
 *     -> reconciler queues a context start
 *       -> BSP drain instantiates it and registers it
 *         -> it executes, partway
 *           -> partition_migrate() checkpoints and transmits it
 *             -> node 2 replays the real packets and resumes
 *               -> the answer matches an uninterrupted run
 *
 * Scenario 2 is the load-bearing one, and the assertion that matters most
 * is the negative control in scenario 1: BEFORE the declaration, the same
 * migration call really does move zero contexts. Without that, "it moved
 * one" would not prove anything had changed.
 *
 * Links the REAL kernel/workload.c, workload_ctx.c, simi_ctx_migrate.c,
 * simi_ckpt.c, simi_interp.c, service_registry.c, partition.c and
 * net/dspp.c. Nothing on the path is reimplemented.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I tools/simi \
 *       -o /tmp/workload_ctx_host_test \
 *       tests/workload_ctx_host_test.c kernel/workload.c kernel/workload_ctx.c net/dspp_checkpoint.c \
 *       kernel/simi_ctx_migrate.c kernel/simi_ckpt.c kernel/simi_interp.c \
 *       kernel/service_registry.c kernel/partition.c net/dspp.c
 *   /tmp/workload_ctx_host_test
 */
#include "kernel/workload.h"
#include "net/e1000.h"   /* NicRole */
#include "kernel/workload_ctx.h"
#include "kernel/simi_ctx_migrate.h"
#include "kernel/service_registry.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "net/dspp.h"
#include "net/net.h"
#include "kernel/loader.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* ─── Phase 6 breaker stub ────────────────────────────────────────────
 * FAITHFUL: service_resolve() now reports the breaker state, and a
 * service with no recorded failures is CLOSED -- which is every service
 * in these tests, none of which report outcomes. Full breaker coverage is
 * in tests/service_mesh_host_test.c. */
int service_breaker_state(const char* n) { (void)n; return 0; /* CB_CLOSED */ }

/* Endpoint-liveness probe sources. FAITHFUL: this test declares no
 * services at all, so nothing is ever probed; and a node with nothing
 * listening and no supervised IPC owner is exactly this situation. */
int tcp_port_is_listening(uint16_t port) { (void)port; return 0; }
int mk_ipc_port_state(uint16_t port) { (void)port; return -1; }


/* kernel/service_registry.c now stamps replicated entries with the
 * kernel tick for TTL purposes. These tests exercise no replication, so a
 * clock frozen at 0 is faithful: every local entry they use is
 * authoritative and never ages. */
volatile uint64_t kernel_tick_counter = 0;






static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_services(void) { }
void persist_workloads(void) { }
void persist_partitions(void) { }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }

/* SIMI catalog bindings -- the same mock the reference interpreter uses. */
uint64_t simi_rt_resolve(const char* n) { (void)n; return 0; }
uint64_t simi_rt_objsize(uint64_t v)    { (void)v; return 0; }
uint64_t simi_rt_objtype(uint64_t v)    { (void)v; return 0xFFFFFFFFu; }

/* partition.c / dspp.c dependencies outside this test's scope. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t              object_catalog_count = 0;
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t process_kill_partition(uint32_t p)  { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int  partition_lease_step_down(uint32_t p) { (void)p; return 1; }
int  stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int  partition_holds_write_lease(uint32_t p) { (void)p; return 1; }
void process_consensus_packet(struct DSPPFullPagePacket* p) { (void)p; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* p) { (void)p; }
int  stream_migrate_recv_begin(uint64_t t, uint32_t p, const char* n, const char* m,
                               uint64_t s, uint32_t f, uint32_t o) {
    (void)t;(void)p;(void)n;(void)m;(void)s;(void)f;(void)o; return 0;
}
int  stream_migrate_recv_page(uint64_t t, uint32_t i, uint32_t f, const uint8_t* d) {
    (void)t;(void)i;(void)f;(void)d; return 0;
}

static uint32_t g_local_node = 1;
uint32_t cluster_local_node_id(void) { return g_local_node; }
MACAddr net_my_mac;

/* ─── The program image ───────────────────────────────────────────────
 * A REAL corpus .tmo, read from disk and handed over exactly as an
 * uploaded binary would be -- raw bytes, header at offset 0. That is the
 * point: it exercises workload_ctx.c's own parse, including the aligned
 * copy it makes because the instruction stream sits at byte offset 20. */
static uint8_t  g_image[LOADER_MAX_BINARY_SIZE];
static uint32_t g_image_size = 0;

/* The REAL uploaded-binary store, not a stub of the lookup. Defining the
 * array and letting workload_ctx.c's own workload_find_program() search it
 * exercises the actual production path -- name matching, active flag,
 * size -- rather than a convenient shortcut around it. 263 KiB is a cheap
 * price for testing the thing that ships. */
struct ServiceBinary service_binaries[MAX_SERVICE_BINARIES];

/* ─── Packet capture, as in the Phase 3 test ─────────────────────────── */
/* ─── Capture depth, derived rather than guessed ──────────────────────────
 * This was `#define MAX_FRAMES 64`, and it broke the moment
 * DSPP_CTX_CHUNK_BYTES dropped from 4096 to a size that fits an Ethernet
 * frame: a full checkpoint is ~66 KiB, so the chunk count quadrupled past
 * 64, e1000_transmit() silently stopped recording beyond that, and
 * reassembly could never complete. The symptom was four failing assertions
 * about a context that "did not arrive" -- nothing to do with the receive
 * path, everything to do with the test's own buffer.
 *
 * Derived from the same constants the real chunk_present[] in
 * kernel/simi_ctx_migrate.c derives its size from, so a future change to
 * the chunk size or the interpreter's memory cannot silently truncate the
 * capture again. */
#define MAX_CKPT_BYTES ((unsigned)(sizeof(struct SimiCkptHeader) \
                      + SIMI_MAX_FRAMES * sizeof(struct SimiFrame) \
                      + SIMI_MEM_SIZE))
#define MAX_FRAMES ((MAX_CKPT_BYTES / DSPP_CTX_CHUNK_BYTES) + 8)
#define FRAME_CAP  (ETH_HDR_LEN + (sizeof(struct DSPPCtxMigrateChunkPacket) > \
                                   sizeof(struct DSPPMigratePagePacket) \
                                 ? sizeof(struct DSPPCtxMigrateChunkPacket) \
                                 : sizeof(struct DSPPMigratePagePacket)))
static uint8_t  cap_buf[MAX_FRAMES][FRAME_CAP];
static uint16_t cap_len[MAX_FRAMES];
static int      cap_count = 0;
static int      capturing = 1;

/* Multi-NIC Phase 2: renamed, and now carries the role. Body unchanged;
 * the role is recorded alongside so DSPP's interface choice is assertable. */
static NicRole last_tx_role = NIC_ROLE_NONE;
void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    last_tx_role = role;
    if (!capturing || cap_count >= MAX_FRAMES) return;
    if (size > FRAME_CAP) size = (uint16_t)FRAME_CAP;
    memcpy(cap_buf[cap_count], buf, size);
    cap_len[cap_count] = size;
    cap_count++;
}

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
    printf("=== Live execution contexts: closing PEC Phase 3's reachability gap ===\n\n");

    FILE* f = fopen("tools/simi/tests/loop_sum.tmo", "rb");
    if (!f) { printf("FAIL: cannot open corpus program\n"); return 1; }
    g_image_size = (uint32_t)fread(g_image, 1, sizeof(g_image), f);
    fclose(f);
    CHECK(g_image_size > 20, "a real corpus .tmo was loaded as the program image");

    /* Publish it into the real binary store, exactly as an upload would. */
    service_binaries[0].active = 1;
    service_binaries[0].is_simi = 1;
    service_binaries[0].size = g_image_size;
    memcpy(service_binaries[0].object_name, "loopsum", 8);
    memcpy(service_binaries[0].data, g_image, g_image_size);
    uint32_t probe_sz = 0;
    CHECK(workload_find_program("loopsum", &probe_sz) == service_binaries[0].data
          && probe_sz == g_image_size,
          "the REAL program lookup finds it in the uploaded-binary store");
    CHECK(workload_find_program("nosuch", &probe_sz) == 0,
          "...and returns nothing for an object that was never uploaded");

    partition_init();
    service_registry_init();
    workload_init();
    wlctx_init();
    simi_ctx_migrate_reset();

    uint32_t papp = partition_create("app");
    partition_owner_table[papp].node_id = 1;
    g_local_node = 1;

    /* ─── Ground truth, computed BEFORE anything else is live ─────────
     * wlctx_step_all() advances EVERY live context -- there is
     * deliberately no per-context step, because the BSP drives the whole
     * pool from one loop. So the reference run has to happen while it is
     * the only context in the pool, or it silently runs the context under
     * test to completion too. (It did, the first time this was written;
     * the symptom was scenario 4 "migrating" an already-halted context
     * and still passing.) */
    uint64_t truth = 0, truth_steps = 0;
    {
        CHECK(wlctx_start("truthrun", g_image, g_image_size, "main", papp) == WLCTX_OK,
              "a reference context runs first, alone in the pool");
        struct SimiContext* t = wlctx_get("truthrun");
        for (int i = 0; i < 1000 && t->status == SIMI_STATUS_OK; i++) wlctx_step_all(64);
        truth = t->result; truth_steps = t->steps;
        CHECK(t->status == SIMI_STATUS_HALTED, "it runs to completion uninterrupted");
        wlctx_stop("truthrun");
        CHECK(wlctx_count() == 0, "and is removed, leaving the pool empty");
        printf("      (uninterrupted: result=%lld in %llu steps)\n",
               (long long)(int64_t)truth, (unsigned long long)truth_steps);
    }

    /* ═══ Scenario 1: THE NEGATIVE CONTROL ════════════════════════════
     * This is exactly what the system did before this change, and it has
     * to be shown failing to move anything -- otherwise scenario 2's
     * success proves nothing. */
    printf("\n-- Scenario 1: before any declaration, migration moves nothing --\n");
    {
        CHECK(simi_ctx_registered_count(papp) == 0,
              "the migration registry is empty, as it was on every boot before this");
        cap_count = 0; capturing = 1;
        uint32_t sent = simi_ctx_migrate_send_partition(papp, 2);
        CHECK(sent == 0, "*** partition migration moves ZERO contexts -- the Phase 3 gap ***");
        CHECK(cap_count == 0, "...and puts nothing on the wire");
    }

    /* ═══ Scenario 2: declare a program; it becomes a live context ════ */
    printf("\n-- Scenario 2: a declaration produces a real running computation --\n");
    {
        reconcile_set_enabled(1);
        CHECK(workload_declare(0, "sumjob", papp, WL_DESIRED_RUNNING,
                               "", SVC_ENDPOINT_TCP, 0,
                               "loopsum", "main", WL_RESTART_NEVER) == WL_OK,
              "a workload is declared WITH a program");

        CHECK(!wlctx_has("sumjob"), "no context exists yet -- declaring is not doing");
        uint32_t acted = cycle();
        CHECK(acted >= 1, "the reconciler acts on it");
        CHECK(wlctx_has("sumjob"),
              "*** a LIVE execution context now exists, created from the declaration ***");
        CHECK(wlctx_count() == 1, "exactly one");
        CHECK(simi_ctx_registered_count(papp) == 1,
              "*** and it is REGISTERED for migration -- the registry is no longer empty ***");
        CHECK(cycle() == 0, "the workload then converges and settles");

        struct SimiContext* c = wlctx_get("sumjob");
        CHECK(c && c->obj && c->obj->num_instr > 0,
              "the context is bound to the parsed program image");
        CHECK(c && c->steps == 0, "it has not executed yet -- creation is not execution");
    }

    /* ═══ Scenario 3: it actually runs ════════════════════════════════ */
    printf("\n-- Scenario 3: the context executes, in bounded steps --\n");
    {
        struct SimiContext* c = wlctx_get("sumjob");
        CHECK(wlctx_count() == 1, "'sumjob' is the only live context, so stepping is unambiguous");
        uint32_t n = wlctx_step_all(truth_steps / 2);
        CHECK(n == 1, "stepping advances exactly the one live context");
        CHECK(c->steps > 0 && c->steps < truth_steps,
              "it is genuinely partway through -- work done, work remaining");
        CHECK(c->status == SIMI_STATUS_OK, "and still runnable");
        CHECK(c->result != truth || truth == 0,
              "it has NOT yet computed the answer, so finishing it elsewhere proves real resumption");
    }

    /* ═══ Scenario 4: THE PAYOFF ══════════════════════════════════════
     * The same partition_migrate() call from scenario 1, now moving real
     * running work. */
    printf("\n-- Scenario 4: partition_migrate() now moves a RUNNING computation --\n");
    {
        struct SimiContext* c = wlctx_get("sumjob");
        uint32_t mid_pc    = c->pc;
        uint64_t mid_steps = c->steps;

        cap_count = 0; capturing = 1; g_local_node = 1;
        int rc = partition_migrate(papp, 2);
        CHECK(rc == 0, "the real partition_migrate() runs");
        CHECK(cap_count > 1,
              "*** it put a checkpoint on the wire -- BEGIN plus chunks ***");
        if (cap_count == 0) { printf("      (nothing transmitted -- later checks skipped)\n");
                              printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
                              return 1; }

        struct DSPPCtxMigrateHeader* h =
            (struct DSPPCtxMigrateHeader*)(cap_buf[0] + ETH_HDR_LEN);
        CHECK(h->magic == DSPP_MIGRATE_MAGIC &&
              h->opcode == DSPP_MIGRATE_CTX_BEGIN_REQ,
              "the first packet is a context-migration BEGIN");
        CHECK(strcmp(h->ctx_name, "sumjob") == 0,
              "...naming the workload the operator declared");
        CHECK(h->partition_id == papp, "...and its partition");

        /* Become node 2 and replay the captured frames through the real
         * receive path, exactly as tests/simi_ctx_migrate_host_test.c does. */
        capturing = 0;
        g_local_node = 2;
        simi_ctx_migrate_reset();
        /* Deliberately NOT re-registering the program image here. A
         * checkpoint carries a program HASH, not the program, so the
         * receiving node must already hold a matching image or the restore
         * is refused -- and the thing that registered one is wlctx_start()
         * itself. Registering it here instead would mask whether
         * wlctx_start() does its job; mutation testing showed exactly
         * that (removing its register_image() call survived). In a real
         * two-node cluster the destination registers the same image when
         * it starts its own context from the same uploaded program. */
        for (int i = 0; i < cap_count; i++)
            dspp_rx_dispatch(cap_buf[i] + ETH_HDR_LEN, (uint16_t)(cap_len[i] - ETH_HDR_LEN));

        CHECK(simi_ctx_migrate_arrived(),
              "node 2 received and restored the context (its image was registered at start)");
        if (!simi_ctx_migrate_arrived()) {
            /* Do not run an unrestored context -- ctx->obj would be NULL
             * and the interpreter would fault, turning a clean failure
             * report into a crash. */
            printf("      (skipping the resume checks -- nothing landed)\n");
        } else {
            struct SimiContext* land = simi_ctx_migrate_landed();
            CHECK(land->pc == mid_pc && land->steps == mid_steps,
                  "*** it resumes at the exact instruction node 1 stopped at ***");

            SimiStatus st = SIMI_STATUS_OK;
            for (int i = 0; i < 1000 && st == SIMI_STATUS_OK; i++) st = simi_interp_run(land, 64);
            CHECK(st == SIMI_STATUS_HALTED, "node 2 runs it to completion");
            CHECK(land->result == truth,
                  "*** and produces the SAME answer as the uninterrupted run ***");
            CHECK(land->steps == truth_steps,
                  "with the work split across the two nodes summing to the single-node total");
            printf("      (node 1 ran %llu steps, node 2 the remaining %llu, result=%lld)\n",
                   (unsigned long long)mid_steps,
                   (unsigned long long)(truth_steps - mid_steps),
                   (long long)(int64_t)land->result);
        }
    }

    /* ═══ Scenario 5: STOPPED withdraws the context ═══════════════════ */
    printf("\n-- Scenario 5: the reconciler tears contexts down too --\n");
    {
        g_local_node = 1;
        partition_owner_table[papp].node_id = 1;
        CHECK(workload_declare(0, "sumjob", papp, WL_DESIRED_STOPPED,
                               "", SVC_ENDPOINT_TCP, 0, "loopsum", "main", WL_RESTART_NEVER) == WL_OK,
              "the workload is flipped to STOPPED");
        for (int i = 0; i < 5 && cycle(); i++) { }
        CHECK(!wlctx_has("sumjob"), "its live context is torn down");
        CHECK(simi_ctx_registered_count(papp) == 0,
              "...and unregistered, so migration stops moving it");
        CHECK(cycle() == 0, "and it settles");
    }

    /* ═══ Scenario 6: refusals ════════════════════════════════════════ */
    printf("\n-- Scenario 6: bad images are refused, not half-loaded --\n");
    {
        wlctx_init();
        uint8_t junk[64]; memset(junk, 0xAB, sizeof junk);
        CHECK(wlctx_start("bad", junk, sizeof junk, "main", papp) == WLCTX_ERR_BAD_IMAGE,
              "a buffer with no SIMI magic is refused");
        CHECK(wlctx_start("short", g_image, 8, "main", papp) == WLCTX_ERR_BAD_IMAGE,
              "an image shorter than its own header is refused");

        /* A header claiming more content than the buffer holds. This is
         * the one that would walk off the end if unchecked. */
        uint8_t trunc[128];
        memcpy(trunc, g_image, sizeof trunc);
        trunc[4] = 0xFF; trunc[5] = 0xFF; trunc[6] = 0; trunc[7] = 0;  /* 65535 instructions */
        CHECK(wlctx_start("lies", trunc, sizeof trunc, "main", papp) == WLCTX_ERR_TOO_BIG,
              "a header claiming more instructions than the caps allow is refused");

        uint8_t big[128];
        memcpy(big, g_image, sizeof big);
        big[4] = 0x00; big[5] = 0x01; big[6] = 0; big[7] = 0;          /* 256 instructions */
        CHECK(wlctx_start("overrun", big, sizeof big, "main", papp) == WLCTX_ERR_BAD_IMAGE,
              "a header whose declared content exceeds the buffer is refused");

        CHECK(wlctx_start("noentry", g_image, g_image_size, "nosuchentry", papp)
              == WLCTX_ERR_NO_ENTRY, "an unknown entry point is refused");
        CHECK(wlctx_count() == 0, "no partial context was left behind by any refusal");

        CHECK(wlctx_start("ok", g_image, g_image_size, "main", papp) == WLCTX_OK,
              "a good image starts");
        CHECK(wlctx_start("ok", g_image, g_image_size, "main", papp) == WLCTX_ERR_EXISTS,
              "starting the same workload twice is refused");
        CHECK(wlctx_stop("nope") == WLCTX_ERR_NOT_FOUND,
              "stopping an unknown workload is reported");
    }

    /* ═══ Scenario 7: pool capacity ═══════════════════════════════════ */
    printf("\n-- Scenario 7: the context pool is bounded --\n");
    {
        wlctx_init();
        char nm[16];
        int started = 0;
        for (int i = 0; i < WLCTX_MAX + 3; i++) {
            nm[0]='c'; nm[1]=(char)('0'+i/10); nm[2]=(char)('0'+i%10); nm[3]='\0';
            if (wlctx_start(nm, g_image, g_image_size, "main", papp) == WLCTX_OK) started++;
        }
        CHECK(started == WLCTX_MAX, "the pool fills to exactly WLCTX_MAX");
        CHECK(wlctx_start("extra", g_image, g_image_size, "main", papp) == WLCTX_ERR_FULL,
              "one more is refused with a distinct status");
        CHECK(simi_ctx_registered_count(papp) == WLCTX_MAX,
              "every live context is registered for migration");

        /* Terminal contexts stay registered but stop consuming budget. */
        for (int i = 0; i < 2000; i++) if (wlctx_step_all(64) == 0) break;
        CHECK(wlctx_step_all(64) == 0,
              "once every context has halted, stepping does no further work");
        CHECK(simi_ctx_registered_count(papp) == WLCTX_MAX,
              "...but halted contexts stay registered -- their final state is still worth moving");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
