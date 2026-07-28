/*
 * simi_ctx_migrate_host_test.c — Persistent Execution Contexts Phase 3:
 * a RUNNING computation moves between nodes and resumes where it stopped.
 *
 * Links the REAL, unmodified net/dspp.c, kernel/simi_ctx_migrate.c,
 * kernel/simi_ckpt.c and kernel/simi_interp.c. Nothing on the path from
 * "live context" to "Ethernet frame" to "resumed context" is
 * reimplemented here.
 *
 * ─── The technique, and what it does and does not prove ───────────────
 * Same two-simulated-node capture-and-replay that tests/cross_node_
 * migration_host_test.c established for stream migration: run the real
 * sender as node 1, capture every real Ethernet-framed packet
 * dspp_transmit_raw() actually produces, flip the settable node-identity
 * fake to node 2, reset all receive-side state, and feed those exact
 * captured bytes back through the real dspp_rx_dispatch(). One process
 * stands in for two machines.
 *
 * PROVES: wire encode/decode fidelity, dispatcher routing, reassembly,
 * every rejection path, and that the resumed computation produces the
 * same answer as an uninterrupted one.
 * DOES NOT PROVE: behaviour against a real NIC, real packet loss, or
 * reordering beyond what is injected here deliberately. There is no
 * retransmission in this phase (fire-and-forget, as with streams), so a
 * genuinely dropped chunk leaves a transfer incomplete -- scenario 5
 * verifies that this is detected and refused rather than silently
 * restoring a context with a hole in it.
 *
 * ─── Why scenario 6 is the centrepiece ────────────────────────────────
 * Phase 2 taught this the hard way: a checkpoint test can pass 29/29 and
 * still have a hole, because a program restarted from the top often
 * recomputes the right answer anyway. Migrating at ONE convenient point
 * proves almost nothing. Scenario 6 migrates at every instruction
 * boundary in the program and requires all of them to resume to the
 * identical result -- so a field that is only live mid-loop or between a
 * CALL and its RET cannot hide.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I tools/simi \
 *       -o /tmp/simi_ctx_migrate_host_test \
 *       tests/simi_ctx_migrate_host_test.c net/dspp.c kernel/simi_ctx_migrate.c \
 *       kernel/simi_ckpt.c kernel/simi_interp.c tools/simi/simi_obj.c
 *   /tmp/simi_ctx_migrate_host_test
 */
#include "tools/simi/simi_isa.h"
#include "tools/simi/simi_obj.h"
#include "kernel/simi_interp.h"
#include "kernel/simi_ckpt.h"
#include "kernel/simi_ctx_migrate.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "net/dspp.h"
#include "net/net.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── SIMI catalog bindings: the same mock the reference interpreter
 * uses, so corpus programs touching RESOLVE/OBJSIZE/OBJTYPE behave
 * identically here and in tests/simi_ckpt_host_test.c. ───────────────── */
static const struct { const char* name; uint64_t base; uint32_t size; uint32_t type; }
g_mock[] = { { "simi_add_test2", 0x2000, 303, 1 }, { "simi_verify3", 0x3000, 100, 2 } };
#define MOCK_N (sizeof(g_mock)/sizeof(g_mock[0]))
uint64_t simi_rt_resolve(const char* n) {
    for (size_t i = 0; i < MOCK_N; i++) if (strcmp(g_mock[i].name, n) == 0) return g_mock[i].base;
    return 0;
}
uint64_t simi_rt_objsize(uint64_t v) {
    for (size_t i = 0; i < MOCK_N; i++) if (g_mock[i].base == v) return g_mock[i].size;
    return 0;
}
uint64_t simi_rt_objtype(uint64_t v) {
    for (size_t i = 0; i < MOCK_N; i++) if (g_mock[i].base == v) return g_mock[i].type;
    return 0xFFFFFFFFu;
}
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── net/dspp.c dependencies outside this test's scope ────────────────
 * The Phase 5 page-routing family has its own dedicated coverage in
 * tests/dspp_phase5_host_test.c; permissive stand-ins here, the same
 * judgment call tests/cross_node_migration_host_test.c makes. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
