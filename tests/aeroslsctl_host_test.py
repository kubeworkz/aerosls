#!/usr/bin/env python3
"""
aeroslsctl_host_test.py — drives the REAL tools/aeroslsctl as a subprocess
against a stub kernel, and checks exit codes as hard as it checks output.

─── Why a stub kernel rather than mocks ──────────────────────────────────
The thing most likely to be wrong in a REST client is not its rendering,
it is its reading of a reply that does not look like failure. The kernel
answers HTTP 200 for refusals and carries {"ok":"false"} -- a JSON *string*
-- in the body. Mocking _request() would test the parts that were never in
doubt and skip the one that was. So this serves real HTTP on a real socket
and runs the real argv path, top to bottom.

Every response shape below was read off net/http.c, not invented:
  api_workload_post   -> {"ok":"false","error":"..."} at HTTP 200
  api_cluster_peer    -> {"ok":"true","detail":"already a member (no-op)"}
  api_shell_exec_post -> {"recognized":..., "output":...}
  api_cluster_nodes   -> peer rows OMIT the counters, they are not zeroed

Run:
  python3 tests/aeroslsctl_host_test.py
"""

import http.server
import json
import subprocess
import sys
import threading
import os

CTL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "aeroslsctl")

passed = 0
failed = 0


def check(cond, msg):
    global passed, failed
    if cond:
        print(f"ok:   {msg}")
        passed += 1
    else:
        print(f"FAIL: {msg}")
        failed += 1


# ─── stub kernel ──────────────────────────────────────────────────────────

ROUTES = {}
LAST_REQUEST = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _serve(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode() if length else ""
        LAST_REQUEST["path"] = self.path
        LAST_REQUEST["method"] = method
        LAST_REQUEST["body"] = json.loads(raw) if raw else None
        LAST_REQUEST["auth"] = self.headers.get("Authorization")
        entry = ROUTES.get((method, self.path))
        if entry is None:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'{"error":"no such route"}')
            return
        status, payload = entry
        body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._serve("GET")

    def do_POST(self):
        self._serve("POST")


def run(*argv, host=None):
    """Runs the real CLI. Returns (exit_code, stdout, stderr)."""
    cmd = [sys.executable, CTL, "--host", host or HOST, *argv]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    return p.returncode, p.stdout, p.stderr


srv = http.server.HTTPServer(("127.0.0.1", 0), Handler)
HOST = f"127.0.0.1:{srv.server_address[1]}"
threading.Thread(target=srv.serve_forever, daemon=True).start()


