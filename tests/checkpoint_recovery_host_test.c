/*
 * checkpoint_recovery_host_test.c — Core Backup Strategies Step 5:
 * proves that a surviving node can detect peer failure, load a received
 * checkpoint, and adopt the dead node's partitions.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/checkpoint_recovery_host_test \
 *       tests/checkpoint_recovery_host_test.c \
 *       kernel/failover.c kernel/state_tree.c net/dspp_checkpoint.c
 *   /tmp/checkpoint_recovery_host_test
 */
#include "kernel/failover.h"
#include "kernel/state_tree.h"
#include "kernel/env_ckpt.h"   /* P3 (b): env_ckpt_table[] feeds the tree's ST_NODE_ENVIRONMENT nodes */
#include "kernel/env_service.h" /* P3 increment 11: struct EnvSvcRestoreReport, the replay driver's report */
#include "kernel/env_replica.h" /* P3 (a) increment 3: the promotion seam failover now consults */
#include "kernel/env_migrate.h"  /* ENV_MIGRATE_REFUSE_*: the restore status this test scripts */
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/process.h"
#include "kernel/stream.h"
#include "net/dspp.h"
#include "net/net.h"
#include "net/e1000.h"
#include "net/consensus.h"   /* struct ClusterPeer, CLUSTER_NODE_MAX */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Kernel global stubs ──────────────────────────────────────────── */
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

/* POSIX-Environments P3 (b): the environment records state_tree_build()
 * turns into ST_NODE_ENVIRONMENT nodes. Defined here rather than linked
 * from kernel/env_ckpt.c, exactly like partition_table[]/proc_table[]/
 * stream_store[] above — this test drives the CONSUMER (failover) and only
 * needs the table to be populatable. Real record-layer coverage is
 * tests/env_ckpt_host_test.c's. */
struct EnvCkptRecord env_ckpt_table[ENV_CKPT_MAX];
/* P3 increment 11: failover.c's restore half walks the record store's LIVE
 * image, bounded by env_ckpt_count exactly like the real env_ckpt.c defines
 * it. Defined here for the same reason as the table above. */
uint32_t             env_ckpt_count = 0;

MACAddr net_my_mac;
volatile uint64_t kernel_tick_counter = 0;
uint64_t dspp_tx_oversize_dropped = 0;
uint16_t dspp_max_wire_payload = DSPP_MAX_WIRE_PAYLOAD;
uint64_t dspp_tx_reentrant_dropped = 0;

static uint32_t g_local_node = 2;
uint32_t cluster_local_node_id(void) { return g_local_node; }

/* The test node is a follower -- recovery is exercised by calling
 * failover_recover_from() directly, not via failover_tick()'s leader gate. */
int cluster_is_leader(void) { return 0; }

/* net/consensus.c's sweep heartbeat, pumped by failover_sweep_keepalive()
 * from the blocking waits (the death-during-create fix). A no-op here:
 * the test drives failover_tick() directly and TX is not its subject. */
void check_consensus_heartbeat_tick(uint64_t now) { (void)now; }

/* The failover deferral gate (failover.c -> env_migrate_in_flight): a
 * controllable stand-in so the migration deferral itself is exercised below,
 * not just bypassed. The real one lives in kernel/env_migrate.c. */
int g_env_migrate_in_flight = 0;
int env_migrate_in_flight(void) { return g_env_migrate_in_flight; }

/* The peer half of the same gate (the idle-observer arm): failover.c asks
 * env_migrate_peer_in_flight() about the SILENT PEER before declaring it
 * dead, because the knowledge lives on that peer (it announced intent before
 * blocking its sweep), not here. The stub records which node was asked so the
 * pin can prove the gate consults the right identity. */
static uint32_t g_peer_in_flight_asked = 0;
static int g_env_migrate_peer_in_flight = 0;
int env_migrate_peer_in_flight(uint32_t node_id) {
    g_peer_in_flight_asked = node_id;
    return g_env_migrate_peer_in_flight;
}

/* ─── P3 increment 11: the failover restore-half seams ────────────────────
 * failover_recover_from() now restores the adopted partitions' environments
 * from THIS node's durable records through the standard replay path: arm
 * each record (env_ckpt_restore_arm), drain through the driver
 * (env_service_restore_pending), then classify per record — still-pending
 * means the replay never got to it, refused means it did and was refused,
 * otherwise restored. This test drives the CONSUMER (failover) and links
 * neither kernel/env_ckpt.c nor kernel/env_service.c, so the three queries
 * and the one call are controllable stand-ins — exactly like
 * partition_table[] above; the real record layer is env_ckpt_host_test.c's.
 * What is REAL here is failover.c's own accounting: which records it arms,
 * which questions it asks of each, and how every answer is counted. */
static uint32_t g_arm_count;
static uint32_t g_arm_p[ENV_CKPT_MAX], g_arm_idx[ENV_CKPT_MAX];
static int      g_arm_pending[ENV_CKPT_MAX];   /* armed, not yet processed */
static int      g_arm_refused[ENV_CKPT_MAX];   /* the driver refused it */
static int      g_replay_ready;                /* 0: the environment manager is
                                                * not up — the driver settles
                                                * nothing and returns -1 */
static int      g_replay_refuse_slot;          /* which armed slot is refused */
static uint32_t g_replay_calls;

static void restore_stub_reset(void) {
    memset(g_arm_p, 0, sizeof(g_arm_p));
    memset(g_arm_idx, 0, sizeof(g_arm_idx));
    memset(g_arm_pending, 0, sizeof(g_arm_pending));
    memset(g_arm_refused, 0, sizeof(g_arm_refused));
    g_arm_count = 0;
    g_replay_ready = 0;
    g_replay_refuse_slot = -1;
    g_replay_calls = 0;
}

