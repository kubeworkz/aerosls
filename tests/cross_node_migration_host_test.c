/*
 * cross_node_migration_host_test.c -- Multi-Node Partition Scaling Roadmap
 * Phase 7 (real cross-node data movement) verification: a standalone
 * host-buildable test proving a stream's data genuinely survives a real
 * DSPP-wire round trip between two simulated nodes, linked against the
 * REAL, unmodified net/dspp.c and kernel/stream.c -- not a reimplementation
 * of either.
 *
 * ─── Why "two simulated nodes" means what it means here ────────────────
 * This codebase has exactly one compiled kernel image; two real, separate
 * machines each have their own independent stream_store[]/NVMe globals by
 * construction (they're two different running processes on two different
 * computers). A host test cannot spin up two kernels, so -- the same
 * "reuse the one process, replay state to represent the other side"
 * technique tests/persist_partition_host_test.c already established for
 * simulating a reboot without wiping the fake NVMe -- this file runs the
 * REAL sender-side code first (acting as "node A"), captures every real
 * Ethernet-framed DSPP packet it actually transmits, then resets
 * stream_store[] (node identity's own bookkeeping) to represent a fresh
 * "node B" and feeds those exact captured bytes through the REAL receive
 * path (net/net.c's own ETHERTYPE_DSPP branch logic, net/dspp.c's
 * dspp_rx_dispatch(), kernel/stream.c's stream_migrate_recv_begin()/
 * _page()). The underlying fake NVMe byte store is NOT reset between the
 * two phases -- unlike stream_store[] (each node's own separate
 * bookkeeping), the "physical medium" a fake NVMe stands in for doesn't
 * need to simulate non-interference between two real disks; two real
 * machines are naturally on separate hardware regardless, so this test
 * only needs to prove correct wire encode/decode and apply-side fidelity,
 * which a single shared backing store proves just as well as two would.
 * cluster_local_node_id() is a settable fake (not the real net/consensus.c)
 * so this file can represent "whichever node's identity is currently
 * active" without linking consensus.c's own heavier machinery, which this
 * data-movement-focused test has no interest in exercising.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I user \
 *       -o /tmp/cross_node_migration_host_test \
 *       tests/cross_node_migration_host_test.c kernel/stream.c net/dspp.c
 *   /tmp/cross_node_migration_host_test
 */
#include "kernel/stream.h"
#include "net/e1000.h"   /* NicRole */
#include "kernel/partition.h"
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include "net/dspp.h"
#include "net/net.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "kernel/simi_ctx_migrate.h"   // PEC Phase 3 -- stubbed below

/* ─── PEC Phase 3 stubs: net/dspp.c's context-migration receive path ───
 * FAITHFUL. This test never sends CTX_* opcodes, so these are not
 * reached; and were one to arrive, the real receiver with no registered
 * program image returns exactly SIMI_CTXMIG_ERR_NO_PROGRAM. Full
 * coverage of this path lives in tests/simi_ctx_migrate_host_test.c. */
SimiCtxMigStatus simi_ctx_migrate_recv_begin(uint64_t tid, const char* name,
                                             uint64_t total_bytes,
                                             uint32_t total_chunks,
                                             uint64_t program_hash) {
    (void)tid; (void)name; (void)total_bytes; (void)total_chunks; (void)program_hash;
    return SIMI_CTXMIG_ERR_NO_PROGRAM;
}
SimiCtxMigStatus simi_ctx_migrate_recv_chunk(uint64_t tid, uint32_t idx,
                                             const uint8_t* data, uint32_t n) {
    (void)tid; (void)idx; (void)data; (void)n;
    return SIMI_CTXMIG_ERR_NO_TRANSFER;
}


static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else          { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Stubs for kernel/stream.c's dependencies (mirrors tests/migration_
 * data_movement_host_test.c's own identical choices) ────────────────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }
uint64_t sys_sls_insert(struct SLSRecordRequest* req) { (void)req; return 0; }
uint64_t sys_sls_update(struct SLSRecordRequest* req) { (void)req; return 0; }
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 1; }
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) { (void)partition_id; return malloc(FRAME_SIZE); }