int partition_is_local(uint32_t p) { (void)p; return 1; }
int partition_holds_write_lease(uint32_t p) { (void)p; return 1; }
void process_consensus_packet(struct DSPPFullPagePacket* p) { (void)p; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* p) { (void)p; }

/* ─── The stream migrate handlers, as OBSERVABLE stubs ─────────────────
 * Deliberately not the real kernel/stream.c. Both header families share
 * DSPP_MIGRATE_MAGIC, so the dispatcher has to tell them apart by opcode
 * -- and it must do so BEFORE applying a minimum-length check, because a
 * context header is SMALLER than a stream one. Get that wrong and every
 * context BEGIN is silently dropped as "too short", or worse, a context
 * packet is handed to the stream handler and read as a stream header.
 * Counting calls here makes both directions of that mistake visible. */
static int stream_begin_calls = 0;
static int stream_page_calls  = 0;
int stream_migrate_recv_begin(uint64_t tid, uint32_t pid, const char* name,
                              const char* mime, uint64_t size,
                              uint32_t frames_used, uint32_t owner_uid) {
    (void)tid; (void)pid; (void)name; (void)mime; (void)size;
    (void)frames_used; (void)owner_uid;
    stream_begin_calls++; return 0;
}
int stream_migrate_recv_page(uint64_t tid, uint32_t idx, const uint8_t* data) {
    (void)tid; (void)idx; (void)data;
    stream_page_calls++; return 0;
}

/* ─── Settable node identity: the one fake this test hinges on ─────────
 * Two real nodes are two processes each reading their own compiled-in
 * identity. Here, one mutable global the test flips between the send
 * phase (node 1) and the receive phase (node 2). */
static uint32_t g_local_node = 1;
uint32_t cluster_local_node_id(void) { return g_local_node; }

MACAddr net_my_mac;

/* ─── Packet capture ──────────────────────────────────────────────────
 * During the send phase, every frame is recorded. During replay the
 * receiver emits ACKs through this same path; those are counted
 * separately rather than polluting the capture being replayed. */
#define MAX_FRAMES 64
/* Sized to the LARGER of the two packet families, via the same
 * compile-time ternary dspp_transmit_raw() itself uses -- scenario 3
 * replays real STREAM packets too, and those are bigger (a stream header
 * is 177 bytes to a context header's 121). Sizing this off the context
 * packet alone silently truncates every stream page on capture, and the
 * receive path then correctly rejects it as short: a test-harness bug
 * that reads exactly like a routing regression. */
#define FRAME_CAP  (ETH_HDR_LEN + (sizeof(struct DSPPCtxMigrateChunkPacket) > \
                                   sizeof(struct DSPPMigratePagePacket) \
                                 ? sizeof(struct DSPPCtxMigrateChunkPacket) \
                                 : sizeof(struct DSPPMigratePagePacket)))
static uint8_t  cap_buf[MAX_FRAMES][FRAME_CAP];
static uint16_t cap_len[MAX_FRAMES];
static int      cap_count = 0;
static int      capturing = 1;
static int      ack_count = 0;
static uint8_t  last_ack_status = 0xFF;
static uint16_t last_ack_opcode = 0;

void e1000_transmit_packet(void* buf, uint16_t size) {
    if (!capturing) {
        /* An ACK from the simulated receiver. Nothing reads these in
         * production yet (fire-and-forget), but they carry the refusal
         * reason, which is what the negative scenarios assert on. */
        struct DSPPCtxMigrateHeader h;
        if (size >= ETH_HDR_LEN + sizeof(h)) {
            memcpy(&h, (uint8_t*)buf + ETH_HDR_LEN, sizeof(h));
            last_ack_status = h.status;
            last_ack_opcode = h.opcode;
        }
        ack_count++;
        return;
    }
    if (cap_count >= MAX_FRAMES) return;
    if (size > FRAME_CAP) size = (uint16_t)FRAME_CAP;
    memcpy(cap_buf[cap_count], buf, size);
    cap_len[cap_count] = size;
    cap_count++;
}

/* Feeds one captured frame back through the REAL receive path, stripping
 * the Ethernet header exactly as net/net.c's ETHERTYPE_DSPP branch does. */
