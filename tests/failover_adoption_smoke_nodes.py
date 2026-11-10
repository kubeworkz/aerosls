#!/usr/bin/env python3
"""Fake AeroSLS nodes for tests/failover_adoption_live_smoke.sh.

Stands up three HTTP servers shaped like the handful of routes the failover
adoption live guard touches -- /api/health, /api/cluster, /api/partitions
and POST /api/shell/exec -- so the guard can be driven through every
verdict without a real cluster. Not a simulator of the kernel: only of the
response shapes the guard must tell apart, plus the log strings the guard's
evidence gates on and the one piece of real state the property depends on:
the wire-in-miniature announce and the leader's death.

The property: when the leader dies, a survivor becomes leader, declares the
death (silent >= 300 ticks), adopts the dead leader's partitions from the
held checkpoint, and the owner handoff replicates to the other survivor.
The fake models the kernel's log strings the guard gates on (each printed
by exactly one real site):

    RX (create learned):  "[PARTITION] sync: partition N '<name>' (owner
    node L) learned from node L." then "[DSPP-CKPT] RX: COMPLETE seq=N" --
    the guard requires the checkpoint line AFTER the learn line, proving
    the create reached the held checkpoint.
    leader death:          "[FAILOVER] Node L declared DEAD (silent 300
    ticks)" then "[FAILOVER] Adopted partition N from dead node L" then
    "[FAILOVER] recovery for dead node L: rc=0 (OK - adopted)" -- the
    guard requires the adoption AFTER the declaration on the adopter.
    handoff:               "[PARTITION] sync: partition N '<name>' (owner
    node M) learned from node M." on the OBSERVER -- only the leader
    adopts, so a learned row with owner = the adopter is the handoff.
    lease acquire:         "[CONSENSUS] partition N lease initialised
    (FOLLOWER, term=0)." (every receiver of the campaign), "campaigning
    for write lease", the "[MMU-LEASE] partition N: page permissions
    force_read_only=1" strip on campaign and "=0" restore on the quorum
    win, then "quorum stable, node M elected LEADER (write lease)". The
    row is written WHOLE (tmp + os.replace) and carries the campaign's
    term, so a receiver can tell a re-driven campaign from the first one
    (migrate_lost1) -- a hop delivers a row, never half a packet.
    migrate:               the source logs "voluntarily stepped down from
    LEADER -- lease relinquished" then "[PARTITION] migrated partition N:
    node S -> node D. Lease relinquished=yes, ..." and the receivers log
    "[PARTITION] sync: partition N claimed by node S and node D --
    owner-initiated transfer, applying." (partition_sync_upsert's
    source == current-owner branch) then the learn with owner = D.

The leader's death is detected the way the kernel detects it: the follower
polls the leader's HTTP port; when it stops answering, the follower
transitions. Which follower becomes leader, and what it does with the row,
is the mode:

Modes (written to <statedir>/mode by the smoke):
    adopted       node 2 becomes leader, declares, adopts (owner=2); node 3
                  observes the handoff -> guard PASS. The guard's step 11
                  then migrates the partition back to node 1 (the original
                  leader, resurrected in step 10): the owner-initiated
                  transfer out-resolves node 1's stale learned row, node 1
                  is killed mid-flight and relaunched, the cluster
                  converges to owner=1 with no flap, the adopter's write
                  lease is relinquished at the migrate and re-acquired on
                  node 1 by a fresh 2-of-3 quorum -> still PASS.
    notadopted    node 2 becomes leader and declares, but never adopts (row
                  stays owned by the dead leader) -> guard FAIL
    nolearn       followers never learn the create -> guard FAIL at the
                  learn gate
    nockpt        followers learn but no checkpoint flows -> guard FAIL at
                  the checkpoint gate
    learnlost1    the FIRST announce's packet is LOST on the wire for BOTH
                  followers: they only ever see the row on the owner's next
                  periodic re-announce pass (~10 s later), so the
                  create-announce gate must wait out another period and
                  pass. Its control is the SAME mode with the re-drive
                  disabled, which must still FAIL the gate
    ckptlost1     the same loss one stream over: both followers learn from
                  the first announce, but the checkpoint frame carrying the
                  row is lost for that pass and lands with the next one, so
                  the checkpoint gate (a CKPT line AFTER the sync line) must
                  wait out another period -> guard PASS (same control shape
                  as learnlost1). Both drop to the "adopted" path after the
                  pre-kill gates, so the whole failover chain still runs
    splitbrain    BOTH survivors become leader at once -> guard FAIL at the
                  split-brain check: the step-7 simultaneous-lead read when
                  one poll happens to catch both, and otherwise the
                  two-owner read the later gates ask (each survivor serves
                  the partition as its own, so a poll that straddles the two
                  flips cannot decide the verdict or its message)
    observeradopts  node 2 adopts normally, but node 3 (the observer) ALSO
                  prints the Adopted line while staying FOLLOWER -- a
                  follower that recovered -> guard FAIL at the step-9
                  never-adopt check (cluster_is_leader gate)
    lateflip      node 2 adopts normally; node 3 stays FOLLOWER through the
                  adoption, then flips to LEADER after a delay -> guard
                  FAIL at the step-9 stays-FOLLOWER check (late
                  split-brain)
    resurrect_stable  the guard RELAUNCHES the killed leader mid-adoption.
                  The relaunched leader restores its stale row from disk
                  (logs the restore) and re-announces it; both survivors
                  REJECT the stale claim (log the reject) and keep owner 2,
                  and the leader CONVERGES to owner 2 -> guard PASS. The
                  step-11 migration back to node 1 then behaves exactly
                  like "adopted" above -> still PASS.
    resurrect_flap  the survivors APPLY the stale claim instead of
                  rejecting it (the old "last announce wins" bug): the
                  partition flaps back to owner 1 -> guard FAIL
    resurrect_nostale  the relaunched leader never restores its stale row
                  (no restore log, no re-announce): there is nothing to
                  conflict over -> guard FAIL
    migrate_stable  step 10 behaves like resurrect_stable; step 11's
                  migration is handled correctly -> guard PASS (the teeth
                  that prove step 11 CAN pass, not just fail)
    migrate_flap  step 11's migration is applied to the WRONG owner: the
                  receivers log the owner-initiated transfer but the row
                  flaps back to the adopter (owner 2) -> guard FAIL at the
                  step-11 convergence gate
    migrate_nolease  node 2's `partition lease acquire` never holds (the
                  command succeeds but no row is won) -> guard FAIL at the
                  step-11 lease-held gate
    migrate_noapply  the transfer announce never reaches the receivers (no
                  owner-initiated line logged) -> guard FAIL at the step-11
                  transfer gate
    migrate_nostale  the step-11 relaunch of the destination never restores
                  its row -> guard FAIL at the step-11 restore gate
    migrate_noreacquire  node 1's re-acquire after the migrate never holds
                  -> guard FAIL at the step-11 re-acquire gate
    migrate_lost1  the FIRST step-11 lease campaign's packet never reaches
                  the OBSERVER (a lost HOP, not a broken RX path): it only
                  ever logs the row of the SECOND campaign, i.e. the
                  guard's re-drive -> guard PASS (the re-drive gate)
    migrate_norow  the OBSERVER never receives the campaign at all (its
                  lease RX path is broken): no row is ever logged, no
                  matter how often the guard re-drives -> guard FAIL at
                  the step-11 lease-learn gate

Both survivors DECLARE the death (every node runs failover_tick); only the
leader recovers. So the observer's log always carries its own declaration
line -- that is what the step-9 log check compares the Adopted line
against.

The resurrect choreography is a second wire-in-miniature: the relaunched
leader writes <state>/resurrect_claim (its periodic re-announce), the
survivors poll it and log their verdict, and (stable) write
<state>/resurrect_rejected so the leader can log its convergence -- the
same file-passing the learn/death threads already use. Step 11 uses two
more files: <state>/lease.json (the acquire, written by the acquirer;
everyone else logs its lease-row create) and <state>/migrate.json (the
migrate, written by the adopter; the destination and the observer log the
owner-initiated transfer apply).
"""

