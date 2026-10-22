#ifndef STATE_TREE_H
#define STATE_TREE_H

#include <stdint.h>

/*
 * state_tree.h — hierarchical snapshot of kernel state
 * (Core Backup Strategies, Step 2).
 *
 * Builds a tree representation of the entire system state:
 *   ROOT
 *   └── PARTITION (one per active partition)
 *       ├── OBJECT (catalog entries in this partition)
 *       └── PROCESS (processes assigned to this partition)
 *
 * The tree is a flat array of nodes with parent_id links. Serialization
 * produces a self-contained buffer suitable for NVMe storage or DSPP
 * transfer. Deserialization restores partition/object/process state.
 *
 * The dirty-tracking fields (last_ckpt_seq, dirty) provide the foundation
 * for Step 4 (incremental checkpointing).
 */

// ─── Node Types ───────────────────────────────────────────────────────────────
#define ST_NODE_ROOT       0
#define ST_NODE_PARTITION  1
#define ST_NODE_OBJECT     2
#define ST_NODE_PROCESS    3
#define ST_NODE_STREAM     4
/* POSIX-Environments v0.2 Phase P3 (destination (b), §11 Q5): one node per
 * LIVE environment, attached to its partition, so a survivor adopting the
 * partition can NAME the environments it is not restoring. Identity only —
 * entity_id is the environment's INDEX within its partition (the name the
 * console registry and every control-plane route key it by, kernel/env_ckpt.h)
 * and size is its region count. The payload deliberately does not ride here:
 * this tree is the leader's 24 KiB broadcast and an environment's regions are
 * 5.5 MiB, so carrying the bytes would be a lie about what failover can
 * restore. Carrying the NAME is what turns "the environment vanished" into
 * "environment index 2 of partition 3 was not restored" — the whole of what
 * destination (b) promises. */
#define ST_NODE_ENVIRONMENT 5

// ─── Limits ───────────────────────────────────────────────────────────────────
// Worst case: 1 root + 256 partitions + 128 objects + 16 processes + 8
// streams + 8 environments (ENV_CKPT_MAX)
#define ST_MAX_NODES  418

// Serialized buffer: header + node array. Sized for the worst case.
#define ST_SERIAL_MAX  (sizeof(struct StateTreeHeader) + ST_MAX_NODES * sizeof(struct StateTreeNode))

// ─── Node Struct ──────────────────────────────────────────────────────────────
struct StateTreeNode {
    uint32_t id;            // unique within this tree (1-based)
    uint8_t  type;          // ST_NODE_*
    uint8_t  dirty;         // changed since last checkpoint
    uint16_t _pad;
    uint32_t parent_id;     // 0 = root (no parent)
    uint32_t partition_id;  // which partition this node belongs to
    uint64_t entity_id;     // type-specific: object_id, pid, stream slot, partition_id
    uint64_t size;          // type-specific: size_pages, frame count, etc.
    uint64_t last_ckpt_seq; // sequence number when last checkpointed
};

// ─── Serialization Header ─────────────────────────────────────────────────────
#define ST_MAGIC    0x53545245UL   /* "STRE" */
/* v2 adds ST_NODE_ENVIRONMENT nodes and the num_environments field below.
 * Bumped rather than reusing v1's tail silently, so a v1 tree is REFUSED by
 * name (state_tree_deserialize's version check) instead of being read by a
 * parser that would silently drop the environment names — which is exactly
 * the silent-vanish (b) exists to prevent. Safe to bump: the serialized form
 * crosses only the live cluster's checkpoint broadcast (kernel/failover.c),
 * never NVMe — checkpoint_mgr.c builds a tree in memory and never
 * serializes it. */
#define ST_VERSION  2

struct StateTreeHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t node_count;
    uint32_t node_id;       // cluster node that produced this tree
    uint64_t sequence;      // checkpoint sequence number
    uint64_t timestamp_tsc;
    uint32_t num_partitions;
    uint32_t num_objects;
    uint32_t num_processes;
    uint32_t num_streams;
    uint32_t num_environments;  /* v2: ST_NODE_ENVIRONMENT nodes */
};

// ─── Diff Result ──────────────────────────────────────────────────────────────
struct StateTreeDiff {
    uint32_t changed_count;
    uint32_t added_count;
    uint32_t removed_count;
    uint32_t node_ids[ST_MAX_NODES]; // ids of changed/added nodes
};

// ─── Public API ───────────────────────────────────────────────────────────────

/* Build a state tree from current kernel arrays. Returns node count. */
uint32_t state_tree_build(struct StateTreeNode* nodes, uint32_t max_nodes);

/* Serialize: header + nodes → flat buffer. Returns bytes written. */
uint32_t state_tree_serialize(const struct StateTreeNode* nodes, uint32_t node_count,
                              uint64_t sequence, uint8_t* buf, uint32_t buf_size);

/* Deserialize: flat buffer → header + nodes. Returns node count, 0 on error. */
uint32_t state_tree_deserialize(const uint8_t* buf, uint32_t buf_size,
                                struct StateTreeHeader* hdr_out,
                                struct StateTreeNode* nodes, uint32_t max_nodes);

/* Diff two trees: find nodes present/changed between old and new.
 * Populates diff_out with ids of nodes that are new or changed. */
void state_tree_diff(const struct StateTreeNode* old_nodes, uint32_t old_count,
                     const struct StateTreeNode* new_nodes, uint32_t new_count,
                     struct StateTreeDiff* diff_out);

/* Mark all nodes as clean (last_ckpt_seq = seq). Call after successful checkpoint. */
void state_tree_mark_clean(struct StateTreeNode* nodes, uint32_t count, uint64_t seq);

#endif /* STATE_TREE_H */