static void replay(int i) {
    dspp_rx_dispatch(cap_buf[i] + ETH_HDR_LEN, (uint16_t)(cap_len[i] - ETH_HDR_LEN));
}
static void replay_all(void) { for (int i = 0; i < cap_count; i++) replay(i); }

static struct DSPPCtxMigrateHeader* hdr_of(int i) {
    return (struct DSPPCtxMigrateHeader*)(cap_buf[i] + ETH_HDR_LEN);
}

static struct SimiContext ctx_a;

static SimiStatus finish(struct SimiContext* c) {
    SimiStatus st = SIMI_STATUS_OK;
    for (int g = 0; g < 200000 && st == SIMI_STATUS_OK; g++) st = simi_interp_run(c, 64);
    return st;
}

/* Live-state identity. NOT a memcmp of the whole struct: frames above
 * frame_top are dead, hold stale data on the sender, and are deliberately
 * not serialised, so comparing them would fail for a correct
 * implementation. Everything that IS live is compared field by field --
 * the Phase 2 lesson was that a coarser check passes while a real field
 * is being dropped. */
static int same_live_state(const struct SimiContext* x, const struct SimiContext* y) {
    if (x->pc != y->pc)               return 0;
    if (x->frame_top != y->frame_top) return 0;
    if (x->steps != y->steps)         return 0;
    if (x->result != y->result)       return 0;
    if (x->status != y->status)       return 0;
    if (x->trap_pc != y->trap_pc)     return 0;
    if (x->obj != y->obj)             return 0;
    if (memcmp(x->mem, y->mem, SIMI_MEM_SIZE) != 0) return 0;
    for (int32_t f = 0; f <= x->frame_top; f++)
        if (memcmp(&x->frames[f], &y->frames[f], sizeof(struct SimiFrame)) != 0) return 0;
    return 1;
}

/* One full node-1 -> node-2 migration. Returns chunks sent. */
static uint32_t migrate(struct SimiContext* src, const SimiObject* image_for_b,
                        uint32_t dest_node) {
    cap_count = 0; capturing = 1;
    g_local_node = 1;
    uint32_t chunks = simi_ctx_migrate_send(src, 0xABCDEF01ull, dest_node, 7, "worker");

    /* ── Now we are node 2: fresh receive-side state, new identity ──── */
    capturing = 0;
    g_local_node = dest_node;
    simi_ctx_migrate_reset();
    simi_ctx_migrate_clear_images();
    if (image_for_b) simi_ctx_migrate_register_image(image_for_b);
    ack_count = 0;
    replay_all();
    return chunks;
}

