/*
 * failover.c — node failure detection and checkpoint-based recovery
 * (Core Backup Strategies, Step 5).
 *
 * Freestanding: local helpers, no libc.
 */
#include "failover.h"
#include "env_migrate.h"    /* env_migrate_in_flight() — the migration deferral gate */
#include "state_tree.h"
#include "partition.h"
#include "kernel_io.h"
#include "../net/dspp.h"
#include "../net/consensus.h"

/* ─── Local helpers ──────────────────────────────────────────────────── */
static void fo_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

/* Synthesized name for an adopted partition ("recovered-N"). No libc, so
 * the decimal suffix is hand-rolled; the checkpoint carries no names. */
static void fo_recovered_name(char* dst, uint32_t part_id) {
    static const char prefix[] = "recovered-";
    char tmp[12];
    int n = 0;
    uint32_t v = part_id;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (prefix[i] && i < PARTITION_NAME_LEN - 1) { dst[i] = prefix[i]; i++; }
    while (n > 0 && i < PARTITION_NAME_LEN - 1) dst[i++] = tmp[--n];
    dst[i] = '\0';
}

/* ─── Peer liveness table ────────────────────────────────────────────── */
static struct PeerLiveness peers[FAILOVER_MAX_PEERS];
static uint32_t last_adopted = 0;
static uint32_t last_unrestored_envs = 0;

void failover_init(void) {
    fo_memset(peers, 0, sizeof(peers));
    last_adopted = 0;
    kernel_serial_print("[FAILOVER] Subsystem initialised.\n");
}

static struct PeerLiveness* find_peer(uint32_t node_id) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++)
        if (peers[i].active && peers[i].node_id == node_id)
            return &peers[i];
    return 0;
}

static struct PeerLiveness* alloc_peer(uint32_t node_id) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++) {
        if (!peers[i].active) {
            peers[i].node_id = node_id;
            peers[i].active  = 1;
            peers[i].status  = PEER_ALIVE;
            return &peers[i];
        }
    }
    return 0;
}

void failover_note_heartbeat(uint32_t node_id, uint64_t now) {
    if (node_id == 0 || node_id == cluster_local_node_id()) return;

    struct PeerLiveness* p = find_peer(node_id);
    if (!p) p = alloc_peer(node_id);
    if (!p) return;  /* table full */

    p->last_seen_tick = now;
    p->defer_noted = 0;   /* seen: a later silence earns its own DEFERRED line */
    if (p->status == PEER_DEAD) {
        kernel_serial_printf("[FAILOVER] Node %u came back (was dead)\n", node_id);
    }
    p->status = PEER_ALIVE;
}

