/*
 * state_tree_host_test.c — verification for kernel/state_tree.c
 * (Core Backup Strategies, Step 2).
 *
 * Tests: build from populated kernel arrays, serialize/deserialize round-trip,
 * diff detection (added/changed/removed nodes), and mark_clean.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/state_tree_host_test \
 *       tests/state_tree_host_test.c kernel/state_tree.c
 *   /tmp/state_tree_host_test
 */
#include "kernel/state_tree.h"
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "kernel/process.h"
#include "kernel/stream.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Stubs for kernel globals ─────────────────────────────────────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;

struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];

struct ProcessDescriptor proc_table[PROC_MAX];
struct StreamEntry       stream_store[STREAM_MAX];

uint32_t cluster_local_node_id(void) { return 1; }
static uint64_t g_tsc = 1000;
uint64_t read_tsc(void) { return g_tsc++; }

/* kernel_serial stubs */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── Test harness ─────────────────────────────────────────────────── */
static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { checks_passed++; } \
} while (0)

/* ─── Scenario 1: empty system ─────────────────────────────────────── */
static void test_empty(void) {
    printf("--- Scenario 1: empty system ---\n");
    memset(partition_table, 0, sizeof(partition_table));
    memset(object_catalog, 0, sizeof(object_catalog));
    memset(proc_table, 0, sizeof(proc_table));
    memset(stream_store, 0, sizeof(stream_store));
    object_catalog_count = 0;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    /* Only root node */
    CHECK(count == 1, "empty system has 1 node (root)");
    CHECK(nodes[0].type == ST_NODE_ROOT, "first node is ROOT");
    CHECK(nodes[0].parent_id == 0, "root has no parent");
}

/* ─── Scenario 2: populated system ─────────────────────────────────── */
static void test_populated(void) {
    printf("--- Scenario 2: populated system ---\n");
    memset(partition_table, 0, sizeof(partition_table));
    memset(object_catalog, 0, sizeof(object_catalog));
    memset(proc_table, 0, sizeof(proc_table));
    memset(stream_store, 0, sizeof(stream_store));

    /* 2 partitions */
    partition_table[0].partition_id = 0;
    partition_table[0].active = 1;
    partition_table[1].partition_id = 1;
    partition_table[1].active = 1;

    /* 3 objects: 2 in partition 0, 1 in partition 1 */
    object_catalog[0].object_id = 100; object_catalog[0].active = 1;
    object_catalog[0].partition_id = 0; object_catalog[0].size_pages = 4;
    object_catalog[1].object_id = 200; object_catalog[1].active = 1;
    object_catalog[1].partition_id = 0; object_catalog[1].size_pages = 8;
    object_catalog[2].object_id = 300; object_catalog[2].active = 1;
    object_catalog[2].partition_id = 1; object_catalog[2].size_pages = 2;
    object_catalog_count = 3;

    /* 1 process in partition 1 */
    proc_table[0].pid = 42; proc_table[0].active = 1;
    proc_table[0].partition_id = 1;

    /* 1 stream in partition 0 */
    stream_store[0].active = 1; stream_store[0].partition_id = 0;
    stream_store[0].frames_used = 10;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    /* 1 root + 2 partitions + 3 objects + 1 process + 1 stream = 8 */
    CHECK(count == 8, "populated system has 8 nodes");

    /* Verify partition nodes */
    int part_count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].type == ST_NODE_PARTITION) part_count++;
    CHECK(part_count == 2, "2 partition nodes");

    /* Verify object nodes */
    int obj_count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].type == ST_NODE_OBJECT) obj_count++;
    CHECK(obj_count == 3, "3 object nodes");

    /* Verify process node */
    int proc_count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].type == ST_NODE_PROCESS) proc_count++;
    CHECK(proc_count == 1, "1 process node");

    /* Verify stream node */
    int strm_count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].type == ST_NODE_STREAM) strm_count++;
    CHECK(strm_count == 1, "1 stream node");

    /* Verify parent linkage: object 300 should be child of partition 1 */
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type == ST_NODE_OBJECT && nodes[i].entity_id == 300) {
            /* Find the partition 1 node's id */
            for (uint32_t p = 0; p < count; p++) {
                if (nodes[p].type == ST_NODE_PARTITION && nodes[p].partition_id == 1) {
                    CHECK(nodes[i].parent_id == nodes[p].id,
                          "object 300 is child of partition 1");
                    break;
                }
            }
            break;
        }
    }
}

