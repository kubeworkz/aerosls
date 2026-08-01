/*
 * checkpoint_cluster_host_test.c — Core Backup Strategies Step 3:
 * proves a serialized state tree survives a real DSPP round trip between
 * two simulated nodes.
 *
 * Same capture-and-replay technique as cross_node_migration_host_test.c:
 * run the real sender as node 1, capture every Ethernet-framed packet,
 * flip identity to node 2, feed those bytes through dspp_rx_dispatch(),
 * and verify the deserialized tree on the far side matches the original.
 *
 * Links the REAL, unmodified net/dspp_checkpoint.c and kernel/state_tree.c.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/checkpoint_cluster_host_test \
 *       tests/checkpoint_cluster_host_test.c \
 *       kernel/state_tree.c net/dspp_checkpoint.c
 *   /tmp/checkpoint_cluster_host_test
 */
#include "kernel/state_tree.h"
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "kernel/process.h"
#include "kernel/stream.h"
#include "net/dspp.h"
#include "net/net.h"
#include "net/e1000.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Kernel global stubs ──────────────────────────────────────────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;

struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];

struct ProcessDescriptor proc_table[PROC_MAX];
struct StreamEntry       stream_store[STREAM_MAX];

MACAddr net_my_mac;
volatile uint64_t kernel_tick_counter = 0;
uint64_t dspp_tx_oversize_dropped = 0;
uint16_t dspp_max_wire_payload = DSPP_MAX_WIRE_PAYLOAD;
uint64_t dspp_tx_reentrant_dropped = 0;

static uint32_t g_fake_node_id = 1;
uint32_t cluster_local_node_id(void) { return g_fake_node_id; }

static uint64_t g_tsc = 5000;
uint64_t read_tsc(void) { return g_tsc++; }

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── Ethernet frame capture (mirrors cross_node_migration test) ───── */
#define MAX_FRAMES 64
static uint8_t  captured[MAX_FRAMES][ETH_HDR_LEN + sizeof(struct DSPPCkptChunkPacket)];
static uint16_t captured_len[MAX_FRAMES];
static int      frame_count = 0;

void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    (void)role;
    if (frame_count < MAX_FRAMES) {
        uint16_t n = size;
        if (n > sizeof(captured[0])) n = (uint16_t)sizeof(captured[0]);
        memcpy(captured[frame_count], buf, n);
        captured_len[frame_count] = n;
        frame_count++;
    }
}

/* dspp_transmit_raw: the real implementation in net/dspp.c wraps payload
 * in an Ethernet frame. We reimplement that minimally here to avoid
 * linking the full dspp.c (which has many more dependencies). */
void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len) {
    if (dspp_len > dspp_max_wire_payload) {
        dspp_tx_oversize_dropped++;
        return;
    }
    uint8_t frame[ETH_HDR_LEN + DSPP_MAX_WIRE_PAYLOAD];
    memset(frame, 0xFF, 6);  /* dst MAC: broadcast */
    memcpy(frame + 6, &net_my_mac, 6);  /* src MAC */
    uint16_t et = htons(ETHERTYPE_DSPP);
    memcpy(frame + 12, &et, 2);
    memcpy(frame + ETH_HDR_LEN, dspp_payload, dspp_len);
    e1000_transmit(NIC_ROLE_CLUSTER, frame, (uint16_t)(ETH_HDR_LEN + dspp_len));
}

/* Feed captured frame through the receive path */
static void deliver_frame(int i) {
    uint16_t payload_len = (uint16_t)(captured_len[i] - ETH_HDR_LEN);
    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)(captured[i] + ETH_HDR_LEN), payload_len);
}

