/*
 * partition_conflict_host_test.c — verification of the partition-sync
 * ownership-conflict resolution added to partition_sync_upsert()
 * (kernel/partition.c): when two nodes claim one partition id, the row
 * must resolve deterministically in the failover-safe direction, not by
 * announce arrival order.
 *
 * Why this matters: the old rule was "last announce wins" (mirroring the
 * duplicate-service-name rule). That was safe for concurrent creators but
 * FATAL for failover — a resurrected owner restores its stale row from
 * disk at boot, re-announces it, and "last wins" hands the adopted
 * partition back to the node that lost it, flapping the ownership the
 * adoption just settled. This test pins the three claim classes:
 *
 *   * source == current owner           -> owner-initiated transfer
 *     (partition_migrate()): apply.
 *   * source == owner == cluster leader -> the leader's own claim (the
 *     failover adoption announce, or a leader re-assert): apply — the
 *     leader's view is authoritative.
 *   * anything else (a non-owner, non-leader self-claim colliding with a
 *     live owner — the resurrected-owner shape): REJECT, keep the live
 *     owner, transmit nothing.
 *
 * The reject path must be silent on the wire: the stale claim must never
 * produce a "learned" announce of its own, or the flap would simply move
 * to the next hop. The announce counter below asserts that.
 *
 * Linked against the REAL, unmodified kernel/partition.c — not a
 * reimplementation. The externs partition.c touches (cluster_local_node_id,
 * cluster_leader_id, dspp_partition_announce, persist_partitions, the
 * kernel_io loggers, and the destroy/migrate/lease helpers this test never
 * calls) are stubbed settable/counting the same way persist_partition_host_
 * test.c already does; the two consensus accessors this test IS about are
 * the settable kind so every conflict class can be driven.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/partition_conflict_host_test \
 *       tests/partition_conflict_host_test.c kernel/partition.c
 *   /tmp/partition_conflict_host_test
 */
#include "kernel/partition.h"
#include "kernel/kernel_io.h"
#include "kernel/persist.h"
#include "kernel/process.h"
#include "kernel/object_catalog.h"
#include "kernel/frame_pool.h"
#include "kernel/stream.h"
#include "kernel/simi_ctx_migrate.h"
#include "kernel/service_registry.h"
#include "../net/consensus.h"
#include "../net/dspp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Settable consensus identity: the two accessors this test drives ── */
static uint32_t g_fake_local_node_id = 0;
static uint32_t g_fake_leader_id     = 0;
uint32_t cluster_local_node_id(void)          { return g_fake_local_node_id; }
uint32_t cluster_leader_id(void)              { return g_fake_leader_id; }

/* ─── Announce capture: proves the reject path transmits nothing ──────── */
static int g_announce_count = 0;
static uint32_t g_last_announce_owner = 0;
void dspp_partition_announce(uint32_t partition_id, const char* name,
                             uint32_t owner_node_id) {
    (void)partition_id; (void)name;
    g_announce_count++;
    g_last_announce_owner = owner_node_id;
}
void dspp_partition_withdraw(uint32_t partition_id) { (void)partition_id; }
void dspp_partition_ownedset_send(uint32_t generation, const uint32_t* ids,
                                  uint32_t count) { (void)generation; (void)ids; (void)count; }

