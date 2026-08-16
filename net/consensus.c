#include "consensus.h"
#include "dspp.h"
#include "../kernel/failover.h"

// Multi-Node Partition Scaling Roadmap Phase 7: every send site in this file
// now goes through dspp_transmit_raw() (net/dspp.c), which adds the real
// Ethernet framing this file's own direct e1000_transmit_packet() calls
// never had -- e1000.h is no longer included directly here.
#include "../kernel/kernel_io.h"

extern void update_page_table_permissions_globally(uint32_t force_read_only);
/* Multi-Node Partition Scaling Roadmap Phase 4: partition-scoped sibling of
 * the extern above -- see kernel/stubs.c for both. Deliberately a separate
 * function, not update_page_table_permissions_globally(partition_id, ...),
 * to avoid changing that extern's signature out from under Phase 1's
 * still-real, still-tested global-strip call sites below. */
extern void update_page_table_permissions_for_partition(uint32_t partition_id, uint32_t force_read_only);

/* Phase 1 (Multi-Node Partition Scaling Roadmap): single, real definition
 * of the cluster state and roster -- see consensus.h's header comment for
 * why the old `static struct ClusterNode` defined directly in the header
 * was a real (if so far unexercised) bug, not just cosmetically ugly. */
struct ClusterNode  local_cluster_state = {0};
struct ClusterPeer  cluster_roster[CLUSTER_NODE_MAX];
uint32_t             cluster_roster_count = 0;

/* The leader's node id as last observed on heartbeat RX. Only leaders
 * send heartbeats, so the sender of a heartbeat AT OUR TERM OR HIGHER is
 * by construction the current leader -- see cluster_leader_id()'s header
 * comment in consensus.h for who reads it and why. Deliberately a
 * file-static rather than a field on local_cluster_state: it is
 * peer-observed evidence, not this node's own consensus state, and no
 * other consensus.c function needs it. Cleared to 0 at cluster_init()
 * ("nobody known yet") and set when this node wins an election. */
static uint32_t s_leader_id = 0;

uint32_t cluster_leader_id(void) { return s_leader_id; }

/* ─── How much of a consensus packet actually goes on the wire ────────────
 *
 * Every send site below builds a struct DSPPFullPagePacket and used to
 * transmit sizeof(that) -- 4132 bytes -- when the meaningful content is a
 * 36-byte DSPPPacketHeader followed by a 20-byte ConsensusMessage. The
 * other 4076 bytes are the unused tail of payload_4kb, a field this family
 * of opcodes only borrows because ConsensusMessage had to live somewhere.
 *
 * That was not merely wasteful, it was fatal: 4132 + 14 bytes of Ethernet
 * header is nearly three times the 1500-byte link MTU, so no REQUEST_VOTE
 * or VOTE_REPLY ever reached another node. Nodes campaigned, heard nothing
 * back, timed out, and campaigned again -- a cluster reporting CANDIDATE
 * with a term climbing once per election timeout, indefinitely. The
 * cluster-wide HEARTBEAT was the one consensus message that worked, and
 * only because it sends a bare DSPPPacketHeader.
 *
 * Receivers are unaffected: dspp_rx_dispatch() admits anything at least
 * sizeof(struct DSPPPacketHeader), and both handlers read only the leading
 * ConsensusMessage out of payload_4kb. */
#define CONSENSUS_WIRE_LEN \
    ((uint16_t)(sizeof(struct DSPPPacketHeader) + sizeof(struct ConsensusMessage)))

/* If a future field pushes ConsensusMessage past the link MTU this must
 * fail at compile time, not by silently resuming the old behaviour of
 * emitting frames that go nowhere. */
_Static_assert(CONSENSUS_WIRE_LEN <= DSPP_MAX_WIRE_PAYLOAD,
               "a consensus packet no longer fits a standard Ethernet frame");

/* Recomputes active_nodes_count/stable_quorum_threshold from the roster's
 * real, current size. Called after every membership change rather than
 * kept incrementally in sync, the same "recompute the derived field from
 * the source of truth" discipline database.c's find_or_create_grant()
 * uses for perm_mask -- cheap at this table's size (CLUSTER_NODE_MAX=8),
 * and immune to the update-half-the-paths-and-miss-one class of bug an
 * incremental counter would risk. */
static void cluster_recompute_quorum(void) {
    uint32_t active_peers = 0;
    for (uint32_t i = 0; i < cluster_roster_count; i++) {
        if (cluster_roster[i].active) active_peers++;
    }
    local_cluster_state.active_nodes_count = 1 + active_peers;   /* +1 = self */
    local_cluster_state.stable_quorum_threshold =
        (local_cluster_state.active_nodes_count / 2) + 1;        /* majority */
}

/* Peers heard on the wire but not yet registered. Written by the receive ISR
 * (cluster_note_peer_seen), drained by the BSP sweep. Declared up here because
 * cluster_init() has to be able to clear it -- see below. */
static volatile uint64_t pending_peer_mask = 0;   /* bit (id-1) for ids 1..64 */
uint64_t cluster_peers_autodiscovered = 0;

