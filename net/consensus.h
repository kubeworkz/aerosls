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

/* ─── Election timing ──────────────────────────────────────────────────────
 *
 * Every threshold below is measured in kernel_tick_counter units, which the
 * LAPIC timer IRQ advances at ~100 Hz (kernel/timer.c -- see init_timer()'s
 * own "exact rate is calibration-dependent" caveat, and auth.h, which
 * already relies on the same ~100 Hz for token TTL).
 *
 * WHY THAT MATTERS, and why this block exists at all. Both heartbeat ticks
 * used to count their own CALLS: `heartbeat_ticks_elapsed++` per invocation,
 * campaign at 150. That is only a 1.5-second timeout if the caller runs at
 * exactly 100 Hz, and the caller does not -- the ticks are driven from the
 * BSP's HTTP sweep (net/http.c), which spins as fast as the request load
 * lets it. Counting calls there means the timeout is however long 150
 * sweeps happen to take: milliseconds when idle, many seconds under load,
 * and different on every node. Anchoring to kernel_tick_counter makes 150
 * mean 1.5 seconds no matter who calls, or how often -- which is what the
 * original constant was always documented to mean.
 *
 * Callers therefore pass `now` rather than the tick reading the clock
 * itself, matching service_heartbeat_tick(kernel_tick_counter) in
 * kernel/service_registry.h. It keeps net/consensus.c free of a
 * kernel_tick_counter extern and lets a host test drive time explicitly. */
#define CONSENSUS_TICK_HZ            100u

/* Base election timeout: 1.5 s of silence before a FOLLOWER campaigns. */
#define ELECTION_TIMEOUT_BASE_TICKS  150u

/* Per-node stagger, 250 ms. Raft randomises the election timeout so that
 * symmetric nodes do not all campaign on the same instant and split the
 * vote. This kernel has something better than randomness available: every
 * node already carries a unique, stable id from `node=N` on the boot
 * command line (kernel/boot_params.h). Deriving the offset from that is
 * deterministic -- so a host test can assert the exact tick a given node
 * campaigns on -- and it cannot collide, which random backoff can.
 *
 * The cost, stated plainly: node 1 always campaigns first, so leadership
 * has a fixed priority order rather than going to whichever node happens
 * to notice first. A flapping node 1 will repeatedly take leadership back
 * from a stable node 2. That is the trade real Raft's randomisation buys
 * out of, and if it ever bites, this is the constant to revisit. For a
 * cluster capped at CLUSTER_NODE_MAX=8 it is the better deal: 250 ms is
 * far longer than a vote round-trip on a local segment, so the first
 * candidate wins outright and no split vote occurs at all. */
#define ELECTION_STAGGER_TICKS        25u

/* Leader heartbeat interval, 300 ms -- five per election timeout window.
 *
 * Rate-limiting the LEADER branch is not an optimisation, it is the other
 * half of the same bug: that branch transmitted on EVERY call, which was
 * sane at a fixed 100 Hz and becomes a broadcast storm the moment the
 * caller is a busy loop. Raft's requirement is only that the heartbeat
 * interval sit comfortably below the election timeout; 5x is ample margin
 * for a segment that loses the occasional frame. */
#define LEADER_HEARTBEAT_TICKS        30u

