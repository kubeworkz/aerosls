/*
 * consensus_phase1_host_test.c — Multi-Node Partition Scaling Roadmap,
 * Phase 1 (real node identity & membership registry).
 *
 * Links the real, unmodified net/consensus.c against three small stubs for
 * its extern dependencies (kernel_serial_print/printf, and
 * update_page_table_permissions_globally -- logging/paging side effects
 * this test doesn't need to observe) plus one call-tracking stub for
 * e1000_transmit_packet, which captures the actual bytes of the last
 * packet transmitted so this test can assert on real packet contents, not
 * just the cluster_init()/cluster_register_peer() bookkeeping API.
 *
 * That's the whole point of Scenarios 8-10 below: the bug this phase fixed
 * wasn't just "there's no roster" -- it was that every packet consensus.c
 * ever built had node_source_id/candidate_id hardcoded to the literal `1`,
 * regardless of which node was actually running. A test that only checked
 * cluster_local_node_id() would never catch a regression where a future
 * edit reintroduces a hardcoded literal at one of the four packet
 * construction sites while leaving the accessor correct. Driving the real
 * REQUEST_VOTE/HEARTBEAT/VOTE_REPLY code paths and inspecting the actual
 * transmitted bytes is the only way to prove that class of bug is fixed.
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

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