int cluster_init(uint32_t local_node_id) {
    if (local_node_id == 0) {
        kernel_serial_print("[CONSENSUS] ERROR: node id 0 is reserved (uninitialized sentinel).\n");
        return 1;
    }

    local_cluster_state.node_id                = local_node_id;
    local_cluster_state.current_term           = 0;
    local_cluster_state.voted_for               = 0;
    local_cluster_state.role                    = ROLE_FOLLOWER;
    local_cluster_state.last_heartbeat_tick     = 0;
    local_cluster_state.last_beat_sent_tick     = 0;
    local_cluster_state.accumulated_votes       = 1;   /* self */
    s_leader_id                                = 0;   /* nobody known yet */

    for (uint32_t i = 0; i < CLUSTER_NODE_MAX; i++) cluster_roster[i].active = 0;
    cluster_roster_count = 0;

    /* Discard peers heard before this call. cluster_init() means "form a new
     * cluster as node N", and it deliberately empties the roster -- so leaving
     * a queue of previously-heard node ids to be drained into the fresh roster
     * a moment later would silently undo that. The old membership would
     * reappear on the next BSP sweep, with the quorum recomputed to match, and
     * nothing would say why.
     *
     * Found by a host test whose 176 earlier checks had been processing
     * consensus packets all along: the queue was still full of their senders,
     * so the first drain after a re-init registered all of them at once. That
     * was the test noticing a real leak, not the test being dirty. */
    __atomic_store_n(&pending_peer_mask, 0ULL, __ATOMIC_RELAXED);

    cluster_recompute_quorum();   /* self only: active_nodes_count=1, quorum=1 */

    kernel_serial_printf("[CONSENSUS] node %u initialised (roster empty, quorum=1).\n",
                          (unsigned)local_node_id);
    return 0;
}

int cluster_register_peer(uint32_t node_id) {
    if (node_id == 0 || node_id == local_cluster_state.node_id) {
        kernel_serial_printf("[CONSENSUS] ERROR: invalid peer id %u.\n", (unsigned)node_id);
        return -1;
    }

    for (uint32_t i = 0; i < cluster_roster_count; i++) {
        if (cluster_roster[i].node_id == node_id) {
            if (cluster_roster[i].active) return 1;   /* already active, no-op */
            cluster_roster[i].active = 1;
            cluster_recompute_quorum();
            return 2;                                  /* re-activated */
        }
    }

    if (cluster_roster_count >= CLUSTER_NODE_MAX) {
        kernel_serial_print("[CONSENSUS] ERROR: roster full.\n");
        return -2;
    }

    cluster_roster[cluster_roster_count].node_id = node_id;
    cluster_roster[cluster_roster_count].active  = 1;
    cluster_roster_count++;
    cluster_recompute_quorum();

    kernel_serial_printf("[CONSENSUS] registered peer node %u (active_nodes=%u, quorum=%u).\n",
                          (unsigned)node_id,
                          (unsigned)local_cluster_state.active_nodes_count,
                          (unsigned)local_cluster_state.stable_quorum_threshold);
    return 0;
}

/* ─── Learning peers from the wire ─────────────────────────────────────────
 * cluster_register_peer() above had exactly ONE caller in the whole kernel:
 * POST /api/cluster/peer. Nothing in the receive path ever registered anybody,
 * and run-cluster.sh never peered the nodes at all, so a roster was whatever an
 * operator had POSTed -- in one direction only.
 *
 * The observed result on a real 4-node cluster: node 1 held all four, and nodes
 * 2, 3 and 4 each held only themselves with `quorum threshold 1`. They were
 * FOLLOWER at the leader's term, so heartbeats were arriving and the term was
 * propagating correctly -- but the leader was never added to their rosters.
 * Kill node 1 and all three time out, campaign, and each elects itself with a
 * quorum of one: four single-node clusters, each believing it holds every
 * lease. The same split-brain the vote-candidate fix closed, reached by a
 * different route.
 *
 * ─── Why a deferred queue and not a direct call ────────────────────────────
 * The receive path runs in the timer ISR (kernel/timer.c -> net_poll_tick ->
 * e1000_poll_rx -> dspp_rx_dispatch). cluster_register_peer() mutates
 * cluster_roster[] and recomputes the quorum, both of which the BSP reads while
 * serving /api/cluster. Calling it from the ISR would tear those reads. So the
 * ISR only records that an id was SEEN, in one word, and the BSP sweep drains
 * it -- the same producer/consumer split the AP reconciler already uses.
 *
 * ─── The trust surface, stated plainly ─────────────────────────────────────
 * The cluster segment is unauthenticated L2 broadcast. Anything on it can
 * assert a node id and be believed, which inflates active_nodes and therefore
 * the quorum threshold, and a quorum that cannot be met stalls elections. That
 * is a real denial-of-service and it is accepted deliberately here rather than
 * overlooked: the roster is bounded by CLUSTER_NODE_MAX so the damage is
 * bounded too, every auto-registration is logged with its source, and the
 * running count is exposed on /api/cluster so an operator can see the roster
 * growing without having asked for it. Authenticating DSPP is a separate piece
 * of work and this comment is not a substitute for it. */

