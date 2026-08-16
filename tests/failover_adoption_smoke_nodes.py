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
    win, then "quorum stable, node M elected LEADER (write lease)".
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
                  step-7 simultaneous split-brain check
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


def learn_line(row, owner):
    return ("[PARTITION] sync: partition %s '%s' (owner node %s) learned "
            "from node %s." % (row["id"], row["name"], owner, owner))


def leader_port():
    # The smoke lays out nodes on consecutive ports: 1 -> BASE+1.
    # We only know our own port, so derive the leader's port the same way
    # the smoke does: leader_id is 1, its port is port - node_id + leader_id.
    return port - node_id + leader_id


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
    row = None
    for _ in range(200):
        row = read_created()
        if row:
            break
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
def lease_rx_thread():
    if node_id == 2:
        return   # node 2 is the adopter -- it ISSUES the acquire itself
    for _ in range(600):
        if os.path.exists(lease):
            break
        time.sleep(0.05)
    if os.path.exists(lease):
        pid = json.load(open(lease)).get("partition_id", "1")
        log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)." % pid)


threading.Thread(target=lease_rx_thread, daemon=True).start()


# ─── Death-detection thread: the guard SIGKILLs the leader ────────────────
# The follower polls the leader's port; when it stops answering, the
# follower transitions: the adopter (node 2 in adopted/notadopted, both in
# splitbrain) flips to LEADER and logs the death + (unless notadopted) the
# adoption; the observer (node 3 in adopted) logs the handoff.
def death_thread():
    global transitioned, late_flipped
    if role != "follower":
        return
    for _ in range(600):
        if not leader_alive():
            # 3 consecutive failures to avoid a transient blip.
            ok = True
            for _ in range(3):
                if leader_alive():
                    ok = False
                    break
                time.sleep(0.1)
            if not ok:
                time.sleep(0.2)
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
                                    "migrate_noreacquire"):
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
        time.sleep(0.2)


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
                "migrate_nostale", "migrate_noreacquire")
if role == "leader":

    def resurrect_thread():
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

    threading.Thread(target=resurrect_thread, daemon=True).start()

if role == "follower":
    flapped = False

    def claim_thread():
        global flapped
        claim = os.path.join(state, "resurrect_claim")
        for _ in range(600):
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
    for _ in range(600):
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
    with open(lease, "w") as f:
        json.dump({"partition_id": int(pid)}, f, separators=(",", ":"))
    with lock:
        lease_held = True
    log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)." % pid)
    log("[CONSENSUS] partition %s: node %u campaigning for write lease, term 1."
        % (pid, node_id))
    log("[MMU-LEASE] partition %s: page permissions force_read_only=1" % pid)
    log("[CONSENSUS] partition %s: quorum stable, node %u elected LEADER "
        "(write lease) for term 1." % (pid, node_id))
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
            self._json(200, {"ok": "true", "ready": "true"})
        elif self.path == "/api/cluster":
            self._json(200, {"node_id": node_id, "role": self._role(),
                             "initialised": "true",
                             "active_nodes": self._active(),
                             "quorum_threshold": 2})
        elif self.path == "/api/partitions":
            self._json(200, self._partitions())
        else:
            self._json(404, {"error": f"no fake route {self.path}"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n).decode() or "{}")
        except ValueError:
            req = {}
        if self.path == "/api/shell/exec":
            command = req.get("command", "")
            if role == "leader" and command.startswith("partition create "):
                name = command[len("partition create "):].strip()
                with open(created, "w") as f:
                    json.dump({"id": 1, "owner_node": node_id, "name": name,
                               "state": "active"}, f, separators=(",", ":"))
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


srv = http.server.HTTPServer(("127.0.0.1", port), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(600)