static int stub_slot(uint32_t p, uint32_t idx) {
    for (uint32_t i = 0; i < g_arm_count; i++)
        if (g_arm_p[i] == p && g_arm_idx[i] == idx) return (int)i;
    return -1;
}

int env_ckpt_restore_arm(uint32_t p, uint32_t idx) {
    /* Real semantics: 1 when no such record exists (arming nothing is a
     * refusal); otherwise idempotent arming of exactly this slot. */
    int present = 0;
    for (uint32_t r = 0; r < env_ckpt_count && r < ENV_CKPT_MAX; r++)
        if (env_ckpt_table[r].partition_id == p && env_ckpt_table[r].index == idx)
            { present = 1; break; }
    if (!present || g_arm_count >= ENV_CKPT_MAX) return 1;
    int s = stub_slot(p, idx);
    if (s < 0) {
        s = (int)g_arm_count;
        g_arm_p[g_arm_count] = p;
        g_arm_idx[g_arm_count] = idx;
        g_arm_count++;
    }
    g_arm_pending[s] = 1;
    g_arm_refused[s] = 0;
    return 0;
}

int env_ckpt_restore_pending_record(uint32_t p, uint32_t idx) {
    int s = stub_slot(p, idx);
    return (s >= 0 && g_arm_pending[s]) ? 1 : 0;
}

int env_ckpt_restore_refused_record(uint32_t p, uint32_t idx) {
    int s = stub_slot(p, idx);
    return (s >= 0 && g_arm_refused[s]) ? 1 : 0;
}

int env_service_restore_pending(struct EnvSvcRestoreReport* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    g_replay_calls++;
    if (!g_replay_ready) return -1;   /* manager not ready: nothing settles */
    for (uint32_t i = 0; i < g_arm_count; i++) {
        if (!g_arm_pending[i]) continue;
        if ((int)i == g_replay_refuse_slot) { g_arm_refused[i] = 1; out->n_refused++; }
        else                                 { out->n_replayed++; }
        g_arm_pending[i] = 0;   /* both outcomes leave the pending set */
    }
    out->n_remaining = 0;
    return 0;
}

/* ─── P3 (a) increment 3: the promotion seams ──────────────────────────────
 * failover_recover_from() now consults this survivor's REPLICA band: on a
 * HOLE (no durable record) the replica is the only source that can exist
 * here, and when both exist the NEWER capture wins. It stages through
 * env_replica_promote() and classifies on env_migrate_restore()'s status —
 * the migration restore half, whose create round trip, identity check and
 * pour are the proved replay for wire-numbered bytes.
 *
 * Same posture as the seams above: this test drives the CONSUMER, so the
 * replica's disk and the migration's staging table are controllable
 * stand-ins. What is REAL here is failover's accounting — which source it
 * consults, when it promotes, when it falls back to (b), and how every
 * outcome is counted. The real promote implementation (band → staging,
 * byte-exact, refusal over partial application) is
 * tests/env_replica_host_test.c's section J. */
static int      g_rep_present;          /* the directory holds an entry for any identity */
static uint64_t g_rep_seq;              /* its capture sequence */
static uint32_t g_rep_src;              /* provenance node */
static int      g_promote_rc;           /* env_replica_promote's scripted answer */
static int      g_migrate_restore_rc;   /* env_migrate_restore's scripted answer */
static uint32_t g_promote_calls, g_migrate_restore_calls, g_recv_drop_calls;

static void replica_stub_reset(void) {
    g_rep_present = 0;
    g_rep_seq = 0;
    g_rep_src = 1;
    g_promote_rc = 0;
    g_migrate_restore_rc = 0;
    g_promote_calls = 0;
    g_migrate_restore_calls = 0;
    g_recv_drop_calls = 0;
}

int env_replica_lookup(uint32_t p, uint32_t idx, struct EnvReplicaDirEntry* out) {
    if (!g_rep_present) return 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->partition_id = p;
        out->index        = idx;
        out->flags        = ENV_REPLICA_F_VALID;
        out->seq          = g_rep_seq;
        out->src_node     = g_rep_src;
        out->pages        = 4;
    }
    return 1;
}

int env_replica_promote(uint32_t p, uint32_t idx) {
    (void)p; (void)idx;
    g_promote_calls++;
    return g_promote_rc;
}

const char* env_replica_promotion_name(uint32_t code) {
    return code == 0 ? "ok" : "scripted staging refusal";
}

/* The migration restore this test does NOT link (kernel/env_migrate.c is
 * env_migrate_host_test.c's and env_replica_host_test's). Scripted here so
 * failover's classification of its status is exercisable. */
int env_migrate_restore(uint32_t p, uint32_t idx) {
    (void)p; (void)idx;
    g_migrate_restore_calls++;
    return g_migrate_restore_rc;
}

/* failover.c drops any slot a FAILED promotion staged, on its way back to
 * (b), so exactly one driver owns the replay. Counted, because a staged
 * slot left behind is the double-drive Scenario 4d(g) exists to prevent. */
int env_migrate_recv_drop(uint32_t p, uint32_t idx) {
    (void)p; (void)idx;
    g_recv_drop_calls++;
    return 1;
}

/* Roster stubs -- failover.c's leader broadcast iterates these; an empty
 * roster makes it a no-op in this test (no live cluster). */
struct ClusterPeer cluster_roster[CLUSTER_NODE_MAX];
uint32_t cluster_roster_count = 0;

static uint64_t g_tsc = 1000;
uint64_t read_tsc(void) { return g_tsc++; }

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* partition_get_owner_node / partition_set_owner_node stubs */
uint32_t partition_get_owner_node(uint32_t partition_id) {
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id)
            return partition_owner_table[i].node_id;
    }
    return 0;
}

