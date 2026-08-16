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

The leader's death is detected the way the kernel detects it: the follower
polls the leader's HTTP port; when it stops answering, the follower
transitions. Which follower becomes leader, and what it does with the row,
is the mode:

Modes (written to <statedir>/mode by the smoke):
    adopted       node 2 becomes leader, declares, adopts (owner=2); node 3
                  observes the handoff -> guard PASS
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
                  and the leader CONVERGES to owner 2 -> guard PASS
    resurrect_flap  the survivors APPLY the stale claim instead of
                  rejecting it (the old "last announce wins" bug): the
                  partition flaps back to owner 1 -> guard FAIL
    resurrect_nostale  the relaunched leader never restores its stale row
                  (no restore log, no re-announce): there is nothing to
                  conflict over -> guard FAIL

Both survivors DECLARE the death (every node runs failover_tick); only the
leader recovers. So the observer's log always carries its own declaration
line -- that is what the step-9 log check compares the Adopted line
against.

The resurrect choreography is a second wire-in-miniature: the relaunched
leader writes <state>/resurrect_claim (its periodic re-announce), the
survivors poll it and log their verdict, and (stable) write
<state>/resurrect_rejected so the leader can log its convergence -- the
same file-passing the learn/death threads already use.
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
                # resurrect modes adopt exactly like "adopted" for the
                # death phase; the extra step is the relaunch.
                if row and mode in ("adopted", "observeradopts", "lateflip",
                                    "resurrect_stable", "resurrect_flap",
                                    "resurrect_nostale"):
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
# back -- except in resurrect_nostale, where the restore silently drops it
# (the bug: a boot that loses the stale row leaves nothing to conflict
# over). The leader then re-announces its owned row after a delay (the
# periodic re-announce), and the survivors' claim thread logs the verdict:
# reject + keep the adopter (stable), or apply + flap (flap).
if role == "leader":

    def resurrect_thread():
        row = None
        for _ in range(200):
            row = read_created()
            if row:
                break
            time.sleep(0.05)
        if not row or mode == "resurrect_nostale":
            return
        # The boot restore: the stale row (owner = this node) is durable.
        log("[PERSIST] Partition ownership restored from NVMe.")
        time.sleep(1.0)   # the re-announce period, scaled by FAST
        # The periodic re-announce of the row this node still owns.
        with open(os.path.join(state, "resurrect_claim"), "w") as f:
            f.write(json.dumps(row))
        # Convergence runs for the pass modes (the guard's step 10 runs
        # unconditionally, so "adopted" now exercises the full chain too).
        if mode not in ("resurrect_stable", "adopted"):
            return
        # Convergence: the leader's own periodic re-announce of the row it
        # adopted reaches us; its claim is the leader's, so it wins and we
        # learn the new owner. (Wire-in-miniature: the survivors signal
        # their rejection, standing in for the leader's re-announce.)
        for _ in range(200):
            if os.path.exists(os.path.join(state, "resurrect_rejected")):
                break
            time.sleep(0.05)
        log("[PARTITION] sync: partition %s '%s' claimed by node 1 and node 2 -- "
            "leader node 2's claim wins, applying." % (row["id"], row["name"]))
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
                if mode == "resurrect_flap":
                    # The bug: the survivors APPLY the stale claim (the old
                    # "last announce wins" rule). The partition flaps back
                    # to the resurrected owner.
                    log("[PARTITION] sync: partition %s claimed by node 2 and "
                        "node 1 -- taking the newer." % row["id"])
                    log(learn_line(row, 1))
                    flapped = True
                elif mode in ("resurrect_stable", "adopted"):
                    log("[PARTITION] sync: partition %s claimed by node 2 and "
                        "node 1 -- keeping node 2 (live owner), rejecting the "
                        "stale claim." % row["id"])
                    if not os.path.exists(os.path.join(state, "resurrect_rejected")):
                        with open(os.path.join(state, "resurrect_rejected"), "w") as f:
                            f.write("1")
                return
            time.sleep(0.05)

    threading.Thread(target=claim_thread, daemon=True).start()


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
        the owner depending on what the transition did with it."""
        row = read_created()
        if role == "leader":
            return {"partitions": [row] if row else []}
        if mode == "nolearn":
            return {"partitions": []}
        if not learned:
            return {"partitions": []}
        if transitioned:
            if my_transition == "LEADER":
                if mode == "notadopted":
                    # Adopted nothing: the row still claims the DEAD leader.
                    return {"partitions": [row] if row else []}
                # resurrect_flap: the stale claim was applied, owner = 1.
                if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                    r = dict(row)
                    r["owner_node"] = 1
                    return {"partitions": [r] if row else []}
                # Adopted: owner = this node.
                r = dict(row)
                r["owner_node"] = node_id
                return {"partitions": [r] if row else []}
            # Observer: learned the handoff, owner = the adopter (node 2).
            # resurrect_flap: the stale claim was applied, owner = node 1.
            if mode == "resurrect_flap" and "flapped" in globals() and flapped:
                r = dict(row)
                r["owner_node"] = 1
                return {"partitions": [r] if row else []}
            r = dict(row)
            r["owner_node"] = 2
            return {"partitions": [r] if row else []}
        # Not yet transitioned: the plain learned row.
        return {"partitions": [row] if row else []}

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
            else:
                self._json(200, {"ok": "false", "error": f"not a create: {command}"})
        else:
            self._json(404, {"error": f"no fake POST route {self.path}"})

    def log_message(self, *a):
        pass


srv = http.server.HTTPServer(("127.0.0.1", port), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(600)
