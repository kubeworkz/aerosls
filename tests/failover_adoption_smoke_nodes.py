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
    splitbrain    BOTH survivors become leader -> guard FAIL (split-brain)
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
    global transitioned
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
            if my_transition == "LEADER":
                log("[FAILOVER] Node %u declared DEAD (silent 300 ticks)" % leader_id)
                row = read_created()
                if mode != "notadopted" and row:
                    log("[FAILOVER] Adopted partition %s from dead node %u"
                        % (row["id"], leader_id))
                    log("[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                        % leader_id)
            else:
                # The observer: only the leader adopts, so it merely learns
                # the handoff announce from the adopter.
                row = read_created()
                if row and mode == "adopted":
                    log(learn_line(row, 2))
            return
        time.sleep(0.2)


threading.Thread(target=death_thread, daemon=True).start()


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
                # Adopted: owner = this node.
                r = dict(row)
                r["owner_node"] = node_id
                return {"partitions": [r] if row else []}
            # Observer: learned the handoff, owner = the adopter (node 2).
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
