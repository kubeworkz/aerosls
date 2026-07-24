#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>

/* Forward declaration only (not a full `#include "dspp.h"`): dspp.h has no
 * include guard of its own, so pulling it in here would double-define
 * every struct in it for any .c file that -- like consensus.c itself --
 * already includes both headers separately. A plain incomplete-type
 * forward declaration composes safely regardless of include order, and
 * MUST come before the first function prototype anywhere in this header
 * that takes a `struct DSPPFullPagePacket*` parameter (Phase 4's new
 * process_partition_consensus_packet() below, as well as Phase 1's
 * original process_consensus_packet() further down) -- declaring a struct
 * tag for the first time inside a function prototype's own parameter list,
 * rather than at file scope like this, creates a SECOND, incompatible tag
 * scoped only to that one parameter list, which then conflicts with the
 * real one dspp.h defines once both headers are included together. This is
 * a real bug Phase 1 hit and fixed once already (see its own findings
 * addendum) -- moved to the very top of the file here so no future
 * addition below can silently reintroduce it by being declared before
 * this forward declaration used to appear. */
struct DSPPFullPagePacket;

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
    uint32_t partition_id; /* Multi-Node Partition Scaling Roadmap Phase 4:
                             * which partition this message's lease election
                             * is about. Only meaningful for the three
                             * DSPP_CMD_PARTITION_* opcodes below -- Phase 1's
                             * original cluster-wide REQUEST_VOTE/VOTE_REPLY/
                             * HEARTBEAT traffic never populates or reads this
                             * field. This struct is always carried inside a
                             * struct DSPPFullPagePacket's 4KB payload_4kb
                             * buffer (never marshaled byte-for-byte across a
                             * real wire boundary anywhere in this codebase
                             * today -- cluster_init() is still never called
                             * at boot, per Phase 1's own findings), so
                             * growing it here is free: no existing deployed
                             * node could be running an older layout to
                             * misparse against. */
} __attribute__((packed));

/* Multi-Node Partition Scaling Roadmap Phase 4: partition-scoped write
 * leases, layered ALONGSIDE Phase 1's cluster-wide election mechanism
 * above, not replacing it. Cluster membership -- "which nodes exist," the
 * roster, the majority quorum threshold -- stays exactly what Phase 1
 * built: that's a real, node-level, cluster-wide question, independent of
 * any one partition. What this phase adds on top is a separate question
 * asked once per partition P: "which node currently holds the write lease
 * for P" -- agreed via the same request-vote/heartbeat message SHAPE
 * (struct ConsensusMessage, now carrying partition_id above) but under
 * distinct opcodes and its own per-partition state, since collapsing the
 * two into Phase 1's single term/role would mean one partition's lease
 * churn forces an unrelated cluster-membership re-election -- not what "P's
 * write lease moved to a different node" should imply. See the roadmap
 * doc's own §7 for the full design writeup. */
#define DSPP_CMD_PARTITION_REQUEST_VOTE 0x13
#define DSPP_CMD_PARTITION_VOTE_REPLY   0x14
#define DSPP_CMD_PARTITION_HEARTBEAT    0x15

/* PARTITION_LEASE_MAX deliberately mirrors kernel/partition.h's
 * PARTITION_MAX (256, raised for the Multitenant Isolation Gap Analysis §5
 * item 9 capacity-sizing pass) as an independent constant rather than
 * #include-ing that header here: net/ already has kernel/ headers included
 * INTO it one layer up (kernel/partition.c includes ../net/consensus.h,
 * established in Phase 2), and this project keeps that dependency strictly
 * one-directional -- net/ headers do not reach back into kernel/ headers
 * for shared constants, the same discipline CLUSTER_NODE_MAX above already
 * follows as its own independent constant. Kept numerically in lockstep
 * with PARTITION_MAX deliberately: this table is a linear-scan, fail-closed
 * pool (not indexed by partition_id), so leaving it at the old value would
 * have silently capped write-lease-capable partitions at 16 regardless of
 * how high PARTITION_MAX climbed -- a real functional regression against
 * the whole point of the resize, not a compile-time concern. */
