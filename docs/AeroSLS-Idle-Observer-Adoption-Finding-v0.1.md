# The idle-observer adoption finding (v0.1)

**Status:** diagnosed live, not yet fixed. **Observed:** 2026-10-13, twice, on real
3-node TCG clusters (`tests/env_migrate_confirm_live_check.sh`, fresh disks).
**Reproduce:** `tools/repro_observer_incident.sh` boots a fresh 3-node cluster and
runs the confirm guard; the incident fires within seconds of the guard's
`env create` (guard exits 1 at the migrate step: `partition 1 is already owned by
node 1 -- nothing to migrate`).

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

**A. Migration-intent announce (recommended).** Before the blocking phase
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
