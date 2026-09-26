#!/usr/bin/env python3
"""Fake AeroSLS nodes for tests/failover_2node_live_smoke.sh.

Stands up TWO HTTP servers shaped like the handful of routes the 2-node
failover live guard touches -- /api/health, /api/cluster, /api/partitions
and POST /api/shell/exec -- so the guard can be driven through every
verdict without a real cluster. Not a simulator of the kernel: only of the
response shapes the guard must tell apart, plus the log strings the guard's
evidence gates on and the one piece of real state the property depends on:
the leader's death.

The property: in a 2-node cluster, when the leader dies the sole survivor
must NOT become a second leader, must NOT adopt, and must NOT hold the
partition's write lease. The kernel's majority quorum for 2 nodes is 2-of-2,
nothing shrinks the roster on death, and failover recovery is gated on
cluster_is_leader() -- so a lone survivor can never be elected, never
recovers, and can never win the lease quorum. This fake models the kernel's
log strings the guard gates on (each printed by exactly one real site):

    RX (create learned):  "[PARTITION] sync: partition N '<name>' (owner
    node L) learned from node L." then "[DSPP-CKPT] RX: COMPLETE seq=N" --
    the guard requires the checkpoint line AFTER the learn line, proving
    the create reached the held checkpoint (adoption was data-possible).
    leader death:          "[FAILOVER] Node L declared DEAD (silent 300
    ticks)" -- every node runs failover_tick and declares; only the leader
    would recover. The guard requires the declaration, and requires NO
    Adopted/recovery line after it.
    lease row create:      "[CONSENSUS] partition N lease initialised
    (FOLLOWER, term=0)." -- RXed the leader's campaign; the guard requires
    the survivor to hold a row before it may trust the absence of a
    restore (no row = no contest = vacuous).
    lease strip/restore:   "[MMU-LEASE] partition N: page permissions
    force_read_only=1" on every campaign start, "=0" only on lease
    quorum-achieved. The guard requires the strip and forbids the restore
    on the survivor.

The survivor's death detection mirrors the kernel's: it polls the leader's
HTTP port; when it stops answering, it transitions. What it does then is
the mode:

Modes (written to <statedir>/mode by the smoke):
    staysfollower  node 2 declares the death and stays FOLLOWER forever,
                   never adopting; the row stays owned by node 1; the
                   lease strip is logged and never restored -> PASS
    flipsleader    node 2 declares the death, then flips to LEADER after a
                   delay (a late second leader) -> guard FAIL at the
                   never-LEADER watch
    adopts         node 2 declares the death and ALSO prints the Adopted +
                   recovery lines while staying FOLLOWER (a follower that
                   recovered -- the cluster_is_leader gate bypassed) ->
                   guard FAIL at the never-adopt log check
    nolearn        node 2 never learns the create -> guard FAIL at the
                   learn gate
    nockpt         node 2 learns but no checkpoint line flows -> guard
                   FAIL at the checkpoint gate
    nolease        the leader's `partition lease acquire` never takes (the
                   command succeeds, no row is held) -> guard FAIL at the
                   lease-held gate
    nolearnlease   node 2 never creates its lease row (the REQUEST_VOTE RX
                   path broken) -> guard FAIL at the lease-learn gate
    leaselost1     the first campaign's packet is LOST on the wire: node 2
                   only ever sees the row of the SECOND campaign, i.e. the
                   guard's re-drive. A lost hop must not read as a broken
                   RX path -> guard PASS (the re-drive gate)
    leasestrip     node 2 declares the death but never logs the MMU-LEASE
                   strip -> guard FAIL at the strip gate
    leaserestore   node 2 logs the strip AND then a restore (lease
                   quorum-achieved on a 2-node survivor -- impossible) ->
                   guard FAIL at the restore gate
    leasehold      node 2 reports holds_lease=1 in /api/partitions after
                   the death (partition_holds_write_lease() true on the
                   survivor) -> guard FAIL at the holds_lease watch

The no-cluster tooth starts NO fakes at all.
"""