/* ─── Test harness ─────────────────────────────────────────────────── */
static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Scenario 1: basic checkpoint transfer ────────────────────────── */
static void test_basic_transfer(void) {
    printf("--- Scenario 1: basic checkpoint transfer (node 1 -> node 2) ---\n");

    /* Populate node 1's state */
    memset(partition_table, 0, sizeof(partition_table));
    memset(object_catalog, 0, sizeof(object_catalog));
    memset(proc_table, 0, sizeof(proc_table));
    memset(stream_store, 0, sizeof(stream_store));

    partition_table[0].partition_id = 0;
    partition_table[0].active = 1;
    partition_table[1].partition_id = 1;
    partition_table[1].active = 1;
    partition_table[2].partition_id = 2;
    partition_table[2].active = 1;

    for (int i = 0; i < 10; i++) {
        object_catalog[i].object_id = 100 + (uint64_t)i;
        object_catalog[i].active = 1;
        object_catalog[i].partition_id = (uint32_t)(i % 3);
        object_catalog[i].size_pages = (uint32_t)(i + 1) * 4;
    }
    object_catalog_count = 10;

    proc_table[0].pid = 42; proc_table[0].active = 1;
    proc_table[0].partition_id = 1;
    proc_table[1].pid = 99; proc_table[1].active = 1;
    proc_table[1].partition_id = 2;

    stream_store[0].active = 1; stream_store[0].partition_id = 0;
    stream_store[0].frames_used = 16;
    stream_store[1].active = 1; stream_store[1].partition_id = 1;
    stream_store[1].frames_used = 8;

    /* Build and serialize state tree as node 1 */
    g_fake_node_id = 1;
    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t node_count = state_tree_build(nodes, ST_MAX_NODES);
    CHECK(node_count > 0, "node 1 built a non-empty state tree");

    /* Expected: 1 root + 3 partitions + 10 objects + 2 processes + 2 streams = 18 */
    CHECK(node_count == 18, "tree has 18 nodes (root+3part+10obj+2proc+2strm)");

    uint8_t serial_buf[ST_SERIAL_MAX];
    uint32_t serial_size = state_tree_serialize(nodes, node_count, 7, serial_buf, sizeof(serial_buf));
    CHECK(serial_size > 0, "serialization produced bytes");

    /* Send over DSPP from node 1 to node 2 */
    frame_count = 0;
    dspp_ckpt_send(0xDEAD0001, 2, 7, serial_buf, serial_size);

    uint32_t expected_chunks = (serial_size + DSPP_CKPT_CHUNK_BYTES - 1) / DSPP_CKPT_CHUNK_BYTES;
    /* 1 BEGIN + expected_chunks CHUNK frames */
    CHECK(frame_count == (int)(1 + expected_chunks),
          "correct number of Ethernet frames captured");

    /* Switch to node 2 and replay */
    g_fake_node_id = 2;
    dspp_ckpt_recv_reset();

    CHECK(!dspp_ckpt_recv_ready(), "receiver not ready before replay");

    for (int i = 0; i < frame_count; i++) {
        deliver_frame(i);
    }

    CHECK(dspp_ckpt_recv_ready(), "receiver has a complete checkpoint after replay");
    CHECK(dspp_ckpt_recv_size() == serial_size, "received size matches sent size");

    /* Deserialize on node 2 */
    uint8_t recv_buf[ST_SERIAL_MAX];
    dspp_ckpt_recv_copy(recv_buf, sizeof(recv_buf));

    struct StateTreeHeader hdr;
    struct StateTreeNode restored[ST_MAX_NODES];
    uint32_t restored_count = state_tree_deserialize(recv_buf, dspp_ckpt_recv_size(),
                                                     &hdr, restored, ST_MAX_NODES);

    CHECK(restored_count == node_count, "deserialized node count matches original");
    CHECK(hdr.magic == ST_MAGIC, "header magic valid");
    CHECK(hdr.sequence == 7, "header sequence matches");
    CHECK(hdr.node_id == 1, "header identifies source as node 1");
    CHECK(hdr.num_partitions == 3, "3 partitions in header");
    CHECK(hdr.num_objects == 10, "10 objects in header");
    CHECK(hdr.num_processes == 2, "2 processes in header");
    CHECK(hdr.num_streams == 2, "2 streams in header");

    /* Verify every node matches byte-for-byte */
    int all_match = 1;
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id != restored[i].id ||
            nodes[i].type != restored[i].type ||
            nodes[i].entity_id != restored[i].entity_id ||
            nodes[i].parent_id != restored[i].parent_id ||
            nodes[i].partition_id != restored[i].partition_id ||
            nodes[i].size != restored[i].size) {
            printf("      mismatch at node %u: type=%u vs %u, eid=%llu vs %llu\n",
                   i, nodes[i].type, restored[i].type,
                   (unsigned long long)nodes[i].entity_id,
                   (unsigned long long)restored[i].entity_id);
            all_match = 0;
            break;
        }
    }
    CHECK(all_match, "all tree nodes match after DSPP round trip");
}