def main():
    print("=== aeroslsctl against a stub kernel ===\n")

    # ═══ 1: the refusal trap ══════════════════════════════════════════════
    # The whole reason this file exists. HTTP 200 + ok:"false" is a NO.
    print("-- 1: a refusal carried inside HTTP 200 --")
    ROUTES[("POST", "/api/workload")] = (200, {"ok": "false", "error": "WL_ERR_FULL"})
    rc, out, err = run("workload", "declare", "--name", "x", "--partition", "1")
    check(rc == 2, "*** exit 2, not 0 -- a 200 that says ok:false is a refusal ***")
    check("WL_ERR_FULL" in err, "...the node's own reason reaches stderr")
    check("declared" not in out, "...and it does NOT claim the workload was declared")

    # The string/bool trap specifically: "false" is truthy in Python, so a
    # client checking `if data["ok"]` passes this and reports success.
    ROUTES[("POST", "/api/workload")] = (200, {"ok": "true"})
    rc, out, _ = run("workload", "declare", "--name", "x", "--partition", "1")
    check(rc == 0 and "declared" in out,
          "ok:\"true\" is accepted -- the guard is not just refusing everything")

    # A bare error with no ok field at all (several routes do this).
    ROUTES[("GET", "/api/service/resolve/ghost")] = (200, {"error": "not found"})
    rc, _, err = run("services", "resolve", "ghost")
    check(rc == 2, "*** a bare {\"error\":...} with no ok field is also a refusal ***")

    # ═══ 2: transport failure is distinct from refusal ════════════════════
    print("\n-- 2: unreachable is not the same as refused --")
    rc, _, err = run("health", host="127.0.0.1:1")
    check(rc == 1, "*** an unreachable node exits 1, not 2 -- scripts can tell "
                   "'node is down' from 'node said no' ***")
    check("cannot reach" in err, "...and says so")

    ROUTES[("GET", "/api/health")] = (500, b'{"error":"boom"}')
    rc, _, err = run("health")
    check(rc == 1 and "HTTP 500" in err, "a non-2xx exits 1 with the code shown")

    ROUTES[("GET", "/api/health")] = (200, b'<html>not json</html>')
    rc, _, err = run("health")
    check(rc == 1 and "non-JSON" in err,
          "a non-JSON body is reported whole, not as a parser error")

    # ═══ 3: unknown facts print as em-dash, never 0 ═══════════════════════
    # /api/nodes OMITS counters for peers because they are not replicated.
    # Printing 0 would fabricate a fact; this is the same rule the cluster
    # panel follows.
    print("\n-- 3: what a node cannot know about a peer --")
    ROUTES[("GET", "/api/nodes")] = (200, {"nodes": [
        {"node_id": 1, "self": "true", "detail": "first-hand",
         "partitions_owned": 3, "services_local": 2, "workloads": 0,
         "live_contexts": 0},
        {"node_id": 2, "self": "false", "detail": "membership-only",
         "partitions_owned": 1, "services_announced": 4},
    ]})
    rc, out, _ = run("nodes")
    check(rc == 0, "nodes lists cleanly")
    peer_line = [l for l in out.splitlines() if l.startswith("2 ")][0]
    check("—" in peer_line,
          "*** a peer's unreplicated counters print as — ***")
    check(peer_line.split()[-1] == "—" and " 0 " not in f" {peer_line} ",
          "*** ...and NOT as 0, which would invent a fact ***")
    self_line = [l for l in out.splitlines() if l.startswith("1 ")][0]
    check("0" in self_line,
          "...while a real zero read first-hand still prints as 0")
    check("not replicated" in out, "the table explains why the dashes are there")

    # ═══ 4: cluster init guards a destructive reset ═══════════════════════
    # cluster_init() resets term/role/roster. On a node already in a cluster
    # that drops it out, and it is not a read-modify-write.
    print("\n-- 4: re-initialising a live node is guarded --")
    ROUTES[("GET", "/api/cluster")] = (200, {
        "initialised": "true", "node_id": 7, "role": "LEADER", "term": 4,
        "active_nodes": 2, "quorum_threshold": 2, "roster": []})
    ROUTES[("POST", "/api/cluster/init")] = (200, {"ok": "true", "detail": "ok"})
    LAST_REQUEST.clear()
    rc, _, err = run("cluster", "init", "9")
    check(rc == 2, "*** re-init on a node already in a cluster is refused ***")
    check("RESETS" in err and "drops it out" in err,
          "...naming the consequence, not just 'are you sure'")
    check(LAST_REQUEST.get("path") != "/api/cluster/init",
          "*** ...and the destructive POST was never sent ***")

    rc, _, _ = run("cluster", "init", "9", "--yes")
    check(rc == 0 and LAST_REQUEST["path"] == "/api/cluster/init",
          "--yes sends it")
    check(LAST_REQUEST["body"] == {"node_id": 9}, "...with the id in the body")

    ROUTES[("GET", "/api/cluster")] = (200, {"initialised": "false", "node_id": 0})
    rc, out, _ = run("cluster", "init", "9")
    check(rc == 0, "on a standalone node no confirmation is needed")
    rc, out, _ = run("cluster", "status")
    check("standalone" in out and "no cluster formed" in out,
          "status calls an uninitialised node standalone, not a 1-node cluster")

    # ═══ 5: informative peer codes survive the round trip ═════════════════
    print("\n-- 5: 'already a member' is a success, not a failure --")
    ROUTES[("POST", "/api/cluster/peer")] = (200, {
        "ok": "true", "detail": "already a member (no-op)"})
    rc, out, _ = run("cluster", "peer", "2")
    check(rc == 0, "*** re-registering an existing peer exits 0 -- it is a no-op, "
                   "not an error ***")
    check("already a member" in out, "...and the distinction is shown, not flattened")

    # ═══ 6: the shell passthrough ═════════════════════════════════════════
    # This is how `partition migrate` is reached -- it has no REST route of
    # its own, but sls_shell_execute() captures serial output into the body.
    print("\n-- 6: the shell passthrough reaches what REST does not --")
    ROUTES[("POST", "/api/shell/exec")] = (200, {
        "recognized": "true",
        "output": "[PARTITION] migrate partition=1 -> node=2 -> ok"})
    rc, out, _ = run("shell", "partition", "migrate", "1", "2")
    check(rc == 0, "a recognised shell command exits 0")
    check(LAST_REQUEST["body"] == {"command": "partition migrate 1 2"},
          "*** the whole command line is reassembled, not just the first word ***")
    check("[PARTITION] migrate" in out,
          "*** captured serial output comes back to the caller ***")

    ROUTES[("POST", "/api/shell/exec")] = (200, {
        "recognized": "false", "output": "Unknown command: frobnicate"})
    rc, _, err = run("shell", "frobnicate")
    check(rc == 2, "*** an unrecognised command exits non-zero even though the "
                   "node reports recognized=false rather than ok=false ***")

    rc, _, err = run("shell")
    check(rc == 1 and "needs a command" in err, "bare `shell` is rejected")

    # ═══ 7: verbs the docs promise and nobody built ═══════════════════════
    print("\n-- 7: documented-but-nonexistent verbs --")
    for verb, hint in (("scale", "replica"), ("logs", "serial"),
                       ("exec", "not inside"), ("drain", "migrate")):
        rc, _, err = run("workloads" if verb != "drain" else "nodes", verb)
        check(rc == 2 and "not built" in err,
              f"`{verb}` says it is not built rather than failing as unknown")
        check(hint in err, f"...and points at what to do instead")

    # ═══ 8: plumbing ══════════════════════════════════════════════════════
    print("\n-- 8: auth, --json, raw --")
    ROUTES[("GET", "/api/mesh")] = (200, {"breakers": [
        {"name": "api", "state": "OPEN", "trips": 2, "calls_refused": 9}]})
    rc, out, _ = run("mesh")
    check("OPEN" in out and "api" in out, "mesh renders breaker state")
    check(LAST_REQUEST["auth"].startswith("Bearer "), "a bearer token is sent")

    rc, out, _ = run("--json", "mesh")
    check(json.loads(out)["breakers"][0]["state"] == "OPEN",
          "--json emits the node's payload verbatim and parses")

    rc, out, _ = run("raw", "GET", "api/mesh")
    check(rc == 0 and "breakers" in out, "raw reaches a route with no verb, "
                                         "tolerating a missing leading slash")
    rc, _, err = run("raw", "POST", "/api/mesh", "--body", "{not json")
    check(rc == 1 and "not valid JSON" in err, "raw rejects a malformed --body")

    ROUTES[("GET", "/api/workloads")] = (200, {
        "workloads": [], "queue_depth": 0, "queue_dropped": 3})
    rc, out, _ = run("workloads")
    check("(none)" in out, "an empty table says (none)")
    check("not draining" in out,
          "*** a non-zero dropped count is called out, not just printed ***")

    # ─── A refusal must surface whatever the kernel actually said ──────────
    #
    # /api/shell/exec answers an unrecognised command with
    # {"ok":"false","output":"<what the shell printed>"} and NO error field.
    # The renderer consulted only `error` and `detail`, so a mistyped command
    # printed "refused: no reason given" while the kernel's own explanation
    # sat in the discarded response. That is worse than an obscure message:
    # it tells the operator there is nothing to find out.
    print("\n-- a refusal surfaces the kernel's own words --")
    ROUTES[("POST", "/api/shell/exec")] = (200, {
        "ok": "false", "output": "unknown command: stream list"})
    rc, out, err = run("shell", "stream list")
    check(rc == 2, "an unrecognised shell command still exits REFUSED (2)")
    check("no reason given" not in err,
          "*** it does NOT say 'no reason given' when the kernel explained ***")
    check("unknown command" in err,
          "*** the kernel's own output is what gets printed ***")

    ROUTES[("POST", "/api/shell/exec")] = (200, {
        "ok": "false", "error": "too many concurrent shell sessions"})
    rc, out, err = run("shell", "ls")
    check("too many concurrent" in err,
          "an explicit error field still takes precedence over output")

    ROUTES[("POST", "/api/shell/exec")] = (200, {"ok": "false"})
    rc, out, err = run("shell", "ls")
    check("no reason given" in err,
          "...and the placeholder remains for a refusal that genuinely says nothing")

    ROUTES[("POST", "/api/shell/exec")] = (200, {"ok": "false", "output": "   \n  "})
    rc, out, err = run("shell", "ls")
    check("no reason given" in err,
          "whitespace-only output is not mistaken for an explanation")

    print(f"\n{'='*58}")
    print(f"passed={passed} failed={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    code = main()
    srv.shutdown()
    sys.exit(code)