# ─── The fake must never be the binding clock ─────────────────────────────
# The guard scales its own patience down for the smoke
# (AEROSLS_FAILOVER_FAST=1: WAIT_NODES=10s, WAIT_LEARN=6s, WAIT_CKPT=6s,
# WAIT_OBSERVER=8s). The waits below used to carry fixed iteration caps of
# their own -- 200 x 0.05s = 10s for the create announce and the lease row,
# 600 x 0.05s = 30s for the death poll -- budgets that have nothing to do
# with the guard's, start when the PROCESS starts, and therefore compete
# with the guard's whole fast path. The learn cap was exactly the guard's
# WAIT_NODES pre-create budget, so a host that spent its whole allowance
# forming the cluster put the create announce at the cap's edge and the
# survivor stopped listening before the guard had created the partition.
# failover_adoption_live_smoke.sh paid for exactly that in CI on 2026-09-20
# (nockpt and lateflip failed with "nodes 2/3 never learned partition" while
# their own gates had budget to spare); this smoke and these fakes share the
# shape and had simply not been caught yet.
#
# So every wait here on an event the guard's gates depend on is bounded by
# the process's LIFE, not by a counter: the only clock that may decide a
# tooth is the guard's. The modelled negatives (nolearn, nockpt,
# nolearnlease, ...) are mode/event-gated and still return without logging
# their line, so they still fail the GUARD's gate -- the property under
# test -- rather than racing a fake-side timer. The control is this smoke's
# "latecreate" tooth, which idles the fakes past every old cap before the
# guard is even started. Full rationale and the CI evidence: the same rule
# block at the top of tests/failover_adoption_smoke_nodes.py.

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
logfile = os.path.join(logdir, f"node{node_id}.log")
lock = threading.Lock()

transitioned = False
flipped = False
lease_held_leader = False
lease_held_survivor = False
campaign_term = 0        # one acquire = one campaign (leaselost1 reads it)


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
    # catch the zero-byte window of a write and die with a JSONDecodeError,
    # killing this thread. A dead RX thread never logs the row, so the
    # guard read a racing reader as "the leader's REQUEST_VOTE RX path is
    # broken" -- the CI false RED of 2026-09-25.
    if os.path.exists(lease):
        try:
            return json.load(open(lease))
        except ValueError:
            return None
    return None


def lease_term(row):
    """The campaign term of a lease row (the writer's acquire counter)."""
    try:
        return int(row.get("term", 1))
    except (AttributeError, TypeError, ValueError):
        return 1


def learn_line(row, owner):
    return ("[PARTITION] sync: partition %s '%s' (owner node %s) learned "
            "from node %s." % (row["id"], row["name"], owner, owner))


def leader_port():
    # The smoke lays out nodes on consecutive ports: 1 -> BASE+1.
    return port - node_id + leader_id


# The modelled property is failover_tick's "silent >= FAILOVER_DEAD_TICKS"
# (300 ticks) -- a DURATION of unreachability, not a count of failed
# requests. A poll that timed out because the leader was busy answering
# somebody else is not silence, so death needs DEAD_SILENCE seconds of
# CONTINUOUS unreachability (see death_thread). Full story in
# tests/failover_adoption_smoke_nodes.py.
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
# never learns (the learn gate must bite); nockpt learns but never logs the
# checkpoint RX (the checkpoint gate must bite).
learned = False


def rx_thread():
    global learned
    if role != "follower":
        return
    # Life-bounded, not counter-bounded (see the rule at the top of this
    # file): the create announce can arrive at any point in the guard's run.
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


