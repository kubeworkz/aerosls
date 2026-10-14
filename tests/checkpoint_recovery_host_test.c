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
#include "kernel/env_ckpt.h"   /* P3 (b): env_ckpt_table[] feeds the tree's ST_NODE_ENVIRONMENT nodes */
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/process.h"
#include "kernel/stream.h"
#include "net/dspp.h"
#include "net/net.h"
#include "net/e1000.h"
#include "net/consensus.h"   /* struct ClusterPeer, CLUSTER_NODE_MAX */
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

/* POSIX-Environments P3 (b): the environment records state_tree_build()
 * turns into ST_NODE_ENVIRONMENT nodes. Defined here rather than linked
 * from kernel/env_ckpt.c, exactly like partition_table[]/proc_table[]/
 * stream_store[] above — this test drives the CONSUMER (failover) and only
 * needs the table to be populatable. Real record-layer coverage is
 * tests/env_ckpt_host_test.c's. */
struct EnvCkptRecord env_ckpt_table[ENV_CKPT_MAX];

MACAddr net_my_mac;
volatile uint64_t kernel_tick_counter = 0;
uint64_t dspp_tx_oversize_dropped = 0;
uint16_t dspp_max_wire_payload = DSPP_MAX_WIRE_PAYLOAD;
uint64_t dspp_tx_reentrant_dropped = 0;

static uint32_t g_local_node = 2;
uint32_t cluster_local_node_id(void) { return g_local_node; }

/* The test node is a follower -- recovery is exercised by calling
 * failover_recover_from() directly, not via failover_tick()'s leader gate. */
int cluster_is_leader(void) { return 0; }

/* The failover deferral gate (failover.c -> env_migrate_in_flight): a
 * controllable stand-in so the migration deferral itself is exercised below,
 * not just bypassed. The real one lives in kernel/env_migrate.c. */
int g_env_migrate_in_flight = 0;
int env_migrate_in_flight(void) { return g_env_migrate_in_flight; }

/* The peer half of the same gate (the idle-observer arm): failover.c asks
 * env_migrate_peer_in_flight() about the SILENT PEER before declaring it
 * dead, because the knowledge lives on that peer (it announced intent before
 * blocking its sweep), not here. The stub records which node was asked so the
 * pin can prove the gate consults the right identity. */
static uint32_t g_peer_in_flight_asked = 0;
static int g_env_migrate_peer_in_flight = 0;
int env_migrate_peer_in_flight(uint32_t node_id) {
    g_peer_in_flight_asked = node_id;
    return g_env_migrate_peer_in_flight;
}

/* Roster stubs -- failover.c's leader broadcast iterates these; an empty
 * roster makes it a no-op in this test (no live cluster). */
struct ClusterPeer cluster_roster[CLUSTER_NODE_MAX];
uint32_t cluster_roster_count = 0;

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

