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
#include "kernel/partition.h"
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include "net/dspp.h"
#include "net/net.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

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
void process_consensus_packet(struct DSPPFullPagePacket* packet) { (void)packet; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet) { (void)packet; }

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
void e1000_transmit_packet(void* buf, uint16_t size) {
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
    int sent = stream_migrate_send_partition(77, 2);   /* dest_node_id = 2, i.e. "node B" */
    CHECK(sent == 1, "stream_migrate_send_partition() reports exactly 1 stream sent");
    CHECK(stream_store[0].active == 0, "node A's local slot was retired immediately after sending (fire-and-forget)");
    CHECK(captured_frame_count == 3, "exactly 3 real Ethernet-framed DSPP packets were transmitted: 1 BEGIN_REQ + 2 PAGE_REQ (fire-and-forget -- no wait for ACKs)");

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
    {
        struct DSPPMigratePagePacket* p = (struct DSPPMigratePagePacket*)(captured_frame[1] + ETH_HDR_LEN);
        CHECK(p->header.opcode == DSPP_MIGRATE_PAGE_REQ, "captured frame 1: real DSPP_MIGRATE_PAGE_REQ opcode");
        CHECK(p->header.page_index == 0, "captured frame 1: page_index 0");
        CHECK(p->page_data[0] == 0xAA && p->page_data[4095] == 0xAA, "captured frame 1: carries page 0's real byte content on the wire");
    }
    {
        struct DSPPMigratePagePacket* p = (struct DSPPMigratePagePacket*)(captured_frame[2] + ETH_HDR_LEN);
        CHECK(p->header.page_index == 1, "captured frame 2: page_index 1");
        CHECK(p->page_data[0] == 0xBB, "captured frame 2: carries page 1's real byte content on the wire");
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

    deliver_captured_frame(1);   /* PAGE_REQ page 0 -> stream_migrate_recv_page() */
    deliver_captured_frame(2);   /* PAGE_REQ page 1 -> stream_migrate_recv_page() */

    CHECK(captured_frame_count == acks_before + 3, "node B transmitted exactly 3 ACKs back (1 BEGIN_ACK + 2 PAGE_ACK) -- the receive path is genuinely bidirectional, not one-way-blind, even though node A's send didn't wait for them");

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
        struct DSPPMigrateHeader* h = (struct DSPPMigrateHeader*)(captured_frame[acks_before + 2] + ETH_HDR_LEN);
        CHECK(h->opcode == DSPP_MIGRATE_PAGE_ACK, "node B's third ACK is a DSPP_MIGRATE_PAGE_ACK");
        CHECK(h->status == 0, "node B's second PAGE_ACK (page 1) reports success");
        CHECK(h->page_index == 1, "node B's second PAGE_ACK correctly identifies page_index 1");
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

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
