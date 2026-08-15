#!/usr/bin/env python3
"""Fake AeroSLS nodes for tests/partition_reannounce_live_smoke.sh.

Stands up one HTTP server shaped like the handful of routes the re-announce
live guard touches -- /api/health, /api/cluster, /api/partitions and
POST /api/shell/exec -- so the guard can be driven through every verdict
without a real cluster. Not a simulator of the kernel: only of the response
shapes the guard must tell apart, plus the one piece of real state the
property depends on.

That piece is the announce. The fake leader "creates" a partition by writing
a row file; the fake follower "learns" it by reading that file -- the wire
in miniature. And the follower's behaviour across a restart is what makes
the teeth bite: the guard SIGKILLs the follower and relaunches it with the
SAME argv, so the fake counts its own boots in a file and behaves
differently the second time, exactly as the real kernel does (learned rows
are runtime state and vanish on reboot):

    boot 1 (before the kill):
        serve the row unless mode == "nolearn"
        -- so the guard's pre-reboot gate can pass, or fail, as scripted.
    boot 2 (the relaunch):
        serve the row only when mode == "converged"
        -- so the guard's convergence check can pass, or fail, as scripted.

Modes (written to <statedir>/mode by the smoke):
    converged     boot 1 and boot 2 both serve the row -> guard PASS
    notconverged  boot 1 serves, boot 2 does not       -> guard FAIL
                  (the convergence poll runs out)
    nolearn       boot 1 does not serve                -> guard FAIL
                  (the pre-reboot learn gate runs out)
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

created = os.path.join(state, "created.json")


def partitions_payload():
    """The /api/partitions body, per the mode/boot rules above."""
    row = None
    if os.path.exists(created):
        serve = False
        if role == "leader":
            serve = True
        elif boot <= 1:
            serve = (mode != "nolearn")
        else:
            serve = (mode == "converged")
        if serve:
            row = json.load(open(created))
    return {"partitions": [row] if row else []}


class H(http.server.BaseHTTPRequestHandler):
    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/health":
            self._json(200, {"ok": "true", "ready": "true"})
        elif self.path == "/api/cluster":
            # Uppercase on purpose: consensus_role_name() (net/consensus.c)
            # emits "LEADER"/"FOLLOWER", and the guard compares exactly.
            self._json(200, {"node_id": node_id, "role": role.upper(),
                             "initialised": "true", "active_nodes": 2,
                             "quorum_threshold": 2})
        elif self.path == "/api/partitions":
            self._json(200, partitions_payload())
        else:
            self._json(404, {"error": f"no fake route {self.path}"})

    def do_POST(self):
        if self.path == "/api/shell/exec":
            n = int(self.headers.get("Content-Length", 0))
            try:
                req = json.loads(self.rfile.read(n).decode() or "{}")
            except ValueError:
                req = {}
            command = req.get("command", "")
            if role == "leader" and command.startswith("partition create "):
                name = command[len("partition create "):].strip()
                with open(created, "w") as f:
                    json.dump({"id": 1, "owner_node": node_id, "name": name,
                               "state": "active"}, f)
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
