#!/usr/bin/env bash
# tests/partition_reannounce_live_check.sh — a node that was DOWN when a
# partition was created must converge to it from the periodic re-announce.
#
# ─── Why this is the gate that matters ─────────────────────────────────────
# Partition rows replicate two ways. A mutation (create/migrate/adopt/
# destroy) broadcasts once; that is the path the replication work verified.
# A node that is DOWN for that one broadcast never hears it. Since the
# replication work landed, learned rows are PERSISTED (the RX apply marks a
# dirty flag; the BSP sweep's partition_persist_flush() writes the table out
# -- see partition.c), so a rebooted node restores what it learned and the
# row it needs can also come back from disk. But a row created while the
# node was DOWN is on nobody's disk but the creator's, and nothing restores
# it -- only partition_reannounce_tick() brings it across: every 1000 ticks
# (~10s), each node re-broadcasts the rows it OWNS, so a node that was down
# for the create converges within one period without waiting for the next
# mutation.
#
# That function has no host test: it is called from the BSP sweep in
# net/http.c, which no host test links. Before this guard, the only proof
# it worked was a hand-run cluster session -- which is exactly how the
# entropy path rotted unseen here for days (entropy_init was never called
# and every host test passed anyway). This guard is the repeatable version
# of that session, shaped to stay meaningful now that learned rows persist:
# it creates partition A while both nodes are up (proving the replication
# gate), SIGKILLs the follower, creates partition B while the follower is
# DOWN (so B cannot be on its disk and cannot have been learned), relaunches
# the follower with the same argv, and asserts the rebooted node's partition
# list converges to B. Nothing mutates the cluster between B's create and
# the convergence check, so a converged B can only have come from the
# periodic re-announce.
#
# The sync evidence pins the PATH, not just the outcome: the rebooted
# node's serial log carries `[PARTITION] sync: partition N '<name>' ...
# learned from node <L>` lines, and that string is emitted only by
# partition_sync_upsert() on DSPP_PARTITION_ANNOUNCE RX (kernel/partition.c).
# The checkpoint broadcast -- the other periodic thing that flows -- has no
# partition reference at all (net/dspp_checkpoint.c), so a learned row with
# sync lines in a fresh (post-reboot) log can only have come over the
# announce family.
#
# Learned-row PERSISTENCE itself is not this guard's job: a rebooted node
# keeping rows it learned is pinned deterministically by
# tests/persist_partition_host_test.c (negative tooth: unflushed learn
# vanishes; positive tooth: flushed learn survives; withdraw tooth). The
# guard's create-while-down shape exists precisely so persistence cannot
# fake the re-announce result.
#
# ─── Why it does not launch a cluster ─────────────────────────────────────
# This guard REBOOTS one node of the cluster it runs against, so it must
# only ever touch a cluster its operator started deliberately -- the same
# contract as entropy_boot_diversity_check.sh, one step stronger. That is
# also what makes it safe to run from run_checks.sh: a runtime guard that
# booted a cluster of its own would do that in the middle of a deploy gate.
#
# ─── Usage ────────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (any size >= 2)
#   tests/partition_reannounce_live_check.sh
#
# The guard will SIGKILL the follower it picks, relaunch it from the argv
# captured in /proc, and leave it running (rejoined to your cluster). It
# does not stop the cluster.
#
# Environment:
#   AEROSLS_HTTP_BASE            port base (default 3000)
#   AEROSLS_TOKEN                bearer token (default: aeroslsctl's demo
#                                DB_ADMIN token, which the kernel accepts)
#   AEROSLS_REANNOUNCE_FAKE=1    test-only: allow relaunching a follower
#                                whose argv[0] is not QEMU (the smoke's
#                                fake nodes). Refused by default, because
#                                killing and relaunching something
#                                unidentified is how you take down the
#                                wrong process.
#   AEROSLS_REANNOUNCE_FAST=1    scale every wait down to seconds. Used by
#                                the smoke; never for a real cluster.
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no pid file, /proc unreadable).
#
# GUARD-KIND: runtime
#
# That marker is read by tests/run_checks.sh and it matters the same way it
# does for entropy_boot_diversity_check.sh: this guard needs a RUNNING
# CLUSTER, which a build host and a deploy gate do not have, and --require-all
# deliberately must not force it. A green build gate is therefore NOT
# evidence that it passes -- the guard is reported as owed there, loudly,
# and this file is what you run against a started cluster.
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
FAKE="${AEROSLS_REANNOUNCE_FAKE:-0}"
FAST="${AEROSLS_REANNOUNCE_FAST:-0}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="cluster"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME_A="guard-reannounce-$$"          # created while both nodes are up
NAME_B="guard-reannounce-$$-late"     # created while the follower is DOWN:
                                      # unique per run, so a stale persisted
                                      # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_BOOT=8; WAIT_CONV=8
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_BOOT=240; WAIT_CONV=120
fi