void failover_tick(uint64_t now) {
    for (int i = 0; i < FAILOVER_MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        if (peers[i].status == PEER_DEAD) continue;

        /* Saturating, not wrapping (observed live 2026-10-13: a DEFERRED
         * line printed "silent 18446744073709551606 ticks" = -10 in
         * unsigned). The two operands come from DIFFERENT contexts, so the
         * stamp can legitimately land AHEAD of the sample: `now` is read
         * once at this function's call site on the BSP sweep
         * (net/http.c), while last_seen_tick is stamped by the heartbeat
         * RX path (process_consensus_packet -> failover_note_heartbeat,
         * fed from the timer ISR / AP core). A heartbeat processed between
         * the sweep's read of kernel_tick_counter and this comparison
         * leaves last_seen_tick > now, and the unsigned subtraction wraps
         * to ~2^64 — comfortably past FAILOVER_DEAD_TICKS. The DEFERRED
         * lines below merely printed a silly number; the same arithmetic
         * on an ungated peer would declare DEAD the very sweep the peer
         * was heard on. A stamp at or after `now` means the peer was seen
         * at least as recently as this sample: silence is zero. */
        uint64_t silence = (now > peers[i].last_seen_tick)
                         ? now - peers[i].last_seen_tick : 0;
        if (silence >= FAILOVER_DEAD_TICKS) {
            /* P3 live arm (2026-10-13): a silence this node's OWN
             * environment migration produced is not a death. The source's
             * synchronous payload send and the destination's restore (create
             * round trip + parked-posture wait) both run on the BSP sweep —
             * the same context that transmits the consensus heartbeat — so
             * either end can pass this threshold while both are alive. The
             * live 2-node TCG run did exactly that: BOTH nodes declared the
             * other DEAD and BOTH adopted partition 1, flipping ownership
             * back onto the sender, where the confirmation teardown then
             * correctly refused ("this node owns the partition again") —
             * three correct behaviors composing into one stuck migration.
             * Defer the declaration while env_migrate_in_flight() covers
             * the silence; a peer that really dies is declared the moment
             * the migration resolves (the deferral's inputs are bounded). */
            /* The idle-observer arm (docs/AeroSLS-Idle-Observer-Adoption-
             * Finding-v0.1.md): the local gate below reads THIS node's
             * migration state, which a third node correctly knows nothing
             * about. The live 3-node runs showed what that costs: the busy
             * SOURCE stopped beating (its blocking work runs on the sweep
             * that transmits the leader's heartbeat), a healthy observer won
             * the election the blocked node could not contest and adopted
             * every partition in its checkpoint — every node alive, the
             * migrating environment destroyed. The source announced its
             * intent before blocking (env_migrate_intent_begin()); that
             * announce is the knowledge this gate asks for. Bounded like
             * every other face: the budget expires and a genuinely dead
             * peer is declared. */
            if (env_migrate_peer_in_flight(peers[i].node_id)) {
                if (!peers[i].defer_noted) {
                    peers[i].defer_noted = 1;
                    kernel_serial_printf(
                        "[FAILOVER] Node %u silent %llu ticks -- declaration "
                        "DEFERRED: node %u announced environment-migration "
                        "intent\n",
                        peers[i].node_id, (unsigned long long)silence,
                        peers[i].node_id);
                }
                continue;
            }
            if (env_migrate_in_flight()) {
                if (!peers[i].defer_noted) {
                    peers[i].defer_noted = 1;
                    kernel_serial_printf(
                        "[FAILOVER] Node %u silent %llu ticks -- declaration "
                        "DEFERRED: an environment migration is in flight on "
                        "this node\n",
                        peers[i].node_id, (unsigned long long)silence);
                }
                continue;
            }
            if (peers[i].status != PEER_DEAD) {
                peers[i].status = PEER_DEAD;
                peers[i].declared_dead_tick = now;
                kernel_serial_printf(
                    "[FAILOVER] Node %u declared DEAD (silent %llu ticks)\n",
                    peers[i].node_id, silence);
                /* Only the leader recovers: a partition has exactly one
                 * owner, and this node is the one the cluster elected to
                 * lead after the death (election timeout 150 < the 300
                 * tick death threshold, so the new leader is already in
                 * place when the death is declared). A follower declaring
                 * the same peer dead observes only. The checkpoint the
                 * leader holds is the dead node's own last broadcast
                 * (failover_live_checkpoint_broadcast(), same sweep). */
                if (cluster_is_leader()) {
                    int rc = failover_recover_from(peers[i].node_id);
                    kernel_serial_printf(
                        "[FAILOVER] recovery for dead node %u: rc=%d (%s)\n",
                        peers[i].node_id, rc,
                        rc == FAILOVER_OK ? "OK - adopted" :
                        rc == FAILOVER_NO_CHECKPOINT ? "no checkpoint held" :
                        rc == FAILOVER_NO_PARTITIONS ? "no adoptable partitions" :
                        "deserialize error");
                }
            }
        } else if (silence >= FAILOVER_DEAD_TICKS / 2) {
            peers[i].status = PEER_SUSPECTED;
        }
    }
}

int failover_peer_status(uint32_t node_id) {
    struct PeerLiveness* p = find_peer(node_id);
    if (!p) return -1;
    return p->status;
}

