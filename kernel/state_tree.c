/*
 * state_tree.c — hierarchical snapshot of kernel state
 * (Core Backup Strategies, Step 2).
 *
 * Freestanding: local helpers, no libc, per this codebase's convention.
 */
#include "state_tree.h"
#include "object_catalog.h"
#include "partition.h"
#include "process.h"
#include "stream.h"
#include "kernel_io.h"
#include "../kernel/dashboard.h"
#include "../net/consensus.h"

/* ─── Local helpers ──────────────────────────────────────────────────── */
static void st_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void st_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

/* ─── state_tree_build ───────────────────────────────────────────────── */
uint32_t state_tree_build(struct StateTreeNode* nodes, uint32_t max_nodes) {
    if (!nodes || max_nodes < 2) return 0;
    st_memset(nodes, 0, max_nodes * sizeof(struct StateTreeNode));

    uint32_t n = 0;

    /* Root node */
    nodes[n].id           = 1;
    nodes[n].type         = ST_NODE_ROOT;
    nodes[n].parent_id    = 0;
    nodes[n].partition_id = 0;
    nodes[n].entity_id    = 0;
    nodes[n].dirty        = 1;
    n++;

    /* Partition nodes */
    for (int i = 0; i < PARTITION_MAX && n < max_nodes; i++) {
        if (!partition_table[i].active) continue;
        nodes[n].id           = n + 1;
        nodes[n].type         = ST_NODE_PARTITION;
        nodes[n].parent_id    = 1;  /* child of root */
        nodes[n].partition_id = partition_table[i].partition_id;
        nodes[n].entity_id    = partition_table[i].partition_id;
        nodes[n].dirty        = 1;
        n++;
    }

    /* Object nodes — each attached to its owning partition */
    for (uint32_t i = 0; i < CATALOG_MAX_OBJECTS && n < max_nodes; i++) {
        if (!object_catalog[i].active) continue;

        /* Find parent partition node */
        uint32_t parent = 1;  /* fallback to root if partition node not found */
        for (uint32_t p = 1; p < n; p++) {
            if (nodes[p].type == ST_NODE_PARTITION &&
                nodes[p].partition_id == object_catalog[i].partition_id) {
                parent = nodes[p].id;
                break;
            }
        }

        nodes[n].id           = n + 1;
        nodes[n].type         = ST_NODE_OBJECT;
        nodes[n].parent_id    = parent;
        nodes[n].partition_id = object_catalog[i].partition_id;
        nodes[n].entity_id    = object_catalog[i].object_id;
        nodes[n].size         = object_catalog[i].size_pages;
        nodes[n].dirty        = 1;
        n++;
    }

    /* Process nodes — attached to their partition */
    for (int i = 0; i < PROC_MAX && n < max_nodes; i++) {
        if (!proc_table[i].active) continue;

        uint32_t parent = 1;
        for (uint32_t p = 1; p < n; p++) {
            if (nodes[p].type == ST_NODE_PARTITION &&
                nodes[p].partition_id == proc_table[i].partition_id) {
                parent = nodes[p].id;
                break;
            }
        }

        nodes[n].id           = n + 1;
        nodes[n].type         = ST_NODE_PROCESS;
        nodes[n].parent_id    = parent;
        nodes[n].partition_id = proc_table[i].partition_id;
        nodes[n].entity_id    = proc_table[i].pid;
        nodes[n].size         = 0;
        nodes[n].dirty        = 1;
        n++;
    }

    /* Stream nodes — attached to their partition */
    for (int i = 0; i < STREAM_MAX && n < max_nodes; i++) {
        if (!stream_store[i].active) continue;

        uint32_t parent = 1;
        for (uint32_t p = 1; p < n; p++) {
            if (nodes[p].type == ST_NODE_PARTITION &&
                nodes[p].partition_id == stream_store[i].partition_id) {
                parent = nodes[p].id;
                break;
            }
        }

        nodes[n].id           = n + 1;
        nodes[n].type         = ST_NODE_STREAM;
        nodes[n].parent_id    = parent;
        nodes[n].partition_id = stream_store[i].partition_id;
        nodes[n].entity_id    = (uint64_t)i;  /* stream slot index */
        nodes[n].size         = stream_store[i].frames_used;
        nodes[n].dirty        = 1;
        n++;
    }

    return n;
}