/* ─── Stubs for net/dspp.c's dependencies not under test here ────────────
 * dspp_page_read_allowed()/_write_allowed()/process_dspp_page_packet()'s
 * own real logic already has dedicated coverage in dspp_phase5_host_test.c
 * -- this file only exercises the Phase 7 migrate family, so these are
 * permissive stand-ins, the same judgment call used throughout this
 * suite for out-of-scope cross-subsystem primitives. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
int partition_is_local(uint32_t partition_id) { (void)partition_id; return 1; }
int partition_holds_write_lease(uint32_t partition_id) { (void)partition_id; return 1; }
/* Both now take a kernel_tick_counter reading: accepting a heartbeat or
 * granting a vote restarts an election timer that measures wall-clock
 * ticks rather than call counts (net/consensus.h, "Election timing").
 * FAITHFUL as no-ops here -- this test drives the migration opcodes, never
 * the consensus ones, so neither body is reached. */
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }

/* net/dspp.c reads this to timestamp consensus packets it routes. Held at
 * 0 because this test never exercises that branch; a consensus test would
 * need to drive it (see tests/consensus_phase1_host_test.c's now_ticks). */
volatile uint64_t kernel_tick_counter = 0;

/* ─── Service-replication receive side, as OBSERVABLE stubs ───────────
 * A THIRD header now shares DSPP_MIGRATE_MAGIC. The dispatcher has to
 * tell all three apart by opcode before it can apply any length check --
 * and the service header is a different size again. Recording what
 * arrives is what makes a misroute visible rather than silent. */
static int learn_calls = 0, forget_calls = 0;
static char last_learned[64];
static uint32_t last_node, last_part, last_port;
static uint8_t last_serving;
void service_remote_learn(const char* name, uint32_t node_id, uint32_t partition_id,
                          uint8_t kind, uint32_t port, uint32_t uid, uint8_t serving) {
    (void)kind; (void)uid;
    last_serving = serving;
    learn_calls++;
    for (int i = 0; i < 64; i++) { last_learned[i] = name[i]; if (!name[i]) break; }
    last_node = node_id; last_part = partition_id; last_port = port;
}
void service_remote_forget(const char* name, uint32_t node_id) {
    (void)name; (void)node_id; forget_calls++;
}

/* ─── The one fake this whole test hinges on: settable node identity ─────
 * Real node A and node B are two different processes each with their own
 * real cluster_local_node_id() reading their own compiled-in identity.
 * Simulated here as a single mutable global this test flips between the
 * send phase (acting as node A) and the receive phase (acting as node B)
 * -- the same "g_fake_local_node_id" settable-fake technique tests/
 * partition_host_test.c already established for this exact function. */
static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }

/* net/net.c's real global -- not linked here; a plain zero-init stand-in
 * is fine since this test only checks ethertype/opcode/payload fields, not
 * the src MAC dspp_transmit_raw() stamps into the Ethernet header. */
MACAddr net_my_mac;

/* ─── e1000_transmit_packet(): captures every real Ethernet-framed DSPP
 * packet dspp_transmit_raw() actually produces, instead of a plain counter
 * -- this test's whole point is proving those captured bytes, when fed
 * back through the real receive path, reconstruct the original data. ──── */
#define MAX_CAPTURED_FRAMES 32
static uint8_t  captured_frame[MAX_CAPTURED_FRAMES][ETH_HDR_LEN + sizeof(struct DSPPMigratePagePacket)];
static uint16_t captured_frame_len[MAX_CAPTURED_FRAMES];
static int      captured_frame_count = 0;
/* Multi-NIC Phase 2 renamed this and gave it a role. The capture is
 * unchanged -- this test's point is that the captured bytes replay through
 * the receive path -- but the role is recorded too, so it can also assert
 * DSPP leaves by the CLUSTER interface rather than the management one. */
static NicRole last_tx_role = NIC_ROLE_NONE;
void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    last_tx_role = role;
    if (captured_frame_count >= MAX_CAPTURED_FRAMES) return;
    if (size > sizeof(captured_frame[0])) { size = (uint16_t)sizeof(captured_frame[0]); }
    memcpy(captured_frame[captured_frame_count], buf, size);
    captured_frame_len[captured_frame_count] = size;
    captured_frame_count++;
}

