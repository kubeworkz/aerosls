#include "consensus.h"
#include "dspp.h"

#include "e1000.h"
#include "kernel_io.h"

extern void update_page_table_permissions_globally(uint32_t force_read_only);

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