#define PARTITION_LEASE_MAX 256

struct PartitionLease {
    uint32_t      partition_id;
    uint32_t      term;
    uint32_t      voted_for;              /* node_id this node voted for in
                                            * partition_id's CURRENT term */
    enum NodeRole role;                   /* this node's role for THIS
                                            * partition's lease -- unrelated
                                            * to local_cluster_state.role,
                                            * which is this node's role in
                                            * the cluster-wide membership
                                            * election Phase 1 built */
    uint32_t      heartbeat_ticks_elapsed;
    uint32_t      accumulated_votes;      /* Phase 4: per-partition vote
                                            * count while CANDIDATE. Can't be
                                            * a single `static` local the way
                                            * Phase 1's process_consensus_
                                            * packet() uses for its ONE
                                            * cluster-wide election -- with
                                            * PARTITION_LEASE_MAX partitions
                                            * potentially campaigning
                                            * concurrently at different
                                            * terms, one shared counter would
                                            * conflate votes meant for
                                            * entirely different partitions'
                                            * elections. */
    uint8_t       active;
};

/* No validation against kernel/partition.c's partition_table[] -- this
 * table accepts any partition_id value, the same "doesn't require the
 * partition to actually exist yet" posture frame_pool.h's partition_set_
 * frame_quota() already documents for the identical reason: a lease can
 * usefully be pre-established before a partition is created, and requiring
 * validation would mean net/consensus.c reaching back into kernel/
 * partition.c -- the circular, wrong-direction dependency the comment above
 * PARTITION_LEASE_MAX already explains this project avoids. */
extern struct PartitionLease partition_lease_table[PARTITION_LEASE_MAX];

/* Creates or resets partition_id's lease row: term=0, role=FOLLOWER,
 * voted_for=0, heartbeat_ticks_elapsed=0, accumulated_votes=1 (self),
 * active=1. Find-existing-row-or-create-new-row, the same table shape
 * kernel/partition.c's partition_set_owner_node() already established for
 * partition_owner_table[] in Phase 2. Returns 0 on success, 1 if the table
 * is full and partition_id has no existing row to reset. */
int partition_lease_init(uint32_t partition_id);

/* This node's role for partition_id's lease. Returns ROLE_FOLLOWER if no
 * lease row exists yet for partition_id -- the honest "never contested"
 * default. Deliberately NOT "assume LEADER/writable by default" the way
 * partition_is_local()'s 0==0 comparison one layer down in kernel/
 * partition.c defaults to locally-owned: a partition that has never had a
 * real election must never be treated as though this node already holds
 * its write lease, or every node in an actual multi-node deployment would
 * independently and simultaneously believe itself the writer for every
 * unleased partition -- see partition_holds_write_lease() below, which is
 * the function anything gating a real write should actually call. */
enum NodeRole partition_lease_get_role(uint32_t partition_id);

/* Returns 0 if no lease row exists yet for partition_id. */
uint32_t partition_lease_get_term(uint32_t partition_id);

/* 1 if this node's role for partition_id is ROLE_LEADER (this node may
 * currently accept writes for partition_id), 0 otherwise -- FOLLOWER,
 * CANDIDATE, or no lease row at all. This is the function Phase 6
 * (migration) and any future write-path gating actually need; partition_
 * lease_get_role() above is the lower-level accessor it's built on. */
int partition_holds_write_lease(uint32_t partition_id);

/* Per-partition analogue of check_consensus_heartbeat_tick(): if this node
 * holds the LEADER role for partition_id, broadcasts a
 * DSPP_CMD_PARTITION_HEARTBEAT carrying partition_id; otherwise tracks
 * silence and calls partition_lease_trigger_election() after the same
 * 150-tick threshold Phase 1's cluster-wide mechanism uses. No-ops if no
 * lease row exists yet for partition_id (nothing to tick). */
