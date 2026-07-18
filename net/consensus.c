#include "consensus.h"
#include "dspp.h"

#include "e1000.h"
extern void update_page_table_permissions_globally(uint32_t force_read_only);

// Executed every 10ms by the kernel timer interrupt handler on Core 3
void check_consensus_heartbeat_tick(void) {
    if (local_cluster_state.role == ROLE_LEADER) {
        // LEADER: Broadcast periodic heartbeats to maintain authority
        struct DSPPPacketHeader hb_packet;
        hb_packet.magic = DSPP_MAGIC;
        hb_packet.opcode = DSPP_CMD_HEARTBEAT;
        hb_packet.node_source_id = 1;
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
    local_cluster_state.voted_for = 1; // Vote for self
    
    // Split-Brain Mitigation: Strip local memory pages of write authorizations instantly
    // Restricts the local node to safe, non-mutating read operations while split
    update_page_table_permissions_globally(1); // 1 = Force Read-Only across all objects

    struct DSPPFullPagePacket vote_req;
    vote_req.header.magic = DSPP_MAGIC;
    vote_req.header.opcode = DSPP_CMD_REQUEST_VOTE;
    vote_req.header.node_source_id = 1;
    vote_req.header.transaction_id = local_cluster_state.current_term;

    struct ConsensusMessage* msg = (struct ConsensusMessage*)vote_req.payload_4kb;
    msg->term = local_cluster_state.current_term;
    msg->candidate_id = 1;

    kernel_serial_printf("[CONSENSUS] Terms timeout. Campaigning for Term election: %d\n", local_cluster_state.current_term);
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
        reply.header.node_source_id = 1;
        
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
                kernel_serial_printf("[CONSENSUS] Quorum stable. Node 1 elected LEADER for term %d.\n", local_cluster_state.current_term);
            }
        }
    }
}