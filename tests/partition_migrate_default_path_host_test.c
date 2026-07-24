/*
 * partition_migrate_default_path_host_test.c -- Multi-Node Partition
 * Scaling Roadmap Phase 7 (real cross-node data movement) verification: a
 * standalone host-buildable test proving kernel/partition.c's partition_
 * migrate() keeps its EXACT pre-Phase-7 behavior by default, linked
 * against the REAL, unmodified kernel/partition.c -- not a reimplementation.
 *
 * Phase 7 made partition_migrate() branch between two stream-moving
 * primitives based on cluster_local_node_id(): the OLD, pre-Phase-7
 * stream_relocate_partition() (same-disk relocate) when no real cluster is
 * configured, or the NEW stream_migrate_send_partition() (real DSPP-wire
 * push) once one is. tests/partition_migrate_phase6_host_test.c's own
 * Scenario 6 proves the NEW path is taken once a cluster is configured --
 * but that file calls cluster_init(1) once at the very top of main() and
 * never resets it, so it structurally cannot also prove the OLD path
 * still works, since cluster_local_node_id() reads back nonzero for that
 * entire process's lifetime. Proving "the default, single-node posture
 * every pre-Phase-7 deployment and all 58 pre-Phase-7 host tests actually
 * have is completely unchanged" genuinely requires a FRESH process where
 * cluster_init() is never called at all -- this file is that process.
 * cluster_local_node_id() is stubbed to always return 0 (rather than
 * linking the real net/consensus.c and simply never calling cluster_
 * init()) purely to keep this file's dependency graph minimal, matching
 * tests/migration_data_movement_host_test.c's own identical choice for
 * the identical reason (that file's own header comment: "net/consensus.c
 * is deliberately NOT linked, since partition_migrate() only needs this
 * one function's return value, not real lease semantics").
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel -I drivers -I net \
 *       tests/partition_migrate_default_path_host_test.c kernel/partition.c \
 *       -o /tmp/partition_migrate_default_path_host_test
 *   /tmp/partition_migrate_default_path_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "kernel/partition.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else          { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Stubs for kernel/partition.c's dependencies -- identical set to
 * tests/migration_data_movement_host_test.c's own choices, for the
 * identical reasons (see that file's header comment). ────────────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }

/* The one deliberately-fixed value this entire test hinges on: cluster_
 * local_node_id() always reads back 0 (Phase 1's own "uninitialized"
 * sentinel) because cluster_init() is never called anywhere in this file
 * -- the exact posture every real single-node deployment and every
 * pre-Phase-7 host test has today. */
uint32_t cluster_local_node_id(void) { return 0; }

/* Call-counting stubs -- what this whole file exists to check. */
static int relocate_calls = 0;
static int migrate_send_calls = 0;
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) {
    (void)partition_id; (void)dest_node_id; relocate_calls++; return 0;
}
int stream_migrate_send_partition(uint32_t partition_id, uint32_t dest_node_id) {
    (void)partition_id; (void)dest_node_id; migrate_send_calls++; return 0;
}

int main(void) {
    partition_init();
    CHECK(cluster_local_node_id() == 0, "setup: no cluster configured in this process -- cluster_init() is never called");

    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    CHECK(pid_a != 0xFFFFFFFFu && pid_b != 0xFFFFFFFFu, "two partitions created for this test");

    /* ── The core proof: migrating with no real cluster configured takes
     * the OLD same-disk relocate path, never the new wire-transport one. ── */
    int rc = partition_migrate(pid_a, 42);
    CHECK(rc == 0, "partition_migrate(tenant-a, node 42) succeeds with no cluster configured");
    CHECK(relocate_calls == 1, "the OLD stream_relocate_partition() path was taken exactly once");
    CHECK(migrate_send_calls == 0, "the NEW stream_migrate_send_partition() path was NOT taken -- no real cluster means nowhere else to send the bytes, same as every pre-Phase-7 deployment");
    CHECK(partition_get_owner_node(pid_a) == 42, "ownership still genuinely moved to node 42 -- Phase 7 didn't touch this part of the orchestration");

    /* ── A second migration confirms this isn't a one-off -- the default
     * path holds consistently, not just on the first call. ─────────────── */
    rc = partition_migrate(pid_b, 7);
    CHECK(rc == 0, "partition_migrate(tenant-b, node 7) also succeeds with no cluster configured");
    CHECK(relocate_calls == 2, "the OLD path was taken a second time for tenant-b");
    CHECK(migrate_send_calls == 0, "the NEW path remains untaken -- no cluster was configured at any point in this process's lifetime");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
