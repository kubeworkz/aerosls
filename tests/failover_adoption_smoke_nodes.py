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
leader_id = int(open(os.path.join(state, "leader")).read().strip())

created = os.path.join(state, "created.json")
lease = os.path.join(state, "lease.json")
migrate = os.path.join(state, "migrate.json")
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
else:
    my_transition = "LEADER" if (role == "follower" and node_id == 2) else "FOLLOWER"

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


def learn_line(row, owner):
    return ("[PARTITION] sync: partition %s '%s' (owner node %s) learned "
            "from node %s." % (row["id"], row["name"], owner, owner))


def leader_port():
    # The smoke lays out nodes on consecutive ports: 1 -> BASE+1.
    # We only know our own port, so derive the leader's port the same way
    # the smoke does: leader_id is 1, its port is port - node_id + leader_id.
    return port - node_id + leader_id


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


def leader_alive():
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{leader_port()}/api/health", timeout=0.5):
            return True
    except Exception:
        return False


# ─── RX thread: learn the created row, log learn + checkpoint ─────────────
# The guard creates the partition AFTER the fakes are up, so the learn
# cannot be logged at process start: the follower's announce RX fires when
# the row arrives on the wire (here: when created.json appears). nolearn
# never learns (the guard's learn gate must bite); nockpt learns but never
# logs the checkpoint RX (the checkpoint gate must bite).
learned = False


def rx_thread():
    global learned
    if role != "follower":
        return
    # Life-bounded, not counter-bounded (see the rule at the top of this
    # file): the create announce can arrive at any point in the guard's run,
    # including after its own WAIT_NODES pre-create phase has spent its
    # whole budget.
    row = None
    while row is None:
        row = read_created()
        if row is None:
            time.sleep(0.05)
    if row and mode != "nolearn":
        log(learn_line(row, leader_id))
        learned = True   # learned regardless of nockpt: the learn gate
                         # must pass, and the checkpoint gate is the log
                         # line below, which nockpt skips.
        if mode != "nockpt":
            log("[DSPP-CKPT] RX: COMPLETE seq=1000 (448 bytes)")


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
    if node_id == 2:
        return   # node 2 is the adopter -- it ISSUES the acquire itself
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
    # Same owner derivation as _partitions(): the original leader (or the
    # step-11 destination), node 2 once the adopter transitioned, and node
    # 2 on the leader once it converged (step 10).
    row = read_created()
    if not row:
        return 0
    if role == "leader":
        if migrated_to:
            return migrated_to
        if os.path.exists(os.path.join(state, "leader_converged")):
            return 2
        return node_id
    if migrated_to:
        return migrated_to
    if transitioned:
        return 2
    return leader_id