int partition_set_owner_node(uint32_t partition_id, uint32_t node_id) {
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (partition_owner_table[i].active &&
            partition_owner_table[i].partition_id == partition_id) {
            partition_owner_table[i].node_id = node_id;
            return 0;
        }
    }
    /* Create new row */
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_owner_table[i].active) {
            partition_owner_table[i].partition_id = partition_id;
            partition_owner_table[i].node_id = node_id;
            partition_owner_table[i].active = 1;
            return 0;
        }
    }
    return 1;
}

void persist_partitions(void) { /* no-op in test */ }

/* partition_exists stub -- mirrors partition.c's partition_id_valid(). */
int partition_exists(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) return 1;
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_table[partition_id].active;
}

/* DSPP transmit stub */
void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    (void)role; (void)buf; (void)size;
}

void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len) {
    if (dspp_len > dspp_max_wire_payload) {
        dspp_tx_oversize_dropped++;
        return;
    }
    /* In this test we don't capture frames — we feed directly via dspp_ckpt_rx */
}

/* ─── Test harness ─────────────────────────────────────────────────── */
static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Helper: simulate a checkpoint arriving from node 1 ───────────── */
static void simulate_checkpoint_from_node1(void) {
    /* Build a state tree as if we are node 1 */
    uint32_t saved_local = g_local_node;
    g_local_node = 1;

    memset(partition_table, 0, sizeof(partition_table));
    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    object_catalog[0].object_id = 111; object_catalog[0].active = 1;
    object_catalog[0].partition_id = 5;
    object_catalog[1].object_id = 222; object_catalog[1].active = 1;
    object_catalog[1].partition_id = 7;
    object_catalog_count = 2;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);

    uint8_t serial_buf[ST_SERIAL_MAX];
    uint32_t serial_size = state_tree_serialize(nodes, count, 42, serial_buf, sizeof(serial_buf));

    /* Feed chunks directly into the receiver as if they arrived over DSPP */
    g_local_node = 2;
    dspp_ckpt_recv_reset();

    /* Simulate BEGIN */
    uint32_t total_chunks = (serial_size + DSPP_CKPT_CHUNK_BYTES - 1) / DSPP_CKPT_CHUNK_BYTES;
    struct DSPPCkptHeader begin;
    memset(&begin, 0, sizeof(begin));
    begin.magic = DSPP_MIGRATE_MAGIC;
    begin.opcode = DSPP_CKPT_BEGIN_REQ;
    begin.node_source_id = 1;
    begin.node_dest_id = 2;
    begin.transfer_id = 0xCAFE;
    begin.sequence = 42;
    begin.total_bytes = serial_size;
    begin.total_chunks = total_chunks;
    dspp_ckpt_rx((struct DSPPCkptChunkPacket*)&begin, sizeof(begin));

    /* Simulate chunks */
    for (uint32_t i = 0; i < total_chunks; i++) {
        struct DSPPCkptChunkPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.magic = DSPP_MIGRATE_MAGIC;
        pkt.header.opcode = DSPP_CKPT_CHUNK_REQ;
        pkt.header.node_source_id = 1;
        pkt.header.node_dest_id = 2;
        pkt.header.transfer_id = 0xCAFE;
        pkt.header.chunk_index = i;
        pkt.header.sequence = 42;

        uint32_t offset = i * DSPP_CKPT_CHUNK_BYTES;
        uint32_t remaining = serial_size - offset;
        uint32_t chunk_sz = remaining < DSPP_CKPT_CHUNK_BYTES ? remaining : DSPP_CKPT_CHUNK_BYTES;
        pkt.header.chunk_bytes = chunk_sz;
        memcpy(pkt.chunk_data, serial_buf + offset, chunk_sz);

        dspp_ckpt_rx(&pkt, (uint16_t)(sizeof(struct DSPPCkptHeader) + chunk_sz));
    }

    g_local_node = saved_local;
}