# ─── The fake must never be the binding clock ─────────────────────────────
# Every wait below is on an event the GUARD gates, and the guard already
# scales its own patience down for the smoke (AEROSLS_FAILOVER_FAST=1:
# WAIT_NODES=10s, WAIT_LEARN=6s, WAIT_ADOPT=10s, WAIT_CLAIM=8s, ...). The
# loops used to carry fixed iteration caps of their own -- 200 x 0.05s =
# 10s, 600 x 0.05s = 30s, 900 x 0.05s = 45s -- budgets that have nothing to
# do with the guard's, start when the PROCESS starts, and therefore compete
# with the guard's whole fast path. The learn wait is the worst case: its
# 10s cap was exactly the guard's WAIT_NODES pre-create budget, so any host
# that took that long to form the cluster (a loaded runner; a slow python
# cold start) put the create announce at the cap's edge and a follower
# stopped listening before the guard had even created the partition. CI hit
# exactly that on 2026-09-20: failover_adoption_live_smoke.sh's nockpt and
# lateflip teeth failed with "nodes 2/3 never learned partition" -- exactly
# one follower empty, the other learned -- instead of the gate each tooth
# mutates, so the mutation's verdict was never reached at all.
#
# So every wait here on an event the guard's gates depend on is bounded by
# the process's LIFE, not by a counter: the only clock that may decide a
# tooth is the guard's. A mode that models an event NEVER arriving (nolearn,
# nockpt, migrate_noapply, migrate_nolease, svc_nostale, ...) still returns
# without logging its line, so those teeth keep their teeth -- they now fail
# the GUARD's gate, which is the property under test, rather than racing a
# fake-side timer. Two counters remain, deliberately: reading the row on a
# relaunch (the file preexists there, so it is a read, not a wait) and the
# leader's convergence cadence (it logs either way, so a counter can only
# move WHEN, never WHETHER -- and an unbounded one would stall the modes
# whose survivors never write the reject signal). The negative control for
# this rule is the smoke's "latecreate" tooth: it idles the fakes past every
# old cap before running the guard, and with the old counters restored it
# fails at exactly the CI signature.

import http.server
import json
import os
import sys
import threading
import time
import urllib.request

node_id = int(sys.argv[1])
role = sys.argv[2]
port = int(sys.argv[3])
state = sys.argv[4]
logdir = sys.argv[5]

mode = open(os.path.join(state, "mode")).read().strip()
leader_f = os.path.join(state, "leader")
leader_id = int(open(leader_f).read().strip())


def current_leader():
    """The node the cluster currently elects, per the state/leader file --
    the same file the smoke harness writes the boot value into. For every
    mode but prekillchurn the file names the boot leader forever, so this
    is the boot argv leader; the prekillchurn tooth's churn thread flips it
    mid-run (a survivor won the term) and a survivor's own election CASes
    it back. A missing or torn file means the boot value: a reader that can
    raise turns a torn write into a verdict decided by timing (the rule
    read_created() follows)."""
    try:
        return int(open(leader_f).read().strip())
    except (OSError, ValueError):
        return leader_id


def write_leader(value):
    """Whole-write the leader file (tmp + os.replace) with a bounded retry:
    current_leader() readers open it constantly, and WINDOWS denies
    rename-over-open (a PermissionError that killed the churn thread on a
    relaunched node, seen in the prekillchurn tooth's node2.stderr). A
    racing reader must never decide a verdict -- the same rule
    read_created() follows, applied to the writer's side."""
    tmp = "%s.tmp%d" % (leader_f, os.getpid())
    for attempt in range(5):
        try:
            with open(tmp, "w") as f:
                f.write(str(value))
            os.replace(tmp, leader_f)
            return
        except OSError:
            if attempt == 4:
                raise   # loud, in this process's stderr -- never silent
            time.sleep(0.05)

created = os.path.join(state, "created.json")
# The guard creates a SECOND partition -- the '-env' one it then puts an
# environment inside (step 4.5) -- and later looks the FIRST one ($NAME) up
# again for steps 10-12. created.json therefore keeps holding the first row
# exactly as every earlier gate expects, and later creates append here
# instead of overwriting it. _partitions() serves both.
extra = os.path.join(state, "extra.json")
# The environments the guard's step 4.5 creates: [{partition, index, env_id}].
envs_f = os.path.join(state, "envs.json")
lease = os.path.join(state, "lease.json")
migrate = os.path.join(state, "migrate.json")
# The periodic re-announce's pass counter -- owned by the node that owns the
# row, exactly like partition_reannounce_tick(). See its block below.
reannounce = os.path.join(state, "reannounce")
REANNOUNCE_PERIOD = 10.0   # models 1000 ticks ~= 10 s; see the block below
# The checkpoint BROADCAST period, modelled for followers: the real leader
# puts its state tree out every 100 ticks (~1 s), and the guard's step-4.5
# gate counts DELTAS of "[DSPP-CKPT] RX: COMPLETE" lines that land AFTER the
# -env create. Emitting the line exactly once at learn time (as this fake
# did before step 4.5 existed) leaves that gate at 0 forever no matter how
# long it waits, so followers now log one per period -- same shape, same
# line, bounded by the guard's own windows. 2 s keeps two lines inside one
# FAST 6 s window with margin on a loaded host.
CKPT_PERIOD = 2.0
# Test-only knob for the smoke's "leadergap" tooth: when <state>/health_gap
# holds a number, the LEADER answers /api/health only after that many
# seconds from the guard's create -- a bounded episode of unreachability,
# exactly the shape a busy host produces. A real kernel survives it (its
# death rule wants FAILOVER_DEAD_TICKS of SILENCE); a "3 failed polls" rule
# does not. See gap_until below and the DEAD_SILENCE rule above.
gap_file = os.path.join(state, "health_gap")
gap_until = 0.0
# True only on the RELAUNCHED leader process: the guard creates the
# partition AFTER the original process boots, so the original sees
# created.json appear mid-session, while every relaunch (step 10/11) boots
# with it already present. resurrect_thread must run only on the relaunched
# process -- on the original it fired the moment the create landed,
# truncating the log and (in STEP10 modes) writing leader_converged, which
# flipped svc_owner() to the adopter DURING the guard's step 3.5 and made
# the service-resolve gate intermittently read the pre-kill registration as
# owned by node 2.
created_preexisting = os.path.exists(created)
logfile = os.path.join(logdir, f"node{node_id}.log")
lock = threading.Lock()

# Per-mode: which node becomes leader (1 = node 2, 2 = node 3, 0 = both).
if mode == "splitbrain":
    my_transition = "LEADER" if role == "follower" else "LEADER"
elif mode == "prekillchurn":
    # The churned election: node 1 (the boot leader) lost its role to node
    # 2 mid-run, so after the step-6 kill of node 2 the survivor that
    # adopts is node 1 -- the node the live 2026-10-16 restarts landed on.
    my_transition = "LEADER" if node_id == 1 else "FOLLOWER"
else:
    my_transition = "LEADER" if (role == "follower" and node_id == 2) else "FOLLOWER"

# The node the adoption lands on once the current leader dies: node 2 in
# every mode but prekillchurn (there the churn moved leadership to node 2
# BEFORE the kill, so the remaining survivor, node 1, adopts). Every
# hardcoded 2/1 pair the base choreography carries (the observer's handoff
# learn, the owner derivation after a transition, the convergence pair's
# adopter half) permutes through this and current_leader(), so the base
# bytes stay byte-identical while the ids follow the real election.
ADOPTER_ID = 1 if mode == "prekillchurn" else 2

transitioned = False
# lateflip: the observer flips to LEADER AFTER the adoption has been
# observed (a late split-brain), so the guard's step-9 watch catches it
# rather than the step-7 simultaneous-flip check.
late_flipped = False
# Step 11 lease/migrate state on this process.
lease_held = False
migrated_to = 0        # owner this node serves after the migrate (0 = none yet)

# Service-registry state (the guard's steps 3.5 / 7.5 / 12). Mirrors
# kernel/service_registry.c: svc_local is this process's LOCAL registration
# (its own declare), svc_remote is the node_id of the accepted remote
# announce. Resolution prefers local; local node_id is DERIVED from the
# partition owner, remote node_id is the announcing node -- exactly like the
# real kernel.
svc = os.path.join(state, "svc.json")
svc_local = False
svc_remote = 0
svc_last_gen = 0


def log(line):
    with lock:
        with open(logfile, "a") as f:
            f.write(line + "\n")


def read_created():
    if os.path.exists(created):
        try:
            return json.load(open(created))
        except ValueError:
            return None
    return None


def read_list(path):
    """A JSON list from disk. A missing or torn file is an empty list, never
    an exception: a reader that can raise turns a half-written file into a
    verdict decided by timing (the rule read_created()/read_epoch() follow)."""
    if os.path.exists(path):
        try:
            v = json.load(open(path))
            if isinstance(v, list):
                return v
        except ValueError:
            pass
    return []


def read_envs():
    return read_list(envs_f)


def all_rows():
    """Every partition row this node serves: the primary ($NAME) plus any
    created after it (the guard's -env partition)."""
    row = read_created()
    rows = [row] if row else []
    rows.extend(read_list(extra))
    return rows


def read_epoch():
    """The owner's re-announce pass counter (0 = no pass yet).

    Retry-tolerant like read_created(): a torn or empty file means "not
    yet", never an exception. A dead RX thread never logs its line, so a
    reader that can raise is a verdict decided by timing (the CI false RED
    of 2026-09-25 in the 2-node sibling, same rule).
    """
    if os.path.exists(reannounce):
        try:
            return int(open(reannounce).read().strip() or 0)
        except ValueError:
            return 0
    return 0