void cluster_note_peer_seen(uint32_t node_id) {
    /* ISR context. One atomic OR, no roster access, no logging. Ids outside
     * 1..64 are dropped rather than folded into the mask -- silently mapping a
     * large id onto some other node's bit would register the WRONG peer, which
     * is worse than not registering at all. */
    if (node_id == 0 || node_id > 64) return;
    if (node_id == local_cluster_state.node_id) return;   /* plain read, never written by the ISR */
    __atomic_fetch_or(&pending_peer_mask, 1ULL << (node_id - 1), __ATOMIC_RELAXED);
}

/* The pending queue's contents, for testing only. Exposed because the drain's
 * clearing of it is otherwise unobservable: cluster_register_peer() is
 * idempotent, so a drain that read the mask without clearing would re-process
 * the same ids every sweep, get "already active" every time, and look
 * identical from outside. A mutation replacing the exchange with a plain load
 * passed the whole suite. */
uint64_t cluster_pending_peer_mask_for_test(void) {
    return __atomic_load_n(&pending_peer_mask, __ATOMIC_RELAXED);
}

uint32_t cluster_drain_discovered_peers(void) {
    /* BSP only. Exchange-to-zero so an id noted between the read and the clear
     * is not lost -- it stays set and is picked up on the next sweep. */
    uint64_t seen = __atomic_exchange_n(&pending_peer_mask, 0ULL, __ATOMIC_RELAXED);
    if (seen == 0) return 0;

    uint32_t registered = 0;
    for (uint32_t bit = 0; bit < 64; bit++) {
        if (!(seen & (1ULL << bit))) continue;
        uint32_t id = bit + 1;

        /* Already-active peers return 1 and log nothing, so a steady cluster
         * does not spam the console once per heartbeat. */
        int rc = cluster_register_peer(id);
        if (rc == 0 || rc == 2) {
            cluster_peers_autodiscovered++;
            registered++;
            kernel_serial_printf(
                "[CONSENSUS] node %u learned from the wire, not from an operator -- "
                "auto-registered (%s). Roster is now %u node(s), quorum %u.\n",
                (unsigned)id, rc == 2 ? "re-activated" : "new",
                (unsigned)local_cluster_state.active_nodes_count,
                (unsigned)local_cluster_state.stable_quorum_threshold);
        }
    }
    return registered;
}

uint32_t cluster_local_node_id(void)     { return local_cluster_state.node_id; }

int cluster_is_leader(void)              { return local_cluster_state.role == ROLE_LEADER; }
uint32_t cluster_active_node_count(void) { return local_cluster_state.active_nodes_count; }

// ─── Syscalls: operator-driven node identity configuration ────────────
// See consensus.h's own header comment on SYS_SLS_CLUSTER_INIT for the
// full rationale -- this closes the "cluster_init() is never called from
// any real boot path" gap via a real, live-boot-reachable operator command
// rather than a boot-time cmdline parser (a valid alternative, not
// attempted here).
uint64_t sys_sls_cluster_init(uint32_t node_id) {
    return (uint64_t)cluster_init(node_id);
}

/* Was static until the cluster-view HTTP surface needed it -- exactly the
 * "future cluster-status HTTP/shell surface" this header anticipated. */
const char* consensus_role_name(enum NodeRole r) {
    switch (r) {
        case ROLE_LEADER:    return "LEADER";
        case ROLE_CANDIDATE: return "CANDIDATE";
        case ROLE_FOLLOWER:  default: return "FOLLOWER";
    }
}

void sys_sls_cluster_status(void) {
    kernel_serial_printf(
        "[CONSENSUS] node_id=%u role=%s term=%u active_nodes=%u quorum=%u roster=%u\n",
        (unsigned)local_cluster_state.node_id,
        consensus_role_name(local_cluster_state.role),
        (unsigned)local_cluster_state.current_term,
        (unsigned)local_cluster_state.active_nodes_count,
        (unsigned)local_cluster_state.stable_quorum_threshold,
        (unsigned)cluster_roster_count);
    if (local_cluster_state.node_id == 0) {
        kernel_serial_print("[CONSENSUS] node_id 0 means cluster_init() has not been called on this "
                             "boot -- partition_migrate() will take the same-disk relocate path, "
                             "not the cross-node DSPP wire path. Run 'cluster init <id>' first.\n");
    }
}

/* Driven from the BSP's HTTP sweep (net/http.c), NOT from the timer IRQ.
 *
 * This comment used to read "Executed every 10ms by the kernel timer
 * interrupt handler on Core 3". That was never true: a repo-wide grep found
 * no caller anywhere in the kernel -- only this definition, the header
 * prototype, and one host test. consensus.h's own comment on
 * check_partition_lease_heartbeat_tick() was honest about it ("not actually
 * wired into the real timer yet"); this one contradicted it and read as
 * settled fact, which is how it survived several phases.
 *
 * The visible symptom was a cluster that looked healthy: every node
 * reporting FOLLOWER at term 0 forever, no leader ever elected, and
 * therefore partition_holds_write_lease() false for every partition and
 * dspp_page_write_allowed() (net/dspp.c) permanently closed.
 *
 * `now` is a kernel_tick_counter reading passed by the caller, matching
 * service_heartbeat_tick(kernel_tick_counter) in the same sweep. Both
 * branches below depend on it being wall-clock rather than a call count --
 * see the "Election timing" block in consensus.h. */