die()  { echo; echo "ABORT: $*" >&2; exit 2; }
fail() { echo; echo "FAIL  $*"; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 is needed to parse the /api JSON."
command -v curl    >/dev/null 2>&1 || die "curl not found."
command -v setsid  >/dev/null 2>&1 || die "setsid not found -- the relaunched node must survive the guard's shell exiting."
[ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

TOKEN_ARGS=()
[ -n "$TOKEN" ] && TOKEN_ARGS=(--token "$TOKEN")
CURL_EXTRA=()
[ -n "$TOKEN" ] && CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")

# ─── transport ────────────────────────────────────────────────────────────
# curl, not aeroslsctl, for the read calls: a down QEMU hostfwd port
# accepts the TCP connect and then times out, so a probe needs a bounded
# --max-time rather than urllib's 15s default. Sets FETCH_STATUS /
# FETCH_BODY / FETCH_RC, like the entropy guard, for the same reason: curl's
# exit code is the diagnosis whenever there is no HTTP status.
FETCH_STATUS=""; FETCH_BODY=""; FETCH_RC=0
fetch() {   # $1 = port, $2 = path
    local out
    out="$(curl -s --max-time 3 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    FETCH_RC=$?
    FETCH_STATUS="$(printf '%s' "$out" | tail -n1)"
    FETCH_BODY="$(printf '%s' "$out" | sed '$d')"
}

# Fetch and require 200. 401/403 names the token -- the /api routes sit
# behind the same bearer gate as /api/entropy, and "the cluster is fine,
# this script has no credentials" must not read like "the cluster is down".
# Anything else returns 1 so the caller's poll loop can keep trying.
get200() {   # $1 = port, $2 = path
    fetch "$1" "$2"
    case "$FETCH_STATUS" in
        200) return 0 ;;
        401|403)
            echo "ABORT: node $(( $1 - HTTP_BASE )) rejected the request" >&2
            echo "       (HTTP $FETCH_STATUS): /api routes sit behind the bearer" >&2
            echo "       token gate. Set AEROSLS_TOKEN=<token> and re-run." >&2
            exit 2 ;;
        *) return 1 ;;
    esac
}

# jget <json> <field> -- one JSON field, '' when absent. Flat objects, no
# nesting. JSON and args arrive via argv (not stdin), so an empty body
# cannot wedge the parser and a heredoc keeps the quoting honest.
jget() {
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    d = {}
print(d.get(sys.argv[2], ""))
PY
}

# row_if <json> <name> <owner> -- prints "partition <id> '<name>' (owner
# node <owner>)" when a row with BOTH the name and the owner exists. The
# owner check matters: a row is only convergence if it came from the
# leader's announce, not from some local artifact of the same name.
row_if() {
    python3 - "$1" "$2" "$3" <<'PY'
import json, sys
name, owner = sys.argv[2], sys.argv[3]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name and str(p.get("owner_node")) == owner:
        print("partition %s '%s' (owner node %s)" % (p.get("id"), name, owner))
        break
PY
}

# dump_rows <json> -- every row, for the FAIL diagnostics that must show
# what the node actually held instead of a bare "not found".
dump_rows() {
    python3 - "$1" <<'PY'
import json, sys
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    print("        id=%s owner=%s name=%s" % (p.get("id"), p.get("owner_node"), p.get("name")))
if not rows:
    print("        (no partitions)")
PY
}

# ─── 1. the cluster must exist ────────────────────────────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it. (A node that is DOWN when a
       partition is created must converge from the periodic re-announce --
       that is the property being checked, and it cannot be checked without
       a cluster.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."

