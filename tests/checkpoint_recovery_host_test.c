/*
 * checkpoint_recovery_host_test.c — Core Backup Strategies Step 5:
 * proves that a surviving node can detect peer failure, load a received
 * checkpoint, and adopt the dead node's partitions.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/checkpoint_recovery_host_test \
 *       tests/checkpoint_recovery_host_test.c \
 *       kernel/failover.c kernel/state_tree.c net/dspp_checkpoint.c
 *   /tmp/checkpoint_recovery_host_test
 */
#include "kernel/failover.h"
#include "kernel/state_tree.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
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

static uint32_t g_local_node = 2;
uint32_t cluster_local_node_id(void) { return g_local_node; }

static uint64_t g_tsc = 1000;
uint64_t read_tsc(void) { return g_tsc++; }

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* partition_get_owner_node / partition_set_owner_node stubs */
uint32_t partition_get_owner_node(uint32_t partition_id) {
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id)
            return partition_owner_table[i].node_id;
    }
    return 0;
}

int partition_set_owner_node(uint32_t partition_id, uint32_t node_id) {
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].node_id = node_id;
            return 0;
        }
    }
    /* Create new row */
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_owner_table[i].active) {
            partition_owner_table[i].partition_id = partition_id;
            partition_owner_table[i].node_id = node_id;
            partition_owner_table[i].active = 1;
            return 0;
        }
    }
    return 1;
}

void persist_partitions(void) { /* no-op in test */ }

/* DSPP transmit stub */
void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    (void)role; (void)buf; (void)size;
}

void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len) {
    if (dspp_len > dspp_max_wire_payload) {
        dspp_tx_oversize_dropped++;
        return;
    }
    /* In this test we don't capture frames — we feed directly via dspp_ckpt_rx */
}

/* ─── Test harness ─────────────────────────────────────────────────── */
static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Helper: simulate a checkpoint arriving from node 1 ───────────── */
static void simulate_checkpoint_from_node1(void) {
    /* Build a state tree as if we are node 1 */
    uint32_t saved_local = g_local_node;
    g_local_node = 1;

    memset(partition_table, 0, sizeof(partition_table));
    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    object_catalog[0].object_id = 111; object_catalog[0].active = 1;
    object_catalog[0].partition_id = 5;
    object_catalog[1].object_id = 222; object_catalog[1].active = 1;
    object_catalog[1].partition_id = 7;
    object_catalog_count = 2;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    uint8_t serial_buf[ST_SERIAL_MAX];
    uint32_t serial_size = state_tree_serialize(nodes, count, 42, serial_buf, sizeof(serial_buf));

    /* Feed chunks directly into the receiver as if they arrived over DSPP */
    g_local_node = 2;
    dspp_ckpt_recv_reset();

    /* Simulate BEGIN */
    uint32_t total_chunks = (serial_size + DSPP_CKPT_CHUNK_BYTES - 1) / DSPP_CKPT_CHUNK_BYTES;
    struct DSPPCkptHeader begin;
    memset(&begin, 0, sizeof(begin));
    begin.magic = DSPP_MIGRATE_MAGIC;
    begin.opcode = DSPP_CKPT_BEGIN_REQ;
    begin.node_source_id = 1;
    begin.node_dest_id = 2;
    begin.transfer_id = 0xCAFE;
    begin.sequence = 42;
    begin.total_bytes = serial_size;
    begin.total_chunks = total_chunks;
    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)&begin, sizeof(begin));

    /* Simulate chunks */
    for (uint32_t i = 0; i < total_chunks; i++) {
        struct DSPPCkptChunkPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.magic = DSPP_MIGRATE_MAGIC;
        pkt.header.opcode = DSPP_CKPT_CHUNK_REQ;
        pkt.header.node_source_id = 1;
        pkt.header.node_dest_id = 2;
        pkt.header.transfer_id = 0xCAFE;
        pkt.header.chunk_index = i;
        pkt.header.sequence = 42;

        uint32_t offset = i * DSPP_CKPT_CHUNK_BYTES;
        uint32_t remaining = serial_size - offset;
        uint32_t chunk_sz = remaining < DSPP_CKPT_CHUNK_BYTES ? remaining : DSPP_CKPT_CHUNK_BYTES;
        pkt.header.chunk_bytes = chunk_sz;
        memcpy(pkt.chunk_data, serial_buf + offset, chunk_sz);

        dspp_ckpt_rx(&pkt, (uint16_t)(sizeof(struct DSPPCkptHeader) + chunk_sz));
    }

    g_local_node = saved_local;
}

