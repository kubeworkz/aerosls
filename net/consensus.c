#include "consensus.h"
#include "dspp.h"

#include "e1000.h"
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
    local_cluster_state.heartbeat_ticks_elapsed = 0;

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

// Executed every 10ms by the kernel timer interrupt handler on Core 3
void check_consensus_heartbeat_tick(void) {
    if (local_cluster_state.role == ROLE_LEADER) {
        // LEADER: Broadcast periodic heartbeats to maintain authority
        struct DSPPPacketHeader hb_packet;
        hb_packet.magic = DSPP_MAGIC;
        hb_packet.opcode = DSPP_CMD_HEARTBEAT;
        hb_packet.node_source_id = (uint16_t)local_cluster_state.node_id;
        hb_packet.transaction_id = local_cluster_state.current_term;

        e1000_transmit_packet(&hb_packet, sizeof(struct DSPPPacketHeader));
    }
    else {
        // FOLLOWER/CANDIDATE: Track silence threshold
        local_cluster_state.heartbeat_ticks_elapsed++;

        if (local_cluster_state.heartbeat_ticks_elapsed > 150) { // 1.5 seconds of network silence
            // NETWORK SEVERED / LEADER CRASHED: Trigger an Election Phase
            local_cluster_state.heartbeat_ticks_elapsed = 0;
            trigger_kernel_election_campaign();
        }
    }
}

void trigger_kernel_election_campaign(void) {
    local_cluster_state.role = ROLE_CANDIDATE;
    local_cluster_state.current_term++;
    local_cluster_state.voted_for = local_cluster_state.node_id; // Vote for self

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
    e1000_transmit_packet(&vote_req, sizeof(struct DSPPFullPagePacket));
}

// Processing interface extending our existing 'handle_network_rx_interrupt_packet' handler
void process_consensus_packet(struct DSPPFullPagePacket* packet) {
    struct ConsensusMessage* msg = (struct ConsensusMessage*)packet->payload_4kb;

    if (packet->header.opcode == DSPP_CMD_HEARTBEAT) {
        // Reset local watch dogs
        if (packet->header.transaction_id >= local_cluster_state.current_term) {
            local_cluster_state.current_term = packet->header.transaction_id;
            local_cluster_state.role = ROLE_FOLLOWER;
            local_cluster_state.heartbeat_ticks_elapsed = 0;
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
            reply_msg->vote_granted = 1; // Approve candidate
        } else {
            reply_msg->vote_granted = 0; // Deny candidate
        }

        e1000_transmit_packet(&reply, sizeof(struct DSPPFullPagePacket));
    }

    else if (packet->header.opcode == DSPP_CMD_VOTE_REPLY && local_cluster_state.role == ROLE_CANDIDATE) {
        static uint32_t accumulated_votes = 1; // Start with own vote

        if (msg->term == local_cluster_state.current_term && msg->vote_granted) {
            accumulated_votes++;

            if (accumulated_votes >= local_cluster_state.stable_quorum_threshold) {
                // QUORUM ACHIEVED: Promote node safely to Leader status
                local_cluster_state.role = ROLE_LEADER;
                accumulated_votes = 1;

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
    row->heartbeat_ticks_elapsed = 0;
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

void partition_lease_trigger_election(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) {
        if (partition_lease_init(partition_id) != 0) return;   /* table full -- nothing to campaign with */
        row = find_lease_row(partition_id);
    }

    row->role              = ROLE_CANDIDATE;
    row->term++;
    row->voted_for          = local_cluster_state.node_id;   /* vote for self */
    row->accumulated_votes = 1;                                /* fresh election, own vote counted */

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
    e1000_transmit_packet(&vote_req, sizeof(struct DSPPFullPagePacket));
}

void partition_lease_heartbeat_tick(uint32_t partition_id) {
    struct PartitionLease* row = find_lease_row(partition_id);
    if (!row) return;   /* no lease established for this partition yet -- nothing to tick */

    if (row->role == ROLE_LEADER) {
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

        e1000_transmit_packet(&hb_packet, sizeof(struct DSPPFullPagePacket));
    } else {
        // FOLLOWER/CANDIDATE for this partition: track silence, same
        // 150-tick threshold Phase 1's cluster-wide mechanism uses.
        row->heartbeat_ticks_elapsed++;

        if (row->heartbeat_ticks_elapsed > 150) {
            row->heartbeat_ticks_elapsed = 0;
            partition_lease_trigger_election(partition_id);
        }
    }
}

void check_partition_lease_heartbeat_tick(void) {
    for (uint32_t i = 0; i < PARTITION_LEASE_MAX; i++) {
        if (partition_lease_table[i].active)
            partition_lease_heartbeat_tick(partition_lease_table[i].partition_id);
    }
}

void process_partition_consensus_packet(struct DSPPFullPagePacket* packet) {
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
            row->term                    = msg->term;
            row->role                    = ROLE_FOLLOWER;
            row->heartbeat_ticks_elapsed = 0;
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
            reply_msg->vote_granted = 1;   // Approve candidate
        } else {
            reply_msg->vote_granted = 0;   // Deny candidate
        }

        e1000_transmit_packet(&reply, sizeof(struct DSPPFullPagePacket));
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
