#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>

enum NodeRole {
    ROLE_FOLLOWER,
    ROLE_CANDIDATE,
    ROLE_LEADER
};

struct ClusterNode {
    uint32_t node_id;                 /* Phase 1 (Multi-Node Partition Scaling
                                        * Roadmap): this node's real identity.
                                        * 0 is the reserved "uninitialized"
                                        * sentinel -- see cluster_init(). */
    uint32_t current_term;
    uint32_t voted_for;
    enum NodeRole role;
    uint32_t active_nodes_count;
    uint32_t stable_quorum_threshold;
    uint32_t heartbeat_ticks_elapsed;
};

// Extends the DSPP protocol opcodes designed previously
#define DSPP_CMD_REQUEST_VOTE 0x10
#define DSPP_CMD_VOTE_REPLY   0x11
#define DSPP_CMD_HEARTBEAT    0x12

struct ConsensusMessage {
    uint32_t term;
    uint32_t candidate_id;
    uint32_t last_log_index;
    uint32_t vote_granted; // 1 = Yes, 0 = No
} __attribute__((packed));

/* Phase 1: local_cluster_state used to be a `static struct ClusterNode`
 * defined directly in this header, with a hardcoded initializer --
 * node identity baked in as a literal `1` at every packet-construction call
 * site in consensus.c/prefetch.c, and active_nodes_count=3 /
 * stable_quorum_threshold=2 asserted out of thin air, never derived from
 * anything real. Beyond being fictional data, that was a real latent bug:
 * `static` at file scope in a header gives every .c file that includes
 * this header its OWN private copy of the struct. Harmless so far because
 * only consensus.c ever touched it, but a silent desync waiting to happen
 * the moment a second file (e.g. a future phase's DSPP routing or a
 * cluster-status HTTP/shell surface) needed to read the same state.
 * Declared `extern` here, defined once in consensus.c -- the same
 * single-definition-in-the-.c-file convention every other kernel subsystem's
 * global table already follows (see partition_table[] in partition.c). */
extern struct ClusterNode local_cluster_state;

/* Phase 1: a real node identity plus a small, static membership roster,
 * replacing the hardcoded node_source_id=1 literal previously baked into
 * every packet built in consensus.c/prefetch.c. CLUSTER_NODE_MAX is a
 * small fixed table, matching this project's PARTITION_MAX/PROC_MAX
 * convention rather than any real hardware or protocol limit. node_id 0 is
 * reserved as the "uninitialized" sentinel (the same way PARTITION_SYSTEM
 * uses 0 as a meaningful, reserved id elsewhere in this project) -- a real
 * node must be given a nonzero id via cluster_init() before it can
 * register peers or construct identity-bearing packets.
 *
 * This is static, explicitly-registered membership only: cluster_init()
 * and cluster_register_peer() must both be called by whatever boot-time
 * config source decides a node's own identity and its cluster's initial
 * membership -- deciding what that config source actually is (a command
 * line arg, an NVMe-persisted config block, a build-time constant) is
 * deliberately NOT part of this phase; see the roadmap doc's own phase
 * writeup. No dynamic discovery, no liveness/failure detection beyond the
 * existing heartbeat silence timer in consensus.c -- both explicitly out
 * of scope here. */
#define CLUSTER_NODE_MAX 8

struct ClusterPeer {
    uint32_t node_id;
    uint8_t  active;
};

extern struct ClusterPeer cluster_roster[CLUSTER_NODE_MAX];
extern uint32_t cluster_roster_count;

/* Sets this node's real identity and resets local_cluster_state to a fresh
 * FOLLOWER with an empty roster (self only: active_nodes_count=1,
 * stable_quorum_threshold=1). Returns 0 on success, 1 if local_node_id==0
 * (the reserved sentinel). Safe to call more than once -- each call fully
 * resets term/role/roster rather than merging with prior state, the same
 * re-init-is-a-fresh-start precedent partition_init() already established
 * (it unconditionally rewrites every table slot on every call). */
int cluster_init(uint32_t local_node_id);

/* Registers a peer node as part of this cluster's membership and
 * recomputes active_nodes_count/stable_quorum_threshold from the roster's
 * real size (quorum = majority = (active_nodes_count / 2) + 1). Returns:
 *   0  = newly added
 *   1  = already active (no-op, not an error)
 *   2  = re-activated a previously-registered-but-inactive slot
 *  -1  = invalid node_id (0, or equal to this node's own id -- a node is
 *        not its own peer)
 *  -2  = roster full (CLUSTER_NODE_MAX reached) */
int cluster_register_peer(uint32_t node_id);

/* Accessors -- callers outside consensus.c (a future cluster-status
 * HTTP/shell surface, or a later phase's per-partition lease logic) should
 * use these rather than reaching into local_cluster_state directly, the
 * same accessor-over-raw-struct-access convention partition.c's own
 * partition_get_for_uid() already established. */
uint32_t cluster_local_node_id(void);
uint32_t cluster_active_node_count(void);

/* Pre-existing gap this phase found and closed in passing: none of these
 * three were ever declared in this header, only defined in consensus.c --
 * harmless while nothing outside consensus.c called them (confirmed true
 * for this whole codebase before this phase), but this phase's own host
 * test is the first real external caller either function has ever had, so
 * the missing prototypes surfaced immediately as implicit-declaration
 * warnings. Declared properly now rather than left for whichever future
 * phase wires the heartbeat timer/RX dispatch into the kernel boot
 * sequence to rediscover. */
void check_consensus_heartbeat_tick(void);
void trigger_kernel_election_campaign(void);

/* Forward declaration only (not a full `#include "dspp.h"`): dspp.h has
 * no include guard of its own, so pulling it in here would double-define
 * every struct in it for any .c file that -- like consensus.c itself --
 * already includes both headers separately. A plain incomplete-type
 * forward declaration is enough for a pointer parameter and composes
 * safely regardless of include order. */
struct DSPPFullPagePacket;
void process_consensus_packet(struct DSPPFullPagePacket* packet);

#endif
