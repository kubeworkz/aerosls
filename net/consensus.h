#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>

enum NodeRole {
    ROLE_FOLLOWER,
    ROLE_CANDIDATE,
    ROLE_LEADER
};

struct ClusterNode {
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

static struct ClusterNode local_cluster_state = { .current_term = 0, .voted_for = 0, .role = ROLE_FOLLOWER, .active_nodes_count = 3, .stable_quorum_threshold = 2 };

#endif