# ─── 2. wait for formation: every node answering, a leader with quorum ────
# The pid file is the roster (run-cluster.sh writes one line per node), so
# there is no need to sweep ports; probe exactly the nodes it names.
leader=""; follower=""
echo "==> waiting for the cluster to form (up to ${WAIT_NODES}s)..."
deadline=$(( $(date +%s) + WAIT_NODES ))
while :; do
    up=""; leader=""; follower=""
    for id in $NODE_IDS; do
        port=$((HTTP_BASE + id))
        if get200 "$port" /api/cluster; then
            role="$(jget "$FETCH_BODY" role)"
            active="$(jget "$FETCH_BODY" active_nodes)"
            up="$up $id"
            # A leader is only a leader once it has quorum; a STANDALONE
            # node reports no leader and must not be mistaken for one.
            if [ -z "$leader" ] && [ "$role" = "LEADER" ] \
               && [ "${active:-0}" -ge 2 ] 2>/dev/null; then
                leader="$id"
            fi
        fi
    done
    for id in $up; do
        [ "$id" != "$leader" ] && [ -z "$follower" ] && follower="$id"
    done
    [ -n "$leader" ] && [ -n "$follower" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$leader" ] || die "no leader emerged within ${WAIT_NODES}s. The nodes that
       answered:${up:- none}. Election needs a quorum -- are all nodes on the
       same segment?"
[ -n "$follower" ] || die "no follower found -- the re-announce test needs at
       least two nodes. Start a bigger cluster."
echo "   leader node $leader, follower node $follower"

L_PORT=$((HTTP_BASE + leader))
F_PORT=$((HTTP_BASE + follower))

# ─── 3. create partition A on the leader; the follower must learn it ───────
# The pre-reboot gate. If the mutation-driven announce is broken, nothing
# after this point is meaningful -- and it also lets the follower persist
# its learned row (partition_persist_flush in the sweep), which is what the
# host test pins; here it just proves the replication path still works.
echo "==> creating partition '$NAME_A' on node $leader (both nodes up)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME_A" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"

echo "==> waiting for node $follower to learn '$NAME_A' from the create announce (up to ${WAIT_LEARN}s)"
learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$F_PORT" /api/partitions; then
        learned="$(row_if "$FETCH_BODY" "$NAME_A" "$leader")"
        [ -n "$learned" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $follower never learned partition '$NAME_A' from the
       create announce within ${WAIT_LEARN}s. DSPP_PARTITION_ANNOUNCE
       replication is broken -- the re-announce test cannot proceed. The
       follower's list held:
$(dump_rows "$FETCH_BODY")"
echo "   node $follower learned $learned"

# ─── 4. capture the follower's identity BEFORE touching it ────────────────
F_PID="$(awk -v n="$follower" '$1==n{print $2}' "$PID_FILE")"
[ -n "$F_PID" ] || die "no pid for node $follower in $PID_FILE"
kill -0 "$F_PID" 2>/dev/null || die "node $follower (pid $F_PID) is not running"

# The argv comes from /proc, not from re-deriving it: it is what is ACTUALLY
# running, including whatever RAM/SMP autosizing chose, so the restarted
# node cannot silently differ from the one that was killed.
mapfile -t -d '' F_ARGV < "/proc/$F_PID/cmdline" \
    || die "cannot read /proc/$F_PID/cmdline"
[ "${#F_ARGV[@]}" -gt 3 ] || die "node $follower's argv came back with only ${#F_ARGV[@]} element(s)"
if [ "$FAKE" != "1" ]; then
    case "${F_ARGV[0]}" in
        *qemu*) ;;
        *) die "pid $F_PID does not look like QEMU (argv[0]='${F_ARGV[0]}') --
       refusing to kill and relaunch something unidentified." ;;
    esac
fi
echo "   node $follower = pid $F_PID, ${#F_ARGV[@]} argv elements, ${F_ARGV[0]##*/}"

# ─── 5. kill the follower ─────────────────────────────────────────────────
echo "==> SIGKILL node $follower (pid $F_PID)"
kill -9 "$F_PID" 2>/dev/null || true
gone=0
for _ in $(seq 1 50); do
    kill -0 "$F_PID" 2>/dev/null || { gone=1; break; }
    sleep 0.1