/* ─── Scenario 3: serialize/deserialize round-trip ─────────────────── */
static void test_serialize(void) {
    printf("--- Scenario 3: serialize/deserialize round-trip ---\n");
    /* Reuse populated state from scenario 2 */
    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    uint8_t buf[ST_SERIAL_MAX];
    uint32_t bytes = state_tree_serialize(nodes, count, 42, buf, sizeof(buf));
    CHECK(bytes > 0, "serialization produces non-zero bytes");
    CHECK(bytes == sizeof(struct StateTreeHeader) + count * sizeof(struct StateTreeNode),
          "serialization size matches expected");

    /* Deserialize */
    struct StateTreeHeader hdr;
    struct StateTreeNode restored[ST_MAX_NODES];
    uint32_t restored_count = state_tree_deserialize(buf, bytes, &hdr, restored, ST_MAX_NODES);

    CHECK(restored_count == count, "deserialized node count matches");
    CHECK(hdr.magic == ST_MAGIC, "header magic correct");
    CHECK(hdr.sequence == 42, "header sequence correct");
    CHECK(hdr.node_id == 1, "header node_id correct");
    CHECK(hdr.num_partitions == 2, "header partition count correct");
    CHECK(hdr.num_objects == 3, "header object count correct");

    /* Compare nodes */
    int match = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].id != restored[i].id ||
            nodes[i].type != restored[i].type ||
            nodes[i].entity_id != restored[i].entity_id) {
            match = 0; break;
        }
    }
    CHECK(match, "all nodes match after round-trip");
}

/* ─── Scenario 4: diff detection ───────────────────────────────────── */
static void test_diff(void) {
    printf("--- Scenario 4: diff detection ---\n");
    /* Build "old" tree */
    struct StateTreeNode old_nodes[ST_MAX_NODES];
    uint32_t old_count = state_tree_build(old_nodes, ST_MAX_NODES);

    /* Modify: add a new object, remove the process */
    object_catalog[3].object_id = 400; object_catalog[3].active = 1;
    object_catalog[3].partition_id = 0; object_catalog[3].size_pages = 16;
    object_catalog_count = 4;
    proc_table[0].active = 0;

    /* Build "new" tree */
    struct StateTreeNode new_nodes[ST_MAX_NODES];
    uint32_t new_count = state_tree_build(new_nodes, ST_MAX_NODES);

    struct StateTreeDiff diff;
    state_tree_diff(old_nodes, old_count, new_nodes, new_count, &diff);

    CHECK(diff.added_count == 1, "diff detects 1 added node (new object)");
    CHECK(diff.removed_count == 1, "diff detects 1 removed node (dead process)");
    CHECK(diff.changed_count >= 1, "diff changed_count includes the addition");
}

/* ─── Scenario 5: mark_clean ───────────────────────────────────────── */
static void test_mark_clean(void) {
    printf("--- Scenario 5: mark_clean ---\n");
    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    /* All start dirty */
    int all_dirty = 1;
    for (uint32_t i = 0; i < count; i++)
        if (!nodes[i].dirty) { all_dirty = 0; break; }
    CHECK(all_dirty, "all nodes start dirty after build");

    state_tree_mark_clean(nodes, count, 99);

    int all_clean = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].dirty || nodes[i].last_ckpt_seq != 99) {
            all_clean = 0; break;
        }
    }
    CHECK(all_clean, "all nodes clean with seq=99 after mark_clean");
}

/* ─── Scenario 6: bad input rejection ──────────────────────────────── */
static void test_reject(void) {
    printf("--- Scenario 6: bad input rejection ---\n");
    CHECK(state_tree_build(NULL, 100) == 0, "NULL nodes returns 0");
    CHECK(state_tree_build((struct StateTreeNode*)1, 0) == 0, "max=0 returns 0");

    uint8_t bad_buf[16] = {0};
    struct StateTreeHeader hdr;
    struct StateTreeNode nodes[4];
    CHECK(state_tree_deserialize(bad_buf, sizeof(bad_buf), &hdr, nodes, 4) == 0,
          "bad magic returns 0");
    CHECK(state_tree_serialize(nodes, 1, 1, bad_buf, 1) == 0,
          "too-small buffer returns 0");
}

int main(void) {
    test_empty();
    test_populated();
    test_serialize();
    test_diff();
    test_mark_clean();
    test_reject();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