# ─── Lease RX thread: create the survivor's lease row when the leader's
# campaign arrives (here: when lease.json appears). The guard requires
# `[CONSENSUS] partition N lease initialised` in the survivor's log BEFORE
# the kill -- without a row, the absence of a restore would be vacuous.
# nolearnlease never logs it (the lease-learn gate must bite). leaselost1
# logs it only for the SECOND campaign: the first one's packet was lost on
# the wire, which is what the guard's re-drive gate must survive. The
# leader side creates its own row on `partition lease acquire` (below).
def lease_rx_thread():
    global lease_held_survivor
    if role != "follower":
        return
    # Life-bounded, and retry-tolerant (read_lease): the guard's lease
    # phase runs after its own cluster-forming and learn gates, and a read
    # that lands mid-write must retry, not kill this thread.
    row = None
    while row is None:
        row = read_lease()
        if row is None:
            time.sleep(0.05)
    if mode == "nolearnlease":
        return   # the lease-learn gate must bite: no row is ever RXed
    if mode == "leaselost1" and lease_term(row) < 2:
        # The first campaign's packet never landed (a lost HOP, not a
        # broken RX path): this node only ever sees the row of the second
        # campaign -- the guard's re-drive. With a single bounded wait the
        # guard FAILed here with "never created a lease row" (the CI false
        # RED of 2026-09-25), so this mode is the control for the re-drive
        # gate: a lost hop must PASS, a broken RX path (nolearnlease) must
        # still FAIL.
        while True:
            row = read_lease()
            if row is not None and lease_term(row) >= 2:
                break
            time.sleep(0.05)
    # A lease row exists but the survivor does NOT hold it -- the global
    # lease_held_survivor stays False (default). The leader does (its own
    # acquire below) -- the row here is the follower's mirror of the same
    # partition's lease state.
    log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)."
        % row.get("partition_id", "1"))


threading.Thread(target=lease_rx_thread, daemon=True).start()


