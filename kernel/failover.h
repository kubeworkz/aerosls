#ifndef FAILOVER_H
#define FAILOVER_H

#include <stdint.h>

/*
 * failover.h — node failure detection and checkpoint-based recovery
 * (Core Backup Strategies, Step 5).
 *
 * Tracks peer node liveness via heartbeat observation. When a peer is
 * declared dead, triggers failover: loads the most recent checkpoint
 * received from that peer (via DSPP, Step 3), deserializes the state tree,
 * and adopts ownership of the dead node's partitions.
 *
 * Builds on:
 * - net/consensus.h's heartbeat timing constants
 * - net/dspp_checkpoint.c's received checkpoint buffer
 * - kernel/state_tree.h's deserialize()
 * - kernel/partition.h's partition_set_owner_node()
 */

// ─── Peer Liveness ────────────────────────────────────────────────────────────
#define FAILOVER_MAX_PEERS       8
#define FAILOVER_DEAD_TICKS    300u  /* 3 seconds at ~100 Hz — 2x election timeout */
#define FAILOVER_CKPT_PERIOD_TICKS 100u  /* leader checkpoint broadcast period */

/* Peer liveness state */
#define PEER_ALIVE     0
#define PEER_SUSPECTED 1
#define PEER_DEAD      2

struct PeerLiveness {
    uint32_t node_id;
    uint8_t  status;         /* PEER_ALIVE / SUSPECTED / DEAD */
    uint8_t  active;
    uint64_t last_seen_tick; /* kernel_tick_counter when last heartbeat received */
    uint64_t declared_dead_tick;
};

// ─── Failover Result ──────────────────────────────────────────────────────────
#define FAILOVER_OK              0
#define FAILOVER_NO_CHECKPOINT   1  /* no checkpoint data from that peer */
#define FAILOVER_DESERIALIZE_ERR 2
#define FAILOVER_NO_PARTITIONS   3  /* checkpoint had no partitions to adopt */

// ─── Public API ───────────────────────────────────────────────────────────────

/* Initialize failover subsystem. */
void failover_init(void);

/* Record that we heard from node_id at time `now`. */
void failover_note_heartbeat(uint32_t node_id, uint64_t now);

/* Tick: check all peers for liveness. Call from the BSP sweep. */
void failover_tick(uint64_t now);

/* Query a peer's status. Returns PEER_ALIVE/SUSPECTED/DEAD, or -1 if unknown. */
int failover_peer_status(uint32_t node_id);

/* Attempt to recover dead_node_id's partitions from a received checkpoint.
 * Returns FAILOVER_* status. */
int failover_recover_from(uint32_t dead_node_id);

/* Leader's periodic state-tree checkpoint broadcast (live cluster wiring).
 * Called from the BSP sweep; it TRANSMITS, so like the consensus heartbeat
 * it must stay off the AP tick. The leader serializes its own partition
 * table (state_tree_build) and pushes the transfer to every roster peer
 * via dspp_ckpt_send(), rate-limited to one per FAILOVER_CKPT_PERIOD_TICKS.
 * Followers hold the latest transfer in the single-slot RX buffer, which
 * failover_recover_from() consumes when the leader is declared dead. */
void failover_live_checkpoint_broadcast(uint64_t now);

/* How many partitions were adopted in the last failover_recover_from() call. */
uint32_t failover_last_adopted_count(void);

#endif /* FAILOVER_H */
