/*
 * partition_migrate_phase6_host_test.c — Multi-Node Partition Scaling
 * Roadmap Phase 6: a standalone host-buildable test for kernel/partition.c's
 * new partition_migrate() orchestration and net/consensus.c's new
 * partition_lease_step_down(), linked against the REAL, unmodified
 * kernel/partition.c and net/consensus.c — not a reimplementation of
 * either. Reuses the exact same three-file dependency graph Phase 5's
 * dspp_phase5_host_test.c already established (kernel/partition.c +
 * net/consensus.c, same stub set) since partition_migrate() itself calls
 * straight into Phase 4's real partition_lease_step_down()/partition_
 * holds_write_lease() and Phase 2's real partition_set_owner_node()/
 * partition_is_local() — proving real cross-phase orchestration, not three
 * isolated units.
 *
 * partition_reclaim_all_frames() (Phase 3, frame_pool.c) is stubbed as a
 * call-tracking counter here rather than linked for real — the same
 * technique tests/scheduler_fairness_host_test.c already used to verify
 * partition_destroy()'s own multi-step orchestration (order + arguments,
 * not frame_pool.c's real per-frame bitmap logic, which already has its
 * own dedicated coverage in tests/frame_quota_host_test.c). The roadmap
 * doc's own Phase 6 verification plan explicitly names this as the
 * intended technique for this phase's test.
 *
 * net/dspp.h is included for struct DSPPFullPagePacket's full definition
 * (consensus.h only forward-declares it) -- net/dspp.c itself is NOT linked
 * below, since nothing here calls into any of its functions.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel -I drivers -I net \
 *       tests/partition_migrate_phase6_host_test.c kernel/partition.c net/consensus.c \
 *       -o /tmp/partition_migrate_phase6_host_test
 *   /tmp/partition_migrate_phase6_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "kernel/partition.h"
#include "net/consensus.h"
#include "net/dspp.h"   /* full struct DSPPFullPagePacket definition -- consensus.h only forward-declares it */

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else          { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Stubs for kernel/partition.c's dependencies (Phase 8/10/14 precedent,
 * identical set dspp_phase5_host_test.c already uses) ──────────────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) {}
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }

/* Phase 3 frame reclamation -- call-tracking stub, the scheduler_fairness_
 * host_test.c technique the roadmap's own Phase 6 verification plan names. */
static int      g_reclaim_calls = 0;
static uint32_t g_reclaim_last_arg = 0xFFFFFFFFu;
uint32_t partition_reclaim_all_frames(uint32_t partition_id) {
    g_reclaim_calls++;
    g_reclaim_last_arg = partition_id;
    return 7;   /* arbitrary nonzero "frames freed" so the test can also check it's surfaced */
}

/* ─── Stubs for net/consensus.c's dependencies (Phase 1/4 precedent) ──── */
void update_page_table_permissions_globally(uint32_t force_read_only) { (void)force_read_only; }
void update_page_table_permissions_for_partition(uint32_t partition_id, uint32_t force_read_only) {
    (void)partition_id; (void)force_read_only;
}
static int transmit_call_count = 0;
void e1000_transmit_packet(void* buf, uint16_t size) { (void)buf; (void)size; transmit_call_count++; }