void check_consensus_heartbeat_tick(uint64_t now) {
    if (local_cluster_state.role == ROLE_LEADER) {
        /* LEADER: broadcast periodic heartbeats to maintain authority.
         *
         * Rate-limited. Unconditional transmission here was tolerable when
         * the caller was believed to be a 100 Hz timer; from a sweep that
         * spins as fast as the request load allows, it is a broadcast storm
         * on a shared segment every other node has to receive and parse. */
        if (now - local_cluster_state.last_beat_sent_tick < LEADER_HEARTBEAT_TICKS) return;
        local_cluster_state.last_beat_sent_tick = now;

        struct DSPPPacketHeader hb_packet;
        hb_packet.magic = DSPP_MAGIC;
        hb_packet.opcode = DSPP_CMD_HEARTBEAT;
        hb_packet.node_source_id = (uint16_t)local_cluster_state.node_id;
        hb_packet.transaction_id = local_cluster_state.current_term;

        dspp_transmit_raw(&hb_packet, sizeof(struct DSPPPacketHeader));
    }
    else {
        /* FOLLOWER/CANDIDATE: measure silence against wall-clock ticks.
         *
         * A node that has never heard anything has last_heartbeat_tick 0,
         * so on a freshly booted node this elapses from boot -- which is
         * the intent: nobody is leading, somebody should campaign. */
        uint64_t silent_for = now - local_cluster_state.last_heartbeat_tick;

        if (silent_for > consensus_election_timeout(local_cluster_state.node_id)) {
            /* NETWORK SEVERED / LEADER CRASHED / nobody ever led: campaign.
             * trigger_kernel_election_campaign() re-stamps
             * last_heartbeat_tick, so the next timeout is measured from
             * this campaign rather than firing again on the very next
             * sweep. */
            trigger_kernel_election_campaign(now);
        }
    }
}

void trigger_kernel_election_campaign(uint64_t now) {
    local_cluster_state.role = ROLE_CANDIDATE;
    local_cluster_state.current_term++;
    local_cluster_state.voted_for = local_cluster_state.node_id; // Vote for self

    /* Restart the clock on this campaign. Without this the CANDIDATE still
     * reads as silent and re-campaigns on the next tick, incrementing the
     * term every sweep -- a term counter running away at loop speed, which
     * on a real segment also invalidates every in-flight vote reply. */
    local_cluster_state.last_heartbeat_tick = now;

    /* Fresh election, own vote only. Previously a function-local `static`
     * in process_consensus_packet() that was reset ONLY on winning, so a
     * failed campaign's votes carried into the next one and quorum could be
     * declared on fewer real votes than the threshold. Matches what
     * partition_lease_trigger_election() below has always done. */
    local_cluster_state.accumulated_votes = 1;

    // Split-Brain Mitigation: Strip local memory pages of write authorizations instantly
    // Restricts the local node to safe, non-mutating read operations while split
    update_page_table_permissions_globally(1); // 1 = Force Read-Only across all objects

    struct DSPPFullPagePacket vote_req;
    vote_req.header.magic = DSPP_MAGIC;
    vote_req.header.opcode = DSPP_CMD_REQUEST_VOTE;
    vote_req.header.node_source_id = (uint16_t)local_cluster_state.node_id;
    vote_req.header.transaction_id = local_cluster_state.current_term;

    struct ConsensusMessage* msg = (struct ConsensusMessage*)vote_req.payload_4kb;
    msg->term = local_cluster_state.current_term;
    msg->candidate_id = local_cluster_state.node_id;

    kernel_serial_printf("[CONSENSUS] Terms timeout. Node %u campaigning for Term election: %d\n",
                          (unsigned)local_cluster_state.node_id, local_cluster_state.current_term);
    dspp_transmit_raw(&vote_req, CONSENSUS_WIRE_LEN);
}