int main(void) {
    printf("=== PEC Phase 3: live context migration across nodes ===\n\n");

    SimiObject obj = {0};
    if (simi_obj_read("tools/simi/tests/loop_sum.tmo", &obj) != 0) {
        printf("FAIL: cannot read corpus program\n");
        return 1;
    }
    SimiObject other = {0};
    int have_other = (simi_obj_read("tools/simi/tests/call_ret.tmo", &other) == 0);

    /* Ground truth: one uninterrupted run on a single node. */
    simi_interp_init(&ctx_a, &obj, "main");
    SimiStatus st = finish(&ctx_a);
    uint64_t truth       = ctx_a.result;
    uint64_t truth_steps = ctx_a.steps;
    CHECK(st == SIMI_STATUS_HALTED, "baseline: program completes on one node, uninterrupted");
    printf("      (uninterrupted: result=%lld in %llu steps)\n",
           (long long)(int64_t)truth, (unsigned long long)truth_steps);

    /* ═══ Scenario 1: THE DEMO ═══════════════════════════════════════
     * A computation halfway through a loop on node 1, resuming on node 2
     * with the loop counter intact. */
    printf("\n-- Scenario 1: halfway through a loop on node 1, finishes on node 2 --\n");
    {
        simi_interp_init(&ctx_a, &obj, "main");
        SimiStatus mid = simi_interp_run(&ctx_a, truth_steps / 2);
        CHECK(mid == SIMI_STATUS_OK,
              "node 1: stopped mid-execution, still runnable (not halted, not trapped)");
        CHECK(ctx_a.steps > 0 && ctx_a.steps < truth_steps,
              "node 1: genuinely partway through -- some work done, some remaining");
        CHECK(ctx_a.result != truth || truth == 0,
              "node 1: has NOT yet computed the answer (so finishing it proves real resumption)");
        uint32_t mid_pc    = ctx_a.pc;
        uint64_t mid_steps = ctx_a.steps;

        uint32_t chunks = migrate(&ctx_a, &obj, 2);
        CHECK(chunks > 1, "context spans multiple chunks (a checkpoint exceeds one 4 KiB packet)");
        CHECK(simi_ctx_migrate_arrived(), "node 2: context arrived and restored");

        struct SimiContext* land = simi_ctx_migrate_landed();
        CHECK(land->pc == mid_pc,
              "node 2: resumes at the EXACT instruction node 1 stopped at");
        CHECK(land->steps == mid_steps,
              "node 2: retired-step count carried across -- the loop counter is intact");
        CHECK(same_live_state(&ctx_a, land),
              "node 2: every live field identical to node 1 (registers, frames, memory, status)");

        SimiStatus fin = finish(land);
        CHECK(fin == SIMI_STATUS_HALTED, "node 2: the migrated computation runs to completion");
        CHECK(land->result == truth,
              "node 2: produces the SAME answer as the uninterrupted single-node run");
        CHECK(land->steps == truth_steps,
              "node 2: total work across both nodes equals the single-node total -- no lost or repeated steps");
        printf("      (node 1 ran %llu steps, node 2 ran the remaining %llu, result=%lld)\n",
               (unsigned long long)mid_steps,
               (unsigned long long)(truth_steps - mid_steps),
               (long long)(int64_t)land->result);
    }

    /* ═══ Scenario 2: what actually went on the wire ══════════════════ */
    printf("\n-- Scenario 2: wire format --\n");
    {
        simi_interp_init(&ctx_a, &obj, "main");
        simi_interp_run(&ctx_a, truth_steps / 2);

        /* Poison the context's flat memory before checkpointing. This is
         * what makes the "final chunk's tail is zeroed" check below mean
         * anything. A checkpoint's payload ENDS with ctx->mem, and this
         * program leaves almost all 64 KiB of it zero -- so the last
         * chunk's unused tail holds leftover bytes from the previous
         * chunk that are themselves zero, and the check passes whether
         * or not the sender actually zeroes anything. Mutation testing
         * caught exactly that: removing the zeroing loop in
         * dspp_ctx_migrate_send_chunk() left the test still passing.
         * With a non-zero pattern in memory, a non-zeroed tail is
         * visibly the previous chunk's data. */
        for (uint32_t i = 0; i < SIMI_MEM_SIZE; i++) ctx_a.mem[i] = (uint8_t)(0xC0 | (i & 0x0F));

        uint32_t chunks = migrate(&ctx_a, &obj, 2);

        CHECK(cap_count == (int)chunks + 1, "one BEGIN packet followed by exactly total_chunks CHUNK packets");

        struct EthernetHeader* eth = (struct EthernetHeader*)cap_buf[0];
        CHECK(ntohs(eth->ethertype) == ETHERTYPE_DSPP,
              "frames are real Ethernet frames carrying ETHERTYPE_DSPP");

        struct DSPPCtxMigrateHeader* b = hdr_of(0);
        CHECK(b->magic == DSPP_MIGRATE_MAGIC, "BEGIN carries the shared migrate magic");
        CHECK(b->opcode == DSPP_MIGRATE_CTX_BEGIN_REQ, "first packet is CTX_BEGIN_REQ");
        CHECK(b->node_source_id == 1 && b->node_dest_id == 2, "BEGIN is addressed node 1 -> node 2");
        CHECK(b->partition_id == 7, "partition id travels with the context");
        CHECK(strcmp(b->ctx_name, "worker") == 0, "context name survives the wire");
        CHECK(b->total_chunks == chunks, "BEGIN announces the chunk count the sender actually sends");
        CHECK(b->program_hash == simi_ckpt_program_hash(&obj),
              "BEGIN carries the program hash, so the receiver can refuse a mismatched image");
        CHECK(b->total_bytes == simi_ckpt_size(&ctx_a),
              "BEGIN announces exactly the checkpoint size");

        /* Interior chunks full, last one short -- and the short one's
         * unused tail must not leak whatever was on the stack. */
        struct DSPPCtxMigrateHeader* c1 = hdr_of(1);
        CHECK(c1->opcode == DSPP_MIGRATE_CTX_CHUNK_REQ, "subsequent packets are CTX_CHUNK_REQ");
        CHECK(c1->chunk_bytes == DSPP_CTX_CHUNK_BYTES, "interior chunks are full");
        struct DSPPCtxMigrateChunkPacket* last =
            (struct DSPPCtxMigrateChunkPacket*)(cap_buf[cap_count - 1] + ETH_HDR_LEN);
        CHECK(last->header.chunk_index == chunks - 1, "final chunk is the last index");
        CHECK(last->header.chunk_bytes > 0 && last->header.chunk_bytes <= DSPP_CTX_CHUNK_BYTES,
              "final chunk declares a valid short length");
        int tail_clean = 1;
        for (uint32_t i = last->header.chunk_bytes; i < DSPP_CTX_CHUNK_BYTES; i++)
            if (last->chunk_data[i] != 0) { tail_clean = 0; break; }
        CHECK(tail_clean, "final chunk's unused tail is zeroed, not leaked kernel memory");

        uint64_t total = 0;
        for (int i = 1; i < cap_count; i++) total += hdr_of(i)->chunk_bytes;
        CHECK(total == b->total_bytes, "chunk lengths sum exactly to the announced total");
        CHECK(ack_count == cap_count, "receiver ACKs every packet it accepts");
    }

    /* ═══ Scenario 3: dispatcher routing ══════════════════════════════
     * The regression this guards: both families share a magic, and the
     * context header is SMALLER than the stream header. */
    printf("\n-- Scenario 3: dispatcher tells the two migrate families apart --\n");
    {
        stream_begin_calls = stream_page_calls = 0;
        simi_interp_init(&ctx_a, &obj, "main");
        simi_interp_run(&ctx_a, truth_steps / 2);
        migrate(&ctx_a, &obj, 2);
        CHECK(stream_begin_calls == 0 && stream_page_calls == 0,
              "context packets never reach the stream handler");
        CHECK(simi_ctx_migrate_arrived(),
              "a bare CTX_BEGIN is NOT dropped for being shorter than a stream header");

        /* And the other direction: a real stream packet still routes to
         * the stream handler now that a second family shares the magic. */
        capturing = 1; cap_count = 0; g_local_node = 1;
        dspp_migrate_send_begin(1, 2, 3, "s", "text/plain", 4096, 1, 0);
        uint8_t page[4096]; memset(page, 0xA5, sizeof page);
        dspp_migrate_send_page(1, 2, 3, 0, page);
        capturing = 0; g_local_node = 2;
        stream_begin_calls = stream_page_calls = 0;
        replay_all();
        CHECK(stream_begin_calls == 1, "a stream BEGIN still routes to the stream handler");
        CHECK(stream_page_calls == 1,  "a stream PAGE still routes to the stream handler");
    }

    /* ═══ Scenario 4: self-filtering ══════════════════════════════════ */
    printf("\n-- Scenario 4: a node ignores traffic addressed elsewhere --\n");
    {
        simi_interp_init(&ctx_a, &obj, "main");
        simi_interp_run(&ctx_a, truth_steps / 2);
        /* Sent to node 2, but node 3 is listening. DSPP is L2-broadcast,
         * so node 3 genuinely receives these frames and must discard them. */
        migrate(&ctx_a, &obj, 2);
        CHECK(simi_ctx_migrate_arrived(), "sanity: the addressed node does accept it");

        cap_count = 0; capturing = 1; g_local_node = 1;
        simi_ctx_migrate_send(&ctx_a, 0x55ull, 2, 7, "worker");
        capturing = 0;
        g_local_node = 3;                       /* an unrelated node on the same segment */
        simi_ctx_migrate_reset();
        simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        ack_count = 0;
        replay_all();
        CHECK(!simi_ctx_migrate_arrived(), "an unaddressed node does not restore the context");
        CHECK(ack_count == 0, "an unaddressed node stays silent -- no ACK storm from a broadcast");
    }

    /* ═══ Scenario 5: every refusal path ══════════════════════════════
     * A migration layer that accepts a damaged or mismatched transfer is
     * worse than one that refuses, because it resumes execution against
     * state that means something else. */
    printf("\n-- Scenario 5: refusals --\n");
    {
        simi_interp_init(&ctx_a, &obj, "main");
        simi_interp_run(&ctx_a, truth_steps / 2);
        cap_count = 0; capturing = 1; g_local_node = 1;
        uint32_t chunks = simi_ctx_migrate_send(&ctx_a, 0x77ull, 2, 7, "worker");
        capturing = 0; g_local_node = 2;

        /* (a) No local image with that program hash. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        ack_count = 0; last_ack_status = 0xFF;
        replay(0);
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_NO_PROGRAM,
              "BEGIN refused early when no local image matches the program hash");
        CHECK(last_ack_opcode == DSPP_MIGRATE_CTX_BEGIN_ACK, "refusal comes back as a BEGIN_ACK");
        replay(1);
        CHECK(!simi_ctx_migrate_arrived(), "chunks for a refused transfer are not reassembled");

        /* (b) Wrong image registered -- a real program, but not this one. */
        if (have_other) {
            simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
            simi_ctx_migrate_register_image(&other);
            last_ack_status = 0xFF;
            replay(0);
            CHECK(last_ack_status == SIMI_CTXMIG_ERR_NO_PROGRAM,
                  "a DIFFERENT program image is refused, not accepted as close enough");
        }

        /* (c) A dropped chunk leaves the transfer incomplete. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        for (int i = 0; i < cap_count; i++) if (i != 2) replay(i);
        CHECK(!simi_ctx_migrate_arrived(),
              "a dropped chunk is detected -- no restore from a partial checkpoint");

        /* (d) The same chunk twice must not be counted twice. This is the
         * subtle one: counting arrivals instead of tracking WHICH arrived
         * would call the transfer complete with a hole still in it. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        replay(0);
        for (int i = 1; i < cap_count; i++) { if (i != 2) { replay(i); replay(i); } }
        CHECK(!simi_ctx_migrate_arrived(),
              "duplicate chunks do not substitute for a missing one");
        replay(2);
        CHECK(simi_ctx_migrate_arrived(), "...and supplying the missing chunk then completes it");

        /* (e) Out-of-order delivery is fine -- chunks are indexed. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        replay(0);
        for (int i = cap_count - 1; i >= 1; i--) replay(i);
        CHECK(simi_ctx_migrate_arrived(), "chunks arriving in reverse order still reassemble");
        CHECK(same_live_state(&ctx_a, simi_ctx_migrate_landed()),
              "...and reassemble to the identical state");

        /* (f) A corrupted byte is caught by the checkpoint checksum. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        struct DSPPCtxMigrateChunkPacket* c =
            (struct DSPPCtxMigrateChunkPacket*)(cap_buf[1] + ETH_HDR_LEN);
        uint8_t save = c->chunk_data[100];
        c->chunk_data[100] ^= 0xFF;
        last_ack_status = 0xFF;
        replay_all();
        CHECK(!simi_ctx_migrate_arrived(), "a single flipped payload byte prevents the restore");
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_RESTORE,
              "...and the sender is told the restore was refused, not that it succeeded");
        c->chunk_data[100] = save;

        /* (g) A second transfer while one is in flight. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        replay(0); replay(1);
        struct DSPPCtxMigrateHeader other_begin = *hdr_of(0);
        other_begin.transfer_id = 0x999ull;
        last_ack_status = 0xFF;
        dspp_rx_dispatch(&other_begin, (uint16_t)sizeof(other_begin));
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_BUSY,
              "a competing transfer is refused rather than clobbering the one in flight");
        for (int i = 2; i < cap_count; i++) replay(i);
        CHECK(simi_ctx_migrate_arrived(), "...and the original transfer still completes");

        /* (h) A chunk for a transfer that never began. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        last_ack_status = 0xFF;
        replay(1);
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_NO_TRANSFER,
              "a chunk with no preceding BEGIN is rejected");

        /* (i) A BEGIN whose chunk count contradicts its byte count. */
        struct DSPPCtxMigrateHeader bad = *hdr_of(0);
        bad.total_chunks = chunks + 5;
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        last_ack_status = 0xFF;
        dspp_rx_dispatch(&bad, (uint16_t)sizeof(bad));
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_BAD_CHUNK,
              "a BEGIN whose chunk count disagrees with its byte count is rejected");

        /* (j) A checkpoint larger than the receiver can hold. */
        bad = *hdr_of(0);
        bad.total_bytes  = 0xFFFFFFFFull;
        bad.total_chunks = (uint32_t)((bad.total_bytes + DSPP_CTX_CHUNK_BYTES - 1) / DSPP_CTX_CHUNK_BYTES);
        simi_ctx_migrate_reset();
        last_ack_status = 0xFF;
        dspp_rx_dispatch(&bad, (uint16_t)sizeof(bad));
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_TOO_LARGE,
              "an oversized checkpoint is refused up front, not overflowed into");

        /* (k) A short interior chunk would leave a zero gap the length
         * arithmetic still considers covered. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        replay(0);
        struct DSPPCtxMigrateChunkPacket runt;
        memcpy(&runt, cap_buf[1] + ETH_HDR_LEN, sizeof(runt));
        runt.header.chunk_bytes = 16;
        last_ack_status = 0xFF;
        dspp_rx_dispatch(&runt, (uint16_t)sizeof(runt));
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_BAD_CHUNK,
              "a short INTERIOR chunk is rejected (only the last may be short)");

        /* (l) A chunk index past the end of the transfer. */
        memcpy(&runt, cap_buf[1] + ETH_HDR_LEN, sizeof(runt));
        runt.header.chunk_index = chunks + 100;
        last_ack_status = 0xFF;
        dspp_rx_dispatch(&runt, (uint16_t)sizeof(runt));
        CHECK(last_ack_status == SIMI_CTXMIG_ERR_BAD_CHUNK,
              "a chunk index past the end of the transfer is rejected");

        /* (m) A truncated CHUNK packet -- header present, payload not. */
        simi_ctx_migrate_reset(); simi_ctx_migrate_clear_images();
        simi_ctx_migrate_register_image(&obj);
        replay(0);
        ack_count = 0;
        dspp_rx_dispatch(cap_buf[1] + ETH_HDR_LEN, (uint16_t)sizeof(struct DSPPCtxMigrateHeader));
        CHECK(ack_count == 0, "a CHUNK truncated before its payload is dropped, not read past");
    }

    /* ═══ Scenario 6: migrate at EVERY instruction boundary ═══════════
     * The Phase 2 lesson, applied. One convenient migration point proves
     * almost nothing, because a program restarted from the top often
     * recomputes the right answer anyway. */
    printf("\n-- Scenario 6: migrate at every instruction boundary --\n");
    {
        int points = 0, ok = 0, arrived = 0, exact = 0;
        for (uint64_t at = 0; at < truth_steps; at++) {
            simi_interp_init(&ctx_a, &obj, "main");
            if (at) simi_interp_run(&ctx_a, at);
            points++;

            migrate(&ctx_a, &obj, 2);
            if (!simi_ctx_migrate_arrived()) continue;
            arrived++;

            struct SimiContext* land = simi_ctx_migrate_landed();
            if (same_live_state(&ctx_a, land)) exact++;

            SimiStatus f = finish(land);
            if (f == SIMI_STATUS_HALTED && land->result == truth && land->steps == truth_steps) ok++;
        }
        CHECK(points > 0, "the sweep actually ran");
        CHECK(arrived == points, "every migration point transfers and restores");
        CHECK(exact == points, "every migration point restores byte-identical live state");
        CHECK(ok == points, "every migration point resumes to the identical final result");
        printf("      (%d migration points, all resumed to result=%lld in %llu total steps)\n",
               points, (long long)(int64_t)truth, (unsigned long long)truth_steps);
    }

    /* ═══ Scenario 7: a context that has not started, and one that has
     * finished. Both are legal things to move. ═══════════════════════ */
    printf("\n-- Scenario 7: edge-of-lifetime contexts --\n");
    {
        simi_interp_init(&ctx_a, &obj, "main");
        migrate(&ctx_a, &obj, 2);
        CHECK(simi_ctx_migrate_arrived(), "a context that has not executed a single step migrates");
        struct SimiContext* land = simi_ctx_migrate_landed();
        CHECK(finish(land) == SIMI_STATUS_HALTED && land->result == truth,
              "...and runs from the beginning on the new node to the correct result");

        simi_interp_init(&ctx_a, &obj, "main");
        finish(&ctx_a);
        migrate(&ctx_a, &obj, 2);
        CHECK(simi_ctx_migrate_arrived(), "a HALTED context migrates (its result is the payload)");
        CHECK(simi_ctx_migrate_landed()->result == truth,
              "...carrying its computed result to the new node");
        CHECK(simi_ctx_migrate_landed()->status == SIMI_STATUS_HALTED,
              "...and remains halted rather than becoming runnable again");
    }

    /* ═══ Scenario 8: the partition-level registry ════════════════════
     * This is what partition_migrate() Step 3b iterates. It is empty on
     * every current boot (nothing in the kernel creates long-lived
     * contexts yet), so without this scenario the whole partition path
     * would ship untested purely because its producer does not exist. */
    printf("\n-- Scenario 8: partition-level context registry --\n");
    {
        static struct SimiContext c1, c2, c3;
        simi_interp_init(&c1, &obj, "main"); simi_interp_run(&c1, 10);
        simi_interp_init(&c2, &obj, "main"); simi_interp_run(&c2, 20);
        simi_interp_init(&c3, &obj, "main"); simi_interp_run(&c3, 30);

        CHECK(simi_ctx_registered_count(7) == 0, "registry starts empty");
        CHECK(simi_ctx_migrate_send_partition(7, 2) == 0,
              "migrating a partition with no live contexts sends nothing (the current-boot case)");

        CHECK(simi_ctx_register(7, "a", &c1) == 0, "a context registers to a partition");
        CHECK(simi_ctx_register(7, "b", &c2) == 0, "a second context registers to the same partition");
        CHECK(simi_ctx_register(9, "c", &c3) == 0, "a context registers to a DIFFERENT partition");
        CHECK(simi_ctx_registered_count(7) == 2 && simi_ctx_registered_count(9) == 1,
              "counts are per-partition");
        CHECK(simi_ctx_register(7, "a-again", &c1) == 0 && simi_ctx_registered_count(7) == 2,
              "registering the same context twice is idempotent, not a duplicate");

        cap_count = 0; capturing = 1; g_local_node = 1;
        uint32_t sent = simi_ctx_migrate_send_partition(7, 2);
        CHECK(sent == 2, "migrating partition 7 sends exactly its OWN two contexts");
        CHECK(simi_ctx_registered_count(9) == 1, "partition 9's context is untouched");

        /* Each context must get a distinct transfer_id, or the receiver
         * would treat the second BEGIN as a restart of the first. */
        uint64_t tid_a = 0, tid_b = 0;
        int begins = 0;
        for (int i = 0; i < cap_count; i++) {
            if (hdr_of(i)->opcode != DSPP_MIGRATE_CTX_BEGIN_REQ) continue;
            if (begins++ == 0) tid_a = hdr_of(i)->transfer_id; else tid_b = hdr_of(i)->transfer_id;
        }
        CHECK(begins == 2, "one BEGIN per migrated context");
        CHECK(tid_a != tid_b, "each context gets a distinct transfer id");

        simi_ctx_unregister(&c1);
        CHECK(simi_ctx_registered_count(7) == 1, "unregistering removes exactly one context");
        capturing = 1; cap_count = 0;
        CHECK(simi_ctx_migrate_send_partition(7, 2) == 1, "...and it is no longer migrated");

        simi_ctx_unregister(&c2); simi_ctx_unregister(&c3);
        CHECK(simi_ctx_registered_count(7) == 0 && simi_ctx_registered_count(9) == 0,
              "registry drains cleanly");
        capturing = 0;
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