int main(void) {
    partition_init();
    cluster_init(1);   /* this node is node 1, empty roster, quorum=1 (self only) */

    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    CHECK(pid_a != 0xFFFFFFFFu && pid_b != 0xFFFFFFFFu, "two partitions created for this test");
    CHECK(partition_get_owner_node(pid_a) == 1, "tenant-a starts out owned by this node (1), the default at creation");

    /* ── Scenario 1: validation guards, each a no-op (no state changed) ── */
    CHECK(partition_migrate(PARTITION_SYSTEM, 2) == 1, "migrating PARTITION_SYSTEM is rejected");
    CHECK(partition_migrate(99, 2) == 1, "migrating an unknown/inactive partition id is rejected");
    CHECK(partition_migrate(pid_a, 0) == 1, "migrating to node 0 (the reserved sentinel) is rejected");
    CHECK(partition_migrate(pid_a, 1) == 1, "migrating to the CURRENT owner (1) is rejected as a no-op");
    CHECK(partition_get_owner_node(pid_a) == 1, "none of the rejected calls above changed tenant-a's ownership");
    CHECK(partition_is_paused(pid_a) == 0, "none of the rejected calls above paused tenant-a either -- true no-ops");
    CHECK(g_reclaim_calls == 0, "no frames were reclaimed by any of the rejected validation calls");

    /* ── Scenario 2: partition_lease_step_down() in isolation, before any
     * migration -- covers both its "nothing to step down" and "real
     * step-down" outcomes directly, not just through partition_migrate(). ── */
    CHECK(partition_lease_step_down(pid_b) == 1, "stepping down a partition with no lease row at all returns 1 (nothing to relinquish)");
    partition_lease_trigger_election(pid_a);
    {
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic  = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_PARTITION_VOTE_REPLY;
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->partition_id = pid_a;
        m->term         = partition_lease_get_term(pid_a);
        m->vote_granted = 1;
        process_partition_consensus_packet(&incoming);
    }
    CHECK(partition_lease_get_role(pid_a) == ROLE_LEADER, "tenant-a's lease reached quorum (1) and is now LEADER");
    CHECK(partition_holds_write_lease(pid_a) == 1, "tenant-a genuinely holds its own write lease before migration");
    CHECK(partition_lease_step_down(pid_a) == 0, "stepping down a partition that WAS LEADER returns 0 (really relinquished)");
    CHECK(partition_lease_get_role(pid_a) == ROLE_FOLLOWER, "tenant-a's role is FOLLOWER immediately after stepping down");
    CHECK(partition_holds_write_lease(pid_a) == 0, "tenant-a no longer holds its write lease after stepping down");
    /* Re-win it for real, so Scenario 3's migrate() call below has a real
     * lease to relinquish as part of the orchestration, not an already-
     * stepped-down no-op -- proves partition_migrate() itself drives this,
     * not that the lease just happened to already be down. */
    partition_lease_trigger_election(pid_a);
    {
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic  = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_PARTITION_VOTE_REPLY;
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->partition_id = pid_a;
        m->term         = partition_lease_get_term(pid_a);
        m->vote_granted = 1;
        process_partition_consensus_packet(&incoming);
    }
    CHECK(partition_holds_write_lease(pid_a) == 1, "tenant-a holds its write lease again, ready for a real migration test");

    /* ── Scenario 3: a real, successful migration -- the whole orchestration,
     * end to end. ────────────────────────────────────────────────────────── */
    int rc = partition_migrate(pid_a, 42);
    CHECK(rc == 0, "partition_migrate(tenant-a, node 42) succeeds");
    CHECK(partition_get_owner_node(pid_a) == 42, "tenant-a's ownership (Phase 2) genuinely moved to node 42");
    CHECK(partition_is_local(pid_a) == 0, "tenant-a no longer reads as local on this node (node 1) after migration");
    CHECK(partition_is_paused(pid_a) == 1, "tenant-a is left PAUSED after migration -- resume must happen on the destination, by design");
    CHECK(partition_holds_write_lease(pid_a) == 0, "tenant-a's write lease was relinquished (Phase 4 step-down) as part of the migration");
    CHECK(partition_lease_get_role(pid_a) == ROLE_FOLLOWER, "tenant-a's lease role is FOLLOWER post-migration, not still LEADER");
    CHECK(g_reclaim_calls == 1, "partition_reclaim_all_frames() (Phase 3) was called exactly once by this migration");
    CHECK(g_reclaim_last_arg == pid_a, "the frame reclamation call's argument was tenant-a's real partition id, not some other value");

    /* ── Scenario 4: migrating a partition that was NEVER leased at all --
     * proves the lease-step-down step doesn't block migration just because
     * there was nothing to relinquish (Scenario 1 already showed step_down
     * alone returns 1 for this; migrate() must still succeed end to end). ── */
    int reclaim_calls_before = g_reclaim_calls;
    rc = partition_migrate(pid_b, 42);
    CHECK(rc == 0, "migrating tenant-b (never leased) still succeeds");
    CHECK(partition_get_owner_node(pid_b) == 42, "tenant-b's ownership moved to node 42 despite never having a lease row");
    CHECK(partition_is_paused(pid_b) == 1, "tenant-b is also left paused post-migration");
    CHECK(g_reclaim_calls == reclaim_calls_before + 1, "frame reclamation still ran exactly once more for tenant-b's migration");
    CHECK(g_reclaim_last_arg == pid_b, "and was called with tenant-b's id specifically, not tenant-a's stale id");

    /* ── Scenario 5: transmitted-packet count sanity check -- migrate()
     * itself never transmits anything (partition_lease_step_down() is
     * explicitly local-only, per its own header comment); every real
     * transmission counted here came from the two REQUEST_VOTE broadcasts
     * in Scenario 2's elections, not from either migration. ─────────────── */
    CHECK(transmit_call_count == 2, "exactly 2 real packets were transmitted total, both from Scenario 2's two elections -- partition_migrate() itself transmits nothing");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
