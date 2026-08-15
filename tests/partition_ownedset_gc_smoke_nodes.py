#!/usr/bin/env python3
"""Fake AeroSLS nodes for tests/partition_ownedset_gc_live_smoke.sh.

Stands up one HTTP server shaped like the handful of routes the owned-set
GC live guard touches -- /api/health, /api/cluster, /api/partitions and
POST /api/shell/exec (create) and /api/partition/destroy -- so the guard
can be driven through every verdict without a real cluster. Not a
simulator of the kernel: only of the response shapes the guard must tell
apart, plus the log strings the guard's evidence gates on.

The property: a node that is DOWN when its owner destroys a partition
restores the ghost from disk at boot and must then collect it from the
owner's periodic full-owned-set frame. The fake models the kernel's log
strings the guard gates on (each printed by exactly one real site):

    boot 1 (before the kill):  learn  -> "[PARTITION] sync: partition N
    '<name>' (owner node L) learned from node L." then flush ->
    "[PERSIST] Partitions snapshot written." -- the guard requires the
    flush line AFTER the learn line, proving the ghost is on disk. The
    learn is also PERSISTED to the follower's own "disk" (a state file),
    because that is what a reboot restores.
    boot 2 (the relaunch):     restore -> "[PERSIST] Partition ownership
    restored from NVMe." then, when the owned-set arrives, collect ->
    "no longer owned -- collected from the owned-set." -- the guard
    requires restore before collect AND the row gone from /api/partitions.

Modes (written to <statedir>/mode by the smoke):
    collected     boot 1 learns+flushes; boot 2 restores then collects
                  -> guard PASS
    notcollected  boot 1 learns+flushes; boot 2 restores but NEVER
                  collects -> guard FAIL (the row stays; the poll runs out)
    nolearn       boot 1 never learns -> guard FAIL at the learn gate
                  (the create announce did not arrive)
    nopersist     boot 1 learns but never flushes -> guard FAIL at the
                  flush gate (the ghost never reached disk, so the test
                  would prove nothing)

The follower counts its own boots (bootcount-<id>) so the smoke can assert
the guard really did kill and relaunch it. "created.json" under the state
dir is the wire in miniature: the leader writes a row for `partition
create`, the follower "learns" it by reading that file (and persists it to
its own disk file), and the leader removes it on `partition destroy` -- so
the destroy is never seen by the follower while it is down, exactly like a
real withdraw broadcast.
"""

import http.server
import json
import os
import sys
import threading
import time

node_id = int(sys.argv[1])
role = sys.argv[2]
port = int(sys.argv[3])
state = sys.argv[4]
logdir = sys.argv[5]

mode = open(os.path.join(state, "mode")).read().strip()

bootfile = os.path.join(state, f"bootcount-{node_id}")
boot = 0
if os.path.exists(bootfile):
    try:
        boot = int(open(bootfile).read().strip())
    except ValueError:
        boot = 0
boot += 1
open(bootfile, "w").write(str(boot))

created = os.path.join(state, "created.json")      # the leader's wire row
disk = os.path.join(state, f"disk-{node_id}.json")  # the follower's NVMe
logfile = os.path.join(logdir, f"node{node_id}.log")
lock = threading.Lock()


def log(line):
    with lock:
        with open(logfile, "a") as f:
            f.write(line + "\n")


def read_json(path):
    if os.path.exists(path):
        try:
            return json.load(open(path))
        except ValueError:
            return None
    return None


def learn_line(row):
    return ("[PARTITION] sync: partition %s '%s' (owner node %s) learned "
            "from node %s." % (row["id"], row["name"], row["owner_node"],
                               row["owner_node"]))


def collect_line(row):
    return ("[PARTITION] sync: partition %s (owner node %s) no longer "
            "owned -- collected from the owned-set."
            % (row["id"], row["owner_node"]))


# ─── boot-1 RX evidence + persist ────────────────────────────────────────
# The leader creates the row AFTER the fakes are up, so the learn cannot be
# logged at process start: the follower's announce RX fires when the row
# arrives on the wire (here: when created.json appears), logs the learn
# line, then the sweep flush logs the snapshot line AND writes the row to
# the follower's own disk -- the ghost. nopersist skips BOTH the flush log
# and the disk write, so the guard's disk gate must bite.
def rx_thread():
    if role == "follower" and boot == 1:
        row = None
        for _ in range(100):
            row = read_json(created)
            if row:
                break
            time.sleep(0.05)
        if row and mode != "nolearn":
            log(learn_line(row))
            if mode != "nopersist":
                log("[PERSIST] Partitions snapshot written.")
                with open(disk, "w") as f:
                    json.dump(row, f)


threading.Thread(target=rx_thread, daemon=True).start()

# ─── boot-2 evidence: the ghost comes back from disk ──────────────────────
if role == "follower" and boot == 2:
    ghost = read_json(disk)
    if ghost and mode != "nopersist":
        log("[PERSIST] Partition ownership restored from NVMe.")


class H(http.server.BaseHTTPRequestHandler):
    def _json(self, code, obj):
        # Compact separators: the real kernel's jb_* writers emit
        # {"ok":"true"} with no space after the colon, and the guard
        # matches the exact wire shape.
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _partitions(self):
        """The /api/partitions body per mode + boot (the guard's tooth
        logic). The leader always serves the current created row; the
        follower serves what it has: boot 1 the learn, boot 2 the disk
        restore -- minus the collected ghost once the owned-set lands."""
        if role == "leader":
            row = read_json(created)
            return {"partitions": [row] if row else []}
        if boot == 1:
            row = read_json(created)
            return {"partitions": [row] if row and mode != "nolearn" else []}
        # boot 2: what the follower restored from ITS disk, not the
        # leader's wire (which no longer has the row -- it was destroyed
        # while the follower was down).
        row = read_json(disk)
        collected = os.path.exists(os.path.join(state, "collected"))
        if row and collected:
            return {"partitions": []}
        return {"partitions": [row] if row and mode != "nopersist" else []}

    def do_GET(self):
        if self.path == "/api/health":
            self._json(200, {"ok": "true", "ready": "true"})
        elif self.path == "/api/cluster":
            self._json(200, {"node_id": node_id, "role": role.upper(),
                             "initialised": "true", "active_nodes": 2,
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
        elif self.path == "/api/partition/destroy":
            if role == "leader" and os.path.exists(created):
                # The withdraw broadcast -- but the follower is DOWN for it
                # (that is the whole point of the test), so it never hears
                # the row's absence here.
                os.remove(created)
                self._json(200, {"ok": "true"})
            else:
                self._json(200, {"ok": "false", "error": "not destroyed"})
        else:
            self._json(404, {"error": f"no fake POST route {self.path}"})

    def log_message(self, *a):
        pass


srv = http.server.HTTPServer(("127.0.0.1", port), H)


def ownedset_timer():
    """Boot-2 owned-set arrival, kernel-style. For the collected tooth
    only: log the collect line (restore is already logged, before it), then
    mark the state so _partitions() stops serving the ghost."""
    if role == "follower" and boot == 2 and mode == "collected":
        time.sleep(1.0)
        ghost = read_json(disk)
        # log() takes the lock itself -- do NOT hold it here, or the same
        # thread deadlocks on a non-reentrant threading.Lock.
        if ghost:
            log(collect_line(ghost))
        open(os.path.join(state, "collected"), "w").write("1")


threading.Thread(target=ownedset_timer, daemon=True).start()
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(600)