/* ─── Scenario 1: heartbeat tracking and failure detection ─────────── */
static void test_liveness(void) {
    printf("--- Scenario 1: heartbeat tracking and failure detection ---\n");
    failover_init();

    /* Node 1 sends heartbeats at tick 0, 50, 100 */
    failover_note_heartbeat(1, 0);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 alive after heartbeat");

    failover_note_heartbeat(1, 50);
    failover_tick(100);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 still alive at tick 100");

    /* Silence: no heartbeat for 300+ ticks. FAILOVER_DEAD_TICKS alone is
     * SUSPICION — the declaration additionally requires the per-episode
     * silence grace (FAILOVER_DECLARE_TICKS), because every declaration
     * three consecutive live suite runs made was false and all of them
     * landed between 300 and 596 ticks of a peer that was alive; the fourth
     * run's four false declarations all landed just past the line, sizing
     * the grace against stalls of ~2900 ticks (see failover.h). The tick
     * samples below are written against the MACRO, not the number, so the
     * grace can be re-sized without this test silently pinning a stale
     * threshold. */
    failover_tick(351);
    CHECK(failover_peer_status(1) != PEER_DEAD,
          "*** 301 ticks of silence does NOT declare: the silence grace holds it ***");
    failover_tick(50 + FAILOVER_DECLARE_TICKS + 1);  /* silence DECLARE+1 */
    CHECK(failover_peer_status(1) == PEER_DEAD,
          "...and a full FAILOVER_DECLARE_TICKS of silence declares: the grace delays, it does not blindfold");

    /* Suspected state at half the dead threshold */
    failover_init();
    failover_note_heartbeat(3, 0);
    failover_tick(160);  /* 160 > 150 (half of 300) */
    CHECK(failover_peer_status(3) == PEER_SUSPECTED, "node 3 suspected after 160 ticks");

    /* Migration deferral (POSIX-Environments P3 live arm): while this node's
     * own environment migration is in flight, a silent peer is NOT declared
     * dead — the silence a synchronous payload send or a sweep-blocking
     * restore produces on the COUNTERPARTY is not a death, and declaring it
     * one flipped ownership back onto the sender on the live 2-node TCG run
     * (both nodes adopted; the confirmation teardown then correctly
     * refused). The declaration must fire on the first tick after the
     * migration resolves — that pairing is what makes the deferral a delay,
     * not a blindfold. */
    failover_init();
    g_env_migrate_in_flight = 1;
    failover_note_heartbeat(4, 0);
    failover_tick(FAILOVER_DECLARE_TICKS + 1);  /* silence DECLARE+1, but a migration is in flight */
    CHECK(failover_peer_status(4) != PEER_DEAD,
          "*** node 4 NOT declared dead past the silence grace while a migration is in flight ***");
    g_env_migrate_in_flight = 0;
    failover_tick(FAILOVER_DECLARE_TICKS + 2);
    CHECK(failover_peer_status(4) == PEER_DEAD,
          "...and declared dead on the first tick after the migration resolves");

    /* The idle-observer arm (docs/AeroSLS-Idle-Observer-Adoption-Finding-
     * v0.1.md): a peer that ANNOUNCED migration intent is not declared dead
     * while its budget stands — even when THIS node holds no migration state
     * of its own. That is the live 3-node failure: the busy SOURCE stopped
     * beating (its blocking work runs on the sweep that transmits the
     * leader's heartbeat), an idle observer won the election the blocked
     * node could not contest and adopted every partition in the source's
     * checkpoint — every node alive, the migrating environment destroyed.
     * The gate must ask about the SILENT PEER by identity, and the deferral
     * must end when the budget does. */
    failover_init();
    failover_note_heartbeat(7, 0);
    g_env_migrate_peer_in_flight = 1;
    g_peer_in_flight_asked = 0;
    failover_tick(FAILOVER_DECLARE_TICKS + 1);  /* silence DECLARE+1, but node 7 announced intent */
    CHECK(failover_peer_status(7) != PEER_DEAD,
          "*** node 7 NOT declared dead while its announced migration intent stands ***");
    CHECK(g_peer_in_flight_asked == 7,
          "*** ...and the gate asked about node 7 -- the SILENT PEER, by identity ***");
    g_env_migrate_peer_in_flight = 0;
    failover_tick(FAILOVER_DECLARE_TICKS + 2);
    CHECK(failover_peer_status(7) == PEER_DEAD,
          "...and declared dead on the first tick after the intent budget expires");
    /* The same transition now also happens EARLY, on the peer's own
     * release: env_migrate_intent_end() broadcasts budget 0, recv_intent
     * drops the latch, and env_migrate_peer_in_flight() answers 0 here —
     * the exact stub flip above. The mechanics of the release (local gate
     * cleared, budget-0 frame on the wire, latch dropped, no-op without a
     * begin) are pinned against the real definitions in
     * tests/env_migrate_host_test.c; this file pins what the failover gate
     * does with the answer, and that answer's contract is unchanged. */

    /* The stamp-ahead case (observed live 2026-10-13, a DEFERRED line that
     * printed "silent 18446744073709551606 ticks" = -10 unsigned): the two
     * operands of the silence subtract come from different contexts, so the
     * stamp may legitimately land AHEAD of the sample. `now` is read once at
     * failover_tick()'s BSP-sweep call site (net/http.c), while
     * last_seen_tick is stamped by the heartbeat RX path
     * (process_consensus_packet -> failover_note_heartbeat, timer ISR / AP
     * core). A heartbeat landing between those two reads leaves
     * last_seen_tick > now, and an unsigned subtraction wraps to ~2^64 —
     * past FAILOVER_DEAD_TICKS. The arithmetic must saturate: a peer heard
     * at or after `now` has silence zero, not ~2^64. The pair below pins
     * both halves — not declared on the wrapped tick, and still declared
     * once real silence crosses the threshold from the same stamp — because
     * a saturation that simply never declared would be a blindfold, not a
     * fix. */
    failover_init();
    failover_note_heartbeat(9, 400);
    failover_tick(390);   /* now is 10 ticks BEHIND the stamp: the wrap case */
    CHECK(failover_peer_status(9) != PEER_DEAD,
          "*** a heartbeat stamped ahead of the tick sample is NOT ~2^64 ticks of silence ***");
    CHECK(failover_peer_status(9) != PEER_SUSPECTED,
          "...and is not even suspected: from this sample the peer was just heard");
    failover_tick(699);   /* 699 - 400 = 299, still under the threshold */
    CHECK(failover_peer_status(9) != PEER_DEAD,
          "...and 299 ticks of REAL silence from that stamp still defer the declaration");
    failover_tick(400 + FAILOVER_DECLARE_TICKS + 1);  /* silence DECLARE+1 from that stamp */
    CHECK(failover_peer_status(9) == PEER_DEAD,
          "...but a full FAILOVER_DECLARE_TICKS of real silence from the SAME stamp declares it dead");
}

/* ─── Scenario 1b: the relaunch grace (a just-relaunched node) ─────── */
/* Both directions of FAILOVER_RELAUNCH_GRACE_TICKS, because the live
 * suite run (2026-10-24) hit BOTH: a freshly booted node declaring a
 * healthy peer DEAD five times over (its own restore burst stalling that
 * peer), and a just-relaunched migration destination re-declared DEAD
 * 106 log lines after "came back", which re-adopted every partition onto
 * the wrong owner and failed failover_adoption_live_check's convergence
 * gate.
 *
 * Every check below is a PAIR — held during the window, declared on the
 * first tick after it. The second half is the whole point: a grace that
 * suppressed the declaration for ever would be a blindfold, and a death
 * that happened inside the window must still be reported once the window
 * closes (the silence keeps accruing while the grace stands). */