def next_reannounce():
    """Block until one periodic pass AFTER the pass this node is reacting
    to. max(..., 1): seeing the row before the owner has written pass 1 is
    still the mutation announce, i.e. pass 1, not a pass of its own."""
    at = max(read_epoch(), 1)
    while read_epoch() <= at:
        time.sleep(0.05)


def read_lease():
    # A half-arrived row is NOT a broken RX path. read_created()'s rule,
    # applied to the lease row too: a partial read means "not yet", never
    # an exception. The writer os.replace()s the row in whole (below), but
    # the reader must not be what decides a verdict anyway -- the old
    # `json.load(open(lease))` right after `os.path.exists(lease)` could
    # catch the zero-byte window of the in-place buffered write this file
    # used to do and die with a JSONDecodeError, killing this thread; a
    # dead RX thread never logs the row, so the guard read a racing reader
    # as "the lease RX path is broken" -- the CI false RED of 2026-09-25
    # in the 2-node sibling, same reader, same fix.
    if os.path.exists(lease):
        try:
            return json.load(open(lease))
        except ValueError:
            return None
    return None


def lease_term(row):
    """The campaign term of a lease row (0 when no row has arrived yet)."""
    try:
        return int(row.get("term", 0))
    except (AttributeError, TypeError, ValueError):
        return 0


def read_migrate():
    """The migrate (transfer) row. read_created()/read_lease()'s rule: a
    half-arrived row means 'not yet', never an exception. A bare
    json.load() in migrate_poll_thread() is what killed that RX thread --
    the writer's in-place window (before this file's whole-write rule
    reached migrate.json) let the destination read a zero-byte file once
    and the thread died silently, so the transfer line never appeared:
    observed as migrate_stable's FAIL ('the destination never logged the
    owner-initiated transfer') while migrate_noapply, which models exactly
    that absence, passed on the same gate."""
    if os.path.exists(migrate):
        try:
            return json.load(open(migrate))
        except ValueError:
            return None
    return None


def row_owned_by(nid):
    """The row node `nid` owns on disk: what ITS boot restore brings back
    (the resurrect thread passes this node) and what its death leaves
    behind (the death thread passes the dead id -- the adoption, the
    observer's handoff learn and the classification are all ABOUT that
    row, and in a restarted prekillchurn run the guard's gates are scoped
    to it by NAME: the '-1' row in extra.json, owned by the churned
    winner). The base modes have only created.json, whose row names the
    boot leader; the fallback keeps them byte-identical (a torn or
    partial view means 'not yet', never an exception)."""
    for r in all_rows():
        if int(r.get("owner_node", 0) or 0) == nid:
            return r
    return read_created()


def learn_line(row, owner):
    return ("[PARTITION] sync: partition %s '%s' (owner node %s) learned "
            "from node %s." % (row["id"], row["name"], owner, owner))


# The modelled property is failover_tick's "silent >= FAILOVER_DEAD_TICKS"
# (300 ticks) -- a DURATION of unreachability, not a count of failed
# requests. A poll that timed out because the leader was busy answering
# somebody else is not silence, so death needs DEAD_SILENCE seconds of
# CONTINUOUS unreachability (see death_thread).
#
# Why 4s: the rule it replaces (three failed polls, 0.5s timeout each, 0.1s
# apart) fired after ~2.3s of unreachability, and a loaded host produced
# episodes that long -- observed: a follower declared the leader DEAD while
# it was alive, transitioned, and the guard then read its row as "never
# learned" although both followers' logs showed the learn. 4s sits above
# every such episode seen (a real SIGKILL is unbounded) and inside the
# guard's own post-kill gates (WAIT_ADOPT=10s, WAIT_OBSERVER=8s,
# WAIT_HANDOFF=6s), so the declaration after a real kill is still prompt.
DEAD_SILENCE = 4.0


def leader_alive(target=None):
    """Poll a node's health. Without a target, the CURRENT election owner's
    port (the prekillchurn flip moves it mid-run). With one, that EXACT
    node: a silence observation, once started, stays pinned to the node
    that went silent -- otherwise the other survivor's CAS (or, in
    prekillchurn, the churn flip) redirects this poll to a live node, the
    silence resets, and this thread never observes the death (seen in the
    smoke: splitbrain's loser polling the winner post-CAS and never
    transitioning)."""
    nid = current_leader() if target is None else target
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port - node_id + nid}/api/health",
                timeout=0.5):
            return True
    except Exception:
        return False


# ─── Periodic re-announce: the owner's pass counter ──────────────────────
# Announce-on-change converges the nodes that were up for a change; a node
# whose packet was LOST on the wire waits for the next mutation -- unless
# the owner's periodic re-announce reaches it. partition_reannounce_tick()
# (kernel/partition.c, called from the BSP sweep) re-broadcasts the rows a
# node OWNS every 1000 ticks (~10 s at the nominal 100 Hz sweep), and every
# node does it for its own rows. The fake models exactly that as a PASS
# COUNTER written by the OWNER while it still holds the row: pass 1 is the
# one-shot mutation announce, pass 2+ are the periodic ones. The followers'
# lost-packet modes (learnlost1, ckptlost1) key off it, so "the first packet
# was lost, the re-announce re-delivered it" is a real second event owned by
# the SENDER -- not a receiver-side sleep, which would model the wrong node
# and would keep delivering after the owner died.
#
# Why 10 s: that is the kernel's own period (PARTITION_REANNOUNCE_TICKS
# 1000). The smoke's FAST guard windows are 6 s, so pass 2 lands in the
# guard's SECOND window -- later than one window (the first genuinely times
# out: that is the tooth) and comfortably inside two, with seconds of
# margin at both edges so a loaded host cannot move it out. Do not shorten
# it below one window: the lost-packet modes would stop modelling a loss at
# all and their teeth would go vacuous (the smokes assert the guard printed
# its re-drive precisely so that cannot pass unnoticed).
if role == "leader":

    def reannounce_thread():
        epoch = 0
        last = 0.0
        while True:
            row = read_created()
            if not row or int(row.get("owner_node") or 0) != node_id:
                epoch = 0   # nothing owned: the tick broadcasts nothing
                time.sleep(0.05)
                continue
            now = time.monotonic()
            if epoch == 0 or now - last >= REANNOUNCE_PERIOD:
                epoch += 1
                last = now
                # Whole-write (tmp + os.replace), the same rule the lease row
                # follows: a reader sees a pass NUMBER or the previous one,
                # never a torn file.
                tmp = "%s.tmp%d" % (reannounce, os.getpid())
                with open(tmp, "w") as f:
                    f.write(str(epoch))
                os.replace(tmp, reannounce)
            time.sleep(0.05)

    threading.Thread(target=reannounce_thread, daemon=True).start()


# ─── RX thread: learn the created row, log learn + checkpoint ─────────────
# The guard creates the partition AFTER the fakes are up, so the learn
# cannot be logged at process start: the follower's announce RX fires when
# the row arrives on the wire (here: when created.json appears). nolearn
# never learns (the guard's learn gate must bite); nockpt learns but never
# logs the checkpoint RX (the checkpoint gate must bite). learnlost1 drops
# the FIRST announce's packet for BOTH followers and learns on the owner's
# next periodic re-announce -- the guard's second window, so its
# announce gate must re-drive; ckptlost1 drops the checkpoint frame carrying
# the row for that same pass, which lands with the next one, so its
# checkpoint gate must re-drive too.
learned = False