/* ─── Scenario 1: heartbeat tracking and failure detection ─────────── */
static void test_liveness(void) {
    printf("--- Scenario 1: heartbeat tracking and failure detection ---\n");
    failover_init();

    /* Node 1 sends heartbeats at tick 0, 50, 100 */
    failover_note_heartbeat(1, 0);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 alive after heartbeat");

    failover_note_heartbeat(1, 50);
    failover_tick(100);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 still alive at tick 100");

    /* Silence: no heartbeat for 300+ ticks */
    failover_tick(351);
    CHECK(failover_peer_status(1) == PEER_DEAD, "node 1 declared dead after 300 ticks of silence");

    /* Suspected state at half the dead threshold */
    failover_init();
    failover_note_heartbeat(3, 0);
    failover_tick(160);  /* 160 > 150 (half of 300) */
    CHECK(failover_peer_status(3) == PEER_SUSPECTED, "node 3 suspected after 160 ticks");
}

/* ─── Scenario 2: node comes back from the dead ────────────────────── */
static void test_resurrection(void) {
    printf("--- Scenario 2: node resurrection ---\n");
    failover_init();
    failover_note_heartbeat(1, 0);
    failover_tick(400);  /* declare dead */
    CHECK(failover_peer_status(1) == PEER_DEAD, "node 1 dead");

    failover_note_heartbeat(1, 500);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 alive again after heartbeat");
}

/* ─── Scenario 3: failover with no checkpoint ──────────────────────── */
static void test_no_checkpoint(void) {
    printf("--- Scenario 3: failover with no checkpoint available ---\n");
    dspp_ckpt_recv_reset();
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "returns NO_CHECKPOINT when nothing received");
}

/* ─── Scenario 4: successful failover — adopt partitions ───────────── */
static void test_successful_failover(void) {
    printf("--- Scenario 4: successful failover ---\n");

    /* Setup: partitions 5 and 7 are owned by node 1 */
    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));

    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    partition_owner_table[0].partition_id = 0; partition_owner_table[0].node_id = 2;
    partition_owner_table[0].active = 1;
    partition_owner_table[1].partition_id = 5; partition_owner_table[1].node_id = 1;
    partition_owner_table[1].active = 1;
    partition_owner_table[2].partition_id = 7; partition_owner_table[2].node_id = 1;
    partition_owner_table[2].active = 1;

    /* Simulate checkpoint arriving from node 1 */
    simulate_checkpoint_from_node1();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    /* Perform failover */
    g_local_node = 2;
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "failover_recover_from() succeeds");
    CHECK(failover_last_adopted_count() == 2, "adopted 2 partitions");

    /* Verify ownership transferred */
    CHECK(partition_get_owner_node(5) == 2, "partition 5 now owned by node 2");
    CHECK(partition_get_owner_node(7) == 2, "partition 7 now owned by node 2");
    CHECK(partition_get_owner_node(0) == 2, "partition 0 was already node 2 (unchanged)");
}

/* ─── Scenario 5: wrong source node in checkpoint ──────────────────── */
static void test_wrong_source(void) {
    printf("--- Scenario 5: checkpoint from wrong source rejected ---\n");

    /* Simulate checkpoint from node 1, then try to recover "node 3" */
    simulate_checkpoint_from_node1();
    int rc = failover_recover_from(3);
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "rejects checkpoint not from dead node");
}

/* ─── Scenario 6: unknown peer returns -1 ──────────────────────────── */
static void test_unknown_peer(void) {
    printf("--- Scenario 6: unknown peer status ---\n");
    failover_init();
    CHECK(failover_peer_status(99) == -1, "unknown peer returns -1");
}

int main(void) {
    test_liveness();
    test_resurrection();
    test_no_checkpoint();
    test_successful_failover();
    test_wrong_source();
    test_unknown_peer();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