// Processing interface extending our existing 'handle_network_rx_interrupt_packet' handler
//
// `now` is a kernel_tick_counter reading, needed because accepting a
// heartbeat restarts this node's election timer and that timer is now
// wall-clock rather than a call count. Supplied by the RX path
// (dspp_rx_dispatch(), net/dspp.c).
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) {
    struct ConsensusMessage* msg = (struct ConsensusMessage*)packet->payload_4kb;

    /* Any consensus traffic from a node id proves that node exists and is
     * talking. Noted before the opcode is even examined, so a stale-term
     * heartbeat or a vote reply addressed to someone else still teaches us the
     * roster -- the early returns below would otherwise skip it. */
    cluster_note_peer_seen((uint32_t)packet->header.node_source_id);

    if (packet->header.opcode == DSPP_CMD_HEARTBEAT) {
        uint32_t hb_term = packet->header.transaction_id;
        /* Liveness for the failover subsystem, noted BEFORE the stale-term
         * return for the same reason cluster_note_peer_seen() runs at the
         * top of this function: a stale-term heartbeat is still a node
         * provably talking. The leader is the only heartbeat sender, so
         * followers track the leader here; when it dies, the silence is
         * what failover_tick() (net/http.c BSP sweep) turns into a DEAD
         * declaration after FAILOVER_DEAD_TICKS. */
        failover_note_heartbeat((uint32_t)packet->header.node_source_id, now);
        if (hb_term < local_cluster_state.current_term) return;   /* stale */

        /* Resetting the watchdog is right for ANY term at least ours: an
         * equal-term heartbeat is the normal case -- the leader beating at
         * the term we already hold -- and a follower that ignored it would
         * time out and depose a healthy leader. */
        local_cluster_state.last_heartbeat_tick = now;

        /* The heartbeat sender is the leader; record it for the
         * partition-sync conflict resolution (cluster_leader_id()). Set
         * only past the stale-term return above: a stale-term heartbeat
         * identifies the OLD leader, and using it to resolve an ownership
         * conflict would be exactly the wrong authority. */
        s_leader_id = (uint32_t)packet->header.node_source_id;

        if (hb_term > local_cluster_state.current_term) {
            /* Genuinely ahead: adopt it and stand down whatever we were. */
            local_cluster_state.current_term = hb_term;
            local_cluster_state.role = ROLE_FOLLOWER;
            return;
        }

        /* ─── Equal term: a LEADER must NOT stand down here ────────────────
         * This used to be `>=`, demoting unconditionally. So a leader that
         * received another node's heartbeat at its OWN term immediately
         * became a follower.
         *
         * Combined with the broadcast vote-counting bug above -- which let
         * two nodes lead the same term -- the two leaders demoted each other
         * on their first heartbeats, leaving nobody leading. Every node then
         * timed out and campaigned, and the term climbed by hundreds per
         * minute. A real cluster went from term 48 to 553 between two
         * consecutive `cluster status` calls.
         *
         * With the vote fix, two same-term leaders can no longer arise, so
         * this branch should be unreachable for a LEADER. It is written
         * defensively rather than asserted: standing down on an equal term is
         * the behaviour that turns one stray frame into a cluster-wide
         * election storm, and a CANDIDATE stepping aside for a leader at the
         * same term is correct and still happens. */
        if (local_cluster_state.role == ROLE_CANDIDATE)
            local_cluster_state.role = ROLE_FOLLOWER;
        return;
    }

    if (packet->header.opcode == DSPP_CMD_REQUEST_VOTE) {
        /* Zeroed, not left as whatever was on the stack. CONSENSUS_WIRE_LEN
         * puts the first 20 bytes of payload_4kb on the wire, so an
         * uninitialised reply transmitted 20 bytes of this node's stack to
         * every peer -- wrong values in fields the receiver reads, and a
         * small information leak besides. dspp_migrate_send_ack() zeroes its
         * unused fields explicitly for the same reason. */
        struct DSPPFullPagePacket reply;
        for (uint32_t z = 0; z < sizeof(struct DSPPPacketHeader); z++)
            ((uint8_t*)&reply)[z] = 0;
        for (uint32_t z = 0; z < sizeof(struct ConsensusMessage); z++)
            reply.payload_4kb[z] = 0;

        reply.header.magic = DSPP_MAGIC;
        reply.header.opcode = DSPP_CMD_VOTE_REPLY;
        reply.header.node_source_id = (uint16_t)local_cluster_state.node_id;

        struct ConsensusMessage* reply_msg = (struct ConsensusMessage*)reply.payload_4kb;

        /* ─── WHO the vote is for, which nothing recorded before ───────────
         * DSPP is pure L2 broadcast: there is no node-to-MAC table, so every
         * frame reaches every node (see dspp_transmit_raw()). A VOTE_REPLY
         * therefore arrives at every candidate, not just the one it answers.
         *
         * With no candidate named, the reply handler below counted ANY
         * granted reply at its own term. Two nodes campaigning at the same
         * term both counted the same three grants, both reached quorum, and
         * both became LEADER -- textbook split brain, on a cluster whose
         * whole purpose is to prevent exactly that.
         *
         * Carrying the candidate's id costs a field that already existed on
         * the struct and was simply never assigned. */
        reply_msg->candidate_id = msg->candidate_id;

        if (msg->term > local_cluster_state.current_term) {
            local_cluster_state.current_term = msg->term;
            local_cluster_state.role = ROLE_FOLLOWER;
            local_cluster_state.voted_for = msg->candidate_id;
            /* Granting a vote also restarts this node's own election timer.
             * Without it, every follower that just voted would time out and
             * campaign against the candidate it is still waiting on --
             * turning one election into a term-inflation race. */
            local_cluster_state.last_heartbeat_tick = now;
            reply_msg->vote_granted = 1; // Approve candidate
        } else {
            reply_msg->vote_granted = 0; // Deny candidate
        }

        /* Stamped AFTER the branch above, deliberately, and this line is the
         * whole election.
         *
         * It used to sit before the `if`, so a granting voter replied with
         * the term it held BEFORE adopting the candidate's. The candidate
         * counts a reply only when msg->term == its own current_term, so
         * every granted vote arrived one term stale and was discarded. Votes
         * were cast correctly and thrown away on receipt: four nodes, all
         * willing, none ever reaching quorum, each campaigning again on
         * timeout. A cluster reporting CANDIDATE forever with no error
         * anywhere.
         *
         * A denial still carries this node's unchanged current_term, which
         * is what a candidate needs to see that it is behind. */
        reply_msg->term = local_cluster_state.current_term;

        dspp_transmit_raw(&reply, CONSENSUS_WIRE_LEN);
    }

    else if (packet->header.opcode == DSPP_CMD_VOTE_REPLY && local_cluster_state.role == ROLE_CANDIDATE) {
        /* The vote must be FOR THIS NODE. On a broadcast segment every
         * candidate receives every reply, so without this a vote granted to
         * node 2 was also counted by nodes 3 and 4 -- and two candidates at
         * one term could both declare quorum and both lead. See the
         * candidate_id assignment in the REQUEST_VOTE branch above. */
        if (msg->candidate_id != local_cluster_state.node_id) return;

        if (msg->term == local_cluster_state.current_term && msg->vote_granted) {
            local_cluster_state.accumulated_votes++;

            if (local_cluster_state.accumulated_votes >= local_cluster_state.stable_quorum_threshold) {
                // QUORUM ACHIEVED: Promote node safely to Leader status
                local_cluster_state.role = ROLE_LEADER;

                /* Send the authority-asserting heartbeat on the very next
                 * tick rather than up to LEADER_HEARTBEAT_TICKS later: the
                 * followers that just voted are counting down their own
                 * timeouts, and the new leader's first heartbeat is what
                 * stops them campaigning.
                 *
                 * Written as a saturating subtraction rather than a bare
                 * `now - LEADER_HEARTBEAT_TICKS`. Unsigned wraparound would
                 * in fact still compare correctly here, but relying on that
                 * is not worth the reader's double-take. The clamped case
                 * (now < 30, i.e. the first 300 ms of a boot) cannot occur
                 * anyway: winning an election requires having first waited
                 * out an election timeout of at least 150 ticks. */
                local_cluster_state.last_beat_sent_tick =
                    (now >= LEADER_HEARTBEAT_TICKS) ? (now - LEADER_HEARTBEAT_TICKS) : 0;

                // Restore full Read-Write authorizations down into Process page tables
                update_page_table_permissions_globally(0);
                /* This node IS the leader now; record itself so its own
                 * adoption/ownership announces resolve as leader claims. */
                s_leader_id = local_cluster_state.node_id;
                kernel_serial_printf("[CONSENSUS] Quorum stable. Node %u elected LEADER for term %d.\n",
                                      (unsigned)local_cluster_state.node_id, local_cluster_state.current_term);
            }
        }
    }
}

