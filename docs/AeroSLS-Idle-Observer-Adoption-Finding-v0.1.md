# The idle-observer adoption finding (v0.1)

**Status:** diagnosed live 2026-10-13, FIXED the same day (migration-intent
announce, option A below — see "Resolution"). **Observed:** twice, on real
3-node TCG clusters (`tests/env_migrate_confirm_live_check.sh`, fresh disks).
**Reproduce:** `tools/repro_observer_incident.sh` boots a fresh 3-node cluster
and runs the confirm guard — it now PASSES on that shape; before the fix it
exited 1 at the migrate step (`partition 1 is already owned by node 1 --
nothing to migrate`).

## What was seen (live evidence)

Run 1 (2026-10-13, cluster node3.log 5250-5333): node 1, the consensus leader,
was mid `env create` + migrate send for partition 3 when it stopped beating.
Node 3 printed `Terms timeout. Node 3 campaigning for Term election: 3`, won
the election, and two sweeps later printed `Node 1 declared DEAD (silent 311
ticks)` followed by `Adopted partition 1/2/3 from dead node 1` (checkpoint
seq=55232). Every node in the cluster was alive. The migrating environment was
destroyed: the destination refused to restore a partition it no longer owned,
the source's records were orphaned against an owner that holds no data.

Run 2 (repro, /tmp/inc_n1..3.log): identical chain, roles rotated. Node 2 won
term 2 as leader, blocked on the guard's `env create`, went silent. Node 1
elected itself leader for term 3, then `Node 2 declared DEAD (silent 300
ticks)`, `partition 1 ownership set to node 1`, `Adopted partition 1 from dead
node 2` (checkpoint seq=1751). Node 3 also declared node 2 dead (303 ticks)
but did not recover — a follower declaring dead observes only.

## The mechanism, step by step

1. The consensus leader is the ONLY heartbeat transmitter
   (`net/consensus.c:309` `check_consensus_heartbeat_tick()`, every
   `LEADER_HEARTBEAT_TICKS`=30) and heartbeats transmit from the BSP sweep —
   the same context `net/http.c` runs the blocking control-plane work in.
2. Blocking work the repo sanctions blocks that sweep for far more than
   `FAILOVER_DEAD_TICKS` (300, `kernel/failover.h:24`): the env-create route
   runs the create RPC wait — sidecar boot plus the 30 s parked-posture wait —
   and the migrate send is a synchronous 1600-page round trip with per-page
   ACKs. The blocked leader transmits nothing; it cannot.
3. Followers' election timers (`ELECTION_TIMEOUT_BASE_TICKS`=150) expire; one
   of them campaigns and wins the term the blocked node cannot contest.
4. `failover_note_heartbeat()` (`kernel/failover.c:65`) is fed only by
   `DSPP_CMD_HEARTBEAT` receipt (`net/consensus.c:408`), i.e. only by the
   leader's stream. The new leader's peer entry for the old leader froze at
   the last leadership-era heartbeat; `failover_tick()` sees silence cross 300
   and declares DEAD (`kernel/failover.c:86`).
5. Only the leader recovers (`kernel/failover.c:117`), and
   `failover_recover_from()` adopts EVERY partition node in the dead node's
   last checkpoint — an ownership flip with synthesized names and no data
   (`kernel/failover.c:180-208`). For a partition whose payload is mid-flight
   to another node this is silent data loss.

## Why the existing deferral cannot cover it

`env_migrate_in_flight()` (`kernel/env_migrate.c:935`) reads THIS node's
migration state: a young staging slot (destination side), an armed teardown
confirmation (source side, after the send), the resolved tail. In this
incident the knowledge lives on the **blocked source**, and the declaration is
made by a **third node** that holds no migration state at all — its gate reads
false correctly. Worse, the source's own `in_flight()` is also 0 during
create+capture+send: its inputs arm only at destination-side BEGIN and at the
teardown confirm, i.e. after the block that matters.

The deferral was built for the 2-node shape, where the only counterparty is
the destination and its staging slot covers the whole event. That shape is
safe and stays safe. The 3-node shape adds an observer that knows nothing.

## The class, not the instance

The trigger is not migration specifically: ANY leader-side sweep block longer
than ~3 s (election timeout 150, death threshold 300) produces leadership
churn, a former-leader DEAD declaration, and a partition adoption. Migration
merely makes such blocks routine and sustained. Any fix aimed only at
`env_migrate_*` call sites will leave the class open.

## Fix options

**A. Migration-intent announce (recommended; IMPLEMENTED 2026-10-13).** Before the blocking phase
begins — at the env-create route entry (`net/http.c:3063`) and at the migrate
send entry — the source stamps a local intent and TXes a one-shot cluster
announce ("node N has env work in flight for partition P, budget B"). Receiving
nodes latch `peer_env_intent_until[node] = now + B`; `failover_tick()` consults
a new `env_migrate_peer_in_flight(node)` beside the existing local gate. B is
the honest attempt budget (create tries x (RTT + 30 s parked wait) + send ACK
deadline; `ENV_MIGRATE_SLOT_GRACE_TICKS` is the conservative choice). Trade-off,
same philosophy as the existing deferral: a leader that genuinely dies
mid-migration delays failover by at most the remaining budget — bounded, and
death is declared the moment the intent budget expires. Wire change is
single-tree (a new announce opcode or a flags field in the sync row); all
nodes upgrade together, as they already do for DSPP evolutions.

**B. Follower liveness visibility (complementary hygiene, not the fix).**
Liveness currently tracks only the leader's stream; follower peer entries
freeze at their last leadership. Noting ANY DSPP receipt as liveness
(`cluster_note_peer_seen` already does this for the roster) would widen
detection — but a blocked node transmits nothing at all, so B alone never
covers the incident above. Do it for correctness of the liveness table, not
as the answer.

