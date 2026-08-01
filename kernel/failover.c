/*
 * failover.c — node failure detection and checkpoint-based recovery
 * (Core Backup Strategies, Step 5).
 *
 * Freestanding: local helpers, no libc.
 */
#include "failover.h"
#include "state_tree.h"
#include "partition.h"
#include "kernel_io.h"
#include "../net/dspp.h"
#include "../net/consensus.h"

/* ─── Local helpers ──────────────────────────────────────────────────── */
static void fo_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

/* ─── Peer liveness table ────────────────────────────────────────────── */
static struct PeerLiveness peers[FAILOVER_MAX_PEERS];
static uint32_t last_adopted = 0;

void failover_init(void) {
    fo_memset(peers, 0, sizeof(peers));
    last_adopted = 0;
    kernel_serial_print("[FAILOVER] Subsystem initialised.\n");
}

static struct PeerLiveness* find_peer(uint32_t node_id) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++)
        if (peers[i].active && peers[i].node_id == node_id)
            return &peers[i];
    return 0;
}

static struct PeerLiveness* alloc_peer(uint32_t node_id) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++) {
        if (!peers[i].active) {
            peers[i].node_id = node_id;
            peers[i].active  = 1;
            peers[i].status  = PEER_ALIVE;
            return &peers[i];
        }
    }
    return 0;
}

void failover_note_heartbeat(uint32_t node_id, uint64_t now) {
    if (node_id == 0 || node_id == cluster_local_node_id()) return;

    struct PeerLiveness* p = find_peer(node_id);
    if (!p) p = alloc_peer(node_id);
    if (!p) return;  /* table full */

    p->last_seen_tick = now;
    if (p->status == PEER_DEAD) {
        kernel_serial_printf("[FAILOVER] Node %u came back (was dead)\n", node_id);
    }
    p->status = PEER_ALIVE;
}

void failover_tick(uint64_t now) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        if (peers[i].status == PEER_DEAD) continue;

        uint64_t silence = now - peers[i].last_seen_tick;
        if (silence >= FAILOVER_DEAD_TICKS) {
            if (peers[i].status != PEER_DEAD) {
                peers[i].status = PEER_DEAD;
                peers[i].declared_dead_tick = now;
                kernel_serial_printf(
                    "[FAILOVER] Node %u declared DEAD (silent %llu ticks)\n",
                    peers[i].node_id, silence);
            }
        } else if (silence >= FAILOVER_DEAD_TICKS / 2) {
            peers[i].status = PEER_SUSPECTED;
        }
    }
}

int failover_peer_status(uint32_t node_id) {
    struct PeerLiveness* p = find_peer(node_id);
    if (!p) return -1;
    return p->status;
}

/* ─── Recovery: adopt partitions from a received checkpoint ──────────── */
int failover_recover_from(uint32_t dead_node_id) {
    last_adopted = 0;

    if (!dspp_ckpt_recv_ready()) return FAILOVER_NO_CHECKPOINT;

    /* Deserialize the received state tree */
    uint32_t recv_size = dspp_ckpt_recv_size();
    uint8_t recv_buf[ST_SERIAL_MAX];
    if (recv_size > sizeof(recv_buf)) return FAILOVER_DESERIALIZE_ERR;

    dspp_ckpt_recv_copy(recv_buf, sizeof(recv_buf));

    struct StateTreeHeader hdr;
    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_deserialize(recv_buf, recv_size, &hdr, nodes, ST_MAX_NODES);
    if (count == 0) return FAILOVER_DESERIALIZE_ERR;

    /* Verify the checkpoint came from the dead node */
    if (hdr.node_id != dead_node_id) {
        kernel_serial_printf(
            "[FAILOVER] Checkpoint is from node %u, not dead node %u\n",
            hdr.node_id, dead_node_id);
        return FAILOVER_NO_CHECKPOINT;
    }

    /* Adopt partitions: for each partition node in the tree that was owned
     * by the dead node, transfer ownership to this node */
    uint32_t adopted = 0;
    uint32_t my_id = cluster_local_node_id();

    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type != ST_NODE_PARTITION) continue;
        uint32_t part_id = (uint32_t)nodes[i].entity_id;

        /* Check if this partition was owned by the dead node */
        if (partition_get_owner_node(part_id) == dead_node_id) {
            partition_set_owner_node(part_id, my_id);
            adopted++;
            kernel_serial_printf(
                "[FAILOVER] Adopted partition %u from dead node %u\n",
                part_id, dead_node_id);
        }
    }

    if (adopted == 0) return FAILOVER_NO_PARTITIONS;

    last_adopted = adopted;
    dspp_ckpt_recv_reset();

    kernel_serial_printf(
        "[FAILOVER] Recovery complete: adopted %u partition(s) from node %u "
        "(checkpoint seq=%llu)\n", adopted, dead_node_id, hdr.sequence);
    return FAILOVER_OK;
}

uint32_t failover_last_adopted_count(void) { return last_adopted; }
