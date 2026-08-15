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

/* Synthesized name for an adopted partition ("recovered-N"). No libc, so
 * the decimal suffix is hand-rolled; the checkpoint carries no names. */
static void fo_recovered_name(char* dst, uint32_t part_id) {
    static const char prefix[] = "recovered-";
    char tmp[12];
    int n = 0;
    uint32_t v = part_id;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (prefix[i] && i < PARTITION_NAME_LEN - 1) { dst[i] = prefix[i]; i++; }
    while (n > 0 && i < PARTITION_NAME_LEN - 1) dst[i++] = tmp[--n];
    dst[i] = '\0';
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
                /* Only the leader recovers: a partition has exactly one
                 * owner, and this node is the one the cluster elected to
                 * lead after the death (election timeout 150 < the 300
                 * tick death threshold, so the new leader is already in
                 * place when the death is declared). A follower declaring
                 * the same peer dead observes only. The checkpoint the
                 * leader holds is the dead node's own last broadcast
                 * (failover_live_checkpoint_broadcast(), same sweep). */
                if (cluster_is_leader()) {
                    int rc = failover_recover_from(peers[i].node_id);
                    kernel_serial_printf(
                        "[FAILOVER] recovery for dead node %u: rc=%d (%s)\n",
                        peers[i].node_id, rc,
                        rc == FAILOVER_OK ? "OK - adopted" :
                        rc == FAILOVER_NO_CHECKPOINT ? "no checkpoint held" :
                        rc == FAILOVER_NO_PARTITIONS ? "no adoptable partitions" :
                        "deserialize error");
                }
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

    /* Adopt partitions: for each partition node in the tree, transfer
     * ownership to this node.
     *
     * The provenance replaces an ownership lookup: the checkpoint came
     * FROM dead_node_id (checked above), and the state tree is built from
     * the producer's OWN local partition table (state_tree_build()
     * iterates partition_table[]), so every partition node in it was, by
     * construction, owned by the dead node. The earlier code re-checked
     * this against the SURVIVOR's local table -- which never contains
     * another node's partitions, because partition tables are local-only
     * in this kernel (partition_create() stamps owner = local node). A
     * live cluster therefore never adopted anything; only the host test,
     * which pre-populates the survivor's table with the dead node's
     * partitions, saw the check pass. */
    uint32_t adopted = 0;
    uint32_t my_id = cluster_local_node_id();

    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type != ST_NODE_PARTITION) continue;
        uint32_t part_id = (uint32_t)nodes[i].entity_id;

        /* PARTITION_SYSTEM (id 0) never migrates: every node owns its own
         * system partition (partition_init() stamps it local). Adopting it
         * would steal another node's system partition. */
        if (part_id == PARTITION_SYSTEM || part_id >= PARTITION_MAX) continue;

        /* The survivor's table may have no row for this partition at all
         * (the live case -- the dead node created it). Create the row so
         * the ownership handoff has something to write to. The id is
         * preserved because catalog objects reference partition_id; the
         * name is synthesized because the checkpoint carries no names. */
        if (!partition_exists(part_id)) {
            partition_table[part_id].partition_id = part_id;
            fo_recovered_name(partition_table[part_id].name, part_id);
            partition_table[part_id].active = 1;
        }

        partition_set_owner_node(part_id, my_id);
        adopted++;
        kernel_serial_printf(
            "[FAILOVER] Adopted partition %u from dead node %u\n",
            part_id, dead_node_id);
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

/* ─── Live checkpoint broadcast (leader -> followers) ────────────────── */
static uint64_t failover_last_ckpt_tick = 0;

void failover_live_checkpoint_broadcast(uint64_t now) {
    /* Only the leader produces checkpoints: it is the only node whose
     * partition table is authoritative for what it owns, and the
     * followers' single-slot RX buffers hold the transfer until
     * failover_recover_from() consumes it after the leader dies.
     * Rate-limited like the consensus heartbeat (the sweep is
     * load-dependent and unbounded). */
    if (!cluster_is_leader()) return;
    if (now - failover_last_ckpt_tick < FAILOVER_CKPT_PERIOD_TICKS) return;
    failover_last_ckpt_tick = now;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);
    if (count == 0) return;

    uint8_t buf[ST_SERIAL_MAX];
    uint32_t size = state_tree_serialize(nodes, count, now, buf, sizeof(buf));
    if (size == 0) return;

    for (uint32_t i = 0; i < cluster_roster_count; i++) {
        if (!cluster_roster[i].active) continue;
        uint32_t peer = cluster_roster[i].node_id;
        if (peer == 0 || peer == cluster_local_node_id()) continue;
        dspp_ckpt_send(now, peer, now, buf, size);
    }
}