def rx_thread():
    global learned
    if role != "follower" and mode != "prekillchurn":
        return

    def defer():
        """The current election owner never RX's: it serves its own tree.
        In every base mode that is the argv leader, which returns above;
        prekillchurn also defers the ORIGINAL leader once the churn thread
        flips the election away from it (its own unseen rows must be its to
        BROADCAST -- the live ex-leader's exact lost precondition) and the
        RELAUNCHED leader for as long as it leads."""
        while current_leader() == node_id:
            time.sleep(0.05)

    defer()
    # Life-bounded, not counter-bounded (see the rule at the top of this
    # file): the create announce can arrive at any point in the guard's run,
    # including after its own WAIT_NODES pre-create phase has spent its
    # whole budget.
    row = None
    while row is None:
        row = read_created()
        if row is None:
            time.sleep(0.05)
    if mode == "learnlost1":
        # The first announce's packet never landed (a lost HOP, not a broken
        # RX path): this node only ever sees what the owner's periodic
        # re-announce carries. learned stays False until then, so the row is
        # not served either -- the guard must spend another window, which is
        # exactly what its re-drive is for. With a single bounded window the
        # guard FAILed here with "nodes 2/3 never learned partition", a
        # message that accuses partition_sync_upsert for a hop that had not
        # landed.
        next_reannounce()
    if row and mode != "nolearn":
        # Learn every row this node has not seen yet, in id order. The base
        # modes reach here with only created.json, so the loop body runs
        # once exactly as the old single learn did; prekillchurn's restarted
        # run creates the '-1' partition while this node is a plain
        # follower, and the cadence below picks it up on its next tick.
        seen = set()
        while True:
            if current_leader() == node_id:
                # The election moved under us mid-loop (the churn thread's
                # flip): this node is the broadcaster now -- it must stop
                # receiving and never "learn" the rows it owns.
                defer()
                continue
            # A node never "learns" a row it OWNS: the relaunched follower
            # in prekillchurn restored the transferred '-1' row (its own)
            # from disk, and an rx learn of it would log "(owner node
            # $adopter) learned from node $adopter" -- the step-11e no-flap
            # gate's exact forbidden line. The base relaunch is the argv
            # leader (rx never runs there), so this only bites the churn
            # shape; the base followers own nothing, so their loop is
            # byte-identical.
            fresh = [r for r in all_rows()
                     if int(r["id"]) not in seen
                     and int(r.get("owner_node", 0) or 0) != node_id]
            if not fresh:
                # Everything this node should have RECEIVED has arrived
                # (its own rows never arrive -- they are served from its
                # disk), so the serving gate below may open. Setting the
                # flag only after a non-empty pass left prekillchurn's
                # churned ex-leader (which owns the run-1 rows and receives
                # only the restarted run's '-1') serving [] forever while
                # its log carried the learn.
                learned = True
                break
            for r in sorted(fresh, key=lambda r: int(r["id"])):
                if current_leader() == node_id:
                    defer()
                    break
                log(learn_line(r, current_leader()))
                seen.add(int(r["id"]))
            learned = True   # learned regardless of nockpt: the learn gate
                             # must pass, and the checkpoint gate is the log
                             # line below, which nockpt skips.
        if mode == "ckptlost1":
            # The checkpoint frame carrying the row was lost for the pass
            # that delivered the announce; the next periodic broadcast
            # carries it (the real leader's state tree goes out every 100
            # ticks -- the fake collapses the two periods onto the one clock
            # the guard's windows straddle, which can only make the guard's
            # job harder, never easier). The guard's checkpoint gate wants a
            # CKPT line AFTER the sync line, so it must wait out another
            # period too.
            next_reannounce()
            # pass 2 lands at create+10 (REANNOUNCE_PERIOD) -- but the gate's
            # first deadline is create+S+6 where S is how long step 3 took,
            # and S stretched to 4-6 s on a loaded host, so create+10 sat ON
            # the deadline: one run's poll caught it inside window 1 (no
            # re-drive note, tooth vacuous), another run's control missed it
            # by a hair. create+12 puts it strictly between window 1's
            # deadline (S+6 < 12 whenever S < 6, which step 3's own re-drive
            # rule guarantees) and window 2's (S+12 > 12 always) -- the first
            # window misses (the tooth's note) and the second catches it,
            # with margins that only close if step 3 itself nearly timed out.
            time.sleep(2.0)
        if mode != "nockpt":
            log("[DSPP-CKPT] RX: COMPLETE seq=1000 (448 bytes)")
            # Periodic broadcast from here on: this is the follower's RX
            # clock, so it stops with this thread (which is life-bounded) and
            # never runs on a node that did not learn. ckptlost1 (above) delays
            # the FIRST of these and nockpt suppresses every one, which is what
            # keeps those two teeth biting through this loop.
            seq = 1001
            while True:
                time.sleep(CKPT_PERIOD)
                log("[DSPP-CKPT] RX: COMPLETE seq=%d (448 bytes)" % seq)
                seq += 1
                # A row can land long after this thread's first pass (the
                # guard's -env partition, the restarted run's '-1'): learn
                # it on the same RX cadence the checkpoint frames ride --
                # the periodic re-announce is what re-delivers a late row in
                # the real kernel too. Base modes: every row already sits in
                # `seen`, so this is a no-op for them. The leader deferral
                # is what stops the churned node's clock at the flip.
                while True:
                    if current_leader() == node_id:
                        defer()
                        continue
                    fresh = [r for r in all_rows()
                             if int(r["id"]) not in seen
                             and int(r.get("owner_node", 0) or 0)
                             != node_id]
                    if not fresh:
                        break
                    for r in sorted(fresh, key=lambda r: int(r["id"])):
                        if current_leader() == node_id:
                            defer()
                            break
                        log(learn_line(r, current_leader()))
                        seen.add(int(r["id"]))


threading.Thread(target=rx_thread, daemon=True).start()


# ─── Lease RX thread: everyone but the acquirer logs its lease-row create
# when the acquire arrives (here: when lease.json appears). The guard's
# step-11 lease-learn gate checks the OBSERVER's log for "partition N lease
# initialised" -- without a row, the absence of a restore would be vacuous.
# migrate_nolease never writes lease.json (the acquirer's hold never takes)
# and migrate_noreacquire suppresses the row on node 1's re-acquire.
# migrate_lost1 models the LOST HOP at this receiver: the observer never
# sees the first campaign's packet and only logs the row of the second
# campaign (the guard's re-drive); migrate_norow models the BROKEN RX path
# on the observer: it never logs a row at all, however often the guard
# re-drives.
def lease_rx_thread():
    # The acquirer ISSUES the acquire itself (its own acquire_lease logs
    # the campaign); everyone else mirrors the row. Which node that is
    # comes from the row itself (base: node 2; prekillchurn: node 1, the
    # churned election's adopter) -- the skip lands after the row wait, a
    # net no-op for the base argv shape.
    if mode == "migrate_norow" and node_id == 3:
        return   # the observer's campaign RX never fires (the gate must bite)
    # Life-bounded, and retry-tolerant (read_lease): step 11 runs after the
    # whole death/adoption/relaunch dance, far outside any counter started
    # at process boot, and a read that lands mid-write must retry, not kill
    # this thread.
    row = None
    while row is None:
        row = read_lease()
        if row is None:
            time.sleep(0.05)
    try:
        if int(row.get("acquirer", 2)) == node_id:
            return   # we issued this acquire ourselves; acquire_lease
                     # already logged its campaign on this node
    except (TypeError, ValueError):
        pass
    if mode == "migrate_lost1" and node_id == 3 and lease_term(row) < 2:
        # The first campaign's packet (term 1) never landed on the OBSERVER
        # -- a lost HOP, not a broken RX path -- so this node only ever
        # sees the row of the second campaign: the guard's re-drive. The
        # guard's lease-learn gate waits on the OBSERVER's log, so with a
        # single bounded window it FAILed here with "never created a lease
        # row" (the same false RED the 2-node pair fixed on 2026-09-25),
        # which makes this mode the control for the re-drive gate: a lost
        # hop must PASS, a broken RX path (migrate_norow) must still FAIL.
        while True:
            row = read_lease()
            if row is not None and lease_term(row) >= 2:
                break
            time.sleep(0.05)
    # The row is this node's MIRROR of the acquirer's lease state -- this
    # node does NOT hold the lease (its holds_lease stays 0).
    log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)."
        % row.get("partition_id", "1"))


threading.Thread(target=lease_rx_thread, daemon=True).start()


# ─── Service-registry modeling ───────────────────────────────────────────
# The wire-in-miniature for services is <state>/svc.json, one generation
# per declare (the declaring process bumps "gen"). Every other process's
# svc_rx thread applies service_remote_learn()'s claim-class resolver:
# accept when the declarer is the CURRENT partition owner (owner-initiated
# claim), otherwise REJECT when a live entry from another node exists (the
# resurrected-owner shape) -- or APPLY in svc_flap (the old "last announce
# wins" bug, so the guard's step-12 no-flap gate can bite). svc_nostale
# suppresses the stale declare entirely (the announce never fires).
def svc_owner():
    # Same owner derivation as _partitions(): the current election owner
    # (or the step-11 destination), ADOPTER_ID once a survivor transitioned,
    # and ADOPTER_ID on the leader once it converged (step 10). The
    # `or (role == "leader" and ...)` keeps the base argv leader's local
    # derivation even after the CAS moved the election elsewhere -- the
    # RELAUNCHED leader resolves from its restored row, which is exactly
    # what the resurrect_flap tooth's step-12 gate reads. Every branch is
    # byte-equivalent to the argv-only form for the base modes (the file
    # names the boot leader until a survivor's own election, which in base
    # happens exactly at the adoption).
    row = read_created()
    if not row:
        return 0
    if (current_leader() == node_id
            or (role == "leader" and mode != "prekillchurn")):
        if migrated_to:
            return migrated_to
        if os.path.exists(os.path.join(state, "leader_converged")):
            return ADOPTER_ID
        return node_id
    if migrated_to:
        return migrated_to
    if transitioned:
        return ADOPTER_ID
    return current_leader()