void partition_lease_heartbeat_tick(uint32_t partition_id);

/* Ticks every currently-active row in partition_lease_table[] via
 * partition_lease_heartbeat_tick() above. Mirrors check_consensus_
 * heartbeat_tick()'s own "called every 10ms by the kernel timer interrupt
 * handler" comment as the single entry point a future boot-time timer
 * wiring phase would call -- same "not actually wired into the real timer
 * yet" honesty caveat Phase 1's own heartbeat function carries (see its
 * findings addendum). */
void check_partition_lease_heartbeat_tick(void);

/* Per-partition analogue of trigger_kernel_election_campaign(): moves
 * partition_id's lease to CANDIDATE, increments its term, votes for self,
 * strips write access to JUST partition_id's objects via update_page_
 * table_permissions_for_partition(partition_id, 1) -- not every SLS object
 * on the node, the all-or-nothing behavior this phase's whole point is to
 * narrow -- and broadcasts a DSPP_CMD_PARTITION_REQUEST_VOTE carrying
 * partition_id. Creates a fresh lease row first if partition_id has none
 * yet (mirrors partition_lease_init()'s find-or-create posture). */
void partition_lease_trigger_election(uint32_t partition_id);

/* Multi-Node Partition Scaling Roadmap Phase 6 (cold migration): the
 * voluntary opposite of partition_lease_trigger_election() -- relinquishes
 * THIS node's lease claim for partition_id rather than campaigning for one.
 * Sets role=FOLLOWER, voted_for=0, accumulated_votes=1, heartbeat_ticks_
 * elapsed=0 on the existing row; does NOT bump term (stepping down isn't
 * itself a new term -- the destination node's own future election, if any,
 * advances the term when it actually campaigns, the same way Raft never
 * needs an outgoing leader to manufacture a term bump for itself). Returns
 * 0 if an active lease row existed and was stepped down, 1 if partition_id
 * had no lease row at all -- "nothing to step down from" is a normal,
 * non-error outcome for a partition that was never contested on this node,
 * not a failure; callers that care (Phase 6's partition_migrate()) can use
 * the return value purely for logging, not as a gate.
 *
 * Deliberately local-only, transmits nothing: unlike partition_lease_
 * trigger_election()'s REQUEST_VOTE broadcast, there is no DSPP_CMD_
 * PARTITION_* opcode for "I am voluntarily stepping down" and no RX
 * dispatcher anywhere in this codebase that would receive one if there
 * were (Phase 5's own finding, unchanged) -- so this only ever updates
 * local state. A real multi-node deployment's destination node would
 * simply call partition_lease_trigger_election() itself once it observes
 * (via Phase 2's now-updated partition_owner_table[], not a pushed
 * message) that it owns partition_id and no one is heartbeating it. */
int partition_lease_step_down(uint32_t partition_id);

/* Per-partition analogue of process_consensus_packet(): reads partition_id
 * out of the ConsensusMessage payload and routes DSPP_CMD_PARTITION_
 * HEARTBEAT/REQUEST_VOTE/VOTE_REPLY to that specific partition's lease row,
 * not the single global local_cluster_state Phase 1's mechanism uses.
 * Creates a fresh lease row first if partition_id has none yet, the same
 * as partition_lease_trigger_election() above (an incoming REQUEST_VOTE
 * for a partition this node has never leased before must still be able to
 * vote on it). Dispatches purely on packet->header.opcode -- callers are
 * responsible for routing DSPP_CMD_PARTITION_* opcodes here and Phase 1's
 * original three opcodes to process_consensus_packet() instead, the same
 * way any future RX dispatcher would need to distinguish them. */
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet);

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

/* struct DSPPFullPagePacket's forward declaration lives at the very top of
 * this file now -- see the comment there. */
void process_consensus_packet(struct DSPPFullPagePacket* packet);

#endif