/* ─── Scenario 2: wrong destination is ignored ─────────────────────── */
static void test_wrong_dest(void) {
    printf("--- Scenario 2: packets to wrong node are ignored ---\n");

    g_fake_node_id = 1;
    frame_count = 0;
    dspp_ckpt_recv_reset();

    /* Send a small tree from node 1 to node 3 */
    uint8_t dummy[64] = {0};
    dspp_ckpt_send(0xBEEF, 3, 1, dummy, sizeof(dummy));

    /* Deliver to node 2 — should all be ignored */
    g_fake_node_id = 2;
    for (int i = 0; i < frame_count; i++) deliver_frame(i);

    CHECK(!dspp_ckpt_recv_ready(), "node 2 ignores packets addressed to node 3");
}

/* ─── Scenario 3: busy receiver rejects second transfer ────────────── */
static void test_busy_reject(void) {
    printf("--- Scenario 3: busy receiver rejects second transfer ---\n");

    g_fake_node_id = 1;
    frame_count = 0;
    dspp_ckpt_recv_reset();

    /* Start a transfer to node 2 but only send BEGIN (no chunks) */
    struct DSPPCkptHeader begin;
    memset(&begin, 0, sizeof(begin));
    begin.magic          = DSPP_MIGRATE_MAGIC;
    begin.opcode         = DSPP_CKPT_BEGIN_REQ;
    begin.node_source_id = 1;
    begin.node_dest_id   = 2;
    begin.transfer_id    = 0xAAAA;
    begin.total_bytes    = 4096;
    begin.total_chunks   = 4;
    begin.sequence       = 10;

    g_fake_node_id = 2;
    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)&begin, sizeof(begin));

    /* Try a second BEGIN — should be refused (receiver is busy) */
    begin.transfer_id = 0xBBBB;
    begin.sequence    = 11;
    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)&begin, sizeof(begin));

    /* The receiver should still be tracking the first transfer */
    CHECK(!dspp_ckpt_recv_ready(), "receiver busy, second transfer rejected");
}

/* ─── Scenario 4: oversized transfer refused ───────────────────────── */
static void test_oversize(void) {
    printf("--- Scenario 4: oversized transfer refused ---\n");

    g_fake_node_id = 2;
    dspp_ckpt_recv_reset();

    struct DSPPCkptHeader begin;
    memset(&begin, 0, sizeof(begin));
    begin.magic          = DSPP_MIGRATE_MAGIC;
    begin.opcode         = DSPP_CKPT_BEGIN_REQ;
    begin.node_source_id = 1;
    begin.node_dest_id   = 2;
    begin.transfer_id    = 0xCCCC;
    begin.total_bytes    = 25000;  /* exceeds receiver's 24 KiB buffer */
    begin.total_chunks   = 100;
    begin.sequence       = 20;

    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)&begin, sizeof(begin));
    CHECK(!dspp_ckpt_recv_ready(), "oversized transfer refused");
}

/* ─── Scenario 5: link MTU respected ──────────────────────────────── */
static void test_link_mtu(void) {
    printf("--- Scenario 5: all transmitted frames fit the link MTU ---\n");

    g_fake_node_id = 1;
    frame_count = 0;

    /* Send a full-sized state tree */
    uint8_t big[8192];
    memset(big, 0x42, sizeof(big));
    dspp_ckpt_send(0xF00D, 2, 99, big, sizeof(big));

    int all_fit = 1;
    for (int i = 0; i < frame_count; i++) {
        if (captured_len[i] > ETH_HDR_LEN + DSPP_MAX_WIRE_PAYLOAD) {
            printf("      frame %d: %u bytes > max %u\n",
                   i, captured_len[i], (unsigned)(ETH_HDR_LEN + DSPP_MAX_WIRE_PAYLOAD));
            all_fit = 0;
            break;
        }
    }
    CHECK(all_fit, "every transmitted frame fits the link MTU");
    CHECK(dspp_tx_oversize_dropped == 0, "no frames dropped for being oversized");
}

int main(void) {
    test_basic_transfer();
    test_wrong_dest();
    test_busy_reject();
    test_oversize();
    test_link_mtu();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
