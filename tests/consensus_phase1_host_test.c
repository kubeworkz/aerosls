/*
 * consensus_phase1_host_test.c — Multi-Node Partition Scaling Roadmap,
 * Phase 1 (real node identity & membership registry) AND Phase 4
 * (partition-scoped consensus leases), extended in place rather than
 * split into a second file -- both phases live in the same net/consensus.c
 * and need the identical stub set (e1000_transmit_packet's call-tracking
 * in particular), the same "reuse the test that already links the right
 * file" precedent this project follows elsewhere (e.g. partition_host_
 * test.c growing across LPAR/Multi-Node phases without a rename).
 *
 * Links the real, unmodified net/consensus.c against small stubs for its
 * extern dependencies (kernel_serial_print/printf, update_page_table_
 * permissions_globally, and Phase 4's update_page_table_permissions_for_
 * partition -- logging/paging side effects this test doesn't need real
 * bodies for, though the Phase 4 one IS call-tracked, see Scenario 13
 * below) plus one call-tracking stub for e1000_transmit_packet, which
 * captures the actual bytes of the last packet transmitted so this test
 * can assert on real packet contents, not just the bookkeeping API.
 *
 * That's the whole point of Scenarios 8-10 below: the bug Phase 1 fixed
 * wasn't just "there's no roster" -- it was that every packet consensus.c
 * ever built had node_source_id/candidate_id hardcoded to the literal `1`,
 * regardless of which node was actually running. A test that only checked
 * cluster_local_node_id() would never catch a regression where a future
 * edit reintroduces a hardcoded literal at one of the four packet
 * construction sites while leaving the accessor correct. Driving the real
 * REQUEST_VOTE/HEARTBEAT/VOTE_REPLY code paths and inspecting the actual
 * transmitted bytes is the only way to prove that class of bug is fixed.
 * Scenarios 11+ apply the identical discipline to Phase 4's new per-
 * partition lease opcodes: real election/heartbeat/vote-reply execution,
 * real transmitted-packet inspection (now including the new partition_id
 * field), not just accessor return values.
 *
 * Build and run:
 *   gcc -std=c11 -I . -I kernel -I drivers -I net \
 *       tests/consensus_phase1_host_test.c net/consensus.c \
 *       -o /tmp/consensus_phase1_host_test && /tmp/consensus_phase1_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "../net/consensus.h"
#include "../net/dspp.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else          { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* --- Stubs for consensus.c's extern dependencies --- */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void update_page_table_permissions_globally(uint32_t force_read_only) { (void)force_read_only; }

/* Phase 4: call-tracked, unlike the global stub above -- Scenario 13 below
 * needs to prove partition_lease_trigger_election()/the quorum-achieved
 * path in process_partition_consensus_packet() call THIS function with the
 * real partition_id, not the old global all-or-nothing strip. */
static int      partition_perm_calls = 0;
static uint32_t partition_perm_last_partition_id = 0xFFFFFFFFu;
static uint32_t partition_perm_last_force_read_only = 0xFFFFFFFFu;
void update_page_table_permissions_for_partition(uint32_t partition_id, uint32_t force_read_only) {
    partition_perm_calls++;
    partition_perm_last_partition_id = partition_id;
    partition_perm_last_force_read_only = force_read_only;
}

static uint8_t  last_packet[8192];
static uint16_t last_packet_size = 0;
static int      transmit_call_count = 0;
void e1000_transmit_packet(void* buf, uint16_t size) {
    transmit_call_count++;
    if (size <= sizeof(last_packet)) memcpy(last_packet, buf, size);
    last_packet_size = size;
}