/* ─── Recovery: adopt partitions from a received checkpoint ──────────── */
int failover_recover_from(uint32_t dead_node_id) {
    last_adopted = 0;
    /* Reset at ENTRY, not in the reporting block below: a recovery that
     * returns early (no checkpoint, wrong source, no partitions) reported no
     * environments, and a caller reading a stale count from the PREVIOUS
     * recovery would be reading the last one's gap as this one's. */
    last_unrestored_envs = 0;

    if (!dspp_ckpt_recv_ready()) return FAILOVER_NO_CHECKPOINT;

    /* Deserialize the received state tree */
    uint32_t recv_size = dspp_ckpt_recv_size();
    uint8_t recv_buf[ST_SERIAL_MAX];
    if (recv_size > sizeof(recv_buf)) return FAILOVER_DESERIALIZE_ERR;

    dspp_ckpt_recv_copy(recv_buf, sizeof(recv_buf));

    struct StateTreeHeader hdr;
    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_deserialize(recv_buf, recv_size, &hdr, nodes, ST_MAX_NODES);
    if (count == 0) return FAILOVER_DESERIALIZE_ERR;

    /* Verify the checkpoint came from the dead node */
    if (hdr.node_id != dead_node_id) {
        kernel_serial_printf(
            "[FAILOVER] Checkpoint is from node %u, not dead node %u\n",
            hdr.node_id, dead_node_id);
        return FAILOVER_NO_CHECKPOINT;
    }

    /* Adopt partitions: for each partition node in the tree, transfer
     * ownership to this node.
     *
     * The provenance replaces an ownership lookup: the checkpoint came
     * FROM dead_node_id (checked above), and the state tree is built from
     * the producer's OWN local partition table (state_tree_build()
     * iterates partition_table[]), so every partition node in it was, by
     * construction, owned by the dead node. The earlier code re-checked
     * this against the SURVIVOR's local table -- which never contains
     * another node's partitions, because partition tables are local-only
     * in this kernel (partition_create() stamps owner = local node). A
     * live cluster therefore never adopted anything; only the host test,
     * which pre-populates the survivor's table with the dead node's
     * partitions, saw the check pass. */
    uint32_t adopted = 0;
    uint32_t my_id = cluster_local_node_id();

    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type != ST_NODE_PARTITION) continue;
        uint32_t part_id = (uint32_t)nodes[i].entity_id;

        /* PARTITION_SYSTEM (id 0) never migrates: every node owns its own
         * system partition (partition_init() stamps it local). Adopting it
         * would steal another node's system partition. */
        if (part_id == PARTITION_SYSTEM || part_id >= PARTITION_MAX) continue;

        /* The survivor's table may have no row for this partition at all
         * (the live case -- the dead node created it). Create the row so
         * the ownership handoff has something to write to. The id is
         * preserved because catalog objects reference partition_id; the
         * name is synthesized because the checkpoint carries no names. */
        if (!partition_exists(part_id)) {
            partition_table[part_id].partition_id = part_id;
            fo_recovered_name(partition_table[part_id].name, part_id);
            partition_table[part_id].active = 1;
        }

        partition_set_owner_node(part_id, my_id);
        adopted++;
        kernel_serial_printf(
            "[FAILOVER] Adopted partition %u from dead node %u\n",
            part_id, dead_node_id);
    }

    if (adopted == 0) return FAILOVER_NO_PARTITIONS;

    /* POSIX-Environments v0.2 Phase P3, failover destination (b) (§11 Q5).
     * The adoption above flips OWNERSHIP and restores no environment: the
     * payload is 5.5 MiB of regions and this checkpoint is a 24 KiB
     * broadcast, so the bytes were never here to begin with. What the tree
     * DOES carry is the dead node's environment NAMES (ST_NODE_ENVIRONMENT,
     * kernel/state_tree.c), and this is where that earns its keep.
     *
     * Every environment whose partition this node just adopted is reported
     * BY NAME — partition, index, region count — as not restored. Without
     * this the adoption is silent about them: an operator sees "Adopted
     * partition 3" and has no way to learn that partition 3's environment
     * index 2 went with the dead node. The gap is real either way; naming it
     * is the whole of what destination (b) promises, and the
     * `silent-failover` tooth reddens if this loop is deleted.
     *
     * Reported for the ADOPTED partitions only: an environment in a
     * partition this node did not adopt belongs to whoever owns that
     * partition now, and reporting it here would point an operator at state
     * that is not this node's to restore. */
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type != ST_NODE_ENVIRONMENT) continue;
        uint32_t part_id = nodes[i].partition_id;
        if (partition_get_owner_node(part_id) != my_id) continue;

        last_unrestored_envs++;
        kernel_serial_printf(
            "[FAILOVER] environment index %llu (partition %u, %llu region(s)) "
            "was NOT restored -- failover adopts partitions, not environments; "
            "restore it from its own durable storage or recreate it\n",
            (unsigned long long)nodes[i].entity_id, part_id,
            (unsigned long long)nodes[i].size);
    }
    if (last_unrestored_envs > 0) {
        kernel_serial_printf(
            "[FAILOVER] %u environment(s) of the adopted partitions were NOT "
            "restored (named above) -- their data lived on node %u's own "
            "storage and does not follow ownership\n",
            (unsigned)last_unrestored_envs, dead_node_id);
    }

    last_adopted = adopted;
    dspp_ckpt_recv_reset();

    kernel_serial_printf(
        "[FAILOVER] Recovery complete: adopted %u partition(s) from node %u "
        "(checkpoint seq=%llu)\n", adopted, dead_node_id, hdr.sequence);
    return FAILOVER_OK;
}

uint32_t failover_last_adopted_count(void) { return last_adopted; }

/* How many environments of the adopted partitions the last recovery named
 * as NOT restored (POSIX-Environments P3, destination (b)). Countable
 * rather than only logged so a host test can pin the property and an
 * operator can read it back over the control plane. */
uint32_t failover_last_unrestored_envs(void) { return last_unrestored_envs; }

/* ─── Live checkpoint broadcast (leader -> followers) ────────────────── */
static uint64_t failover_last_ckpt_tick = 0;

void failover_live_checkpoint_broadcast(uint64_t now) {
    /* Only the leader produces checkpoints: it is the only node whose
     * partition table is authoritative for what it owns, and the
     * followers' single-slot RX buffers hold the transfer until
     * failover_recover_from() consumes it after the leader dies.
     * Rate-limited like the consensus heartbeat (the sweep is
     * load-dependent and unbounded). */
    if (!cluster_is_leader()) return;
    if (now - failover_last_ckpt_tick < FAILOVER_CKPT_PERIOD_TICKS) return;
    failover_last_ckpt_tick = now;

    struct StateTreeNode nodes[ST_MAX_NODES];
    uint32_t count = state_tree_build(nodes, ST_MAX_NODES);
    if (count == 0) return;

    uint8_t buf[ST_SERIAL_MAX];
    uint32_t size = state_tree_serialize(nodes, count, now, buf, sizeof(buf));
    if (size == 0) return;

    for (uint32_t i = 0; i < cluster_roster_count; i++) {
        if (!cluster_roster[i].active) continue;
        uint32_t peer = cluster_roster[i].node_id;
        if (peer == 0 || peer == cluster_local_node_id()) continue;
        dspp_ckpt_send(now, peer, now, buf, size);
    }
}