/* partition_exists stub -- mirrors partition.c's partition_id_valid(). */
int partition_exists(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) return 1;
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_table[partition_id].active;
}

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

    /* Migration deferral (POSIX-Environments P3 live arm): while this node's
     * own environment migration is in flight, a silent peer is NOT declared
     * dead — the silence a synchronous payload send or a sweep-blocking
     * restore produces on the COUNTERPARTY is not a death, and declaring it
     * one flipped ownership back onto the sender on the live 2-node TCG run
     * (both nodes adopted; the confirmation teardown then correctly
     * refused). The declaration must fire on the first tick after the
     * migration resolves — that pairing is what makes the deferral a delay,
     * not a blindfold. */
    failover_init();
    g_env_migrate_in_flight = 1;
    failover_note_heartbeat(4, 0);
    failover_tick(351);  /* silence 301 >= DEAD, but a migration is in flight */
    CHECK(failover_peer_status(4) != PEER_DEAD,
          "*** node 4 NOT declared dead at 301 ticks of silence while a migration is in flight ***");
    g_env_migrate_in_flight = 0;
    failover_tick(352);
    CHECK(failover_peer_status(4) == PEER_DEAD,
          "...and declared dead on the first tick after the migration resolves");

    /* The idle-observer arm (docs/AeroSLS-Idle-Observer-Adoption-Finding-
     * v0.1.md): a peer that ANNOUNCED migration intent is not declared dead
     * while its budget stands — even when THIS node holds no migration state
     * of its own. That is the live 3-node failure: the busy SOURCE stopped
     * beating (its blocking work runs on the sweep that transmits the
     * leader's heartbeat), an idle observer won the election the blocked
     * node could not contest and adopted every partition in the source's
     * checkpoint — every node alive, the migrating environment destroyed.
     * The gate must ask about the SILENT PEER by identity, and the deferral
     * must end when the budget does. */
    failover_init();
    failover_note_heartbeat(7, 0);
    g_env_migrate_peer_in_flight = 1;
    g_peer_in_flight_asked = 0;
    failover_tick(351);  /* silence 301 >= DEAD, but node 7 announced intent */
    CHECK(failover_peer_status(7) != PEER_DEAD,
          "*** node 7 NOT declared dead while its announced migration intent stands ***");
    CHECK(g_peer_in_flight_asked == 7,
          "*** ...and the gate asked about node 7 -- the SILENT PEER, by identity ***");
    g_env_migrate_peer_in_flight = 0;
    failover_tick(352);
    CHECK(failover_peer_status(7) == PEER_DEAD,
          "...and declared dead on the first tick after the intent budget expires");

    /* The stamp-ahead case (observed live 2026-10-13, a DEFERRED line that
     * printed "silent 18446744073709551606 ticks" = -10 unsigned): the two
     * operands of the silence subtract come from different contexts, so the
     * stamp may legitimately land AHEAD of the sample. `now` is read once at
     * failover_tick()'s BSP-sweep call site (net/http.c), while
     * last_seen_tick is stamped by the heartbeat RX path
     * (process_consensus_packet -> failover_note_heartbeat, timer ISR / AP
     * core). A heartbeat landing between those two reads leaves
     * last_seen_tick > now, and an unsigned subtraction wraps to ~2^64 —
     * past FAILOVER_DEAD_TICKS. The arithmetic must saturate: a peer heard
     * at or after `now` has silence zero, not ~2^64. The pair below pins
     * both halves — not declared on the wrapped tick, and still declared
     * once real silence crosses the threshold from the same stamp — because
     * a saturation that simply never declared would be a blindfold, not a
     * fix. */
    failover_init();
    failover_note_heartbeat(9, 400);
    failover_tick(390);   /* now is 10 ticks BEHIND the stamp: the wrap case */
    CHECK(failover_peer_status(9) != PEER_DEAD,
          "*** a heartbeat stamped ahead of the tick sample is NOT ~2^64 ticks of silence ***");
    CHECK(failover_peer_status(9) != PEER_SUSPECTED,
          "...and is not even suspected: from this sample the peer was just heard");
    failover_tick(699);   /* 699 - 400 = 299, still under the threshold */
    CHECK(failover_peer_status(9) != PEER_DEAD,
          "...and 299 ticks of REAL silence from that stamp still defer the declaration");
    failover_tick(701);   /* 701 - 400 = 301 >= FAILOVER_DEAD_TICKS */
    CHECK(failover_peer_status(9) == PEER_DEAD,
          "...but 301 ticks of real silence from the SAME stamp declares it dead");
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

/* ─── Scenario 4b: P3 (b) — the adoption NAMES what it did not restore ─ */
/* POSIX-Environments v0.2 Phase P3, failover destination (b) (§11 Q5).
 * Failover adopts partitions by ownership flip and restores no environment
 * — the payload is 5.5 MiB of regions and this checkpoint is a 24 KiB
 * broadcast. What the tree now carries is the dead node's environment
 * NAMES (ST_NODE_ENVIRONMENT), and this scenario pins that they are
 * REPORTED: an operator who sees "Adopted partition 5" must be able to
 * learn that partition 5's environment index 2 did not come with it.
 *
 * Three properties, because each fails differently:
 *   - every environment of an ADOPTED partition is named (count is right);
 *   - an environment in a partition NOT adopted is NOT named (reporting
 *     state that is not this node's to restore would point an operator at
 *     the wrong node's work);
 *   - a recovery that adopts nothing reports ZERO, not the previous
 *     recovery's count (a stale count is a lie of the same family). */
static void test_unrestored_environments(void) {
    printf("--- Scenario 4b: the adoption names the environments it did NOT restore ---\n");

    /* Two live environment records, both in partitions node 1 owned in the
     * fixture: partition 5 index 2, partition 7 index 1. A third sits in
     * partition 9, which the checkpoint does NOT carry — the witness that
     * the report is scoped to adopted partitions. */
    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_table[0].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[0].partition_id = 5;  env_ckpt_table[0].index = 2;
    env_ckpt_table[0].n_regions = 3;
    env_ckpt_table[1].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[1].partition_id = 7;  env_ckpt_table[1].index = 1;
    env_ckpt_table[1].n_regions = 3;
    env_ckpt_table[2].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[2].partition_id = 9;  env_ckpt_table[2].index = 4;
    env_ckpt_table[2].n_regions = 3;

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

    simulate_checkpoint_from_node1();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    g_local_node = 2;
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_adopted_count() == 2, "adopted 2 partitions");
    CHECK(failover_last_unrestored_envs() == 2,
          "*** exactly the 2 environments of the ADOPTED partitions were named as NOT restored ***");
    CHECK(failover_last_unrestored_envs() != 3,
          "...and the environment in partition 9 (not adopted) was NOT named — the report is scoped to what this node now owns");

    /* A second recovery that adopts nothing must report zero, not the
     * count left behind by the one above. */
    dspp_ckpt_recv_reset();
    rc = failover_recover_from(3);   /* no checkpoint from node 3 */
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "the second recovery adopts nothing");
    CHECK(failover_last_unrestored_envs() == 0,
          "*** ...and reports ZERO unrestored environments, not the previous recovery's 2 ***");

    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
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
    test_unrestored_environments();
    test_wrong_source();
    test_unknown_peer();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
