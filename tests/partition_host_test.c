/*
 * partition_host_test.c — Phase 8 verification: a standalone host-buildable
 * unit test for kernel/partition.c's core logic (partition_create/
 * partition_assign_uid/partition_get_for_uid), compiled and linked directly
 * against the REAL, unmodified kernel/partition.c — not a reimplementation.
 *
 * As of Phase 8, kernel/partition.c had exactly one external dependency
 * (kernel_io.h's kernel_serial_print/kernel_serial_printf). Phase 10 added
 * a persist_partitions() call, and Phase 14 added partition_destroy()'s
 * three cross-subsystem calls (process_kill_partition(),
 * catalog_vfree_partition(), partition_reset_frame_usage()) — none of
 * which this test exercises, so they're stubbed the same way
 * persist_partition_host_test.c and ipc_partition_host_test.c already
 * stub them. Still cheap enough to link and actually EXECUTE, giving
 * stronger verification than the compile-check-only precedent every prior
 * phase's kernel-side wiring has settled for (see
 * AeroSLS-SIMI-ISA-v0.1.md §15 for the write-up).
 *
 * Multi-Node Partition Scaling Roadmap Phase 2 added partition_get_owner_
 * node()/partition_set_owner_node()/partition_is_local(), plus ownership-
 * table stamping inside partition_create()/partition_init()/partition_
 * destroy(). Rather than link the real net/consensus.c just to drive
 * cluster_local_node_id() (a much heavier dependency for a partition-
 * focused test), this file stubs it as a *settable* fake — g_fake_local_
 * node_id below — so partition_is_local()'s real comparison logic still
 * executes against multiple real node-id values, not just the single
 * fixed 0 every other partition.c-linking host test stubs it to.
 *
 * Not part of any Makefile target — a one-off verification artifact, built
 * and run directly:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/partition_host_test \
 *       tests/partition_host_test.c kernel/partition.c
 *   /tmp/partition_host_test
 */