/* ─── Stateful fake NVMe (identical technique to tests/migration_data_
 * movement_host_test.c's own -- see this file's header comment on why one
 * shared backing store correctly serves both simulated nodes here). ────── */
void* io_sq = (void*)1;
void* io_cq = (void*)1;
#define FAKE_NVME_MAX_PAGES 64
static uint64_t fake_lba[FAKE_NVME_MAX_PAGES];
static uint8_t  fake_page[FAKE_NVME_MAX_PAGES][4096];
static int      fake_count = 0;
static int fake_find(uint64_t slba) {
    for (int i = 0; i < fake_count; i++) if (fake_lba[i] == slba) return i;
    return -1;
}
int nvme_read_sync(uint64_t slba, void* buf) {
    int idx = fake_find(slba);
    if (idx < 0) { memset(buf, 0, 4096); return 0; }
    memcpy(buf, fake_page[idx], 4096);
    return 0;
}
int nvme_write_pages_gather_sync(uint64_t slba, const void* const* pages, uint32_t page_count) {
    /* Scatter-gather transfer (drivers/nvme_io.h): kernel/stream.c batches a
     * stream's scattered frame-pool frames into one command per contiguous LBA
     * run. Faithful loop over the single-page fake above rather than a no-op,
     * so the bytes still land at exactly the LBAs the real driver would use --
     * every existing assertion in this file keeps verifying real behaviour
     * through the new path. */
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_write_sync(slba + (uint64_t)i * 8, pages[i]) != 0) return 1;
    return 0;
}
int nvme_read_pages_gather_sync(uint64_t slba, void* const* pages, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_read_sync(slba + (uint64_t)i * 8, pages[i]) != 0) return 1;
    return 0;
}
int nvme_write_sync(uint64_t slba, const void* buf) {
    int idx = fake_find(slba);
    if (idx < 0) {
        if (fake_count >= FAKE_NVME_MAX_PAGES) return -1;
        idx = fake_count++;
        fake_lba[idx] = slba;
    }
    memcpy(fake_page[idx], buf, 4096);
    return 0;
}

/* Feeds one captured Ethernet frame through the real receive path exactly
 * the way net/net.c's net_rx_dispatch() would -- strips the Ethernet
 * header and hands the rest to dspp_rx_dispatch(), the real function, not
 * a reimplementation of its routing logic. */
static void deliver_captured_frame(int i) {
    struct EthernetHeader* eth = (struct EthernetHeader*)captured_frame[i];
    CHECK(ntohs(eth->ethertype) == ETHERTYPE_DSPP, "captured frame really is Ethernet-framed with ETHERTYPE_DSPP, not a bare struct with no L2 framing");
    dspp_rx_dispatch(captured_frame[i] + ETH_HDR_LEN, (uint16_t)(captured_frame_len[i] - ETH_HDR_LEN));
}