def svc_rx_thread():
    global svc_local, svc_remote, svc_last_gen
    if role == "leader" and mode != "prekillchurn":
        return   # local registrations shadow everything on the owner
    # Life-bounded: the step-12 re-announce comes long after the pre-kill
    # registration, way outside a counter started at process boot.
    while True:
        if current_leader() == node_id:
            # The current election owner serves its LOCAL registry; it must
            # not accept a remote announce. The base argv leader returned
            # above, but prekillchurn's churned ex-leader reaches this line
            # after the flip -- and the restarted leader, while it leads.
            # A declare that lands while we lead is picked up on the next
            # pass, once the election is elsewhere (generation-gated, so
            # nothing is lost).
            time.sleep(0.05)
            continue
        if not os.path.exists(svc):
            time.sleep(0.05)
            continue
        try:
            d = json.load(open(svc))
        except ValueError:
            time.sleep(0.05)
            continue
        gen = d.get("gen", 0)
        if gen <= svc_last_gen:
            time.sleep(0.05)
            continue
        svc_last_gen = gen
        name = d.get("name", "svc")
        pid = d.get("partition_id", 1)
        declarer = d.get("declarer", 0)
        if declarer == node_id:
            svc_local = True
            continue   # our own registration; local always wins
        owner = svc_owner()
        if declarer == owner:
            if svc_remote and svc_remote != declarer:
                log("[SERVICE] '%s' claimed by node %u and node %u -- "
                    "partition owner node %u's claim wins, applying."
                    % (name, svc_remote, declarer, declarer))
            svc_remote = declarer
            continue
        if svc_remote and svc_remote != declarer:
            if mode == "svc_flap":
                # The bug: the old "last announce wins" rule applies the
                # stale claim and the registration flaps.
                log("[SERVICE] '%s' claimed by node %u and node %u -- "
                    "taking the newer." % (name, svc_remote, declarer))
                svc_remote = declarer
            else:
                log("[SERVICE] '%s' claimed by node %u and node %u -- "
                    "keeping node %u (live owner), rejecting the stale "
                    "claim." % (name, svc_remote, declarer, svc_remote))
            continue
        # First claim for this name: nothing to protect, it lands.
        svc_remote = declarer


threading.Thread(target=svc_rx_thread, daemon=True).start()