done
[ "$gone" -eq 1 ] || die "node $follower (pid $F_PID) survived SIGKILL"
# Let the console/REST hostfwd ports drain before the relaunch binds them.
sleep 1

# ─── 6. create partition B while the follower is DOWN ─────────────────────
# The tooth of this guard. B is announced once (nobody hears it) and then
# re-announced on the leader's period; the follower is down for BOTH. B is
# on the leader's disk only -- the follower never learned it, so its own
# disk cannot hold it, and learned-row persistence cannot fake this result.
echo "==> creating partition '$NAME_B' on node $leader while node $follower is DOWN"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME_B" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"
echo "   '$NAME_B' was never announced to a live follower and cannot be on its disk"

# ─── 7. relaunch it with the same argv ────────────────────────────────────
# setsid: the relaunched node must outlive this script. It is the contract
# of this guard that the follower rejoins the operator's cluster and stays
# there; a node that dies when the invoking shell exits (WSL sessions, SSH
# one-shots) would make "converged" a lie a minute after it printed. setsid
# detaches it into its own session; the pid it reports is the node's.
setsid "${F_ARGV[@]}" </dev/null >/dev/null 2>"$CLUSTER_DIR/node$follower.stderr" &
NEW_PID=$!
echo "   relaunched as pid $NEW_PID"
# Keep cluster.pids honest, or './run-cluster.sh --stop' later kills the pid
# we just replaced and leaves this node running -- and the next launch then
# refuses to start, blaming "nodes from a previous run".
awk -v n="$follower" -v p="$NEW_PID" \
    '$1==n {print n, p; next} {print}' "$PID_FILE" \
    > "$PID_FILE.new" && mv "$PID_FILE.new" "$PID_FILE"

# ─── 8. wait for the rebooted node to answer ──────────────────────────────
echo "==> waiting for node $follower to come back up (up to ${WAIT_BOOT}s)"
up=0
deadline=$(( $(date +%s) + WAIT_BOOT ))
while :; do
    if get200 "$F_PORT" /api/cluster; then up=1; break; fi
    kill -0 "$NEW_PID" 2>/dev/null || die "node $follower died during boot. stderr:
$(sed 's/^/       /' "$CLUSTER_DIR/node$follower.stderr" | tail -5)"
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$up" -eq 1 ] || die "node $follower never answered on port $F_PORT within ${WAIT_BOOT}s"
echo "   node $follower is up"

# ─── 9. convergence: the periodic re-announce, with no mutation ───────────
# Between B's create and this check nothing mutates the cluster. B was
# never learned and is on nobody's disk but the leader's, so a converged B
# on the rebooted follower can only have come from the periodic re-announce.
echo "==> waiting for node $follower to converge to '$NAME_B' (up to ${WAIT_CONV}s; the re-announce period is 1000 ticks ~= 10s)"
conv=""
deadline=$(( $(date +%s) + WAIT_CONV ))
while :; do
    if get200 "$F_PORT" /api/partitions; then
        conv="$(row_if "$FETCH_BODY" "$NAME_B" "$leader")"
        [ -n "$conv" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$conv" ] || fail "node $follower never converged to partition '$NAME_B' within
       ${WAIT_CONV}s -- the periodic re-announce did not reach it. '$NAME_B'
       was created while this node was DOWN, so it cannot be on disk and
       cannot have been learned; a converged row can only come from
       partition_reannounce_tick(). Its list held:
$(dump_rows "$FETCH_BODY")"

sync_ev="$(grep -h "PARTITION] sync.*$NAME_B" "$CLUSTER_DIR/node$follower.log" 2>/dev/null | tail -2)"
echo
echo "PASS  node $follower converged to $conv after a reboot, with no partition"
echo "      mutation in between -- the row was created while it was DOWN and came"
echo "      from the periodic re-announce."
if [ -n "$sync_ev" ]; then
    echo "      sync evidence ([PARTITION] sync is emitted only by the announce RX):"
    printf '%s\n' "$sync_ev" | sed 's/^/        /'
fi
exit 0