int main(void) {
    /* Scenario 1: cluster_init() rejects the reserved sentinel id 0. */
    int r = cluster_init(0);
    CHECK(r == 1, "cluster_init(0) should be rejected (reserved sentinel)");

    /* Scenario 2: a real node id initialises cleanly: self-only roster,
     * quorum=1, fresh FOLLOWER/term-0 state. */
    r = cluster_init(42);
    CHECK(r == 0, "cluster_init(42) should succeed");
    CHECK(cluster_local_node_id() == 42, "cluster_local_node_id() should return 42");
    CHECK(cluster_active_node_count() == 1, "active_node_count should be 1 (self only)");
    CHECK(local_cluster_state.stable_quorum_threshold == 1, "quorum of 1 node should be 1");
    CHECK(local_cluster_state.role == ROLE_FOLLOWER, "fresh init should be FOLLOWER");
    CHECK(local_cluster_state.current_term == 0, "fresh init should be term 0");

    /* Scenario 3: registering peers grows the roster and recomputes
     * quorum as a real majority, not a hardcoded constant. */
    r = cluster_register_peer(43);
    CHECK(r == 0, "first registration of peer 43 should return 0 (newly added)");
    CHECK(cluster_active_node_count() == 2, "active_node_count should be 2 after one peer");
    CHECK(local_cluster_state.stable_quorum_threshold == 2, "quorum of 2 nodes should be 2 (majority)");

    r = cluster_register_peer(44);
    CHECK(r == 0, "second registration of peer 44 should return 0");
    CHECK(cluster_active_node_count() == 3, "active_node_count should be 3");
    CHECK(local_cluster_state.stable_quorum_threshold == 2, "quorum of 3 nodes should be 2 (majority)");

    /* Scenario 4: duplicate registration is a no-op, not a double-count. */
    r = cluster_register_peer(43);
    CHECK(r == 1, "re-registering an already-active peer should return 1");
    CHECK(cluster_active_node_count() == 3, "active_node_count should stay 3 after duplicate registration");

    /* Scenario 5: invalid peer ids (0, and the local node's own id) are rejected. */
    r = cluster_register_peer(0);
    CHECK(r == -1, "registering peer id 0 should be rejected");
    r = cluster_register_peer(42); /* own id */
    CHECK(r == -1, "registering own node id as a peer should be rejected");
    CHECK(cluster_active_node_count() == 3, "active_node_count should be unaffected by rejected registrations");

    /* Scenario 6: roster fills to CLUSTER_NODE_MAX (8) and the next
     * registration past capacity is rejected, not silently dropped or
     * overflowed. Already have 2 peers (43, 44); add 6 more distinct
     * peers to reach the cap. */
    for (uint32_t i = 0; i < 6; i++) {
        r = cluster_register_peer(100 + i);
        CHECK(r == 0, "filling roster to capacity should keep succeeding");
    }
    CHECK(cluster_roster_count == 8, "roster should now hold exactly CLUSTER_NODE_MAX (8) entries");
    r = cluster_register_peer(999);
    CHECK(r == -2, "registering beyond CLUSTER_NODE_MAX should return -2 (roster full)");

    /* Scenario 7: re-init resets everything to a fresh state, including a
     * full roster from Scenario 6 -- proving cluster_init() is a true
     * reset, not a merge with prior state. */
    r = cluster_init(7);
    CHECK(r == 0, "re-init with a different node id should succeed");
    CHECK(cluster_local_node_id() == 7, "node id should now be 7");
    CHECK(cluster_active_node_count() == 1, "roster should be empty again after re-init (self only)");
    CHECK(cluster_roster_count == 0, "cluster_roster_count should be reset to 0");

    /* Scenario 8: real execution proof that packets built by consensus.c
     * now carry the real, configured node id -- not the old hardcoded
     * literal `1`. Drives trigger_kernel_election_campaign() (the
     * REQUEST_VOTE path) and inspects the actual transmitted packet. */
    cluster_init(55);
    transmit_call_count = 0;
    trigger_kernel_election_campaign();
    CHECK(transmit_call_count == 1, "trigger_kernel_election_campaign should transmit exactly one packet");
    {
        struct DSPPFullPagePacket* sent = (struct DSPPFullPagePacket*)last_packet;
        CHECK(sent->header.node_source_id == 55,
              "REQUEST_VOTE packet's node_source_id should be the real node id (55), not the old hardcoded 1");
        struct ConsensusMessage* msg = (struct ConsensusMessage*)sent->payload_4kb;
        CHECK(msg->candidate_id == 55,
              "REQUEST_VOTE's candidate_id should be the real node id (55), not the old hardcoded 1");
        CHECK(local_cluster_state.voted_for == 55,
              "campaigning node should vote for its own real id, not the old hardcoded 1");
    }

    /* Scenario 9: HEARTBEAT packets (the LEADER path) also carry the real
     * node id. Force LEADER role directly to reach that branch without a
     * full election. */
    local_cluster_state.role = ROLE_LEADER;
    transmit_call_count = 0;
    check_consensus_heartbeat_tick();
    CHECK(transmit_call_count == 1, "LEADER heartbeat tick should transmit exactly one packet");
    {
        struct DSPPPacketHeader* hb = (struct DSPPPacketHeader*)last_packet;
        CHECK(hb->node_source_id == 55, "HEARTBEAT packet's node_source_id should be the real node id (55)");
    }

    /* Scenario 10: VOTE_REPLY packets (process_consensus_packet's
     * REQUEST_VOTE-received branch) also carry the real node id, and the
     * real requesting candidate's id -- not 1 -- is what gets recorded as
     * voted_for. */
    {
        local_cluster_state.role = ROLE_FOLLOWER;
        local_cluster_state.current_term = 1;
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_REQUEST_VOTE;
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->term = 2;
        m->candidate_id = 999; /* some other node campaigning */
        transmit_call_count = 0;
        process_consensus_packet(&incoming);
        CHECK(transmit_call_count == 1, "receiving REQUEST_VOTE should transmit exactly one VOTE_REPLY");
        struct DSPPFullPagePacket* reply = (struct DSPPFullPagePacket*)last_packet;
        CHECK(reply->header.node_source_id == 55, "VOTE_REPLY packet's node_source_id should be the real node id (55)");
        CHECK(local_cluster_state.voted_for == 999, "should record having voted for the real requesting candidate (999), not 1");
    }

    /* ═══ Multi-Node Partition Scaling Roadmap Phase 4: partition-scoped
     * write leases ═══════════════════════════════════════════════════════
     * local_cluster_state is still node 55 with an empty roster (quorum=1)
     * from Scenario 8's cluster_init(55) -- register two more peers here
     * so the partition-lease quorum tests below (which reuse this same
     * roster-derived threshold, by design -- see consensus.h's own
     * comment on why) can prove both "one vote is not enough" and "the
     * second vote reaches quorum," the same rigor Phase 1's own test
     * applies to cluster-wide quorum. */
    cluster_register_peer(300);
    cluster_register_peer(301);
    CHECK(local_cluster_state.stable_quorum_threshold == 2,
          "Phase 4 setup: 3 active nodes (self+2 peers) -> quorum 2, reused by partition leases below");

    /* Scenario 11: an unleased partition's accessors return honest
     * "never contested" defaults, not a false LEADER/writable claim. */
    CHECK(partition_lease_get_role(5) == ROLE_FOLLOWER, "partition 5 with no lease row -> ROLE_FOLLOWER (honest default)");
    CHECK(partition_lease_get_term(5) == 0, "partition 5 with no lease row -> term 0");
    CHECK(partition_holds_write_lease(5) == 0, "partition 5 with no lease row -> NOT writable (never assumed local/leader by default)");

    /* Scenario 12: partition_lease_init() creates a fresh FOLLOWER row. */
    CHECK(partition_lease_init(5) == 0, "partition_lease_init(5) succeeds");
    CHECK(partition_lease_get_role(5) == ROLE_FOLLOWER, "partition 5 freshly initialised -> ROLE_FOLLOWER");
    CHECK(partition_lease_get_term(5) == 0, "partition 5 freshly initialised -> term 0");

    /* Scenario 13: triggering an election for partition 5 -- real state
     * transition, real transmitted packet (inspected, not assumed), and
     * real partition-scoped page-table-permission stub call, all proving
     * this is genuinely scoped to partition 5 and not the old global path
     * (update_page_table_permissions_globally is never called from here --
     * confirmed by NOT incrementing any counter for it, since Phase 1 left
     * that stub uninstrumented; the call-tracked one below is the proof). */
    transmit_call_count = 0;
    partition_perm_calls = 0;
    partition_lease_trigger_election(5);
    CHECK(partition_lease_get_role(5) == ROLE_CANDIDATE, "partition 5 is CANDIDATE after triggering its election");
    CHECK(partition_lease_get_term(5) == 1, "partition 5's term incremented to 1");
    CHECK(transmit_call_count == 1, "triggering partition 5's election transmits exactly one packet");
    CHECK(partition_perm_calls == 1, "update_page_table_permissions_for_partition called exactly once");
    CHECK(partition_perm_last_partition_id == 5, "the page-table-permission call is scoped to partition 5, not a global strip");
    CHECK(partition_perm_last_force_read_only == 1, "election start strips write access (force_read_only=1) for partition 5 only");
    {
        struct DSPPFullPagePacket* sent = (struct DSPPFullPagePacket*)last_packet;
        CHECK(sent->header.opcode == DSPP_CMD_PARTITION_REQUEST_VOTE, "the transmitted packet is a PARTITION_REQUEST_VOTE, not the cluster-wide REQUEST_VOTE opcode");
        CHECK(sent->header.node_source_id == 55, "PARTITION_REQUEST_VOTE carries the real node id (55)");
        struct ConsensusMessage* msg = (struct ConsensusMessage*)sent->payload_4kb;
        CHECK(msg->partition_id == 5, "PARTITION_REQUEST_VOTE's payload carries partition_id=5 -- the new Phase 4 field, actually populated");
        CHECK(msg->candidate_id == 55, "PARTITION_REQUEST_VOTE's candidate_id is the real node id (55)");
        CHECK(msg->term == 1, "PARTITION_REQUEST_VOTE's term matches partition 5's lease term (1)");
    }

    /* Scenario 14: partition 6's own election, triggered WHILE partition
     * 5's is still mid-flight as CANDIDATE -- proves the two partitions'
     * lease state is genuinely independent, not aliased through any
     * shared/global term or vote counter (the exact risk a naive `static`
     * accumulated_votes, copy-pasted from Phase 1's single-election
     * design, would have created). */
    transmit_call_count = 0;
    partition_perm_calls = 0;
    partition_lease_trigger_election(6);
    CHECK(partition_lease_get_role(6) == ROLE_CANDIDATE, "partition 6 is CANDIDATE after its own election trigger");
    CHECK(partition_lease_get_term(6) == 1, "partition 6's own term is 1, independent of partition 5's term");
    CHECK(partition_perm_last_partition_id == 6, "the page-table-permission call for partition 6's election is scoped to 6, not 5");
    CHECK(partition_lease_get_role(5) == ROLE_CANDIDATE && partition_lease_get_term(5) == 1,
          "partition 5's state is completely unaffected by partition 6's later election -- true per-partition isolation");

    /* Raise quorum to 3 (need active_nodes_count=5: self + 4 peers) so the
     * quorum test below can prove BOTH "one external vote is not enough"
     * AND "the second external vote reaches quorum" -- the same two-step
     * rigor Phase 1's own cluster-wide quorum test applies. With only 2
     * peers (quorum=2, Scenario 14's setup), a single external vote would
     * have reached quorum immediately, since self already counts as 1 --
     * not a strong enough test of the accumulation logic itself. */
    cluster_register_peer(302);
    cluster_register_peer(303);
    CHECK(local_cluster_state.stable_quorum_threshold == 3,
          "5 active nodes (self+4 peers) -> quorum 3, reused by the partition-lease quorum test below");

    /* Scenario 15: quorum for partition 5's lease -- one granted VOTE_REPLY
     * is NOT enough (self=1, +1=2, still below quorum 3); a second one
     * reaches quorum and promotes to LEADER, calling the partition-scoped
     * page-table-permission stub with force_read_only=0 (restore write
     * access) for partition 5 specifically. Throughout, partition 6's
     * still-CANDIDATE state (Scenario 14) stays completely untouched --
     * proof these two elections never cross-contaminate. */
    {
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_PARTITION_VOTE_REPLY;
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->partition_id = 5;
        m->term = 1;
        m->vote_granted = 1;

        partition_perm_calls = 0;
        process_partition_consensus_packet(&incoming);
        CHECK(partition_lease_get_role(5) == ROLE_CANDIDATE, "one external granted vote (total 2, quorum 3) is NOT enough -- partition 5 still CANDIDATE");
        CHECK(partition_perm_calls == 0, "no page-table-permission call yet -- quorum not reached");

        process_partition_consensus_packet(&incoming);   /* a second granted VOTE_REPLY */
        CHECK(partition_lease_get_role(5) == ROLE_LEADER, "second external granted vote (total 3) reaches quorum -- partition 5 promoted to LEADER");
        CHECK(partition_holds_write_lease(5) == 1, "partition_holds_write_lease(5) now correctly reports true");
        CHECK(partition_perm_calls == 1, "page-table-permission stub called exactly once, on the quorum-achieving vote");
        CHECK(partition_perm_last_partition_id == 5, "the restore-write-access call is scoped to partition 5");
        CHECK(partition_perm_last_force_read_only == 0, "quorum achieved -> force_read_only=0 (write access restored) for partition 5");
    }
    CHECK(partition_lease_get_role(6) == ROLE_CANDIDATE, "partition 6 is STILL CANDIDATE, completely unaffected by partition 5's votes/promotion -- true isolation");
    CHECK(partition_holds_write_lease(6) == 0, "partition 6 correctly does NOT hold a write lease -- it never reached its own quorum");

    /* Scenario 16: a real LEADER (partition 5) ticking its own heartbeat
     * transmits a real PARTITION_HEARTBEAT carrying partition_id -- proof
     * this needed the full 4KB packet (payload for partition_id), unlike
     * Phase 1's cluster-wide heartbeat which only ever sends a bare
     * DSPPPacketHeader (no partition_id field exists there). */
    transmit_call_count = 0;
    partition_lease_heartbeat_tick(5);
    CHECK(transmit_call_count == 1, "LEADER heartbeat tick for partition 5 transmits exactly one packet");
    {
        struct DSPPFullPagePacket* hb = (struct DSPPFullPagePacket*)last_packet;
        CHECK(hb->header.opcode == DSPP_CMD_PARTITION_HEARTBEAT, "transmitted packet is a PARTITION_HEARTBEAT");
        CHECK(hb->header.node_source_id == 55, "PARTITION_HEARTBEAT carries the real node id (55)");
        struct ConsensusMessage* msg = (struct ConsensusMessage*)hb->payload_4kb;
        CHECK(msg->partition_id == 5, "PARTITION_HEARTBEAT's payload carries partition_id=5");
    }

    /* Scenario 17: receiving a PARTITION_HEARTBEAT for partition 6 (still
     * CANDIDATE from Scenario 14/15) demotes it back to FOLLOWER and resets
     * its silence counter -- mirrors Phase 1's cluster-wide HEARTBEAT
     * handling, now scoped to just partition 6's own lease row. */
    {
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_PARTITION_HEARTBEAT;
        incoming.header.transaction_id = 5;   /* term 5, higher than partition 6's current term 1 */
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->partition_id = 6;
        m->term = 5;
        process_partition_consensus_packet(&incoming);
        CHECK(partition_lease_get_role(6) == ROLE_FOLLOWER, "partition 6 demoted to FOLLOWER on receiving a HEARTBEAT for its lease");
        CHECK(partition_lease_get_term(6) == 5, "partition 6's term catches up to the heartbeat's term (5)");
    }
    CHECK(partition_lease_get_role(5) == ROLE_LEADER, "partition 5 is completely unaffected by partition 6's incoming heartbeat -- still LEADER");

    /* Scenario 18: an incoming REQUEST_VOTE for a partition that has NEVER
     * had a lease row (partition 9) is still handled correctly -- process_
     * partition_consensus_packet() creates a fresh FOLLOWER row on demand
     * (mirrors partition_lease_trigger_election()'s own find-or-create
     * posture) rather than silently dropping the packet. */
    {
        struct DSPPFullPagePacket incoming;
        memset(&incoming, 0, sizeof(incoming));
        incoming.header.magic = DSPP_MAGIC;
        incoming.header.opcode = DSPP_CMD_PARTITION_REQUEST_VOTE;
        struct ConsensusMessage* m = (struct ConsensusMessage*)incoming.payload_4kb;
        m->partition_id = 9;
        m->term = 1;
        m->candidate_id = 301;
        transmit_call_count = 0;
        process_partition_consensus_packet(&incoming);
        CHECK(transmit_call_count == 1, "REQUEST_VOTE for a never-before-seen partition (9) still transmits exactly one VOTE_REPLY");
        struct DSPPFullPagePacket* reply = (struct DSPPFullPagePacket*)last_packet;
        CHECK(reply->header.opcode == DSPP_CMD_PARTITION_VOTE_REPLY, "the reply is a PARTITION_VOTE_REPLY");
        struct ConsensusMessage* reply_msg = (struct ConsensusMessage*)reply->payload_4kb;
        CHECK(reply_msg->partition_id == 9, "the VOTE_REPLY's partition_id matches the incoming request (9)");
        CHECK(reply_msg->vote_granted == 1, "term 1 > partition 9's freshly-created term 0 -- vote granted");
        CHECK(partition_lease_get_term(9) == 1, "partition 9 now has a real lease row, term caught up to 1");
    }

    /* Scenario 19: check_partition_lease_heartbeat_tick() ticks every
     * currently-active row -- with partitions 5 (LEADER), 6 (FOLLOWER), and
     * 9 (FOLLOWER) all active, exactly 3 ticks happen. Partition 5 (the
     * only LEADER) transmits its heartbeat; 6 and 9 are FOLLOWER so they
     * only increment silence counters, no transmission. */
    transmit_call_count = 0;
    check_partition_lease_heartbeat_tick();
    CHECK(transmit_call_count == 1, "check_partition_lease_heartbeat_tick(): exactly one transmission -- only partition 5's LEADER heartbeat, not one per active row");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