# ─── Death-detection thread: the guard SIGKILLs the leader ────────────────
# The follower polls the leader's port; when it stops answering, the
# follower transitions: the adopter (node 2 in adopted/notadopted, both in
# splitbrain) flips to LEADER and logs the death + (unless notadopted) the
# adoption; the observer (node 3 in adopted) logs the handoff.
def death_thread():
    global transitioned, late_flipped
    if role != "follower" and mode != "prekillchurn":
        return   # (in prekillchurn the argv leader enters too: it polls its
                 # own port until the churn flips the election away, then
                 # polls the node that now leads -- the live ex-leader's
                 # exact position)
    # Life-bounded: the guard SIGKILLs the leader after its own gates have
    # run, and this poll must still be listening when it does.
    # Sustained silence, not "3 strikes": the old rule declared the leader
    # DEAD after three failed polls, which a busy leader can produce. With
    # three smokes running at once a follower's 0.5s poll timed out behind
    # the guard's own requests, it declared the leader dead while the leader
    # was answering, transitioned, and the guard then read its row as
    # "never learned" -- a verdict decided by a client timeout.
    silent_since = None
    target = current_leader()
    while True:
        if silent_since is None:
            # Not observing silence: follow the CURRENT election (the
            # prekillchurn flip moves who we poll). Once silence starts the
            # target is PINNED -- see leader_alive() -- so neither the
            # other survivor's CAS nor the flip can erase it or redirect
            # the declaration to the wrong node.
            target = current_leader()
        if leader_alive(target):
            silent_since = None
            time.sleep(0.2)
            continue
        if silent_since is None:
            silent_since = time.monotonic()
        if time.monotonic() - silent_since < DEAD_SILENCE:
            time.sleep(0.1)
            continue
        transitioned = True
        # The node that died is the one this thread was observing: the
        # election owner at the start of the silence (the boot leader in
        # every base mode; node 2 in prekillchurn, whose file the churn
        # thread flipped before the kill). Every "dead node %u" line below
        # carries it.
        dead_id = target
        if my_transition == "LEADER" and mode == "prekillchurn":
            # The compare-and-swap a real campaign performs on the cluster's
            # leader view -- PREKILLCHURN ONLY. There it moves the view from
            # the churned winner (node 2, now dead) to this node (node 1),
            # which is what makes the guard's /api/cluster role reads, the
            # reject line's owner and the post-adoption resolve settle on
            # the adopter, exactly as the live runs showed. The base modes
            # NEVER write the file: the whole argv choreography (and every
            # relaunched process's boot-time `leader_id`, which gates
            # e.g. migrate_noreacquire's suppression) reads it as the boot
            # leader -- observed the hard way: a base CAS made the
            # step-11e relaunch boot with leader_id=2, its
            # `node_id == leader_id` suppression never fired, and
            # migrate_noreacquire passed VACUOUSLY. A split-brain candidate
            # would race this CAS the way two campaigns race an election;
            # step-7's both-survivors-LEADER check is what must catch that.
            try:
                cur = int(open(leader_f).read().strip())
            except (OSError, ValueError):
                cur = leader_id
            if cur == dead_id:
                write_leader(node_id)
        # Prefetch BEFORE the declare: the guard's step-7.5 budget starts at
        # the adopt line, so no file read (9p on a loaded host) may sit
        # between the adopt line and the classification lines behind it.
        # Observed a run where adopt+recovery landed and the classification
        # never did within 10 s -- the gate read an I/O stall as "the
        # adoption dropped the name", the increment-9 defect's exact shape.
        row = row_owned_by(dead_id)
        block = None
        if my_transition == "LEADER" and mode != "notadopted" and row:
            # The adopted partitions carry the guard's environment (step
            # 4.5), and this survivor's own store holds no record of it --
            # every node has its own disk, so the honest classification is
            # the NOT-restored one. A RESTORED line here is the increment-11
            # lie the guard's step 7.5 fails on, so it is never printed by
            # any mode.
            #
            # Both lines are byte-shaped after kernel/failover.c's own
            # printf's (331-411): the count in the summary is a bare
            # "%u environment(s)" -- NOT "[%u]" -- and the per-env line ends
            # in "; recreate it". The guard greps these literally.
            #
            # Bounded re-read while nothing matches: the registry provably
            # exists (step 4.5's pre-gate proved the env live), so an empty
            # or non-matching first read is torn/not-yet, never "no
            # environment" -- a single-shot read that scored 0 printed
            # nothing at all and failed the gate on that silence.
            env_lines = []
            n = 0
            for _ in range(60):
                adopted = {int(r["id"]) for r in all_rows()}
                n = 0
                env_lines = []
                for e in read_envs():
                    if int(e["partition"]) in adopted:
                        n += 1
                        env_lines.append(
                            "[FAILOVER] environment index %u (partition %u, 3 "
                            "region(s)) was NOT restored -- no durable record "
                            "for it on this survivor; recreate it"
                            % (int(e["index"]), int(e["partition"])))
                if n:
                    break
                time.sleep(0.05)
            if n:
                env_lines.append(
                    "[FAILOVER] %u environment(s) of the adopted partitions "
                    "were NOT restored (named above) -- their payload lived "
                    "on node %u's own storage and does not follow ownership"
                    % (n, dead_id))
            # ONE append for adopt+recovery+classification: the guard can
            # never observe the adopt line without the classification lines
            # already behind it in the same file.
            block = "\n".join(
                ["[FAILOVER] Adopted partition %s from dead node %u"
                 % (row["id"], dead_id),
                 "[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                 % dead_id]
                + env_lines)
        # Every node that observes the death DECLARES it (failover_tick
        # runs everywhere); only the leader recovers.
        log("[FAILOVER] Node %u declared DEAD (silent 300 ticks)" % dead_id)
        if my_transition == "LEADER":
            if block is not None:
                log(block)
        else:
            # The observer: it merely learns the handoff announce from
            # the adopter -- except in the misbehaviour modes. The
            # resurrect/migrate modes adopt exactly like "adopted" for
            # the death phase; the extra steps are the relaunch and the
            # step-11 migration.
            if row and mode in ("adopted", "learnlost1", "ckptlost1",
                                "prekillchurn",
                                "observeradopts", "lateflip",
                                "resurrect_stable", "resurrect_flap",
                                "resurrect_nostale", "migrate_stable",
                                "migrate_flap", "migrate_nolease",
                                "migrate_noapply", "migrate_nostale",
                                "migrate_noreacquire", "migrate_lost1",
                                "migrate_norow", "svc_stable",
                                "svc_flap", "svc_nostale"):
                log(learn_line(row, ADOPTER_ID))
            if row and mode == "observeradopts":
                # The bug: a FOLLOWER that recovered. cluster_is_leader()
                # should have gated this to the leader; the guard's
                # step-9 log check must catch the second Adopted line.
                log("[FAILOVER] Adopted partition %s from dead node %u"
                    % (row["id"], dead_id))
                log("[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                    % dead_id)
            if mode == "lateflip":
                # The other bug: the loser flips to LEADER after the
                # adoption (late split-brain). Delay so the step-7
                # simultaneous-flip check has already broken out.
                def flip():
                    global late_flipped
                    time.sleep(1.5)
                    late_flipped = True
                threading.Thread(target=flip, daemon=True).start()
        return


threading.Thread(target=death_thread, daemon=True).start()


# ─── Pre-kill election churn: the live 2026-10-16 shape ──────────────────
# The env create (step 4.5) is the longest synchronous shell command in the
# system; on a loaded host its block outlasts the election timeout and a
# SURVIVOR wins the term mid-run. The leader the guard picked loses the
# role it must keep through the pre-kill phase -- its tree stops
# broadcasting, so the environment NAME its checkpoints must carry never
# enters any broadcast again -- and the guard's post-create checkpoint gate
# loses its precondition. The guard must diagnose that as a LOST
# PRECONDITION (a churned election, not a lost hop) and restart itself
# against the node that now leads, rather than burning every remaining
# window and failing the adoption arm it could still have run.
#
# The fake flips the leader file the moment the '-env' partition exists --
# the same instant the live create's block produced the flip. Nothing
# after that is faked: the guard's own re-drive loop, its role diagnosis,
# the exec'd restart, and the full 13-step chain against the NEW leader
# (which is then killed at step 6, adopted from, relaunched at step 10 and
# migrated-to at step 11 -- every id the base choreography hardcodes
# permutes through ADOPTER_ID and current_leader()).
if mode == "prekillchurn" and not created_preexisting:
    # Original boots only: the RELAUNCHED node is the resurrected
    # ex-leader, not an election winner -- re-flipping the file at its boot
    # (the '-env' row already exists, so the wait passes instantly) would
    # hand the dead node the election back mid-step-10, and on Windows the
    # write raced the constant current_leader() readers into a
    # PermissionError that killed the thread. The flip belongs to the
    # original node 2's mid-run position, once.

    def churn_thread():
        while True:
            if any(str(r.get("name", "")).endswith("-env")
                   for r in read_list(extra)):
                break
            time.sleep(0.05)
        # Land mid-create, like the live runs: after the -env partition
        # exists (the guard's env create follows it immediately) but before
        # its post-create checkpoint gate has counted anything -- node 2's
        # RX cadence is now paused by the leader deferral, so its gain in
        # the gate's window is 0 or 1, never the 2 the gate wants.
        time.sleep(0.3)
        write_leader(2)

    threading.Thread(target=churn_thread, daemon=True).start()


# ─── Resurrected-owner choreography (the guard's step 10) ────────────────
# The guard relaunches the killed leader from its saved argv. The relaunched
# fake boots fresh; because created.json already exists (the guard created
# the partition before the kill), the "boot restore" brings the stale row
# back -- except in resurrect_nostale (the bug: a boot that loses the stale
# row leaves nothing to conflict over) and, at the STEP-11 relaunch,
# migrate_nostale (the second relaunch loses the transferred row). The
# leader then re-announces its owned row after a delay (the periodic
# re-announce), and the survivors' claim thread logs the verdict: reject +
# keep the adopter (stable), or apply + flap (flap).
#
# The step-11 relaunch is the SAME thread running in the third process: at
# that point migrate.json already exists, so the row it restores is the
# TRANSFERRED row (owner = this node) -- there is no conflict, so the
# leader-wins convergence dance is skipped and it just re-announces.
# Modes that must pass the guard's step 10 (the resurrected-owner conflict
# gate) -- every PASS mode AND every migrate_* mode, because the migrate
# teeth can only fail at a step-11 gate if step 10 got them there first.
STEP10_MODES = ("adopted", "learnlost1", "ckptlost1", "prekillchurn",
                "resurrect_stable",
                "migrate_stable",
                "migrate_flap", "migrate_nolease", "migrate_noapply",
                "migrate_nostale", "migrate_noreacquire", "migrate_lost1",
                "migrate_norow", "svc_stable", "svc_flap", "svc_nostale")
if role == "leader" or mode == "prekillchurn":

    def resurrect_thread():
        if not created_preexisting:
            return   # the ORIGINAL leader process: the guard SIGKILLs us
                     # and relaunches; only the relaunched process restores
                     # and re-announces its row
        row = None
        for _ in range(200):
            row = row_owned_by(node_id)
            if row:
                break
            time.sleep(0.05)
        if not row:
            return
        # A real relaunch TRUNCATES the node's serial log -- QEMU's chardev
        # logfile starts a fresh file on every process start (observed on a
        # live cluster: node2.log carried exactly one boot banner across
        # three boots of node 2). The guard's step-11e restore gate scopes
        # against the last "AEROSLS BOOT LOGGER" marker precisely because of
        # this. The fake must truncate here too, or the previous boot's
        # restore line lingers and the "lose the transferred row" tooth
        # (migrate_nostale) false-passes.
        with open(logfile, "w"):
            pass
        if mode == "resurrect_nostale":
            return   # first relaunch: never restore (step-10 gate bites)
        if mode == "migrate_nostale" and os.path.exists(migrate):
            return   # step-11 relaunch: lose the transferred row (gate bites)
        # Every real boot starts with kernel.c's generation marker, and the
        # guard scopes each relaunch's restore against the LAST such marker.
        # The fake logs the same marker so the scope gate behaves identically.
        log("[AEROSLS BOOT LOGGER V1.0.0 RUNNING]")
        # The boot restore: the stale (or, at step 11, transferred) row.
        log("[PERSIST] Partition ownership restored from NVMe.")
        time.sleep(1.0)   # the re-announce period, scaled by FAST
        # The periodic re-announce of the row this node owns. The claimer's
        # id rides along: the survivors' verdict lines name both halves of
        # the conflict, and only the claimer knows which half it is (base:
        # node 1, the boot leader; prekillchurn: node 2, the churned
        # winner). Nothing READS the row body -- the survivors gate on the
        # file's existence -- so adding the field cannot break a reader.
        with open(os.path.join(state, "resurrect_claim"), "w") as f:
            f.write(json.dumps(dict(row, claimer=node_id)))
        # Step-11 relaunch: the transferred row is already ours -- no
        # conflict to resolve, the cluster agrees. (The guard's step-11
        # gates only need the restore + the agreement.)
        if os.path.exists(migrate):
            return
        # Step-10 convergence runs for the pass modes (the guard's step 10
        # runs unconditionally, so "adopted" now exercises the full chain
        # too).
        if mode not in STEP10_MODES:
            return
        # Convergence: the leader's own periodic re-announce of the row it
        # adopted reaches us; its claim is the leader's, so it wins and we
        # learn the new owner. (Wire-in-miniature: the survivors signal
        # their rejection, standing in for the leader's re-announce.)
        for _ in range(200):
            if os.path.exists(os.path.join(state, "resurrect_rejected")):
                break
            time.sleep(0.05)
        # The real kernel's conflict lines carry the partition id, not the
        # name (partition_sync_upsert logs the claimed-by pair before the
        # name is in scope) -- keep the fake byte-identical. The pair is
        # this node (the resurrected, still-stale claimer) and the ADOPTER:
        # node 2 in every base mode, node 1 in prekillchurn (byte-identical
        # to the old hardcoded pair, and correct even though the base modes
        # never move the leader file).
        adopter = ADOPTER_ID
        log("[PARTITION] sync: partition %s claimed by node %u and node %u "
            "-- leader node %u's claim wins, applying."
            % (row["id"], node_id, adopter, adopter))
        log(learn_line(row, adopter))
        # Step 12's service resolve on this node derives the node from the
        # partition owner; the owner is 2 once the leader has converged.
        with open(os.path.join(state, "leader_converged"), "w") as fc:
            fc.write("1")

    threading.Thread(target=resurrect_thread, daemon=True).start()

if role == "follower" or mode == "prekillchurn":
    flapped = False

    def claim_thread():
        global flapped
        claim = os.path.join(state, "resurrect_claim")
        if mode == "prekillchurn" and created_preexisting:
            return   # the RELAUNCHED node: it is the claimer here, not a
                     # survivor -- it must not "reject" its own claim (the
                     # base relaunch is the argv leader and never reaches
                     # this thread at all)
        # Life-bounded: the relaunched leader re-announces after its boot,
        # which is far into the guard's run.
        while True:
            if os.path.exists(claim):
                # The verdict is about the row AS CLAIMED: read it from the
                # claim file, so a restarted run's '-1' row (id in
                # extra.json, the row the guard's step-10 grep names) is
                # the one judged -- not this node's own created.json view.
                # The base claim file carries created.json's row, so its
                # bytes are unchanged; a torn file falls back to it.
                try:
                    payload = json.load(open(claim))
                except (OSError, ValueError):
                    payload = {}
                row = payload if payload.get("id") is not None else read_created()
                if not row:
                    return
                try:
                    claimer = int(payload.get("claimer", 1))
                except (AttributeError, TypeError, ValueError):
                    claimer = 1   # the base shape: the boot leader claims
                # The LIVE OWNER from this survivor's view: the adopter in
                # every base mode (byte-identical to the old hardcoded
                # "node 2"), node 1 in prekillchurn.
                owner = ADOPTER_ID
                if os.path.exists(migrate):
                    # Step-11 re-announce of the transferred row (owner =
                    # the destination): consistent with our post-migrate
                    # view -- a plain echo learn, no conflict.
                    m = read_migrate()
                    log(learn_line(row, m["dest"] if m else 1))
                    return
                if mode == "resurrect_flap":
                    # The bug: the survivors APPLY the stale claim (the old
                    # "last announce wins" rule). The partition flaps back
                    # to the resurrected owner -- the CLAIMER, which is the
                    # stale row's own node.
                    log("[PARTITION] sync: partition %s claimed by node %u and "
                        "node %u -- taking the newer."
                        % (row["id"], owner, claimer))
                    log(learn_line(row, claimer))
                    flapped = True
                elif mode in STEP10_MODES:
                    log("[PARTITION] sync: partition %s claimed by node %u and "
                        "node %u -- keeping node %u (live owner), rejecting the "
                        "stale claim."
                        % (row["id"], owner, claimer, owner))
                    if not os.path.exists(os.path.join(state, "resurrect_rejected")):
                        with open(os.path.join(state, "resurrect_rejected"), "w") as f:
                            f.write("1")
                return
            time.sleep(0.05)

    threading.Thread(target=claim_thread, daemon=True).start()


# ─── Step-11 migrate RX: the destination (node 1) and the observer (node 3)
# apply the owner-initiated transfer when the adopter migrates (here: when
# migrate.json appears). The source (node 2) issues the migrate itself.
# migrate_noapply suppresses the apply lines (the transfer gate must bite);
# migrate_flap logs the apply but flips to the WRONG owner (the flap).
def migrate_poll_thread():
    global migrated_to
    # The ADOPTER issues the migrate itself; every other node applies it.
    # Which node that is comes from the row (base: node 2; prekillchurn:
    # node 1, the churned election's adopter), so the argv shape is no
    # longer assumed -- and a base node 2 still skips exactly as before,
    # just after the row lands instead of at boot (nothing logged either
    # way).
    while not os.path.exists(migrate):
        time.sleep(0.05)
    try:
        if int(read_migrate().get("adopter", 2)) == node_id:
            return
    except (AttributeError, TypeError, ValueError):
        return
    # Life-bounded: the step-11 transfer is the last thing the guard does.
    while True:
        if os.path.exists(migrate):
            if mode == "migrate_noapply":
                return   # the transfer never arrives (gate bites)
            # Retry-tolerant, both files: a torn migrate row or a not-yet
            # readable created row means "not yet", never a dead thread.
            # The old `if not row: return` and bare json.load() killed this
            # thread silently -- the single observed migrate_stable miss.
            row = read_created()
            m = read_migrate()
            if row is None or m is None:
                time.sleep(0.05)
                continue
            owner_after = (int(m["dest"]) if mode != "migrate_flap"
                           else int(m["adopter"]))
            log("[PARTITION] sync: partition %s claimed by node %s and node %s -- "
                "owner-initiated transfer, applying."
                % (m["id"], m["adopter"], m["dest"]))
            log(learn_line({"id": m["id"], "name": m["name"]}, owner_after))
            with lock:
                migrated_to = owner_after
            return
        time.sleep(0.05)


threading.Thread(target=migrate_poll_thread, daemon=True).start()


# ─── Step-11 lease + migrate handling (the shell the guard drives) ────────
def acquire_lease(pid):
    global lease_held
    # The command succeeds either way -- the guard's acquire shell step must
    # not die; the GATES read the hold/rows afterwards.
    if mode == "migrate_nolease":
        return   # the adopter's acquire never holds (lease-held gate bites)
    if node_id == leader_id and mode == "migrate_noreacquire":
        return   # the destination's re-acquire never holds (gate bites)
    # Each acquire is one CAMPAIGN and the row carries its term, so the RX
    # side can tell a re-driven campaign from the first one (migrate_lost1).
    # The term comes from the row on disk, so it is monotonic ACROSS
    # processes: step 11a's acquire here (node 2) and step 11f's
    # re-acquire on node 1 count in the same series. The row is written
    # WHOLE (tmp + os.replace): the RX side reads a ROW, never a
    # half-arrived packet. The in-place buffered write this replaces had a
    # zero-byte window, and a reader's bare json.load in exactly that
    # window killed its RX thread (the CI false RED of 2026-09-25 in the
    # 2-node sibling; this file's reader had the same read and the same
    # window).
    term = lease_term(read_lease()) + 1
    tmp = "%s.tmp%d" % (lease, os.getpid())
    with open(tmp, "w") as f:
        # "acquirer": which node ISSUED this campaign -- the RX side's skip
        # rule reads it (the acquirer mirrors nothing; everyone else
        # mirrors the row), so the argv shape need not be assumed.
        json.dump({"partition_id": int(pid), "term": term,
                   "acquirer": node_id},
                  f, separators=(",", ":"))
    os.replace(tmp, lease)
    with lock:
        lease_held = True
    log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)." % pid)
    log("[CONSENSUS] partition %s: node %u campaigning for write lease, term %d."
        % (pid, node_id, term))
    log("[MMU-LEASE] partition %s: page permissions force_read_only=1" % pid)
    log("[CONSENSUS] partition %s: quorum stable, node %u elected LEADER "
        "(write lease) for term %d." % (pid, node_id, term))
    log("[MMU-LEASE] partition %s: page permissions force_read_only=0" % pid)


def do_migrate(pid, dest):
    global lease_held, migrated_to
    row = read_created()
    pid = pid or (str(row["id"]) if row else "1")
    # The row being migrated is the one the COMMAND names (the guard
    # migrates $NAME -- in a restarted prekillchurn run that is the '-1'
    # row in extra.json, not created.json's stale first row; in the base
    # modes the command names created.json's row, so the bytes are
    # unchanged). The id+name travel in migrate.json so the receivers'
    # transfer lines carry the same row the guard scopes its gate to.
    for r in all_rows():
        if str(r["id"]) == str(pid):
            row = r
            break
    # The SAME two lines the real kernel prints inside the shell capture
    # window: partition_migrate() runs synchronously under sls_shell_execute,
    # so its step-down and success lines land in the aeroslsctl RESPONSE,
    # never the serial log. The guard now reads them from the response --
    # return them (and still log them, keeping the node's serial story
    # complete for anyone tailing a fake node).
    out = (
        "[CONSENSUS] partition %s: node %u voluntarily stepped down from LEADER "
        "-- lease relinquished (term 1 unchanged).\n"
        "[PARTITION] migrated partition %s: node %u -> node %u. Lease "
        "relinquished=yes, 0 stream(s) sent and confirmed by the destination, "
        "0 live context(s) checkpointed and sent, 0 physical frame(s) "
        "reclaimed. Partition remains PAUSED." % (pid, node_id, pid, node_id, dest)
    )
    log(out)
    with lock:
        lease_held = False   # the source's write permission is gone
        if mode != "migrate_flap":
            migrated_to = dest
    # Whole-write (tmp + os.replace), the same rule the lease row and the
    # re-announce counter follow: the RX side on nodes 1/3 polls this file
    # every 50 ms from OTHER processes, and an in-place write shows them a
    # zero-byte file for as long as this writer is preempted between open
    # and close -- enough once to kill the destination's transfer thread.
    tmp = "%s.tmp%d" % (migrate, os.getpid())
    with open(tmp, "w") as f:
        json.dump({"id": row["id"] if row else 1,
                   "name": row["name"] if row else "",
                   "dest": dest, "adopter": node_id}, f, separators=(",", ":"))
    os.replace(tmp, migrate)
    return out


class H(http.server.BaseHTTPRequestHandler):
    def _json(self, code, obj):
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _role(self):
        # The leader FILE, not the boot argv: an election won mid-run (the
        # prekillchurn tooth) must move the role view the guard reads off
        # /api/cluster -- which is exactly how the live 2026-10-16 runs
        # diagnosed the churn. For every other mode the file names the boot
        # leader until a survivor's own election, so each state the base
        # choreography reaches returns the argv answer.
        if transitioned:
            if late_flipped:
                return "LEADER"
            return my_transition
        return "LEADER" if current_leader() == node_id else "FOLLOWER"

    def _active(self):
        if transitioned:
            return 2
        return 3

    def _partitions(self):
        """The /api/partitions body. The leader serves the current created
        row(s); a follower serves the rows only once it has learned them, with
        the owner depending on what the transition/migrate did with them.
        holds_lease is partition_holds_write_lease()'s raw output: 1 only
        on the process that won an acquire (the adopter before the migrate,
        the destination after its re-acquire).

        EVERY row (the primary $NAME plus the -env partition the guard's step
        4.5 creates) is decorated identically: they are one cluster table, and
        the guard reads $NAME by name from the adopter in step 10 while reading
        the -env row's id in step 4.5 -- so both must be listed with the same
        owner derivation."""
        lease_val = 1 if lease_held else 0
        rows = all_rows()

        def decorate(owner=None, keep_owner=False):
            out = []
            for row in rows:
                r = dict(row)
                if not keep_owner:
                    r["owner_node"] = owner
                r["holds_lease"] = lease_val
                out.append(r)
            return out

        if (current_leader() == node_id
                or (role == "leader" and mode != "prekillchurn")):
            if not rows:
                return {"partitions": []}
            return {"partitions": decorate(migrated_to if migrated_to
                                            else node_id)}
        if mode == "nolearn":
            return {"partitions": []}
        if not learned and not created_preexisting:
            # A RELAUNCHED node serves what its boot restore brought back
            # (prekillchurn's restarted leader, whose argv is FOLLOWER): the
            # row is on ITS disk, so serving it is the restore, not a leak.
            # Base followers never preexist created.json, so this is the
            # argv gate it replaces, byte for byte.
            return {"partitions": []}
        if transitioned:
            if my_transition == "LEADER":
                if mode == "notadopted":
                    # Adopted nothing: the rows still claim the DEAD leader.
                    return {"partitions": decorate(keep_owner=True)}
                # resurrect_flap: the stale claim was applied, owner = 1.
                if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                    return {"partitions": decorate(1)}
                # Adopted (owner = this node); after the step-11 migrate,
                # migrated_to names the destination.
                return {"partitions": decorate(migrated_to if migrated_to
                                                else node_id)}
            # Observer: learned the handoff, owner = the adopter (node 2);
            # after the step-11 migrate, migrated_to names the destination.
            if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                return {"partitions": decorate(1)}
            return {"partitions": decorate(migrated_to if migrated_to
                                            else ADOPTER_ID)}
        # Not yet transitioned: the plain learned rows.
        out = []
        for row in rows:
            r = dict(row)
            r["holds_lease"] = lease_val
            out.append(r)
        return {"partitions": out}

    def do_GET(self):
        if self.path == "/api/health":
            # The leadergap tooth's bounded unreachability window: sleep out
            # whatever is left of it before answering, so every poll the
            # survivors make during the window really does time out.
            if role == "leader" and gap_until:
                left = gap_until - time.monotonic()
                if left > 0:
                    time.sleep(left)
            self._json(200, {"ok": "true", "ready": "true"})
        elif self.path == "/api/cluster":
            self._json(200, {"node_id": node_id, "role": self._role(),
                             "initialised": "true",
                             "active_nodes": self._active(),
                             "quorum_threshold": 2})
        elif self.path == "/api/partitions":
            self._json(200, self._partitions())
        elif self.path.startswith("/api/service/resolve/"):
            name = self.path[len("/api/service/resolve/"):]
            if svc_local:
                nid = svc_owner()
                self._json(200, {"ok": "true", "name": name,
                                 "partition_id": 1, "node_id": nid,
                                 "is_local": "true", "is_remote": "false"})
            elif svc_remote:
                self._json(200, {"ok": "true", "name": name,
                                 "partition_id": 1, "node_id": svc_remote,
                                 "is_local": "false", "is_remote": "true"})
            else:
                self._json(200, {"ok": "false", "name": name,
                                 "error": "not found"})
        else:
            self._json(404, {"error": f"no fake route {self.path}"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n).decode() or "{}")
        except ValueError:
            req = {}
        if self.path == "/api/service":
            global svc_local
            name = req.get("name", "")
            pid = int(req.get("partition_id", 1))
            gen = 0
            prev_declarer = 0
            if os.path.exists(svc):
                try:
                    pd = json.load(open(svc))
                    gen = pd.get("gen", 0)
                    prev_declarer = pd.get("declarer", 0)
                except ValueError:
                    pass
            if (mode == "svc_nostale" and prev_declarer
                    and prev_declarer != node_id and node_id != svc_owner()):
                # The bug: the resurrected owner's stale re-announce never
                # fires (the announce is broken), so no reject can log.
                # Narrowed to the NON-OWNER re-announce shape: the adopter's
                # own re-registration after the adoption (owner-initiated,
                # node_id == svc_owner()) must NOT be suppressed -- a buggy
                # suppression once made the step-7.5 gate read the adopter's
                # re-registration as inert.
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": "declared (announce suppressed)"})
                return
            with open(svc, "w") as f:
                json.dump({"name": name, "partition_id": pid,
                           "declarer": node_id, "gen": gen + 1},
                          f, separators=(",", ":"))
            svc_local = True
            log("[SERVICE] registered '%s' -> partition %s, TCP port %s."
                % (name, pid, req.get("endpoint_port", 0)))
            self._json(200, {"ok": "true", "recognized": "true",
                             "output": "declared"})
            return
        if self.path == "/api/shell/exec":
            command = req.get("command", "")
            if (current_leader() == node_id or (role == "leader"
                    and mode != "prekillchurn")) \
                    and command.startswith("partition create "):
                name = command[len("partition create "):].strip()
                # The FIRST create ($NAME) owns created.json -- every earlier
                # gate keys on that single row, exactly as before. The guard's
                # SECOND create (the -env partition, step 4.5) appends, so the
                # $NAME row survives for steps 10-12 to look up again, and each
                # create gets its own id (the guard resolves PART_ID2 by name).
                if not os.path.exists(created):
                    pid = 1
                    with open(created, "w") as f:
                        json.dump({"id": pid, "owner_node": node_id, "name": name,
                                   "state": "active"}, f, separators=(",", ":"))
                else:
                    rows = read_list(extra)
                    pid = 1 + max([int(r["id"]) for r in rows], default=1)
                    rows.append({"id": pid, "owner_node": node_id, "name": name,
                                 "state": "active"})
                    with open(extra, "w") as f:
                        json.dump(rows, f, separators=(",", ":"))
                # The smoke's leadergap tooth: this create opens a BOUNDED
                # window in which the leader answers /api/health late (see
                # do_GET), modelling a busy leader rather than a dead one.
                global gap_until
                try:
                    gap_until = time.monotonic() + float(
                        open(gap_file).read().strip() or 0)
                except (OSError, ValueError):
                    pass
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": f"created partition {pid} ({name})"})
            elif command.startswith("partition lease acquire "):
                pid = command[len("partition lease acquire "):].strip()
                acquire_lease(pid)
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": f"lease acquire {pid} issued"})
            elif command.startswith("env list "):
                # The guard polls this to prove the create round trip completed
                # (its step 4.5 console-registration gate).
                pid = int(command[len("env list "):].strip() or 0)
                mine = [e for e in read_envs() if int(e["partition"]) == pid]
                outp = ["[ENV] list partition=%d live=%d" % (pid, len(mine))]
                for e in mine:
                    outp.append("[ENV] env partition=%d env_id=%d index=%d "
                                "posix_pid=%d dropped=0"
                                % (e["partition"], e["env_id"], e["index"],
                                   100 + int(e["env_id"])))
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": "\n".join(outp)})
            elif command.startswith("partition migrate "):
                parts = command[len("partition migrate "):].split()
                pid = parts[0] if parts else ""
                dest = int(parts[1]) if len(parts) > 1 else 0
                out = do_migrate(pid, dest)
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": out})
            elif command.startswith("env create "):
                # The guard's step 4.5: an environment inside the -env
                # partition. The output shape is kernel/env_service_create's
                # shell line -- "[ENV] create partition=P index=I -> ok
                # env_id=N" -- which is what the guard parses its env_id from.
                pid = int(command[len("env create "):].strip() or 0)
                envs = read_envs()
                eid = 1 + max([int(e["env_id"]) for e in envs], default=0)
                envs.append({"partition": pid, "index": 1, "env_id": eid})
                with open(envs_f, "w") as f:
                    json.dump(envs, f, separators=(",", ":"))
                self._json(200, {
                    "ok": "true", "recognized": "true",
                    "output": "[ENV] create partition=%d index=1 -> ok env_id=%d"
                              % (pid, eid)})
            else:
                self._json(200, {"ok": "false", "error": f"not a create: {command}"})
        else:
            self._json(404, {"error": f"no fake POST route {self.path}"})

    def log_message(self, *a):
        pass


# ThreadingHTTPServer, not HTTPServer: the plain one serves ONE request at a
# time, so every concurrent poller queues behind the others -- and every
# client here has a short timeout (the follower's own 0.5s leader poll, the
# guard's curl, the smoke's curl), which turns queueing into a verdict. That
# is not hypothetical: with three smokes running at once, a follower
# declared the leader DEAD while it was alive (its 0.5s poll timed out
# behind the guard's own requests), transitioned, and the guard then read
# its row as "never learned" -- with both followers' logs showing the learn
# and the checkpoint on disk. A real kernel answers its services
# concurrently, so the threaded server is also the more faithful model.
srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(600)