// ─── Multi-Node Partition Scaling Roadmap Phase 4: partition-scoped write
// leases -- see consensus.h's own comment block for the full design
// rationale on why this is a SEPARATE table/mechanism layered alongside
// local_cluster_state/cluster_roster above, not a replacement for them. ──

struct PartitionLease partition_lease_table[PARTITION_LEASE_MAX];

static struct PartitionLease* find_lease_row(uint32_t partition_id) {
    for (uint32_t i = 0; i < PARTITION_LEASE_MAX; i++) {
        if (partition_lease_table[i].active && partition_lease_table[i].partition_id == partition_id)
            return &partition_lease_table[i];
    }
    return 0;
}

int partition_lease_init(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) {
        for (uint32_t i = 0; i < PARTITION_LEASE_MAX; i++) {
            if (!partition_lease_table[i].active) { row = &partition_lease_table[i]; break; }
        }
        if (!row) {
            kernel_serial_printf(
                "[CONSENSUS] ERROR: partition lease table full, cannot init partition %u.\n",
                (unsigned)partition_id);
            return 1;
        }
    }

    row->partition_id            = partition_id;
    row->term                    = 0;
    row->voted_for                = 0;
    row->role                    = ROLE_FOLLOWER;
    row->last_heartbeat_tick     = 0;
    row->last_beat_sent_tick     = 0;
    row->accumulated_votes       = 1;   /* self, matches trigger_election's own reset */
    row->active                  = 1;

    kernel_serial_printf("[CONSENSUS] partition %u lease initialised (FOLLOWER, term=0).\n",
                          (unsigned)partition_id);
    return 0;
}

enum NodeRole partition_lease_get_role(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    return row ? row->role : ROLE_FOLLOWER;
}

uint32_t partition_lease_get_term(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    return row ? row->term : 0;
}

int partition_holds_write_lease(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    return (row && row->role == ROLE_LEADER) ? 1 : 0;
}