def svc_rx_thread():
    global svc_local, svc_remote, svc_last_gen
    if role == "leader":
        return   # local registrations shadow everything on the owner
    # Life-bounded: the step-12 re-announce comes long after the pre-kill
    # registration, way outside a counter started at process boot.
    while True:
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
    if role != "follower":
        return
    # Life-bounded: the guard SIGKILLs the leader after its own gates have
    # run, and this poll must still be listening when it does.
    # Sustained silence, not "3 strikes": the old rule declared the leader
    # DEAD after three failed polls, which a busy leader can produce. With
    # three smokes running at once a follower's 0.5s poll timed out behind
    # the guard's own requests, it declared the leader dead while the leader
    # was answering, transitioned, and the guard then read its row as
    # "never learned" -- a verdict decided by a client timeout.
    silent_since = None
    while True:
        if leader_alive():
            silent_since = None
            time.sleep(0.2)
            continue
        if silent_since is None:
            silent_since = time.monotonic()
        if time.monotonic() - silent_since < DEAD_SILENCE:
            time.sleep(0.1)
            continue
        transitioned = True
        # Every node that observes the death DECLARES it (failover_tick
        # runs everywhere); only the leader recovers.
        log("[FAILOVER] Node %u declared DEAD (silent 300 ticks)" % leader_id)
        row = read_created()
        if my_transition == "LEADER":
            if mode != "notadopted" and row:
                log("[FAILOVER] Adopted partition %s from dead node %u"
                    % (row["id"], leader_id))
                log("[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                    % leader_id)
        else:
            # The observer: it merely learns the handoff announce from
            # the adopter -- except in the misbehaviour modes. The
            # resurrect/migrate modes adopt exactly like "adopted" for
            # the death phase; the extra steps are the relaunch and the
            # step-11 migration.
            if row and mode in ("adopted", "observeradopts", "lateflip",
                                "resurrect_stable", "resurrect_flap",
                                "resurrect_nostale", "migrate_stable",
                                "migrate_flap", "migrate_nolease",
                                "migrate_noapply", "migrate_nostale",
                                "migrate_noreacquire", "migrate_lost1",
                                "migrate_norow", "svc_stable",
                                "svc_flap", "svc_nostale"):
                log(learn_line(row, 2))
            if row and mode == "observeradopts":
                # The bug: a FOLLOWER that recovered. cluster_is_leader()
                # should have gated this to the leader; the guard's
                # step-9 log check must catch the second Adopted line.
                log("[FAILOVER] Adopted partition %s from dead node %u"
                    % (row["id"], leader_id))
                log("[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                    % leader_id)
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
STEP10_MODES = ("adopted", "resurrect_stable", "migrate_stable",
                "migrate_flap", "migrate_nolease", "migrate_noapply",
                "migrate_nostale", "migrate_noreacquire", "migrate_lost1",
                "migrate_norow", "svc_stable", "svc_flap", "svc_nostale")
if role == "leader":

    def resurrect_thread():
        if not created_preexisting:
            return   # the ORIGINAL leader process: the guard SIGKILLs us
                     # and relaunches; only the relaunched process restores
                     # and re-announces its row
        row = None
        for _ in range(200):
            row = read_created()
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
        # The periodic re-announce of the row this node owns.
        with open(os.path.join(state, "resurrect_claim"), "w") as f:
            f.write(json.dumps(row))
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
        # name is in scope) -- keep the fake byte-identical.
        log("[PARTITION] sync: partition %s claimed by node 1 and node 2 -- "
            "leader node 2's claim wins, applying." % row["id"])
        log(learn_line(row, 2))
        # Step 12's service resolve on this node derives the node from the
        # partition owner; the owner is 2 once the leader has converged.
        with open(os.path.join(state, "leader_converged"), "w") as fc:
            fc.write("1")

    threading.Thread(target=resurrect_thread, daemon=True).start()

if role == "follower":
    flapped = False

    def claim_thread():
        global flapped
        claim = os.path.join(state, "resurrect_claim")
        # Life-bounded: the relaunched leader re-announces after its boot,
        # which is far into the guard's run.
        while True:
            if os.path.exists(claim):
                row = read_created()
                if not row:
                    return
                if os.path.exists(migrate):
                    # Step-11 re-announce of the transferred row (owner =
                    # the destination): consistent with our post-migrate
                    # view -- a plain echo learn, no conflict.
                    log(learn_line(row, 1))
                    return
                if mode == "resurrect_flap":
                    # The bug: the survivors APPLY the stale claim (the old
                    # "last announce wins" rule). The partition flaps back
                    # to the resurrected owner.
                    log("[PARTITION] sync: partition %s claimed by node 2 and "
                        "node 1 -- taking the newer." % row["id"])
                    log(learn_line(row, 1))
                    flapped = True
                elif mode in STEP10_MODES:
                    log("[PARTITION] sync: partition %s claimed by node 2 and "
                        "node 1 -- keeping node 2 (live owner), rejecting the "
                        "stale claim." % row["id"])
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
    if node_id == 2:
        return   # the adopter issues the migrate itself
    # Life-bounded: the step-11 transfer is the last thing the guard does.
    while True:
        if os.path.exists(migrate):
            row = read_created()
            if not row:
                return
            m = json.load(open(migrate))
            owner_after = 1 if mode != "migrate_flap" else 2
            if mode == "migrate_noapply":
                return   # the transfer never arrives (gate bites)
            log("[PARTITION] sync: partition %s claimed by node %s and node %s -- "
                "owner-initiated transfer, applying."
                % (row["id"], m["adopter"], m["dest"]))
            log(learn_line(row, owner_after))
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
        json.dump({"partition_id": int(pid), "term": term},
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
    with open(migrate, "w") as f:
        json.dump({"id": row["id"] if row else 1,
                   "name": row["name"] if row else "",
                   "dest": dest, "adopter": node_id}, f, separators=(",", ":"))
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
        if role == "leader":
            return "LEADER"
        if transitioned:
            if late_flipped:
                return "LEADER"
            return my_transition
        return "FOLLOWER"

    def _active(self):
        if role == "leader":
            return 3
        if transitioned:
            return 2
        return 3

    def _partitions(self):
        """The /api/partitions body. The leader serves the current created
        row; a follower serves the row only once it has learned it, with
        the owner depending on what the transition/migrate did with it.
        holds_lease is partition_holds_write_lease()'s raw output: 1 only
        on the process that won an acquire (the adopter before the migrate,
        the destination after its re-acquire)."""
        row = read_created()
        lease_val = 1 if lease_held else 0
        if role == "leader":
            if not row:
                return {"partitions": []}
            r = dict(row)
            r["owner_node"] = migrated_to if migrated_to else node_id
            r["holds_lease"] = lease_val
            return {"partitions": [r]}
        if mode == "nolearn":
            return {"partitions": []}
        if not learned:
            return {"partitions": []}
        if transitioned:
            if my_transition == "LEADER":
                if mode == "notadopted":
                    # Adopted nothing: the row still claims the DEAD leader.
                    r = dict(row)
                    r["holds_lease"] = lease_val
                    return {"partitions": [r]}
                # resurrect_flap: the stale claim was applied, owner = 1.
                if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                    r = dict(row)
                    r["owner_node"] = 1
                    r["holds_lease"] = lease_val
                    return {"partitions": [r]}
                # Adopted (owner = this node); after the step-11 migrate,
                # migrated_to names the destination.
                r = dict(row)
                r["owner_node"] = migrated_to if migrated_to else node_id
                r["holds_lease"] = lease_val
                return {"partitions": [r]}
            # Observer: learned the handoff, owner = the adopter (node 2);
            # after the step-11 migrate, migrated_to names the destination.
            if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                r = dict(row)
                r["owner_node"] = 1
                r["holds_lease"] = lease_val
                return {"partitions": [r]}
            r = dict(row)
            r["owner_node"] = migrated_to if migrated_to else 2
            r["holds_lease"] = lease_val
            return {"partitions": [r]}
        # Not yet transitioned: the plain learned row.
        r = dict(row)
        r["holds_lease"] = lease_val
        return {"partitions": [r]}

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
            if role == "leader" and command.startswith("partition create "):
                name = command[len("partition create "):].strip()
                with open(created, "w") as f:
                    json.dump({"id": 1, "owner_node": node_id, "name": name,
                               "state": "active"}, f, separators=(",", ":"))
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
                                 "output": f"created partition 1 ({name})"})
            elif command.startswith("partition lease acquire "):
                pid = command[len("partition lease acquire "):].strip()
                acquire_lease(pid)
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": f"lease acquire {pid} issued"})
            elif command.startswith("partition migrate "):
                parts = command[len("partition migrate "):].split()
                pid = parts[0] if parts else ""
                dest = int(parts[1]) if len(parts) > 1 else 0
                out = do_migrate(pid, dest)
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": out})
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
