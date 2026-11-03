#ifndef FAILOVER_H
#define FAILOVER_H

#include <stdint.h>

/*
 * failover.h — node failure detection and checkpoint-based recovery
 * (Core Backup Strategies, Step 5).
 *
 * Tracks peer node liveness via heartbeat observation. When a peer is
 * declared dead, triggers failover: loads the most recent checkpoint
 * received from that peer (via DSPP, Step 3), deserializes the state tree,
 * and adopts ownership of the dead node's partitions.
 *
 * Builds on:
 * - net/consensus.h's heartbeat timing constants
 * - net/dspp_checkpoint.c's received checkpoint buffer
 * - kernel/state_tree.h's deserialize()
 * - kernel/partition.h's partition_set_owner_node()
 */

// ─── Peer Liveness ────────────────────────────────────────────────────────────
#define FAILOVER_MAX_PEERS       8
#define FAILOVER_DEAD_TICKS    300u  /* 3 seconds at ~100 Hz — 2x election timeout */
#define FAILOVER_CKPT_PERIOD_TICKS 100u  /* leader checkpoint broadcast period */

/* ─── The silence grace: one bounded defer per silent episode ──────────
 * FAILOVER_DEAD_TICKS says when a peer is SUSPECTED. It is not, by itself,
 * when it is DECLARED: a live peer whose beats stop for 300 ticks is
 * indistinguishable from a dead one, and on this cluster they do stop — the
 * guests stall for whole seconds at a time (TCG, no KVM access, images and
 * serial logs on /mnt/c), measured directly: an HTTP probe timed out twice
 * at 3.0 s while the host sat 70% idle.
 *
 * Every declaration the death watch made in three consecutive live suite
 * runs was false, and the silences they were made on were all just past the
 * line: 300, 301, 302, 303, 304, 305, 306, 309, 311, 316, 318, 319, 321, 322,
 * 324, 328, 332, 338, 342, 349, 356, 374 — and once 596. Not one of them was
 * a death: each peer was answering HTTP lines later, and each one printed
 * "came back" immediately after.
 *
 * So the declaration line moves: silence >= FAILOVER_DEAD_TICKS still
 * SUSPECTS, and the declaration additionally requires the episode grace to
 * have run — FAILOVER_DECLARE_TICKS = DEAD + this. The first cut was 900
 * (1200 total: 4x the election timeout, 20 liveness beats), sized against
 * the worst stall then observed — 596 — twice over.
 *
 * ─── Raised 900 -> 5700 (2026-10-24, the run that sized it again) ──────
 * The next live 3-node suite run made four declarations and EVERY one was
 * false: 1200 (the guard's own SIGKILL — correct), then 1205, 1220 and
 * 1232 ticks of silence against peers that were alive, each followed by
 * "came back" — one of them a node no guard had touched at all. The four
 * landings just past the line mean the stalls were not 1200 ticks, they
 * were LONGER than 1200: the declaration truncates the measurement, so the
 * stall's length has to be bounded from the other end, by the peer's next
 * beat. It is: the relaunch grace pinned the declaration after tick 26227
 * (the peer had been heard at 20227 + the 6000-tick grace), and the peer's
 * own "came back" stamped 27412 and 27931 — stalls of up to ~2900 ticks
 * (29 s) on a cluster where nothing was dead, produced by TCG scheduling
 * while the survivors did 24 adoptions with an NVMe snapshot write each.
 *
 * So the grace now EQUALS the relaunch grace: we never declare a peer dead
 * in a window shorter than the window we already grant a node we KNOW just
 * rebooted and is hammering that same NVMe. 6000 covers the worst stall
 * ever measured (~2900) twice over, with the boot window as the precedent
 * rather than a number picked to clear one run. It costs detection latency,
 * not detection: the silence keeps accruing across the grace, so a peer
 * that really dies is declared on the first tick past the total, and every
 * gate that defers for its own reason (migration in flight, announced
 * intent, relaunch) still defers past it — both halves pinned by
 * tests/checkpoint_recovery_host_test.c, Scenario 1. */