**C. Leadership-age gate on recovery (cheap containment).** Require the new
leader to have held the term for >= FAILOVER_DEAD_TICKS before running
`failover_recover_from()`. In both live incidents the adopter had been leader
for two sweeps; a real death delays adoption by one gate period. This narrows
the window (election churn + adoption no longer land in the same breath) but
a sustained block still outlasts it — containment, not a fix.

**D. Move blocking work off the sweep (correct long-term, out of scope).**
Create waits and synchronous sends belong on a worker context; heartbeats
would then never stop for sanctioned work. This is the real cure but is a
scheduling refactor, not an increment.

## Related weakness found on the way

`failover_recover_from()` adopts partitions by ownership flip only: rows are
created with synthesized names (`fo_recovered_name`) and no data check. Even
for a REAL leader death, adoption can hand out ownership of partitions whose
payload lived only on the dead node's NVMe, silently. Data-preserving
adoption (require the checkpoint to actually carry the partition's payload,
or mark adopted partitions degraded) deserves its own increment.

## Resolution (2026-10-13)

Option A landed, symmetric on both roles of a migration:

- **Source side:** `partition_migrate()` and both operator env-create entries
  (`net/http.c`, `user/shell.c`) call `env_migrate_intent_begin()` BEFORE
  their blocking work — the stamp feeds `env_migrate_in_flight()` (the local
  gate now also covers the create/capture/send window, which previously had
  no deferral face at all) and transmits `DSPP_MIGRATE_ENV_INTENT` (a
  broadcast; every peer latches it through `env_migrate_recv_intent`,
  clamped to `ENV_MIGRATE_INTENT_GRACE_TICKS`).
- **Destination side:** the restore loop announces before each restore
  attempt, so an idle observer cannot declare the RESTORING node dead
  either — the live 3-node run before this half showed exactly that
  (a `declared DEAD` of the restoring destination, observe-only only by
  role luck).
- **The gate:** `failover_tick()` asks `env_migrate_peer_in_flight(peer)`
  before declaring, beside the local gate. Pinned in
  `tests/checkpoint_recovery_host_test.c` (the gate asks about the SILENT
  PEER by identity, and death is declared the tick after the budget
  expires) and `tests/env_migrate_host_test.c` (local stamp, the wire
  frame, the latch, the clamp, the bounded expiry, the reset).
- **Source guard teeth:** clauses Z/AA/AB of `env_migration_check.sh` (+
  teeth in its smoke) hold the wiring: the declaration gate consults the
  latch, every entry announces BEFORE its block (order-checked), the
  opcode is defined/routed/heard before the self-filter.

**Live proof** (fresh-disk clusters, the confirm guard):
3-node PASS twice (19:11, 19:21 — every face fired: both observers latched
the announces and printed `declaration DEFERRED`, the destination printed
`DEFERRED: node N announced environment-migration intent`, the restore
completed LIVE and the teardown ran on the word), and the original 2-node
shape still PASSes (19:40).

Two adjacent defects surfaced while proving this — one fixed, one noted:

1. **A re-BEGIN of the same transfer was refused (FIXED).**
   `env_migrate_recv_begin` refused any BEGIN for an already-staged
   identity (rc=5), so the documented retransmission model failed exactly
   when a lossy/fresh-boot link needed it: a late BEGIN ACK made the source
   re-ask, the refusal abandoned a healthy migrate mid-send ("a page never
   confirmed"). A re-BEGIN carrying the same identity-derived transfer_id
   AND the same record is now answered READY without touching the slot; a
   different record stays refused (host pin E4 holds, new pin E4b).
2. **A uint64 silence underflow was observed once (FIXED 2026-10-13).**
   One `declaration DEFERRED` line printed `silent 18446744073709551606
   ticks` — `now - last_seen_tick` underflowed (last_seen noted "ahead" of
   the sweep's sampled now). The deferral still held, but the same
   arithmetic on an ungated peer would declare instantly. Root cause is
   structural, not a clock bug: the two operands come from different
   contexts. `now` is read once at `failover_tick()`'s call site on the BSP
   sweep (`net/http.c:7226`), while `last_seen_tick` is stamped by the
   heartbeat RX path (`process_consensus_packet()` ->
   `failover_note_heartbeat()`, fed from the timer ISR / AP core), so a
   heartbeat landing between those two reads legitimately leaves
   `last_seen_tick > now` and the unsigned subtraction wraps to ~2^64 —
   past `FAILOVER_DEAD_TICKS`. Fixed with a saturating subtraction in
   `failover_tick()` (a stamp at or after `now` means the peer was seen at
   least as recently as this sample: silence is zero), not with the
   `last_seen = now` rewrite this note originally suggested — clamping the
   stamp would discard the fresher evidence for every later sweep.
   Pinned in `tests/checkpoint_recovery_host_test.c` on both sides (a peer
   stamped ahead of the sample is neither DEAD nor SUSPECTED at that tick,
   and is still declared 300 ticks of real silence later) and held by
   clause AC of `env_migration_check.sh` with its smoke tooth.

## Coverage gaps

- No live check exercises a busy owner plus an idle third node. The
  confirmation guard targets 2 nodes (correctly — until this is fixed);
  `failover_adoption_live_check.sh` kills nodes cleanly, which is a death the
  machinery handles. `tools/repro_observer_incident.sh` is the only repro.
- No host test pins "a node holding no migration knowledge must not adopt a
  partition whose owner announced intent" — because the intent does not exist
  yet. Option A should land with that pin, plus a 3-node live variant of the
  confirm guard with observer teeth (node 3 must print DEFERRED, never
  `ownership set`, and the environment must still restore).