/* ─── Stubs for partition.c's other externs (never exercised here) ────── */
void kernel_serial_print(const char* s)    { (void)s; }
void kernel_serial_printf(const char* f, ...) { (void)f; }
void persist_partitions(void)              { }
void persist_partition_flush(void)         { }
uint32_t process_kill_partition(uint32_t id)            { (void)id; return 0; }
uint32_t catalog_vfree_partition(uint32_t id)           { (void)id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t id)      { (void)id; return 0; }
int stream_relocate_partition(uint32_t id, uint32_t d)  { (void)id; (void)d; return 0; }
int stream_count_for_partition(uint32_t id)             { (void)id; return 0; }
int stream_migrate_send_partition(uint32_t id, uint32_t d) { (void)id; (void)d; return 0; }
int partition_lease_step_down(uint32_t id)              { (void)id; return 1; }
uint32_t service_unregister_partition(uint32_t id)      { (void)id; return 0; }
uint32_t simi_ctx_migrate_send_partition(uint32_t id, uint32_t d) { (void)id; (void)d; return 0; }
int partition_holds_write_lease(uint32_t id)            { (void)id; return 0; }

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* A cluster of three nodes: leader 1, follower 2, observer 3. The
     * row is created on node 1 (the initial leader). */
    g_fake_local_node_id = 1;
    g_fake_leader_id     = 1;
    partition_init();
    uint32_t pid = partition_create("conflict-a");
    CHECK(pid != 0xFFFFFFFFu && partition_exists(pid), "partition created and active");

    /* Baseline: the create announce was sent once, owner = creator. */
    CHECK(g_announce_count == 1 && g_last_announce_owner == 1,
          "create announce sent once with owner node 1");

    /* ─── 1. the failover ADOPTION announce: leader claims the row ────── */
    /* The leader (1) dies; node 2 is elected and adopts the row. The
     * adoption announce is source 2, owner 2, and 2 is now the leader.
     * This must APPLY (the leader's view is authoritative). Observed
     * from node 3, which still holds the stale owner 1. */
    g_fake_leader_id = 2;
    g_fake_local_node_id = 3;                 /* the observer */
    g_announce_count = 0;
    partition_sync_upsert(pid, 2, "conflict-a", 2);
    CHECK(partition_get_owner_node(pid) == 2,
          "adoption: leader node 2's claim wins over stale owner 1");
    CHECK(g_announce_count == 0,
          "adoption: RX apply transmits nothing (no echo announce)");

    /* ─── 2. the RESURRECTED OWNER: the dead leader comes back ────────── */
    /* Node 1 restores its stale row from disk and re-announces owner 1.
     * It is a FOLLOWER now (leader id is 2). The claim must be REJECTED:
     * the adopted partition must not flap back. The owner stays 2 and
     * NOTHING is transmitted. */
    g_announce_count = 0;
    partition_sync_upsert(pid, 1, "conflict-a", 1);
    CHECK(partition_get_owner_node(pid) == 2,
          "resurrected owner: stale claim rejected, owner stays the adopter (2)");
    CHECK(g_announce_count == 0,
          "resurrected owner: reject path transmits nothing (no flap hop)");

    /* ─── 3. the leader's own re-assert of a row it owns ──────────────── */
    /* Node 2 re-announces its OWN row (periodic re-announce): cur ==
     * owner, so no conflict at all; a no-op apply, still silent. */
    g_announce_count = 0;
    partition_sync_upsert(pid, 2, "conflict-a", 2);
    CHECK(partition_get_owner_node(pid) == 2,
          "leader re-assert of its own row: applied as a plain re-assert");
    CHECK(g_announce_count == 0, "re-assert: no echo announce");

    /* ─── 4. owner-initiated TRANSFER (partition_migrate) ─────────────── */
    /* The current owner (2) migrates the row to node 4: announce source
     * 2, owner 4. source == cur, so it must apply. */
    g_announce_count = 0;
    partition_sync_upsert(pid, 4, "conflict-a", 2);
    CHECK(partition_get_owner_node(pid) == 4,
          "owner-initiated transfer: source == current owner, applied");
    CHECK(g_announce_count == 0, "transfer: RX apply transmits nothing");

    /* ─── 5. a concurrent creator (non-leader, non-owner) ─────────────── */
    /* Node 9 claims the row it just "created" locally, colliding with the
     * live owner 4 while the leader is 2. Not the owner, not the leader:
     * rejected, deterministic first-wins instead of last-wins. */
    g_announce_count = 0;
    partition_sync_upsert(pid, 9, "conflict-a", 9);
    CHECK(partition_get_owner_node(pid) == 4,
          "concurrent creator: non-leader self-claim rejected, owner stays 4");
    CHECK(g_announce_count == 0, "concurrent creator: reject path silent");

    /* ─── 6. adoption claim when the leader is UNKNOWN at the receiver ── */
    /* A receiver that has not yet heard the new leader's heartbeat sees
     * leader id 0; the conservative outcome is to reject rather than
     * trust an unproven claim (the kernel orders the adoption well after
     * the first heartbeat, so this is defensive, not the happy path). */
    g_fake_leader_id = 0;
    g_announce_count = 0;
    partition_sync_upsert(pid, 8, "conflict-a", 8);
    CHECK(partition_get_owner_node(pid) == 4,
          "unknown leader: unproven claim rejected, owner stays 4");
    g_fake_leader_id = 2;

    /* ─── 7. a fresh learn (no local row) is unaffected ───────────────── */
    g_announce_count = 0;
    partition_sync_upsert(pid + 1, 3, "fresh-b", 3);
    CHECK(partition_get_owner_node(pid + 1) == 3,
          "fresh learn with no local row applies unconditionally");

    if (g_failures == 0) {
        printf("\npartition_conflict_host_test: all checks passed\n");
        return 0;
    }
    printf("\npartition_conflict_host_test: %d check(s) FAILED\n", g_failures);
    return 1;
}
