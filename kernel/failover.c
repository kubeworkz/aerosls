/*
 * failover.c — node failure detection and checkpoint-based recovery
 * (Core Backup Strategies, Step 5).
 *
 * Freestanding: local helpers, no libc.
 */
#include "failover.h"
#include "env_migrate.h"    /* env_migrate_in_flight() — the migration deferral gate */
#include "env_ckpt.h"       /* P3 increment 11: the durable records the restore half replays from */
#include "env_service.h"    /* P3 increment 11: env_service_restore_pending(), the standard replay driver */
#include "state_tree.h"
#include "partition.h"
#include "kernel_io.h"
#include "timer.h"           /* kernel_tick_counter — failover_sweep_keepalive()'s now */
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
static uint32_t last_restored_envs = 0;

/* The armed set of ONE recovery (P3 increment 11): the (partition, index)
 * pairs whose durable records this pass put into the replay's pending set,
 * so the driver's outcome can be classified PER RECORD afterwards. File
 * scope, not stack: failover_recover_from()'s frame already carries the
 * 16 KiB receive buffer and the full node array, and this is called from the
 * BSP sweep — same context, never re-entered (the same single-threaded
 * posture every table in this file already assumes). */
static uint32_t fo_armed_p[ENV_CKPT_MAX];
static uint32_t fo_armed_idx[ENV_CKPT_MAX];
static uint32_t fo_armed_count = 0;

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
     * recovery would be reading the last one's gap as this one's. The
     * restored counter is reset beside its unrestored sibling for the same
     * reason (P3 increment 11). */
    last_unrestored_envs = 0;
    last_restored_envs = 0;
    fo_armed_count = 0;

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

    /* POSIX-Environments v0.2 Phase P3, failover destination (b) (§11 Q5),
     * increments 9 and 11. The adoption above flips OWNERSHIP. What the tree
     * carries is the dead node's environment NAMES (ST_NODE_ENVIRONMENT,
     * kernel/state_tree.c); what THIS node can restore from is its OWN
     * durable record store (env_ckpt_table, written by persist_environments()
     * and adopted at boot — the survivor's copy exists exactly when its own
     * boot saw the record, the shared single-level store's normal case).
     *
     * Increment 9 named every environment of an ADOPTED partition as not
     * restored — the report was the whole deliverable, and its own addendum
     * recorded the boundary: "the restore half — the survivor actually
     * recovering the environment from its own durable storage — is not
     * implemented". Increment 11 implements it. Per environment, and per
     * outcome: a present record is ARMED and replayed through the same
     * driver a reboot uses (env_service_restore_pending(): create round
     * trip, identity check, pause ledger, payload pour); an absent record is
     * named, with that reason. A blanket "failover adopts partitions, not
     * environments" would now be the lie, because whether an environment
     * comes back is a fact this node can check rather than assume.
     *
     * Reported for the ADOPTED partitions only: an environment in a
     * partition this node did not adopt belongs to whoever owns that
     * partition now, and acting on it here would point an operator at state
     * that is not this node's to restore. The `silent-failover`,
     * `unscoped-report` and `no-durable-restore` teeth hold these lines. */
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i].type != ST_NODE_ENVIRONMENT) continue;
        uint32_t part_id = nodes[i].partition_id;
        if (partition_get_owner_node(part_id) != my_id) continue;

        uint32_t env_idx = (uint32_t)nodes[i].entity_id;

        /* The durable record for this (partition, index) on THIS survivor.
         * A direct table walk for the same reason state_tree.c walks it:
         * the table is the record store's live image, and this lookup is
         * the whole "from its own durable storage" question — present means
         * restorable, absent means the name is the report. */
        const struct EnvCkptRecord* rec = (const struct EnvCkptRecord*)0;
        for (uint32_t r = 0; r < env_ckpt_count && r < ENV_CKPT_MAX; r++) {
            if (env_ckpt_table[r].partition_id == part_id &&
                env_ckpt_table[r].index        == env_idx) {
                rec = &env_ckpt_table[r];
                break;
            }
        }
        if (!rec) {
            last_unrestored_envs++;
            kernel_serial_printf(
                "[FAILOVER] environment index %llu (partition %u, %llu region(s)) "
                "was NOT restored -- no durable record for it on this survivor; "
                "recreate it\n",
                (unsigned long long)env_idx, part_id,
                (unsigned long long)nodes[i].size);
            continue;
        }
        if (env_ckpt_restore_arm(part_id, env_idx) != 0 ||
            fo_armed_count >= ENV_CKPT_MAX) {
            // Defensive: the record was just found above, so the arm can only
            // fail if the table moved underneath this walk. Refused rather
            // than silently dropped — an environment this pass could not even
            // TRY belongs in the unrestored count, not in no count at all.
            last_unrestored_envs++;
            kernel_serial_printf(
                "[FAILOVER] environment index %llu (partition %u) was NOT "
                "restored -- its durable record exists but could not be armed "
                "for replay\n",
                (unsigned long long)env_idx, part_id);
            continue;
        }
        fo_armed_p[fo_armed_count]  = part_id;
        fo_armed_idx[fo_armed_count] = env_idx;
        fo_armed_count++;
    }

    /* Drain exactly what was armed through the standard replay driver. The
     * pending set normally holds nothing else (the boot drains its own at
     * startup), so this pass is the adopted partitions' records; the driver
     * is REUSED rather than duplicated because the create round trip, the
     * identity check, the pause ledger and the payload pour are all its, and
     * a second copy of those is a second place for them to rot. Runs on the
     * BSP sweep — the same context the migration destination's restore
     * already runs in (env_migrate.c), with the same yielding-before-pour
     * discipline env_payload.h requires. */
    if (fo_armed_count > 0) {
        struct EnvSvcRestoreReport rep;
        int rrc = env_service_restore_pending(&rep);
        for (uint32_t a = 0; a < fo_armed_count; a++) {
            uint32_t p = fo_armed_p[a], idx = fo_armed_idx[a];
            /* The three outcomes, from the two facts that tell them apart.
             * Still-pending means the driver never got to this record (rrc
             * != 0: the environment manager is not up). Refused means it got
             * there and was refused — and the refused mark is the ONLY way
             * to know, because both outcomes clear the pending bit. Reading
             * pending alone would count a REFUSED record as restored: the
             * `refused-as-restored` lie, which is why clause K of the guard
             * exists and why that tooth is the sharpest in the smoke. */
            if (rrc != 0 || env_ckpt_restore_pending_record(p, idx)) {
                last_unrestored_envs++;
                kernel_serial_printf(
                    "[FAILOVER] environment index %u (partition %u) was NOT "
                    "restored -- the replay did not complete on this survivor "
                    "(rc=%d); recreate it\n",
                    (unsigned)idx, (unsigned)p, rrc);
            } else if (env_ckpt_restore_refused_record(p, idx)) {
                last_unrestored_envs++;
                kernel_serial_printf(
                    "[FAILOVER] environment index %u (partition %u) was NOT "
                    "restored -- the replay REFUSED its durable record (reason "
                    "printed by [ENV_RESTORE] above); recreate it\n",
                    (unsigned)idx, (unsigned)p);
            } else {
                last_restored_envs++;
                kernel_serial_printf(
                    "[FAILOVER] environment index %u (partition %u) RESTORED "
                    "from this survivor's durable record, replayed through "
                    "create\n", (unsigned)idx, (unsigned)p);
            }
        }
    }

    if (last_unrestored_envs > 0) {
        kernel_serial_printf(
            "[FAILOVER] %u environment(s) of the adopted partitions were NOT "
            "restored (named above) -- their payload lived on node %u's own "
            "storage and does not follow ownership\n",
            (unsigned)last_unrestored_envs, dead_node_id);
    }
    if (last_restored_envs > 0) {
        // Contents, honestly scoped: the replay brings the ENVIRONMENT back
        // (identity, sidecars, bindings). Its bytes follow only as far as
        // the durable extent is readable on this survivor, and the payload
        // layer names that refusal itself ([ENV_PAYLOAD]) — so this line
        // claims the environment, never the contents.
        kernel_serial_printf(
            "[FAILOVER] %u environment(s) of the adopted partitions were "
            "restored from durable storage; contents follow only as far as "
            "each durable extent is readable on this survivor ([ENV_PAYLOAD] "
            "names what is not)\n",
            (unsigned)last_restored_envs);
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
 * as NOT restored (POSIX-Environments P3, destination (b), increment 9).
 * Countable rather than only logged so a host test can pin the property and
 * an operator can read it back over the control plane. */
uint32_t failover_last_unrestored_envs(void) { return last_unrestored_envs; }

/* How many the last recovery actually RESTORED from this survivor's own
 * durable records (P3 increment 11, the restore half of destination (b)).
 * See failover.h for why a refused record counts here as unrestored. */
uint32_t failover_last_restored_envs(void) { return last_restored_envs; }

/* ─── Live checkpoint broadcast (leader -> followers) ────────────────── */
static uint64_t failover_last_ckpt_tick = 0;

void failover_sweep_keepalive(void) {
    /* The sweep's two liveness streams, driven from the middle of a
     * blocking wait: the heartbeat (election watchdogs + last_seen_tick on
     * every observer) and the checkpoint broadcast (the adoption payload).
     * Both gates live in the callees, so calling this at spin rate is a
     * no-op almost always — and a no-op is the point: a create/migrate/
     * restore block that keeps these flowing never deposes its own node,
     * and a node never deposed never turns into the quiet follower that
     * this subsystem would misdeclare dead (the release-vs-heartbeat race,
     * 2026-10-23). Read the tick here rather than trusting a caller's
     * sampled `now`: waits call this precisely because their notion of
     * "later" is frozen at loop entry. */
    uint64_t now = kernel_tick_counter;
    check_consensus_heartbeat_tick(now);
    failover_live_checkpoint_broadcast(now);
}

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