#define FAILOVER_SILENCE_GRACE_TICKS 5700u
#define FAILOVER_DECLARE_TICKS  (FAILOVER_DEAD_TICKS + FAILOVER_SILENCE_GRACE_TICKS) /* 6000 */

/* ─── The relaunch grace ────────────────────────────────────────────────
 * A just-relaunched node is silent for exactly the wrong reasons: its own
 * boot restores the catalog, the partition table and every durable record
 * from NVMe on the sweep, and that burst of I/O stalls the PEERS it is
 * trying to rejoin. Both directions then read as death. The live 3-node
 * suite run (2026-10-24) showed both faces of it:
 *
 *   • the relaunched node as DECLARER: a freshly booted node 1 declared
 *     node 2 DEAD five times in a row (silent 328/319/301/324/332 ticks)
 *     between its own boot's heartbeat 449 and 572 — every one followed
 *     lines later by "came back", because nothing was actually dead;
 *
 *   • the relaunched node as DECLARED: failover_adoption_live_check's
 *     step-11 destination was SIGKILLed mid-migration and relaunched, and
 *     106 log lines after its own "Node 3 came back (was dead)" node 1
 *     printed "Node 3 declared DEAD (silent 300 ticks)" and RE-ADOPTED
 *     all 10 partitions — flipping partition 9's ownership back onto the
 *     adopter and failing the guard's convergence gate on a node whose
 *     HTTP answered 200 the whole time.
 *
 * So the watch grants a grace — bounded, like every other face of this
 * gate: on this node's own boot (failover_note_relaunch_grace(), called
 * once from the kernel boot path beside failover_init()) and on a peer's
 * DEAD→ALIVE return (armed inside failover_note_heartbeat()), no
 * declaration is made until FAILOVER_RELAUNCH_GRACE_TICKS have elapsed.
 * 6000 ticks is 60 seconds at the documented ~100 Hz — long enough to
 * cover the boot/restore burst and the whole post-relaunch convergence
 * window the guard waits on, and short enough that a death during the
 * window is DELAYED, never lost: the silence keeps accruing and the
 * declaration fires on the first tick after the grace expires (pinned by
 * tests/checkpoint_recovery_host_test.c, Scenario 1b, which asserts both
 * halves — held during, declared after — because a grace that never ends
 * would be a blindfold, not a fix). */
#define FAILOVER_RELAUNCH_GRACE_TICKS 6000u

/* Peer liveness state */
#define PEER_ALIVE     0
#define PEER_SUSPECTED 1
#define PEER_DEAD      2

struct PeerLiveness {
    uint32_t node_id;
    uint8_t  status;         /* PEER_ALIVE / SUSPECTED / DEAD */
    uint8_t  active;
    uint64_t last_seen_tick; /* kernel_tick_counter when last heartbeat received */
    uint64_t declared_dead_tick;
    uint8_t  defer_noted;    /* one DEFERRED line per silent episode, not one per
                              * sweep — see failover_tick()'s migration gate */
    uint8_t  grace_noted;    /* likewise for the silence grace's own line */
    uint64_t grace_until_tick; /* 0 = no grace; set when a DEAD peer comes back:
                              * declarations are held until this tick (see
                              * FAILOVER_RELAUNCH_GRACE_TICKS) */
};

// ─── Failover Result ──────────────────────────────────────────────────────────
#define FAILOVER_OK              0
#define FAILOVER_NO_CHECKPOINT   1  /* no checkpoint data from that peer */
#define FAILOVER_DESERIALIZE_ERR 2
#define FAILOVER_NO_PARTITIONS   3  /* checkpoint had no partitions to adopt */

// ─── Public API ───────────────────────────────────────────────────────────────

/* Initialize failover subsystem. */
void failover_init(void);

/* Record that we heard from node_id at time `now`. */
void failover_note_heartbeat(uint32_t node_id, uint64_t now);

/* Tick: check all peers for liveness. Call from the BSP sweep. */
void failover_tick(uint64_t now);

/* Query a peer's status. Returns PEER_ALIVE/SUSPECTED/DEAD, or -1 if unknown. */
int failover_peer_status(uint32_t node_id);