static void test_relaunch_grace(void) {
    printf("--- Scenario 1b: relaunch grace (just-relaunched node) ---\n");

    /* THIS node just came up: failover_note_relaunch_grace() is what the
     * boot path calls beside failover_init(), so the test arms it exactly
     * the way kernel.c does. */
    failover_init();
    failover_note_relaunch_grace(0);
    failover_note_heartbeat(21, 0);
    failover_tick(351);   /* silence 301 — but WE are the node that relaunched */
    CHECK(failover_peer_status(21) != PEER_DEAD,
          "*** a just-relaunched node declares nobody dead during its grace ***");
    failover_tick(FAILOVER_RELAUNCH_GRACE_TICKS + 100);  /* grace 0 + RELAUNCH is over — and the watch RE-BASES */
    CHECK(failover_peer_status(21) != PEER_DEAD,
          "*** ...and the silence that accrued ACROSS our own boot is not counted: "
          "our watch starts when the grace ends ***");
    failover_tick(FAILOVER_RELAUNCH_GRACE_TICKS + FAILOVER_DECLARE_TICKS + 1);  /* the re-based window is a FULL declare window */
    CHECK(failover_peer_status(21) == PEER_DEAD,
          "...and a peer still silent a full FAILOVER_DECLARE_TICKS after the grace ends is declared");

    /* The PEER came back: the grace is armed by the return itself, no call
     * needed — that is the arm failover_note_heartbeat() gained. */
    failover_init();
    failover_note_heartbeat(22, 0);
    failover_tick(FAILOVER_DECLARE_TICKS + 1);  /* past FAILOVER_DECLARE_TICKS */
    CHECK(failover_peer_status(22) == PEER_DEAD,
          "the peer is declared dead BEFORE it comes back (the grace arms on the return)");
    failover_note_heartbeat(22, 1300);  /* DEAD -> ALIVE: window = 1300 + RELAUNCH */
    failover_tick(1300 + FAILOVER_DEAD_TICKS + 1);  /* silence DEAD+1 — enough WITHOUT the relaunch grace */
    CHECK(failover_peer_status(22) != PEER_DEAD,
          "*** a just-relaunched peer is not declared dead during its grace, "
          "even past the silence grace ***");
    failover_tick(1300 + FAILOVER_RELAUNCH_GRACE_TICKS + 1);  /* relaunch grace over; silence DECLARE+1 */
    CHECK(failover_peer_status(22) == PEER_DEAD,
          "...and is declared the moment its own grace expires");

    /* An ordinary beat must NOT re-arm: the grace is per episode, so a peer
     * that goes quiet for good is still declared on the normal terms. */
    failover_init();
    failover_note_heartbeat(23, 0);
    failover_note_heartbeat(23, 5000);  /* still ALIVE: no window to arm */
    failover_tick(5000 + FAILOVER_DEAD_TICKS + 1);  /* silence DEAD+1 from an ordinary beat */
    CHECK(failover_peer_status(23) != PEER_DEAD,
          "301 ticks is held by the silence grace like any other episode");
    failover_tick(5000 + FAILOVER_DECLARE_TICKS + 1);  /* silence DECLARE+1 — a RELAUNCH grace would run to 11000 */
    CHECK(failover_peer_status(23) == PEER_DEAD,
          "*** ...but an ordinary beat never arms a relaunch grace: a full silence grace declares anyway ***");
}

/* ─── Scenario 2: node comes back from the dead ────────────────────── */
static void test_resurrection(void) {
    printf("--- Scenario 2: node resurrection ---\n");
    failover_init();
    failover_note_heartbeat(1, 0);
    failover_tick(FAILOVER_DECLARE_TICKS + 1);  /* declare dead, past the silence grace */
    CHECK(failover_peer_status(1) == PEER_DEAD, "node 1 dead");

    failover_note_heartbeat(1, 500);
    CHECK(failover_peer_status(1) == PEER_ALIVE, "node 1 alive again after heartbeat");
}

/* ─── Scenario 3: failover with no checkpoint ──────────────────────── */
static void test_no_checkpoint(void) {
    printf("--- Scenario 3: failover with no checkpoint available ---\n");
    dspp_ckpt_recv_reset();
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "returns NO_CHECKPOINT when nothing received");
}

/* ─── Scenario 4: successful failover — adopt partitions ───────────── */
static void test_successful_failover(void) {
    printf("--- Scenario 4: successful failover ---\n");

    /* Setup: partitions 5 and 7 are owned by node 1 */
    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));

    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    partition_owner_table[0].partition_id = 0; partition_owner_table[0].node_id = 2;
    partition_owner_table[0].active = 1;
    partition_owner_table[1].partition_id = 5; partition_owner_table[1].node_id = 1;
    partition_owner_table[1].active = 1;
    partition_owner_table[2].partition_id = 7; partition_owner_table[2].node_id = 1;
    partition_owner_table[2].active = 1;

    /* Simulate checkpoint arriving from node 1 */
    simulate_checkpoint_from_node1();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    /* Perform failover */
    g_local_node = 2;
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "failover_recover_from() succeeds");
    CHECK(failover_last_adopted_count() == 2, "adopted 2 partitions");

    /* Verify ownership transferred */
    CHECK(partition_get_owner_node(5) == 2, "partition 5 now owned by node 2");
    CHECK(partition_get_owner_node(7) == 2, "partition 7 now owned by node 2");
    CHECK(partition_get_owner_node(0) == 2, "partition 0 was already node 2 (unchanged)");
}

