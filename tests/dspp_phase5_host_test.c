/*
 * dspp_phase5_host_test.c — Multi-Node Partition Scaling Roadmap Phase 5:
 * a standalone host-buildable test for net/dspp.c's new partition-routing
 * logic, linked against the REAL, unmodified net/dspp.c, kernel/
 * partition.c, and net/consensus.c — not a reimplementation, and not
 * three isolated units either: this proves dspp_resolve_partition_id()/
 * dspp_page_read_allowed()/_write_allowed()/process_dspp_page_packet()
 * genuinely call into Phase 2's real partition_is_local() and Phase 4's
 * real partition_holds_write_lease(), driving an actual election to a
 * real quorum before checking the write-gate flips.
 *
 * object_catalog[]/object_catalog_count are provided as plain dummy
 * globals populated directly via struct writes (not through valloc
 * syscalls) — the same shortcut persist_partition_host_test.c already
 * takes for the identical reason: linking the real kernel/object_
 * catalog.c pulls in a dependency graph (journal/lock_mgr/index_mgr/etc.)
 * this dspp-focused test has no interest in.
 *
 * Build and run:
 *   gcc -std=c11 -I . -I kernel -I drivers -I net \
 *       tests/dspp_phase5_host_test.c net/dspp.c kernel/partition.c net/consensus.c \
 *       -o /tmp/dspp_phase5_host_test
 *   /tmp/dspp_phase5_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "net/consensus.h"
#include "net/dspp.h"
#include "net/net.h"   /* Multi-Node Partition Scaling Roadmap Phase 7 -- MACAddr, for this file's net_my_mac stand-in below */

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else          { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Dummy object_catalog[] — see header comment ──────────────────────── */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;

static void add_fake_object(uint64_t object_id, uint32_t partition_id) {
    struct SLSObjectEntry* e = &object_catalog[object_catalog_count++];
    memset(e, 0, sizeof(*e));
    e->object_id    = object_id;
    e->partition_id = partition_id;
    e->active       = 1;
}

/* ─── Stubs for kernel/partition.c's dependencies (Phase 8/10/14 precedent) ── */
void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
int stream_migrate_send_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 7 addendum (real cross-node data movement) -- not exercised by this test, permissive "nothing to send" stub, sibling of the stream_relocate_partition() stub above (kernel/partition.c now calls whichever of the two applies depending on cluster_local_node_id()) */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { /* Phase 10's persistence hook — irrelevant here */ }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }   /* Phase 3 */

/* ─── Stubs for net/consensus.c's dependencies (Phase 1/4 precedent) ──── */
void update_page_table_permissions_globally(uint32_t force_read_only) { (void)force_read_only; }
void update_page_table_permissions_for_partition(uint32_t partition_id, uint32_t force_read_only) {
    (void)partition_id; (void)force_read_only;
}
static int transmit_call_count = 0;
void e1000_transmit_packet(void* buf, uint16_t size) { (void)buf; (void)size; transmit_call_count++; }

/* ─── Stubs for net/dspp.c's Phase 7 dependencies ──────────────────────────
 * This test links the real net/dspp.c (for its Phase 5 routing logic,
 * process_dspp_page_packet() etc.) which now also contains Phase 7's
 * dspp_transmit_raw()/dspp_migrate_rx() -- neither exercised by this
 * routing-focused test, but both compiled into the same object file, so
 * their own real dependencies (net_my_mac, kernel/stream.c's migrate
 * receive functions) must still resolve at link time. */
MACAddr net_my_mac;   /* net/net.c's real global -- not linked here, plain zero-init stand-in is fine since nothing in this test transmits a migrate packet */
int stream_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                               const char* name, const char* mime_type,
                               uint64_t size, uint32_t frames_used,
                               uint32_t owner_uid) {
    (void)transfer_id; (void)partition_id; (void)name; (void)mime_type;
    (void)size; (void)frames_used; (void)owner_uid;
    return 1;   /* "no free slot" -- not exercised by this test, permissive-denial stub (kernel/stream.c not linked) */
}
int stream_migrate_recv_page(uint64_t transfer_id, uint32_t page_index, const uint8_t* page_data) {
    (void)transfer_id; (void)page_index; (void)page_data;
    return 1;   /* "unknown transfer" -- not exercised by this test */
}