/* Arm THIS node's relaunch grace: for FAILOVER_RELAUNCH_GRACE_TICKS from
 * `now` failover_tick() declares no peer dead. Called once from the boot
 * path right after failover_init() — a node that has just come up is
 * restoring its durable state on the very sweep the watch runs on, and its
 * boot's I/O stalls the peers it is rejoining (see the constant's comment
 * for both live faces). The peer-side half needs no call: a peer that comes
 * back from DEAD arms its own window inside failover_note_heartbeat(). */
void failover_note_relaunch_grace(uint64_t now);

/* Attempt to recover dead_node_id's partitions from a received checkpoint.
 * Returns FAILOVER_* status. */
int failover_recover_from(uint32_t dead_node_id);

/* Leader's periodic state-tree checkpoint broadcast (live cluster wiring).
 * Called from the BSP sweep; it TRANSMITS, so like the consensus heartbeat
 * it must stay off the AP tick. The leader serializes its own partition
 * table (state_tree_build) and pushes the transfer to every roster peer
 * via dspp_ckpt_send(), rate-limited to one per FAILOVER_CKPT_PERIOD_TICKS.
 * Followers hold the latest transfer in the single-slot RX buffer, which
 * failover_recover_from() consumes when the leader is declared dead. */
void failover_live_checkpoint_broadcast(uint64_t now);

/* The sweep's liveness half, callable from the blocking waits that run ON
 * the sweep. The create round trip (env_service_rpc), the migrate send's
 * ACK waits, the restore's parked wait and pour all execute on the BSP
 * sweep — the same context that transmits the consensus heartbeat and the
 * checkpoint broadcast — so while any of them blocks this node's
 * heartbeat stream stops: every observer's election watchdog (150 ticks)
 * fires, the busy node is DEPOSED mid-work, and as a quiet follower —
 * followers transmit nothing that feeds last_seen_tick — it is declared
 * DEAD 300 ticks after its own release while perfectly alive (live 3-node
 * find, 2026-10-23, env_migrate_confirm_live's shape). Calling this from
 * inside those waits keeps both streams flowing. Rate-limited at their
 * own gates (LEADER_HEARTBEAT_TICKS, the checkpoint period), so a tight
 * wait loop pays only a gated call per spin; transmit-only, no yield —
 * legal even inside the pour's straight-line section. */
void failover_sweep_keepalive(void);

/* How many partitions were adopted in the last failover_recover_from() call. */
uint32_t failover_last_adopted_count(void);

/* POSIX-Environments v0.2 Phase P3, failover destination (b) (§11 Q5): how
 * many environments belonging to the partitions the last recovery adopted
 * were named as NOT restored. Failover adopts partitions by ownership flip
 * and restores no environment — the payload is 5.5 MiB and the leader's
 * checkpoint broadcast is 24 KiB — so this counter is the honest size of
 * that gap. Zero is the common case (a partition with no environments) and
 * is what makes a non-zero worth reading. Countable rather than log-only so
 * a host test can pin it and the `silent-failover` tooth has something to
 * bite on beyond a grep of a serial line. */
uint32_t failover_last_unrestored_envs(void);

/* P3 increment 11 — the restore half of destination (b): how many of the
 * adopted partitions' environments the last recovery ACTUALLY RESTORED —
 * replayed from this survivor's own durable records through the standard
 * create round trip (env_service_restore_pending()), the same driver a
 * reboot uses. Complementary to failover_last_unrestored_envs(), reset at
 * recovery entry beside it, and classified per record: a record the replay
 * REFUSED counts unrestored, never restored (both outcomes clear the pending
 * bit; the per-slot refused mark is what tells them apart, and the
 * `refused-as-restored` tooth holds that line). Claims the ENVIRONMENT came
 * back, not its bytes: contents follow only as far as the environment's
 * durable extent is readable on this survivor, and [ENV_PAYLOAD] names what
 * is not. */
uint32_t failover_last_restored_envs(void);

#endif /* FAILOVER_H */