#include "kernel/partition.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ─── Stubs for partition.c's external dependencies (see header comment) ── */
void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { /* Phase 10's persistence hook — irrelevant here */ }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }   /* Multi-Node Partition Scaling Roadmap Phase 3 -- replaces partition_reset_frame_usage() at partition_destroy()'s call site */
static uint32_t g_fake_local_node_id = 0;   /* Multi-Node Partition Scaling Roadmap Phase 2 — settable */
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }   /* Multi-Node Partition Scaling Roadmap Phase 6 -- not exercised by this test, safe "nothing to relinquish" stub */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    partition_init();

    /* ── Defaults, no assignments made yet ────────────────────────────── */
    CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM, "uid 0 -> PARTITION_SYSTEM");
    CHECK(partition_get_for_uid(999) == PARTITION_DEFAULT, "unassigned uid -> PARTITION_DEFAULT");
    CHECK(PARTITION_SYSTEM == PARTITION_DEFAULT, "SYSTEM and DEFAULT are the same value (backward-compat invariant)");

    /* ── Creating partitions ──────────────────────────────────────────── */
    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    CHECK(pid_a != 0xFFFFFFFFu, "partition_create('tenant-a') succeeds");
    CHECK(pid_b != 0xFFFFFFFFu, "partition_create('tenant-b') succeeds");
    CHECK(pid_a != pid_b, "two created partitions get distinct ids");
    CHECK(pid_a != PARTITION_SYSTEM && pid_b != PARTITION_SYSTEM,
          "created partitions never collide with PARTITION_SYSTEM (slot 0 reserved)");

    /* ── Assignment: insert, lookup, update in place ──────────────────── */
    CHECK(partition_assign_uid(42, pid_a) == 0, "assign uid 42 -> tenant-a succeeds");
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42 now resolves to tenant-a");
    CHECK(partition_assign_uid(42, pid_b) == 0, "reassign uid 42 -> tenant-b succeeds (update in place)");
    CHECK(partition_get_for_uid(42) == pid_b, "uid 42 now resolves to tenant-b, not stuck on tenant-a");

    /* ── Multi-Node Partition Scaling Roadmap Phase 2: ownership & pinning ── */
    CHECK(partition_get_owner_node(PARTITION_SYSTEM) == 0,
          "PARTITION_SYSTEM's owner defaults to node 0 (cluster_local_node_id() stub starts at 0)");
    CHECK(partition_get_owner_node(pid_a) == 0,
          "newly created tenant-a's owner also defaults to node 0 (partition_create() stamps cluster_local_node_id() at create time)");
    CHECK(partition_is_local(pid_a) == 1,
          "tenant-a reads as local: owner(0) == cluster_local_node_id()(0), the honest single-node default");

    g_fake_local_node_id = 5;
    CHECK(partition_is_local(pid_a) == 0,
          "tenant-a no longer reads as local once cluster_local_node_id() reports a different node (5) than its owner (0)");
    CHECK(partition_set_owner_node(pid_a, 5) == 0, "explicitly pin tenant-a to node 5 succeeds");
    CHECK(partition_get_owner_node(pid_a) == 5, "tenant-a's owner now reads back as node 5 (update-existing-row branch)");
    CHECK(partition_is_local(pid_a) == 1,
          "tenant-a reads as local again now that its owner(5) matches cluster_local_node_id()(5)");

    g_fake_local_node_id = 0;
    CHECK(partition_is_local(pid_a) == 0,
          "tenant-a reads as remote once cluster_local_node_id() flips back to 0 while its owner stays pinned at 5 — proves a real re-read, not a cached/stale comparison");
    CHECK(partition_set_owner_node(pid_a, 0) == 0,
          "un-pin tenant-a back to node 0 succeeds (still the update-existing-row branch, not a duplicate insert)");
    CHECK(partition_is_local(pid_a) == 1, "tenant-a local again after un-pin");
    CHECK(partition_get_owner_node(pid_b) == 0,
          "tenant-b's owner untouched by any of tenant-a's pinning above (rows are independent)");

    CHECK(partition_get_owner_node(0xDEADBEEF) == 0,
          "owner lookup on an undefined partition id returns 0 (no row found), not garbage");
    CHECK(partition_set_owner_node(0xDEADBEEF, 5) != 0,
          "cannot pin an undefined partition id — rejected by the same partition_id_valid() gate every other mutator uses");

    /* ── partition_destroy() must clear the owner row, not just table/assign
     * rows — a slot reused by a later partition_create() must not inherit a
     * stale owner from whatever partition previously held that id. ─────── */
    uint32_t pid_c = partition_create("tenant-c");
    CHECK(pid_c != 0xFFFFFFFFu, "tenant-c created for the destroy/reuse check");
    CHECK(partition_set_owner_node(pid_c, 7) == 0, "tenant-c pinned to node 7");
    CHECK(partition_destroy(pid_c) == 0, "tenant-c destroyed");
    CHECK(partition_get_owner_node(pid_c) == 0,
          "after destroy, the old slot's owner lookup reads back 0 (row deactivated), not the stale node 7");
    uint32_t pid_c2 = partition_create("tenant-c-reborn");
    CHECK(pid_c2 == pid_c, "the freed slot is reused by the next partition_create() (same id as tenant-c)");
    CHECK(partition_get_owner_node(pid_c2) == g_fake_local_node_id,
          "the reused slot gets a FRESH owner stamp from partition_create() (current cluster_local_node_id()), not tenant-c's old pin of node 7");

    /* ── Rejections ────────────────────────────────────────────────────── */
    CHECK(partition_assign_uid(0, pid_a) != 0, "cannot reassign uid 0 (kernel) — rejected");
    CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM, "uid 0 still PARTITION_SYSTEM after rejected attempt");
    CHECK(partition_assign_uid(7, 0xDEADBEEF) != 0, "assigning to an undefined partition id is rejected");
    CHECK(partition_get_for_uid(7) == PARTITION_DEFAULT, "uid 7 still unassigned/default after rejected attempt");

    char toolong[PARTITION_NAME_LEN + 8];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK(partition_create(toolong) == 0xFFFFFFFFu, "partition_create rejects an oversized name");

    /* ── Capacity: fill the partition table ───────────────────────────── */
    int created = 0;
    char namebuf[PARTITION_NAME_LEN];
    for (;;) {
        snprintf(namebuf, sizeof(namebuf), "p%d", created);
        uint32_t id = partition_create(namebuf);
        if (id == 0xFFFFFFFFu) break;
        created++;
        if (created > PARTITION_MAX + 4) { printf("FAIL: partition_create never reports full\n"); g_fail++; break; }
    }
    /* slot 0 is PARTITION_SYSTEM; 3 were already created above and still
     * active (tenant-a, tenant-b, tenant-c-reborn — tenant-c's original
     * slot was destroyed then immediately reused, so it's still exactly
     * one active slot, not two); remaining free slots = PARTITION_MAX - 1 - 3 */
    CHECK(created == PARTITION_MAX - 4, "partition table fills to exactly PARTITION_MAX (accounting for system + 3 earlier)");
    CHECK(partition_create("one-too-many") == 0xFFFFFFFFu, "partition_create fails once table is full");

    /* ── Capacity: fill the assignment table, confirm updates still work
     * even when the table is full (only NEW inserts should be blocked) ── */
    int assigned = 0;
    for (uint32_t uid = 1000; uid < 1000 + PARTITION_ASSIGN_MAX + 4; uid++) {
        int rc = partition_assign_uid(uid, pid_a);
        if (rc != 0) break;
        assigned++;
    }
    /* uid 42 already occupies one slot from earlier, so capacity for NEW
     * uids is PARTITION_ASSIGN_MAX - 1 */
    CHECK(assigned == PARTITION_ASSIGN_MAX - 1, "assignment table fills to exactly PARTITION_ASSIGN_MAX (accounting for uid 42's earlier slot)");
    CHECK(partition_assign_uid(42, pid_a) == 0, "updating an EXISTING uid's assignment still succeeds when table is full");
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42's update took effect");
    CHECK(partition_assign_uid(99999, pid_a) != 0, "a genuinely NEW uid is rejected once the assignment table is full");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