/* ─── Scenario 4b: P3 (b) — the adoption NAMES what it did not restore ─ */
/* POSIX-Environments v0.2 Phase P3, failover destination (b) (§11 Q5),
 * increment 9's half: the report. Failover adopts partitions by ownership
 * flip; the payload is 5.5 MiB of regions and this checkpoint is a 24 KiB
 * broadcast. What the tree carries is the dead node's environment NAMES
 * (ST_NODE_ENVIRONMENT), and this scenario pins that they are REPORTED: an
 * operator who sees "Adopted partition 5" must be able to learn that
 * partition 5's environment index 2 did not come with it.
 *
 * Under increment 11 the survivor ATTEMPTS a restore from its own durable
 * records before naming anything — so this scenario's fixture keeps the
 * replay driver UNREADY (the environment manager not up), which is the
 * honest case where the attempt exists and cannot complete: every armed
 * record stays pending and is named unrestored. The counts below are
 * therefore also the pin that an incomplete replay NEVER reads as a
 * success. Scenario 4c covers the outcomes when the driver IS ready.
 *
 * Three properties, because each fails differently:
 *   - every environment of an ADOPTED partition is named (count is right);
 *   - an environment in a partition NOT adopted is NOT named (reporting
 *     state that is not this node's to restore would point an operator at
 *     the wrong node's work);
 *   - a recovery that adopts nothing reports ZERO, not the previous
 *     recovery's count (a stale count is a lie of the same family). */
static void test_unrestored_environments(void) {
    printf("--- Scenario 4b: the adoption names the environments it did NOT restore ---\n");

    restore_stub_reset();   /* driver unready: the restore attempt cannot complete */

    /* Two live environment records, both in partitions node 1 owned in the
     * fixture: partition 5 index 2, partition 7 index 1. A third sits in
     * partition 9, which the checkpoint does NOT carry — the witness that
     * the report is scoped to adopted partitions. */
    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_table[0].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[0].partition_id = 5;  env_ckpt_table[0].index = 2;
    env_ckpt_table[0].n_regions = 3;
    env_ckpt_table[1].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[1].partition_id = 7;  env_ckpt_table[1].index = 1;
    env_ckpt_table[1].n_regions = 3;
    env_ckpt_table[2].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[2].partition_id = 9;  env_ckpt_table[2].index = 4;
    env_ckpt_table[2].n_regions = 3;
    env_ckpt_count = 3;

    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));

    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    partition_owner_table[0].partition_id = 0; partition_owner_table[0].node_id = 2;
    partition_owner_table[0].active = 1;
    partition_owner_table[1].partition_id = 5; partition_owner_table[1].node_id = 1;
    partition_owner_table[1].active = 1;
    partition_owner_table[2].partition_id = 7; partition_owner_table[2].node_id = 1;
    partition_owner_table[2].active = 1;

    simulate_checkpoint_from_node1();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    g_local_node = 2;
    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_adopted_count() == 2, "adopted 2 partitions");
    CHECK(g_arm_count == 2,
          "*** the restore ATTEMPT was made: exactly the 2 adopted-partition records were armed for replay ***");
    CHECK(failover_last_unrestored_envs() == 2,
          "*** exactly the 2 environments of the ADOPTED partitions were named as NOT restored ***");
    CHECK(failover_last_unrestored_envs() != 3,
          "...and the environment in partition 9 (not adopted) was NOT named — the report is scoped to what this node now owns");
    CHECK(failover_last_restored_envs() == 0,
          "*** ...and NONE was counted restored — the replay could not complete, so an incomplete attempt is never a success ***");

    /* A second recovery that adopts nothing must report zero, not the
     * count left behind by the one above. */
    dspp_ckpt_recv_reset();
    rc = failover_recover_from(3);   /* no checkpoint from node 3 */
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "the second recovery adopts nothing");
    CHECK(failover_last_unrestored_envs() == 0,
          "*** ...and reports ZERO unrestored environments, not the previous recovery's 2 ***");
    CHECK(failover_last_restored_envs() == 0,
          "...and zero restored, like its unrestored sibling — both counters reset at entry");

    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_count = 0;
}

/* ─── Scenario 4c: P3 increment 11 — the restore half of destination (b) ── */
/* The survivor restores what its OWN durable storage still holds, and the
 * report is per-environment: restored, or not restored with its reason.
 * Increment 9's addendum named this half as its remaining boundary ("the
 * survivor actually recovering the environment from its own durable
 * storage — is not implemented"); this scenario pins that it now is.
 *
 * The fixture separates the two sources of truth the way the live system
 * does: the CHECKPOINT is the DEAD node's broadcast (tree built while all
 * three records were live), the TABLE is the SURVIVOR's own durable store,
 * adjusted after capture to each sub-scenario's state. In a single-process
 * test that ordering is the only way to give the tree and the table
 * different content — and they MUST be able to differ, because "the tree
 * names an environment this survivor holds no record for" is the whole
 * no-durable-storage case.
 *
 * Four properties, because each fails differently:
 *   - a present record is replayed through the standard driver and counted
 *     RESTORED, and the armed set is SCOPED to the adopted partitions (the
 *     partition-9 witness);
 *   - a record the replay REFUSED is counted unrestored, NEVER restored —
 *     both outcomes clear the pending bit, so a caller reading only pending
 *     reports a refusal as a success (the `refused-as-restored` defect);
 *   - with no durable record the environment is named unrestored and the
 *     driver is not even called (no dangling replay of nothing);
 *   - the counters reset at entry, restored beside its unrestored sibling. */