int main(void) {
    partition_init();
    cluster_init(1);   /* this node is node 1, empty roster, quorum=1 (self only) */

    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    uint32_t pid_c = partition_create("tenant-c");
    CHECK(pid_a != 0xFFFFFFFFu && pid_b != 0xFFFFFFFFu && pid_c != 0xFFFFFFFFu,
          "three partitions created for this test");

    add_fake_object(1001, pid_a);
    add_fake_object(1002, pid_b);

    /* ── Scenario 1: dspp_resolve_partition_id() — real object_catalog scan ── */
    CHECK(dspp_resolve_partition_id(1001) == pid_a, "object 1001 resolves to tenant-a's real partition id");
    CHECK(dspp_resolve_partition_id(1002) == pid_b, "object 1002 resolves to tenant-b's real partition id");
    CHECK(dspp_resolve_partition_id(9999) == 0, "an unknown object_id resolves to 0 (honest 'not found' default)");

    /* ── Scenario 2: dspp_page_read_allowed() — Phase 2 local/remote check.
     * pid_a was just created, so its owner defaults to cluster_local_node_
     * id() (1) — this node — making it local by construction. ─────────── */
    CHECK(dspp_page_read_allowed(pid_a) == 1, "tenant-a reads as local (owned by this node by default at creation)");

    /* ── Scenario 3: dspp_page_write_allowed() before any lease exists —
     * local ownership alone is NOT enough to write; the write lease
     * (Phase 4) must also be held. ─────────────────────────────────────── */
    CHECK(dspp_page_write_allowed(pid_a) == 0, "tenant-a is local but has NO write lease yet — write NOT allowed");

    /* ── Scenario 4: win tenant-a's write lease for real (quorum=1, so one
     * granted VOTE_REPLY is enough) — dspp_page_write_allowed() must then
     * flip to true, proving it's reading Phase 4's real state, not a
     * cached/stale value. ──────────────────────────────────────────────── */
    partition_lease_trigger_election(pid_a);
    CHECK(partition_lease_get_role(pid_a) == ROLE_CANDIDATE, "tenant-a's lease election is underway");
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
    CHECK(dspp_page_write_allowed(pid_a) == 1, "tenant-a is local AND holds the write lease — write now allowed");
    CHECK(dspp_page_read_allowed(pid_a) == 1, "tenant-a still reads as local — winning the write lease didn't change ownership");

    /* ── Scenario 5: a REMOTE partition — pin tenant-b's ownership to a
     * different node (42), unrelated to this node's identity (1). Neither
     * read nor write should be allowed, regardless of any lease state. ── */
    CHECK(partition_set_owner_node(pid_b, 42) == 0, "tenant-b's ownership pinned to a remote node (42)");
    CHECK(dspp_page_read_allowed(pid_b) == 0, "tenant-b reads as REMOTE — read NOT allowed");
    CHECK(dspp_page_write_allowed(pid_b) == 0, "tenant-b is remote — write NOT allowed regardless of any lease");

    /* ── Scenario 6: process_dspp_page_packet() end-to-end — the real
     * per-packet gate, resolving partition_id from system_object_id via
     * the object catalog and checking the appropriate allow function. ── */
    {
        struct DSPPFullPagePacket req;
        memset(&req, 0, sizeof(req));
        req.header.magic           = DSPP_MAGIC;
        req.header.opcode          = DSPP_PAGE_READ_REQ;
        req.header.system_object_id = 1001;   /* -> tenant-a, local */
        CHECK(process_dspp_page_packet(&req) == 1, "READ_REQ for object 1001 (tenant-a, local): serviced");

        req.header.system_object_id = 1002;   /* -> tenant-b, remote */
        CHECK(process_dspp_page_packet(&req) == 0, "READ_REQ for object 1002 (tenant-b, remote): denied");

        req.header.opcode = DSPP_PAGE_WRITE_REQ;
        req.header.system_object_id = 1001;   /* -> tenant-a, local + lease held */
        CHECK(process_dspp_page_packet(&req) == 1, "WRITE_REQ for object 1001 (tenant-a, local + lease held): serviced");

        /* tenant-c is local (created just like tenant-a, same default
         * owner) but has never had a lease election — write must still be
         * denied even though it's local, proving the gate checks the
         * LEASE specifically, not just locality. */
        add_fake_object(1003, pid_c);
        req.header.system_object_id = 1003;
        CHECK(process_dspp_page_packet(&req) == 0, "WRITE_REQ for object 1003 (tenant-c, local but NO lease): denied");
        CHECK(dspp_page_read_allowed(pid_c) == 1, "tenant-c is still readable (local) even without ever winning a write lease");

        req.header.opcode = DSPP_PAGE_READ_ACK;   /* not a routing opcode this function handles */
        CHECK(process_dspp_page_packet(&req) == 0, "an unrelated opcode (READ_ACK) is not serviced by process_dspp_page_packet");
    }
    CHECK(transmit_call_count == 1, "exactly 1 real packet was transmitted total -- partition_lease_trigger_election()'s own REQUEST_VOTE in Scenario 4. Feeding the VOTE_REPLY in that same scenario does not itself transmit (a granted vote just updates local state), and process_dspp_page_packet() never transmits either (no page-move plumbing exists, by design)");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