void partition_lease_trigger_election(uint32_t partition_id, uint64_t now) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) {
        if (partition_lease_init(partition_id) != 0) return;   /* table full -- nothing to campaign with */
        row = find_lease_row(partition_id);
    }

    row->role              = ROLE_CANDIDATE;
    row->term++;
    row->voted_for          = local_cluster_state.node_id;   /* vote for self */
    row->accumulated_votes = 1;                                /* fresh election, own vote counted */
    row->last_heartbeat_tick = now;   /* measure the next timeout from THIS campaign,
                                        * not from the last heartbeat -- otherwise the
                                        * row re-campaigns every tick and its term runs
                                        * away at sweep rate. */

    // Split-brain mitigation, now scoped to JUST this partition's objects --
    // not update_page_table_permissions_globally(1)'s all-or-nothing strip,
    // the exact narrowing this whole phase exists to make real.
    update_page_table_permissions_for_partition(partition_id, 1);

    struct DSPPFullPagePacket vote_req;
    vote_req.header.magic         = DSPP_MAGIC;
    vote_req.header.opcode        = DSPP_CMD_PARTITION_REQUEST_VOTE;
    vote_req.header.node_source_id = (uint16_t)local_cluster_state.node_id;
    vote_req.header.transaction_id = row->term;

    struct ConsensusMessage* msg = (struct ConsensusMessage*)vote_req.payload_4kb;
    msg->term          = row->term;
    msg->candidate_id  = local_cluster_state.node_id;
    msg->partition_id  = partition_id;
    msg->vote_granted  = 0;
    msg->last_log_index = 0;

    kernel_serial_printf(
        "[CONSENSUS] partition %u: node %u campaigning for write lease, term %u.\n",
        (unsigned)partition_id, (unsigned)local_cluster_state.node_id, (unsigned)row->term);
    dspp_transmit_raw(&vote_req, CONSENSUS_WIRE_LEN);
}

int partition_lease_step_down(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) {
        kernel_serial_printf(
            "[CONSENSUS] partition %u: step-down requested but no lease row exists -- "
            "nothing to relinquish.\n", (unsigned)partition_id);
        return 1;
    }

    enum NodeRole prior_role = row->role;
    row->role                    = ROLE_FOLLOWER;
    row->voted_for                = 0;
    row->accumulated_votes       = 1;
    row->last_heartbeat_tick     = 0;
    row->last_beat_sent_tick     = 0;

    kernel_serial_printf(
        "[CONSENSUS] partition %u: node %u voluntarily stepped down from %s -- lease relinquished (term %u unchanged).\n",
        (unsigned)partition_id, (unsigned)local_cluster_state.node_id,
        prior_role == ROLE_LEADER ? "LEADER" : (prior_role == ROLE_CANDIDATE ? "CANDIDATE" : "FOLLOWER"),
        (unsigned)row->term);
    return 0;
}

void partition_lease_heartbeat_tick(uint32_t partition_id, uint64_t now) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) return;   /* no lease established for this partition yet -- nothing to tick */

    if (row->role == ROLE_LEADER) {
        /* Rate-limited exactly as the cluster-wide heartbeat is, and it
         * matters more here: check_partition_lease_heartbeat_tick() walks
         * every active row, so an unthrottled send would put one 4 KB frame
         * per leased partition on the wire per sweep. */
        if (now - row->last_beat_sent_tick < LEADER_HEARTBEAT_TICKS) return;
        row->last_beat_sent_tick = now;

        // LEADER for this partition: broadcast a periodic heartbeat
        // carrying partition_id -- unlike Phase 1's cluster-wide HEARTBEAT
        // (a bare struct DSPPPacketHeader), this needs the full 4KB packet
        // since only the ConsensusMessage payload has room for partition_id;
        // DSPPPacketHeader itself has no such field (that's Phase 5's job).
        struct DSPPFullPagePacket hb_packet;
        hb_packet.header.magic         = DSPP_MAGIC;
        hb_packet.header.opcode        = DSPP_CMD_PARTITION_HEARTBEAT;
        hb_packet.header.node_source_id = (uint16_t)local_cluster_state.node_id;
        hb_packet.header.transaction_id = row->term;

        struct ConsensusMessage* msg = (struct ConsensusMessage*)hb_packet.payload_4kb;
        msg->term          = row->term;
        msg->candidate_id  = local_cluster_state.node_id;
        msg->partition_id  = partition_id;
        msg->vote_granted  = 0;
        msg->last_log_index = 0;

        dspp_transmit_raw(&hb_packet, CONSENSUS_WIRE_LEN);
    } else {
        // FOLLOWER/CANDIDATE for this partition: measure silence against
        // wall-clock ticks, using the same staggered threshold Phase 1's
        // cluster-wide mechanism uses.
        if (now - row->last_heartbeat_tick >
                consensus_election_timeout(local_cluster_state.node_id)) {
            /* trigger re-stamps last_heartbeat_tick. */
            partition_lease_trigger_election(partition_id, now);
        }
    }
}

void check_partition_lease_heartbeat_tick(uint64_t now) {
    for (uint32_t i = 0; i < PARTITION_LEASE_MAX; i++) {
        if (partition_lease_table[i].active)
            partition_lease_heartbeat_tick(partition_lease_table[i].partition_id, now);
    }
}