int main(void) {
    /* ── Setup: "node A" (id 1) has a real stream with two pages of known,
     * distinct byte content already on its own disk. ────────────────────── */
    g_fake_local_node_id = 1;
    memset(stream_store, 0, sizeof(stream_store));

    strcpy(stream_store[0].name, "report.pdf");
    strcpy(stream_store[0].mime_type, "application/pdf");
    stream_store[0].size         = 7000;
    stream_store[0].frames_used  = 2;
    stream_store[0].lba_base     = STREAM_DATA_LBA_BASE;   /* slot 0's own real LBA formula */
    stream_store[0].active       = 1;
    stream_store[0].owner_uid    = 500;
    stream_store[0].partition_id = 77;

    uint8_t page0[4096], page1[4096];
    memset(page0, 0xAA, sizeof(page0));
    memset(page1, 0xBB, sizeof(page1));
    CHECK(nvme_write_sync(stream_store[0].lba_base + 0, page0) == 0, "setup: node A's page 0 written to its own disk");
    CHECK(nvme_write_sync(stream_store[0].lba_base + 8, page1) == 0, "setup: node A's page 1 written to its own disk");

    /* ── Scenario 1: the real send. stream_migrate_send_partition() is the
     * exact function kernel/partition.c's partition_migrate() calls once a
     * real cluster is configured -- called directly here to isolate the
     * data-movement mechanics from migration orchestration, which already
     * has its own dedicated coverage (tests/partition_migrate_phase6_
     * host_test.c). ─────────────────────────────────────────────────────── */
    uint64_t oversize_before = dspp_tx_oversize_dropped;
    int sent = stream_migrate_send_partition(77, 2);   /* dest_node_id = 2, i.e. "node B" */
    CHECK(sent == 1, "stream_migrate_send_partition() reports exactly 1 stream sent");
    CHECK(stream_store[0].active == 0, "node A's local slot was retired immediately after sending (fire-and-forget)");

    /* ─── This assertion has been wrong twice; here is the history ────────
     * It first read `captured_frame_count == 3` -- one BEGIN plus one frame
     * per page. That passed for several phases and was false: struct
     * DSPPMigratePagePacket carried a whole 4 KiB page and was 4273 bytes,
     * nearly three times what an Ethernet frame holds. It passed only
     * because this file's e1000_transmit() stub is more permissive than the
     * hardware it stands in for -- it accepted 4287-byte frames without
     * complaint, so the one thing that mattered went unchecked.
     *
     * A 4 KiB page now goes out as DSPP_MIGRATE_FRAGS_PER_PAGE separate
     * frames of DSPP_MIGRATE_FRAG_BYTES each, all of which fit. So the
     * count is BEGIN + (pages x fragments), and -- the part worth asserting
     * -- every frame really is small enough to arrive. */
    const int expect_frames = 1 + 2 * DSPP_MIGRATE_FRAGS_PER_PAGE;
    CHECK(captured_frame_count == expect_frames,
          "one BEGIN_REQ plus one frame per fragment of each of the two pages");
    CHECK(dspp_tx_oversize_dropped == oversize_before,
          "*** nothing was refused for being over the link MTU -- every migrate frame now fits ***");
    {
        int all_fit = 1;
        for (int i = 0; i < captured_frame_count; i++)
            if (captured_frame_len[i] > DSPP_LINK_MTU) all_fit = 0;
        CHECK(all_fit,
              "*** every captured frame is within the 1500-byte MTU -- checked on the "
              "bytes handed to the driver, not on a struct size ***");
    }

    /* Decode the captured BEGIN_REQ frame directly to confirm real wire
     * content, not just a transmit count. */
    {
        struct DSPPMigrateHeader* h = (struct DSPPMigrateHeader*)(captured_frame[0] + ETH_HDR_LEN);
        CHECK(h->magic == DSPP_MIGRATE_MAGIC, "captured frame 0: real DSPP_MIGRATE_MAGIC on the wire");
        CHECK(h->opcode == DSPP_MIGRATE_BEGIN_REQ, "captured frame 0: real DSPP_MIGRATE_BEGIN_REQ opcode");
        CHECK(h->node_dest_id == 2, "captured frame 0: addressed to node 2, the real destination passed in");
        CHECK(h->partition_id == 77, "captured frame 0: carries the real partition_id");
        CHECK(strcmp(h->stream_name, "report.pdf") == 0, "captured frame 0: carries the real stream name, byte-for-byte");
        CHECK(h->stream_frames_used == 2, "captured frame 0: carries the real frame count");
        CHECK(h->stream_owner_uid == 500, "captured frame 0: carries the real owner_uid");
    }
    /* Decode the real fragment frames. Frame 1 is page 0's first slice, so
     * it carries node A's 0xAA bytes; the last frame is page 1's final
     * slice, carrying 0xBB. Reading the actual captured bytes rather than
     * trusting the count is what makes this a wire test. */
    {
        struct DSPPMigratePagePacket* p =
            (struct DSPPMigratePagePacket*)(captured_frame[1] + ETH_HDR_LEN);
        CHECK(p->header.opcode == DSPP_MIGRATE_PAGE_REQ, "captured frame 1: real DSPP_MIGRATE_PAGE_REQ opcode");
        CHECK(p->header.page_index == 0, "captured frame 1: page_index 0");
        CHECK(p->header.frag_index == 0, "captured frame 1: the first fragment of it");
        CHECK(p->page_data[0] == 0xAA && p->page_data[DSPP_MIGRATE_FRAG_BYTES - 1] == 0xAA,
              "captured frame 1: carries page 0's real byte content on the wire");
    }
    {
        struct DSPPMigratePagePacket* p =
            (struct DSPPMigratePagePacket*)(captured_frame[expect_frames - 1] + ETH_HDR_LEN);
        CHECK(p->header.page_index == 1, "the last captured frame belongs to page 1");
        CHECK(p->header.frag_index == DSPP_MIGRATE_FRAGS_PER_PAGE - 1,
              "...and is its final fragment");
        CHECK(p->page_data[0] == 0xBB, "...carrying page 1's real byte content");
    }

    /* ── Scenario 2: switch to "node B" (id 2) -- fresh, empty bookkeeping,
     * same underlying fake disk (see header comment on why that's correct
     * here). Feed every captured frame through the REAL receive path. ───── */
    g_fake_local_node_id = 2;
    memset(stream_store, 0, sizeof(stream_store));
    int acks_before = captured_frame_count;

    deliver_captured_frame(0);   /* BEGIN_REQ -> stream_migrate_recv_begin() */
    CHECK(stream_store[0].active == 1, "node B allocated a real local slot upon receiving BEGIN_REQ");
    CHECK(strcmp(stream_store[0].name, "report.pdf") == 0, "node B's new slot has the real stream name from the wire");
    CHECK(stream_store[0].partition_id == 77, "node B's new slot has the real partition_id from the wire");
    CHECK(stream_store[0].owner_uid == 500, "node B's new slot has the real owner_uid from the wire");

    /* Every real fragment frame, replayed through the real dispatcher. */
    for (int i = 1; i < expect_frames; i++) deliver_captured_frame(i);

    CHECK(captured_frame_count == acks_before + expect_frames,
          "node B ACKed every frame it received (1 BEGIN_ACK + one per fragment) -- the "
          "receive path is genuinely bidirectional, even though node A's send didn't wait");

    /* ── The real proof: node B's own disk, read back through its own new
     * slot's own LBA, holds the EXACT bytes node A originally had. ──────── */
    uint8_t verify0[4096], verify1[4096];
    CHECK(nvme_read_sync(stream_store[0].lba_base + 0, verify0) == 0, "node B: page 0 readable from its own new slot's LBA");
    CHECK(nvme_read_sync(stream_store[0].lba_base + 8, verify1) == 0, "node B: page 1 readable from its own new slot's LBA");
    CHECK(memcmp(verify0, page0, 4096) == 0, "node B's page 0 is byte-for-byte identical to node A's original page 0");
    CHECK(memcmp(verify1, page1, 4096) == 0, "node B's page 1 is byte-for-byte identical to node A's original page 1");

    /* Decode the captured ACK frames to confirm they report success. */
    {
        struct DSPPMigrateHeader* h = (struct DSPPMigrateHeader*)(captured_frame[acks_before] + ETH_HDR_LEN);
        CHECK(h->opcode == DSPP_MIGRATE_BEGIN_ACK, "node B's first ACK is really a DSPP_MIGRATE_BEGIN_ACK");
        CHECK(h->status == 0, "node B's BEGIN_ACK reports success (status 0)");
        CHECK(h->node_dest_id == 1, "node B's ACK is addressed back to node 1 (node_source_id from the request, swapped to dest)");
    }
    {
        /* The LAST ACK, whichever index that now is. Hardcoding `+ 2` here
         * meant "the second page's ACK" when a page was one frame; a page is
         * now DSPP_MIGRATE_FRAGS_PER_PAGE frames, so index 2 is page 0's
         * second fragment and the assertion about page_index 1 failed. Derived
         * from the count so it survives the next change to the fragment size. */
        struct DSPPMigrateHeader* h =
            (struct DSPPMigrateHeader*)(captured_frame[captured_frame_count - 1] + ETH_HDR_LEN);
        CHECK(h->opcode == DSPP_MIGRATE_PAGE_ACK, "node B's final ACK is a DSPP_MIGRATE_PAGE_ACK");
        CHECK(h->status == 0, "...reporting success");
        CHECK(h->page_index == 1, "...for page 1, the last page sent");
    }

    /* ── Scenario 3: self-filtering -- a migrate frame NOT addressed to
     * this node's current identity is silently ignored, not misapplied. ─── */
    {
        int before_active = stream_store[1].active;
        int frame_count_before = captured_frame_count;
        struct DSPPMigrateHeader bogus;
        memset(&bogus, 0, sizeof(bogus));
        bogus.magic        = DSPP_MIGRATE_MAGIC;
        bogus.opcode       = DSPP_MIGRATE_BEGIN_REQ;
        bogus.node_dest_id = 99;   /* not node 2 (current identity) */
        bogus.partition_id = 88;
        strcpy(bogus.stream_name, "not-for-us.bin");
        bogus.stream_frames_used = 1;
        dspp_rx_dispatch(&bogus, (uint16_t)sizeof(bogus));
        CHECK(stream_store[1].active == before_active, "a migrate frame addressed to a different node (99, not this node's 2) allocates nothing");
        CHECK(captured_frame_count == frame_count_before, "and no ACK was transmitted for a frame this node correctly ignored");
    }

    /* ── Scenario 4: unrecognized magic is silently dropped, not misrouted
     * into the migrate family (proves dspp_rx_dispatch()'s magic check
     * really gates before opcode dispatch, not just filters afterward). ─── */
    {
        int frame_count_before = captured_frame_count;
        uint8_t garbage[64];
        memset(garbage, 0x41, sizeof(garbage));
        dspp_rx_dispatch(garbage, sizeof(garbage));
        CHECK(captured_frame_count == frame_count_before, "an unrecognized magic value produces no transmitted ACK and no crash");
    }

    /* ═══ Service-registry replication over the wire ═══════════════════
     * The THIRD opcode family on this magic. Checks that it round-trips
     * through the REAL dspp_service_announce() -> dspp_rx_dispatch()
     * path, and -- just as important -- that adding it did not break the
     * routing of the families that were already here. */
    printf("\n-- Service-registry replication --\n");
    {
        captured_frame_count = 0;
        g_fake_local_node_id = 4;
        dspp_service_announce("cart", 12, 1 /*TCP*/, 8080, 3, 0 /*SVC_SERVING_UP*/);
        CHECK(captured_frame_count == 1, "an announcement is one broadcast frame");

        struct DSPPServiceHeader* h =
            (struct DSPPServiceHeader*)(captured_frame[0] + ETH_HDR_LEN);
        CHECK(h->magic == DSPP_MIGRATE_MAGIC, "it carries the shared family magic");
        CHECK(h->opcode == DSPP_SVC_ANNOUNCE, "...with the ANNOUNCE opcode");
        CHECK(h->node_source_id == 4, "...stamped with this node's id");
        CHECK(h->node_dest_id == 0, "...and broadcast (dest 0), not point-to-point");

        g_fake_local_node_id = 9;
        learn_calls = forget_calls = 0;
        dspp_rx_dispatch(captured_frame[0] + ETH_HDR_LEN,
                         (uint16_t)(captured_frame_len[0] - ETH_HDR_LEN));
        CHECK(learn_calls == 1, "another node learns it");
        CHECK(last_node == 4 && last_part == 12 && last_port == 8080,
              "...with the announcing node, partition and endpoint intact");
        CHECK(last_serving == 0,
              "...and the owning node's own endpoint-liveness verdict rides along");

        g_fake_local_node_id = 4;
        learn_calls = 0;
        dspp_rx_dispatch(captured_frame[0] + ETH_HDR_LEN,
                         (uint16_t)(captured_frame_len[0] - ETH_HDR_LEN));
        CHECK(learn_calls == 0,
              "the ANNOUNCING node ignores its own broadcast -- it would shadow the local entry");

        captured_frame_count = 0;
        dspp_service_withdraw("cart");
        g_fake_local_node_id = 9;
        forget_calls = 0;
        dspp_rx_dispatch(captured_frame[0] + ETH_HDR_LEN,
                         (uint16_t)(captured_frame_len[0] - ETH_HDR_LEN));
        CHECK(forget_calls == 1, "a withdraw reaches the other node");

        captured_frame_count = 0;
        g_fake_local_node_id = 0;
        dspp_service_announce("ghost", 1, 1, 80, 0, 0);
        CHECK(captured_frame_count == 0,
              "a node with no cluster identity announces nothing -- node 0 is the sentinel");

        /* And the pre-existing family still routes correctly. */
        g_fake_local_node_id = 1;
        captured_frame_count = 0;
        dspp_migrate_send_begin(1, 2, 3, "s", "text/plain", 4096, 1, 0);
        g_fake_local_node_id = 2;
        learn_calls = forget_calls = 0;
        dspp_rx_dispatch(captured_frame[0] + ETH_HDR_LEN,
                         (uint16_t)(captured_frame_len[0] - ETH_HDR_LEN));
        CHECK(learn_calls == 0 && forget_calls == 0,
              "a STREAM migrate packet never reaches the replication handler");
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * FRAGMENT ACCOUNTING
     *
     * A 4 KiB page arrives as DSPP_MIGRATE_FRAGS_PER_PAGE frames, so the
     * receiver counts fragments and writes the page only when it has all of
     * them. Two claims in that sentence are load-bearing and neither was
     * tested until a mutation sweep said so: a duplicate fragment must not
     * count twice, and a completed page must not be completable twice.
     *
     * Both mutations survived the whole suite. Both cause the same class of
     * damage -- a transfer reporting itself complete while a page is still
     * missing, which is a silently truncated file rather than a visible
     * failure.
     *
     * Frames are hand-built here rather than captured: the point is
     * receive-side accounting under sequences a correct sender never
     * produces, and the wire format is already proven by Scenario 1.
     * ═══════════════════════════════════════════════════════════════════ */
    printf("\n-- fragment accounting --\n");
    {
        g_fake_local_node_id = 2;
        memset(stream_store, 0, sizeof(stream_store));

        const uint64_t tid = 0xF00D;
        CHECK(stream_migrate_recv_begin(tid, 88, "frag.bin", "application/octet-stream",
                                        4096, 1, 500) == 0,
              "setup: a one-page transfer is announced");
        uint64_t dst_lba = stream_store[0].lba_base;

        /* Pre-fill the destination with a sentinel. "Still holds the
         * sentinel" is the only sound way to say "not written here": this
         * file's fake NVMe deliberately persists across the whole test (see
         * the header comment), so earlier scenarios may well have left real
         * bytes at this LBA and "reads as zeros" proves nothing. The first
         * draft of this scenario got that wrong and failed against correct
         * code. */
        uint8_t sentinel[4096];
        memset(sentinel, 0x5A, sizeof(sentinel));
        CHECK(nvme_write_sync(dst_lba, sentinel) == 0, "setup: destination pre-filled with a sentinel");

        /* Build fragment f of page 0, filled with a recognisable byte. */
        struct DSPPMigratePagePacket fr;
        #define MAKE_FRAG(f, fill) do {                                  \
            memset(&fr, 0, sizeof(fr));                                  \
            fr.header.magic          = DSPP_MIGRATE_MAGIC;               \
            fr.header.opcode         = DSPP_MIGRATE_PAGE_REQ;            \
            fr.header.node_source_id = 1;                                \
            fr.header.node_dest_id   = 2;                                \
            fr.header.transfer_id    = tid;                              \
            fr.header.partition_id   = 88;                               \
            fr.header.page_index     = 0;                                \
            fr.header.frag_index     = (f);                              \
            memset(fr.page_data, (fill), DSPP_MIGRATE_FRAG_BYTES);       \
        } while (0)

        /* ── Claim 1: a duplicate does not stand in for a missing fragment.
         * Deliver all but the LAST fragment, then re-deliver the first
         * several times. If duplicates were counted, the total would reach
         * the threshold and a page with a hole in it would go to disk. */
        for (uint32_t f = 0; f + 1 < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
            MAKE_FRAG(f, 0x11 + f);
            dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));
        }
        for (int rep = 0; rep < 5; rep++) {
            MAKE_FRAG(0, 0x11);
            dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));
        }
        uint8_t chk[4096];
        CHECK(nvme_read_sync(dst_lba, chk) == 0, "destination LBA is readable");
        int untouched = 1;
        for (int b = 0; b < 4096; b++) if (chk[b] != 0x5A) { untouched = 0; break; }
        CHECK(untouched,
              "*** duplicates do NOT complete a page -- nothing was written with a "
              "fragment still missing ***");

        /* The genuinely missing fragment completes it. */
        MAKE_FRAG(DSPP_MIGRATE_FRAGS_PER_PAGE - 1, 0xEE);
        dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));
        CHECK(nvme_read_sync(dst_lba, chk) == 0, "destination re-read after completion");
        CHECK(chk[0] == 0x11, "...page 0's first fragment landed at offset 0");
        CHECK(chk[4095] == 0xEE, "...and the final fragment at the end of the page");

        /* ── Claim 2: a completed page cannot complete again.
         * Re-deliver the final fragment. If the staging state were not
         * retired on completion, this would re-run the write-and-count path
         * and bump received_pages a second time -- so a two-page transfer
         * would report complete having received one page and one duplicate.
         *
         * Observable via the SECOND page: announce a two-page transfer, feed
         * page 0 fully plus duplicates of its last fragment, and check page 1
         * is still absent rather than the transfer being considered done. */
        memset(stream_store, 0, sizeof(stream_store));
        const uint64_t tid2 = 0xBEEF;
        CHECK(stream_migrate_recv_begin(tid2, 88, "two.bin", "application/octet-stream",
                                        8192, 2, 500) == 0,
              "setup: a two-page transfer is announced");
        uint64_t lba2 = stream_store[0].lba_base;
        CHECK(nvme_write_sync(lba2 + 8, sentinel) == 0, "setup: page 1's LBA pre-filled with the sentinel");

        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
            memset(&fr, 0, sizeof(fr));
            fr.header.magic = DSPP_MIGRATE_MAGIC;
            fr.header.opcode = DSPP_MIGRATE_PAGE_REQ;
            fr.header.node_source_id = 1; fr.header.node_dest_id = 2;
            fr.header.transfer_id = tid2; fr.header.partition_id = 88;
            fr.header.page_index = 0; fr.header.frag_index = f;
            memset(fr.page_data, 0x77, DSPP_MIGRATE_FRAG_BYTES);
            dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));
        }
        /* Now the duplicates of page 0's last fragment. */
        for (int rep = 0; rep < 4; rep++) dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));

        uint8_t p1[4096];
        CHECK(nvme_read_sync(lba2 + 8, p1) == 0, "page 1's LBA is readable");
        int p1_absent = 1;
        for (int b = 0; b < 4096; b++) if (p1[b] != 0x5A) { p1_absent = 0; break; }
        CHECK(p1_absent, "page 1 has not been written yet (only page 0 was sent)");

        /* The assertion that actually distinguishes the bug.
         *
         * "Page 1 is absent" is true whether or not the duplicates were
         * miscounted -- nothing had sent page 1 either way, so it proved
         * nothing and the mutation survived it. What the bug really does is
         * make the TRANSFER declare itself finished: received_pages reaches
         * frames_used on page 0 plus a duplicate, the inflight row is
         * retired, and page 1 -- when it legitimately arrives -- is dropped
         * as belonging to an unknown transfer.
         *
         * So the property to assert is that the transfer still works. */
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
            memset(&fr, 0, sizeof(fr));
            fr.header.magic = DSPP_MIGRATE_MAGIC;
            fr.header.opcode = DSPP_MIGRATE_PAGE_REQ;
            fr.header.node_source_id = 1; fr.header.node_dest_id = 2;
            fr.header.transfer_id = tid2; fr.header.partition_id = 88;
            fr.header.page_index = 1; fr.header.frag_index = f;
            memset(fr.page_data, 0x99, DSPP_MIGRATE_FRAG_BYTES);
            dspp_rx_dispatch(&fr, (uint16_t)sizeof(fr));
        }
        CHECK(nvme_read_sync(lba2 + 8, p1) == 0, "page 1's LBA re-read");
        int p1_landed = 1;
        for (int b = 0; b < 4096; b++) if (p1[b] != 0x99) { p1_landed = 0; break; }
        CHECK(p1_landed,
              "*** page 1 still lands after duplicates of page 0 -- the transfer was "
              "NOT retired early by a page being counted twice ***");
        #undef MAKE_FRAG
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