static void fixture_checkpoint_with_envs(void) {
    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_table[0].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[0].partition_id = 5;  env_ckpt_table[0].index = 2;
    env_ckpt_table[0].n_regions = 3;
    env_ckpt_table[1].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[1].partition_id = 7;  env_ckpt_table[1].index = 1;
    env_ckpt_table[1].n_regions = 3;
    env_ckpt_table[2].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[2].partition_id = 9;  env_ckpt_table[2].index = 4;
    env_ckpt_table[2].n_regions = 3;
    env_ckpt_count = 3;

    memset(partition_table, 0, sizeof(partition_table));
    memset(partition_owner_table, 0, sizeof(partition_owner_table));

    partition_table[0].partition_id = 0; partition_table[0].active = 1;
    partition_table[5].partition_id = 5; partition_table[5].active = 1;
    partition_table[7].partition_id = 7; partition_table[7].active = 1;

    partition_owner_table[0].partition_id = 0; partition_owner_table[0].node_id = 2;
    partition_owner_table[0].active = 1;
    partition_owner_table[1].partition_id = 5; partition_owner_table[1].node_id = 1;
    partition_owner_table[1].active = 1;
    partition_owner_table[2].partition_id = 7; partition_owner_table[2].node_id = 1;
    partition_owner_table[2].active = 1;

    simulate_checkpoint_from_node1();
    g_local_node = 2;
}

static void test_restore_half(void) {
    printf("--- Scenario 4c: the survivor restores what its durable storage holds ---\n");

    /* (a) both records present, the replay succeeds. */
    restore_stub_reset();
    g_replay_ready = 1;
    fixture_checkpoint_with_envs();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 2,
          "*** both adopted-partition environments were RESTORED from this survivor's durable records ***");
    CHECK(failover_last_unrestored_envs() == 0, "nothing named unrestored");
    CHECK(g_arm_count == 2, "armed exactly the 2 records of the adopted partitions");
    CHECK(stub_slot(9, 4) == -1,
          "*** ...and never armed the environment of partition 9 — the restore is scoped to what this node adopted ***");

    /* (b) the driver REFUSES one record. Both outcomes clear the pending
     * bit, so only the refused mark separates a restoration from a refusal. */
    restore_stub_reset();
    g_replay_ready = 1;
    g_replay_refuse_slot = 1;   /* the (7,1) record */
    fixture_checkpoint_with_envs();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 1,
          "*** the REFUSED record is NOT counted restored — a refusal reported as a success is the silent lie ***");
    CHECK(failover_last_unrestored_envs() == 1,
          "...and IS counted unrestored, so the gap is still visible");

    /* (c) the tree names two environments this survivor holds NO record
     * for. The capture happened while they were live; the survivor's store
     * has nothing for them (per-node stores, or a store that never saw
     * them). Named unrestored with that reason — and the driver is not even
     * called, because there is nothing durable to replay. */
    restore_stub_reset();
    g_replay_ready = 1;
    fixture_checkpoint_with_envs();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");
    memset(&env_ckpt_table[0], 0, sizeof(env_ckpt_table));  /* drop (5,2) and (7,1) */
    env_ckpt_table[0].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[0].partition_id = 9;  env_ckpt_table[0].index = 4;
    env_ckpt_table[0].n_regions = 3;
    env_ckpt_count = 1;

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 0,
          "no durable record, no restore claim");
    CHECK(failover_last_unrestored_envs() == 2,
          "*** both environments with no record on this survivor were named unrestored ***");
    CHECK(g_replay_calls == 0,
          "*** ...and the replay driver was never called — no dangling replay of nothing ***");

    /* (d) the entry reset: a second recovery that adopts nothing zeroes
     * BOTH counters rather than leaving the previous recovery's standing. */
    dspp_ckpt_recv_reset();
    rc = failover_recover_from(3);
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "the second recovery adopts nothing");
    CHECK(failover_last_restored_envs() == 0 && failover_last_unrestored_envs() == 0,
          "*** both counters zero at entry, not the previous recovery's ***");

    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_count = 0;
    restore_stub_reset();
}

/* ─── Scenario 4d: P3 (a) increment 3 — the replica is promoted ──────────── */
/* The receiver (increment 13) and the capture-side sender (increment 14)
 * make the dead node's bytes ALREADY HERE before anyone dies; this scenario
 * pins the third increment's half — what the survivor DOES with them when
 * it adopts the dead node's partition:
 *
 *   - a HOLE (no durable record) with a replica present: both adopted
 *     environments come back through the promotion + migration restore,
 *     and the (b) driver is never armed — the replica fully replaces (b)
 *     where (b) had nothing;
 *   - a hole whose promotion is REFUSED: named unrestored, and the replay
 *     is never attempted (refusal over partial application);
 *   - a hole whose replay refuses: named with the migration's own status,
 *     never counted restored;
 *   - a hole with NO replica: the honest no-record line of 4c(c), and
 *     promotion is not even attempted;
 *   - a record with an OLDER replica: (b) runs unchanged and the stale
 *     copy is left unapplied (the newer-wins rule, both directions);
 *   - a record with a NEWER replica: the replica wins when it applies, and
 *     when it does not the survivor FALLS BACK to (b) — dropping the staged
 *     slot, so exactly one driver ever owns this environment's replay.
 *
 * The partition-9 witness rides every case: it is never adopted, so no
 * replica is ever consulted for it either. */
static void fixture_hole_without_records(void) {
    fixture_checkpoint_with_envs();
    /* Drop the survivor's records for the two ADOPTED environments, keep
     * the partition-9 witness: the tree still names all three (it was built
     * while they were live), the survivor's store holds only what was never
     * in its care. Exactly 4c(c)'s fixture, factored out because five more
     * cases need the same hole. */
    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_table[0].magic = ENV_CKPT_REC_MAGIC;
    env_ckpt_table[0].partition_id = 9;  env_ckpt_table[0].index = 4;
    env_ckpt_table[0].n_regions = 3;
    env_ckpt_count = 1;
}