/* consensus_election_timeout() computes this node's staggered deadline. It
 * is defined further down, immediately after CLUSTER_NODE_MAX, because it
 * takes the modulus from that constant rather than repeating the literal. */

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

    /* kernel_tick_counter at the last heartbeat this node ACCEPTED (or at
     * the last campaign it started). Replaces a `heartbeat_ticks_elapsed`
     * counter that was incremented once per call -- see the "Election
     * timing" block above for why a call count could not express a
     * 1.5-second timeout once the caller stopped being a 100 Hz timer. */
    uint64_t last_heartbeat_tick;

    /* kernel_tick_counter at the last heartbeat this node SENT as LEADER.
     * Separate from the field above because they measure opposite
     * directions: one is "when did I last hear from the leader", the other
     * "when did I last speak as one". A leader needs both -- it rate-limits
     * its own transmissions with this, and still honours an incoming
     * higher-term heartbeat via the other. */
    uint64_t last_beat_sent_tick;

    /* Votes accumulated in the CURRENT campaign, self included.
     *
     * Was a function-local `static uint32_t accumulated_votes = 1;` inside
     * process_consensus_packet(), reset only on reaching quorum -- so a
     * campaign that FAILED left its votes banked, and the next campaign
     * started pre-loaded and could reach "quorum" on fewer real votes than
     * the threshold. Unreachable while nothing drove elections; a live
     * split-brain risk the moment something did. Phase 4's per-partition
     * lease already had this right (struct PartitionLease below); this is
     * the cluster-wide half catching up. */
    uint32_t accumulated_votes;
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
    uint64_t      last_heartbeat_tick;    /* kernel_tick_counter at the last
                                            * PARTITION_HEARTBEAT accepted for
                                            * this partition, or at its last
                                            * campaign. Same call-count-to-
                                            * wall-clock correction as struct
                                            * ClusterNode above. */
    uint64_t      last_beat_sent_tick;    /* kernel_tick_counter at the last
                                            * heartbeat sent as this
                                            * partition's LEADER. */
    uint32_t      accumulated_votes;      /* Phase 4: per-partition vote
                                            * count while CANDIDATE. Can't be
                                            * a single `static` local the way
                                            * Phase 1's process_consensus_
                                            * packet() used to for its ONE
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
 * voted_for=0, both heartbeat timestamps 0, accumulated_votes=1 (self),
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
 * DSPP_CMD_PARTITION_HEARTBEAT carrying partition_id -- rate-limited to one
 * per LEADER_HEARTBEAT_TICKS; otherwise measures silence against
 * consensus_election_timeout() and calls partition_lease_trigger_election()
 * once it is exceeded. No-ops if no lease row exists yet for partition_id
 * (nothing to tick).
 *
 * `now` is a kernel_tick_counter reading, supplied by the caller. */
void partition_lease_heartbeat_tick(uint32_t partition_id, uint64_t now);

/* Ticks every currently-active row in partition_lease_table[] via
 * partition_lease_heartbeat_tick() above.
 *
 * Driven from the BSP's HTTP sweep in net/http.c, alongside
 * service_heartbeat_tick() and mesh_observe_local(). It TRANSMITS, so it
 * belongs on the BSP with the other senders and must not be called from the
 * AP core -- the NIC TX path is not safe to drive from two cores at once
 * (the same constraint kernel/workload.h documents for the reconciler). */
void check_partition_lease_heartbeat_tick(uint64_t now);

/* Per-partition analogue of trigger_kernel_election_campaign(): moves
 * partition_id's lease to CANDIDATE, increments its term, votes for self,
 * strips write access to JUST partition_id's objects via update_page_
 * table_permissions_for_partition(partition_id, 1) -- not every SLS object
 * on the node, the all-or-nothing behavior this phase's whole point is to
 * narrow -- and broadcasts a DSPP_CMD_PARTITION_REQUEST_VOTE carrying
 * partition_id. Creates a fresh lease row first if partition_id has none
 * yet (mirrors partition_lease_init()'s find-or-create posture).
 *
 * `now` stamps last_heartbeat_tick so the fresh CANDIDATE measures its next
 * timeout from the campaign it just started. Omitting that is not a cosmetic
 * slip: the row would still read as silent, campaign again on the very next
 * tick, and run the term counter away at sweep rate. */
void partition_lease_trigger_election(uint32_t partition_id, uint64_t now);