/* ─── state_tree_serialize ───────────────────────────────────────────── */
uint32_t state_tree_serialize(const struct StateTreeNode* nodes, uint32_t node_count,
                              uint64_t sequence, uint8_t* buf, uint32_t buf_size) {
    uint32_t needed = (uint32_t)sizeof(struct StateTreeHeader) +
                      node_count * (uint32_t)sizeof(struct StateTreeNode);
    if (!buf || buf_size < needed) return 0;

    struct StateTreeHeader hdr;
    st_memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = ST_MAGIC;
    hdr.version       = ST_VERSION;
    hdr.node_count    = node_count;
    hdr.node_id       = cluster_local_node_id();
    hdr.sequence      = sequence;
    hdr.timestamp_tsc = read_tsc();

    /* Count by type */
    for (uint32_t i = 0; i < node_count; i++) {
        switch (nodes[i].type) {
        case ST_NODE_PARTITION: hdr.num_partitions++; break;
        case ST_NODE_OBJECT:   hdr.num_objects++;    break;
        case ST_NODE_PROCESS:  hdr.num_processes++;  break;
        case ST_NODE_STREAM:   hdr.num_streams++;    break;
        default: break;
        }
    }

    st_memcpy(buf, &hdr, sizeof(hdr));
    st_memcpy(buf + sizeof(hdr), nodes, node_count * sizeof(struct StateTreeNode));

    return needed;
}

/* ─── state_tree_deserialize ─────────────────────────────────────────── */
uint32_t state_tree_deserialize(const uint8_t* buf, uint32_t buf_size,
                                struct StateTreeHeader* hdr_out,
                                struct StateTreeNode* nodes, uint32_t max_nodes) {
    if (!buf || buf_size < sizeof(struct StateTreeHeader)) return 0;

    struct StateTreeHeader hdr;
    st_memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != ST_MAGIC || hdr.version != ST_VERSION) return 0;
    if (hdr.node_count > max_nodes) return 0;

    uint32_t needed = (uint32_t)sizeof(struct StateTreeHeader) +
                      hdr.node_count * (uint32_t)sizeof(struct StateTreeNode);
    if (buf_size < needed) return 0;

    st_memcpy(nodes, buf + sizeof(struct StateTreeHeader),
              hdr.node_count * sizeof(struct StateTreeNode));

    if (hdr_out) *hdr_out = hdr;
    return hdr.node_count;
}

/* ─── state_tree_diff ────────────────────────────────────────────────── */
void state_tree_diff(const struct StateTreeNode* old_nodes, uint32_t old_count,
                     const struct StateTreeNode* new_nodes, uint32_t new_count,
                     struct StateTreeDiff* diff_out) {
    if (!diff_out) return;
    st_memset(diff_out, 0, sizeof(*diff_out));

    for (uint32_t i = 0; i < new_count && diff_out->changed_count < ST_MAX_NODES; i++) {
        /* Look for a matching node in old tree (same type + entity_id) */
        int found = 0;
        for (uint32_t j = 0; j < old_count; j++) {
            if (old_nodes[j].type == new_nodes[i].type &&
                old_nodes[j].entity_id == new_nodes[i].entity_id) {
                found = 1;
                /* Check if anything changed */
                if (old_nodes[j].size != new_nodes[i].size ||
                    old_nodes[j].partition_id != new_nodes[i].partition_id ||
                    old_nodes[j].parent_id != new_nodes[i].parent_id) {
                    diff_out->node_ids[diff_out->changed_count++] = new_nodes[i].id;
                }
                break;
            }
        }
        if (!found) {
            diff_out->node_ids[diff_out->changed_count] = new_nodes[i].id;
            diff_out->changed_count++;
            diff_out->added_count++;
        }
    }

    /* Count removed nodes (in old but not in new) */
    for (uint32_t j = 0; j < old_count; j++) {
        int found = 0;
        for (uint32_t i = 0; i < new_count; i++) {
            if (new_nodes[i].type == old_nodes[j].type &&
                new_nodes[i].entity_id == old_nodes[j].entity_id) {
                found = 1;
                break;
            }
        }
        if (!found) diff_out->removed_count++;
    }
}

/* ─── state_tree_mark_clean ──────────────────────────────────────────── */
void state_tree_mark_clean(struct StateTreeNode* nodes, uint32_t count, uint64_t seq) {
    for (uint32_t i = 0; i < count; i++) {
        nodes[i].dirty = 0;
        nodes[i].last_ckpt_seq = seq;
    }
}