static void test_replica_promotion(void) {
    printf("--- Scenario 4d: a survivor promotes the dead node's replica ---\n");

    /* (a) HOLE + replica present: restored via the replica, (b) untouched. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 5; g_rep_src = 1;
    fixture_hole_without_records();
    CHECK(dspp_ckpt_recv_ready(), "checkpoint from node 1 received");

    int rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 2,
          "*** both hole environments came back from the REPLICA — promoted and replayed through the migration restore ***");
    CHECK(failover_last_unrestored_envs() == 0, "nothing named unrestored");
    CHECK(g_promote_calls == 2,
          "the promotion was attempted exactly once per adopted environment");
    CHECK(g_arm_count == 0 && g_replay_calls == 0,
          "*** ...and the durable-record driver was never armed or called: on a hole the replica fully replaces (b) ***");

    /* (b) HOLE + promotion REFUSED: named, and no replay is attempted. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 5;
    g_promote_rc = ENV_REPLICA_PROMOTE_REFUSE_IO;
    fixture_hole_without_records();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_unrestored_envs() == 2,
          "*** a replica that could not be staged is named NOT restored — never silently dropped ***");
    CHECK(failover_last_restored_envs() == 0, "...and never counted restored");
    CHECK(g_migrate_restore_calls == 0,
          "*** ...and the replay was never attempted on a refused promotion (refusal over partial application) ***");

    /* (c) HOLE + staged, but the REPLAY refuses: the migration's own
     * status is what classifies it. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 5;
    g_migrate_restore_rc = ENV_MIGRATE_REFUSE_CREATE;
    fixture_hole_without_records();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_unrestored_envs() == 2 && failover_last_restored_envs() == 0,
          "*** a replica whose replay refused is named NOT restored, with the migrate status ***");
    CHECK(g_migrate_restore_calls == 2, "...and the replay was genuinely attempted for each");

    /* (d) HOLE + NO replica: 4c(c)'s honest line, promotion not attempted. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 0;
    fixture_hole_without_records();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_unrestored_envs() == 2 && failover_last_restored_envs() == 0,
          "no record and no replica: both environments named NOT restored");
    CHECK(g_promote_calls == 0,
          "*** ...and promotion is never attempted when the directory holds nothing — the absent answer comes from the lookup ***");
    CHECK(g_replay_calls == 0, "...and the (b) driver is not called either");

    /* (e) record + OLDER replica: (b) runs unchanged; the stale copy is
     * left unapplied (the fixture's records carry sequence 0). */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 0;
    fixture_checkpoint_with_envs();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 2 && g_arm_count == 2,
          "*** a replica OLDER than the durable record is not applied: (b) runs unchanged ***");
    CHECK(g_promote_calls == 0,
          "*** ...and the promotion entry is never called for the stale copy ***");

    /* (f) record + NEWER replica, promotion applies: the replica wins and
     * (b) never runs. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 9;
    fixture_checkpoint_with_envs();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 2,
          "*** a replica NEWER than the durable record wins — the receiver's own newer-wins rule, applied at promotion ***");
    CHECK(g_arm_count == 0 && g_promote_calls == 2,
          "...and the older record's driver was never armed (exactly one replay path per environment)");

    /* (g) record + NEWER replica, promotion stages but the replay REFUSES:
     * fall back to (b), and drop the staged slot so the migrate sweep's own
     * restore loop cannot later replay over the record (b) chose. */
    replica_stub_reset();
    restore_stub_reset();
    g_replay_ready = 1;
    g_rep_present = 1; g_rep_seq = 9;
    g_migrate_restore_rc = ENV_MIGRATE_REFUSE_PARKED;
    fixture_checkpoint_with_envs();

    rc = failover_recover_from(1);
    CHECK(rc == FAILOVER_OK, "recovery succeeds");
    CHECK(failover_last_restored_envs() == 2 && g_arm_count == 2,
          "*** a newer replica whose replay refuses falls BACK to (b): the older record still restores through the driver ***");
    CHECK(g_recv_drop_calls == 2,
          "*** ...and the staged slot was dropped on the way: exactly one driver owns this environment's replay ***");
    CHECK(failover_last_unrestored_envs() == 0, "the fallback is not reported as a gap");

    replica_stub_reset();
    restore_stub_reset();
    memset(env_ckpt_table, 0, sizeof(env_ckpt_table));
    env_ckpt_count = 0;
}

/* ─── Scenario 5: wrong source node in checkpoint ──────────────────── */
static void test_wrong_source(void) {
    printf("--- Scenario 5: checkpoint from wrong source rejected ---\n");

    /* Simulate checkpoint from node 1, then try to recover "node 3" */
    simulate_checkpoint_from_node1();
    int rc = failover_recover_from(3);
    CHECK(rc == FAILOVER_NO_CHECKPOINT, "rejects checkpoint not from dead node");
}

/* ─── Scenario 6: unknown peer returns -1 ──────────────────────────── */
static void test_unknown_peer(void) {
    printf("--- Scenario 6: unknown peer status ---\n");
    failover_init();
    CHECK(failover_peer_status(99) == -1, "unknown peer returns -1");
}

int main(void) {
    test_liveness();
    test_relaunch_grace();
    test_resurrection();
    test_no_checkpoint();
    test_successful_failover();
    test_unrestored_environments();
    test_restore_half();
    test_replica_promotion();
    test_wrong_source();
    test_unknown_peer();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
