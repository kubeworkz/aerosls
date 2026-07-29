#include "consensus.h"
#include "dspp.h"

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

    for (uint32_t i = 0; i < CLUSTER_NODE_MAX; i++) cluster_roster[i].active = 0;
    cluster_roster_count = 0;

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

uint32_t cluster_local_node_id(void)     { return local_cluster_state.node_id; }
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
    dspp_transmit_raw(&vote_req, sizeof(struct DSPPFullPagePacket));
}

// Processing interface extending our existing 'handle_network_rx_interrupt_packet' handler
//
// `now` is a kernel_tick_counter reading, needed because accepting a
// heartbeat restarts this node's election timer and that timer is now
// wall-clock rather than a call count. Supplied by the RX path
// (dspp_rx_dispatch(), net/dspp.c).
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) {
    struct ConsensusMessage* msg = (struct ConsensusMessage*)packet->payload_4kb;

    if (packet->header.opcode == DSPP_CMD_HEARTBEAT) {
        // Reset local watch dogs
        if (packet->header.transaction_id >= local_cluster_state.current_term) {
            local_cluster_state.current_term = packet->header.transaction_id;
            local_cluster_state.role = ROLE_FOLLOWER;
            local_cluster_state.last_heartbeat_tick = now;
        }
        return;
    }

    if (packet->header.opcode == DSPP_CMD_REQUEST_VOTE) {
        struct DSPPFullPagePacket reply;
        reply.header.magic = DSPP_MAGIC;
        reply.header.opcode = DSPP_CMD_VOTE_REPLY;
        reply.header.node_source_id = (uint16_t)local_cluster_state.node_id;

        struct ConsensusMessage* reply_msg = (struct ConsensusMessage*)reply.payload_4kb;
        reply_msg->term = local_cluster_state.current_term;

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

        dspp_transmit_raw(&reply, sizeof(struct DSPPFullPagePacket));
    }

    else if (packet->header.opcode == DSPP_CMD_VOTE_REPLY && local_cluster_state.role == ROLE_CANDIDATE) {
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
    dspp_transmit_raw(&vote_req, sizeof(struct DSPPFullPagePacket));
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

        dspp_transmit_raw(&hb_packet, sizeof(struct DSPPFullPagePacket));
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
        if (msg->term >= row->term) {
            row->term                = msg->term;
            row->role                = ROLE_FOLLOWER;
            row->last_heartbeat_tick = now;
        }
        return;
    }

    if (packet->header.opcode == DSPP_CMD_PARTITION_REQUEST_VOTE) {
        struct DSPPFullPagePacket reply;
        reply.header.magic         = DSPP_MAGIC;
        reply.header.opcode        = DSPP_CMD_PARTITION_VOTE_REPLY;
        reply.header.node_source_id = (uint16_t)local_cluster_state.node_id;

        struct ConsensusMessage* reply_msg = (struct ConsensusMessage*)reply.payload_4kb;
        reply_msg->term         = row->term;
        reply_msg->partition_id = partition_id;
        reply_msg->candidate_id = local_cluster_state.node_id;

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

        dspp_transmit_raw(&reply, sizeof(struct DSPPFullPagePacket));
        return;
    }

    if (packet->header.opcode == DSPP_CMD_PARTITION_VOTE_REPLY && row->role == ROLE_CANDIDATE) {
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