/* `now` is a kernel_tick_counter reading -- see process_consensus_packet(). */
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) {

    /* Any consensus traffic from a node id proves that node exists and is
     * talking. Noted before the opcode is even examined, so a stale-term
     * heartbeat or a vote reply addressed to someone else still teaches us the
     * roster -- the early returns below would otherwise skip it. */
    cluster_note_peer_seen((uint32_t)packet->header.node_source_id);
    struct ConsensusMessage* msg = (struct ConsensusMessage*)packet->payload_4kb;
    uint32_t partition_id = msg->partition_id;

    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) {
        // An incoming request about a partition this node has never leased
        // before must still be able to participate -- mirrors partition_
        // lease_trigger_election()'s own find-or-create posture.
        if (partition_lease_init(partition_id) != 0) return;   /* table full */
        row = find_lease_row(partition_id);
    }

    if (packet->header.opcode == DSPP_CMD_PARTITION_HEARTBEAT) {
        if (msg->term < row->term) return;                 /* stale */
        row->last_heartbeat_tick = now;                    /* equal term is the normal case */
        if (msg->term > row->term) {
            row->term = msg->term;
            row->role = ROLE_FOLLOWER;
            return;
        }
        /* Equal term: a lease HOLDER must not relinquish. Same reasoning as
         * the cluster-wide handler above -- `>=` made a leader stand down for
         * a peer at its own term, which turns one frame into an election
         * storm. Here it would also mean two nodes trading a WRITE lease back
         * and forth, which is worse than churn. */
        if (row->role == ROLE_CANDIDATE) row->role = ROLE_FOLLOWER;
        return;
    }

    if (packet->header.opcode == DSPP_CMD_PARTITION_REQUEST_VOTE) {
        /* Zeroed for the same reason as the cluster-wide reply: the first 20
         * bytes of payload_4kb go on the wire, and an uninitialised struct
         * puts stack contents there. */
        struct DSPPFullPagePacket reply;
        for (uint32_t z = 0; z < sizeof(struct DSPPPacketHeader); z++)
            ((uint8_t*)&reply)[z] = 0;
        for (uint32_t z = 0; z < sizeof(struct ConsensusMessage); z++)
            reply.payload_4kb[z] = 0;

        reply.header.magic         = DSPP_MAGIC;
        reply.header.opcode        = DSPP_CMD_PARTITION_VOTE_REPLY;
        reply.header.node_source_id = (uint16_t)local_cluster_state.node_id;

        struct ConsensusMessage* reply_msg = (struct ConsensusMessage*)reply.payload_4kb;
        reply_msg->partition_id = partition_id;
        /* The CANDIDATE being voted for, not this voter.
         *
         * This used to be `local_cluster_state.node_id` -- the replying
         * node's own id -- which is not merely useless to the receiver but
         * actively misleading: it named the voter in a field the reply
         * handler needs for "was this vote for me". DSPP broadcasts, so
         * without a correct value every candidate at this term counted this
         * grant, and two nodes could both take the write lease for one
         * partition. */
        reply_msg->candidate_id = msg->candidate_id;

        if (msg->term > row->term) {
            row->term       = msg->term;
            row->role       = ROLE_FOLLOWER;
            row->voted_for  = msg->candidate_id;
            /* Voting restarts this row's own timer -- see the cluster-wide
             * equivalent in process_consensus_packet(). */
            row->last_heartbeat_tick = now;
            reply_msg->vote_granted = 1;   // Approve candidate
        } else {
            reply_msg->vote_granted = 0;   // Deny candidate
        }

        /* AFTER the branch, for the same reason as the cluster-wide handler:
         * a granted vote stamped with the pre-update term is discarded by the
         * candidate, which requires msg->term == its own row->term. Same bug,
         * same one-line ordering, in the mechanism that actually gates
         * writes. */
        reply_msg->term = row->term;

        dspp_transmit_raw(&reply, CONSENSUS_WIRE_LEN);
        return;
    }

    if (packet->header.opcode == DSPP_CMD_PARTITION_VOTE_REPLY && row->role == ROLE_CANDIDATE) {
        /* For THIS node, or not counted -- see the cluster-wide equivalent.
         * Without it, a grant to another candidate counted here too, and two
         * nodes could each believe they hold this partition's write lease. */
        if (msg->candidate_id != local_cluster_state.node_id) return;

        if (msg->term == row->term && msg->vote_granted) {
            row->accumulated_votes++;

            if (row->accumulated_votes >= local_cluster_state.stable_quorum_threshold) {
                // QUORUM ACHIEVED for THIS partition's lease -- reuses
                // Phase 1's roster-derived quorum (the same cluster nodes
                // are being asked, just about a different question), rather
                // than inventing a second majority concept.
                row->role              = ROLE_LEADER;
                row->accumulated_votes = 1;
                /* Assert authority on the next tick, not up to
                 * LEADER_HEARTBEAT_TICKS later -- see the cluster-wide
                 * equivalent for why, and for why this saturates. */
                row->last_beat_sent_tick =
                    (now >= LEADER_HEARTBEAT_TICKS) ? (now - LEADER_HEARTBEAT_TICKS) : 0;

                // Restore write authorization to JUST this partition's
                // objects -- the narrowing this whole phase exists for.
                update_page_table_permissions_for_partition(partition_id, 0);
                kernel_serial_printf(
                    "[CONSENSUS] partition %u: quorum stable, node %u elected LEADER (write lease) for term %u.\n",
                    (unsigned)partition_id, (unsigned)local_cluster_state.node_id, (unsigned)row->term);
            }
        }
    }
}