/* Multi-Node Partition Scaling Roadmap Phase 6 (cold migration): the
 * voluntary opposite of partition_lease_trigger_election() -- relinquishes
 * THIS node's lease claim for partition_id rather than campaigning for one.
 * Sets role=FOLLOWER, voted_for=0, accumulated_votes=1 on the existing row
 * and clears its heartbeat timestamps; does NOT bump term (stepping down isn't
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
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now);

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

/* This node's election timeout, in kernel_tick_counter units: the base
 * threshold plus a per-node stagger. See the "Election timing" block at the
 * top of this header for why the offset comes from the node id rather than
 * from randomness, and what that trade costs.
 *
 * The modulus is CLUSTER_NODE_MAX rather than a repeated literal 8 -- ids
 * are validated against the roster elsewhere, but an out-of-range id
 * reaching here must still produce a bounded timeout rather than a wildly
 * distant one that would look like a hung election.
 *
 * node_id 0 (cluster_init() never ran) gets exactly the base timeout. Such
 * a node has an empty roster and quorum 1, so it is not campaigning against
 * anyone regardless. */
static inline uint64_t consensus_election_timeout(uint32_t node_id) {
    return (uint64_t)ELECTION_TIMEOUT_BASE_TICKS +
           (uint64_t)(node_id % CLUSTER_NODE_MAX) * (uint64_t)ELECTION_STAGGER_TICKS;
}

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
/* Human-readable role, for the cluster-status console line and the
 * cluster-view HTTP surface. */
const char* consensus_role_name(enum NodeRole r);

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

/* The cluster-wide membership heartbeat. As LEADER, broadcasts a
 * DSPP_CMD_HEARTBEAT at most once per LEADER_HEARTBEAT_TICKS. Otherwise
 * measures silence since last_heartbeat_tick and campaigns once
 * consensus_election_timeout() is exceeded.
 *
 * `now` is a kernel_tick_counter reading. Driven from the BSP's HTTP sweep
 * (net/http.c) -- NOT, despite what this function's own comment in
 * consensus.c claimed for several phases, "every 10ms by the kernel timer
 * interrupt handler on Core 3". Nothing called it at all until that wiring
 * landed, which is why every node sat at term 0 / FOLLOWER indefinitely and
 * partition_holds_write_lease() was permanently false. */
void check_consensus_heartbeat_tick(uint64_t now);
void trigger_kernel_election_campaign(uint64_t now);

/* ─── Syscalls: operator-driven node identity configuration ────────────
 * This header's own comment above (on cluster_init()/cluster_register_
 * peer()) named the boot-time config-source question as "deliberately NOT
 * part of this phase" and listed three candidate mechanisms: a command
 * line arg, an NVMe-persisted config block, a build-time constant. None of
 * them existed, and neither did any real call to cluster_init() from any
 * boot path -- confirmed by a repo-wide grep before this addendum, every
 * existing call site was in a host test file. A real multiboot2 command-line
 * parser remains a valid, more automatic alternative for a later phase;
 * this addendum instead closes the gap via the accessor comment's own
 * forward-looking note two paragraphs up ("a future cluster-status HTTP/
 * shell surface"): an operator types a real command on a real boot's
 * serial console and the node's identity genuinely changes, with no ISO
 * rebuild required to test a different node id. Named as the mechanism
 * actually built, not silently implied to be the only possible one. */
#define SYS_SLS_CLUSTER_INIT   279
#define SYS_SLS_CLUSTER_STATUS 280

/* Thin syscall wrapper over cluster_init() above. arg is the raw node_id,
 * passed directly rather than via a request struct -- matches sys_sls_
 * partition_destroy()/_pause()/_resume()'s own single-uint32_t ABI shape
 * (kernel/partition.c), the established precedent for a syscall that only
 * ever needs one value. Returns cluster_init()'s own 0 (success) / 1
 * (rejected -- node_id was 0, the reserved sentinel). */
uint64_t sys_sls_cluster_init(uint32_t node_id);

/* Prints this node's current identity/role/roster size to the serial
 * console -- mirrors sys_sls_partition_list()'s own "list/status prints
 * directly, returns nothing decoded" convention (kernel/partition.c) so a
 * human operator watching the serial console can confirm cluster_init()
 * actually took effect. Safe to call before cluster_init() -- reports
 * node_id 0 (uninitialized), the same BSS-zero-safe default every other
 * accessor in this file already has. */
void sys_sls_cluster_status(void);

/* struct DSPPFullPagePacket's forward declaration lives at the very top of
 * this file now -- see the comment there. */
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now);

#endif