# ─── Death-detection thread: the guard SIGKILLs the leader ────────────────
# The follower polls the leader's port; when it stops answering, it logs
# the death declaration (every node runs failover_tick) and then behaves
# per mode.
def death_thread():
    global transitioned, flipped
    if role != "follower":
        return
    # Life-bounded: the guard SIGKILLs the leader after its own gates have
    # run, and this poll must still be listening when it does.
    # Sustained silence, not "3 strikes": three failed polls can be a busy
    # leader, and a declaration made on them flips this survivor's state
    # before the guard has killed anything.
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
        log("[FAILOVER] Node %u declared DEAD (silent 300 ticks)" % leader_id)
        # The write-lease strip: the survivor campaigns for the lease
        # (its row times out, update_page_table_permissions_for_
        # partition(pid, 1)) -- logged unless leasestrip suppresses it.
        if mode != "leasestrip":
            log("[MMU-LEASE] partition 1: page permissions force_read_only=1")
        if mode == "leaserestore":
            # The bug: a 2-node survivor that "wins" the 2-of-2 lease
            # quorum and restores write permission. The guard's restore
            # gate must catch this line.
            log("[MMU-LEASE] partition 1: page permissions force_read_only=0")
        if mode == "leasehold":
            # The bug at the API level: partition_holds_write_lease()
            # returning true on the survivor. The guard's holds_lease
            # watch must catch it.
            with lock:
                global lease_held_survivor
                lease_held_survivor = True
        row = read_created()
        if mode == "adopts":
            # The bug: a FOLLOWER that recovered. cluster_is_leader()
            # should have gated recovery to the leader (which a lone
            # 2-node survivor can never become); the guard's step-7 log
            # check must catch the Adopted line.
            if row:
                log("[FAILOVER] Adopted partition %s from dead node %u"
                    % (row["id"], leader_id))
                log("[FAILOVER] recovery for dead node %u: rc=0 (OK - adopted)"
                    % leader_id)
        if mode == "flipsleader":
            # The other bug: a late second leader. Delay so a naive
            # first-poll check would miss it; only a watch that HOLDS
            # the full window can catch the flip.
            def flip():
                global flipped
                time.sleep(1.5)
                flipped = True
            threading.Thread(target=flip, daemon=True).start()
        return


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
            if flipped:
                return "LEADER"
            # The sole survivor of a 2-node cluster: allowed to be FOLLOWER
            # or CANDIDATE forever (it keeps campaigning, never winning).
            return "FOLLOWER"
        return "FOLLOWER"

    def _active(self):
        if role == "leader":
            return 2
        if transitioned:
            return 1
        return 2

    def _holds_lease(self):
        # The exact function the API surfaces: on the leader it holds the
        # lease once acquired; on the survivor it must stay false unless
        # the leasehold bug mode flips it after the death.
        if role == "leader":
            return 1 if lease_held_leader else 0
        return 1 if lease_held_survivor else 0

    def _partitions(self):
        """The /api/partitions body. The leader serves the current created
        row; the follower serves the row once learned, owner = the leader,
        except in 'adopts' mode where the buggy recovery claims it."""
        row = read_created()
        if role == "leader":
            if not row:
                return {"partitions": []}
            r = dict(row)
            r["lease_role"] = "LEADER"
            r["holds_lease"] = self._holds_lease()
            return {"partitions": [r]}
        if mode == "nolearn":
            return {"partitions": []}
        if not learned:
            return {"partitions": []}
        if transitioned and mode == "adopts":
            r = dict(row)
            r["owner_node"] = node_id
            r["lease_role"] = "CANDIDATE"
            r["holds_lease"] = self._holds_lease()
            return {"partitions": [r] if row else []}
        r = dict(row)
        r["lease_role"] = "FOLLOWER" if not transitioned else "CANDIDATE"
        r["holds_lease"] = self._holds_lease()
        return {"partitions": [r] if row else []}

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
            elif role == "leader" and command.startswith("partition lease acquire "):
                # The kernel: partition_lease_trigger_election(pid, now)
                # creates the row if missing, campaigns, and wins the 2-of-2
                # quorum while both nodes are alive. nolease suppresses the
                # hold (the command succeeds -- so the guard's acquire
                # shell step does not die -- but no row is held, and the
                # lease-held gate must bite).
                #
                # Each acquire is one CAMPAIGN: the row carries the term,
                # so the RX side can tell a re-driven campaign from the
                # first one (leaselost1). The row is written whole
                # (os.replace): the RX side reads a ROW, never a half-arrived
                # packet, which is the whole point of a wire hop model.
                pid = command[len("partition lease acquire "):].strip()
                if mode != "nolease":
                    global lease_held_leader, campaign_term
                    with lock:
                        campaign_term += 1
                        term = campaign_term
                    tmp = "%s.tmp%d" % (lease, os.getpid())
                    with open(tmp, "w") as f:
                        json.dump({"partition_id": int(pid), "term": term},
                                  f, separators=(",", ":"))
                    os.replace(tmp, lease)
                    with lock:
                        lease_held_leader = True
                    log("[CONSENSUS] partition %s lease initialised (FOLLOWER, term=0)." % pid)
                    log("[CONSENSUS] partition %s: node %u campaigning for write lease, term %d." % (pid, node_id, term))
                    log("[CONSENSUS] partition %s: quorum stable, node %u elected LEADER (write lease) for term %d." % (pid, node_id, term))
                self._json(200, {"ok": "true", "recognized": "true",
                                 "output": f"lease acquire {pid} issued"})
            else:
                self._json(200, {"ok": "false", "error": f"not a create: {command}"})
        else:
            self._json(404, {"error": f"no fake POST route {self.path}"})

    def log_message(self, *a):
        pass


# ThreadingHTTPServer, not HTTPServer: the plain server handles ONE request
# at a time, so concurrent pollers queue and any client timeout (the
# follower's own 0.5s leader poll, the guard's curl, the smoke's curl)
# becomes a verdict -- e.g. a follower declaring the leader DEAD while it is
# answering someone else. A real kernel answers concurrently, so this is the
# faithful model too. Full story in the same place in
# tests/failover_adoption_smoke_nodes.py.
srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(600)
