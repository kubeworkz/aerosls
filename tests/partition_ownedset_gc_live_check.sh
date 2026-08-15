#!/usr/bin/env bash
# tests/partition_ownedset_gc_live_check.sh — a node that was DOWN when a
# partition was destroyed must collect the ghost row from the owner's
# periodic full-owned-set reconciliation.
#
# ─── Why this is the gate that matters ─────────────────────────────────────
# Learned rows persist now (the RX apply sets a dirty flag; the BSP sweep's
# partition_persist_flush() writes the table out), so a node that is DOWN
# when its owner destroys a partition restores the destroyed row from disk
# at its next boot. Withdraws are mutation-only: the destroy broadcasts a
# DSPP_PARTITION_WITHDRAW once, and a node that is down for it never hears
# the row's absence again -- nothing else ever announces "this row no
# longer exists". Before the owned-set reconciliation, that ghost lived on
# the rebooted node's disk forever.
#
# The fix: every PARTITION_OWNEDSET_EVERY re-announce periods, each node
# broadcasts its COMPLETE owned partition-id set (dspp_partition_ownedset_
# send, kernel/partition.c), and partition_sync_ownedset() -- running on
# the RX path -- collects any row in OUR table whose owner is the source
# but whose id is missing from the set, persisting the removal. This guard
# is the repeatable version of the hand-run session that verified it.
#
# The evidence pins the PATH, not just the outcome. Three strings, all in
# the follower's serial log, each emitted by exactly one code site:
#   * `[PERSIST] Partitions snapshot written.`      -- persist.c, only by
#     persist_partitions(); the ghost was ON DISK before the kill.
#   * `[PERSIST] Partition ownership restored from NVMe.` -- persist.c,
#     boot restore; the rebooted node brought the ghost back from disk.
#   * `no longer owned -- collected from the owned-set.` -- partition.c,
#     only by partition_sync_ownedset() on DSPP_PARTITION_OWNEDSET RX.
# The checkpoint broadcast (the other periodic thing that flows) has no
# partition reference at all (net/dspp_checkpoint.c), so a collected row in
# a fresh (post-reboot) log can only have come from the owned-set frame.
#
# ─── Why it does not launch a cluster ─────────────────────────────────────
# This guard KILLS and REBOOTS one node of the cluster it runs against, so
# it must only ever touch a cluster its operator started deliberately --
# the same contract as partition_reannounce_live_check.sh.
#
# ─── Usage ────────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (any size >= 2)
#   tests/partition_ownedset_gc_live_check.sh
#
# The guard will SIGKILL the follower it picks, relaunch it from the argv
# captured in /proc, and leave it running (rejoined to your cluster). It
# does not stop the cluster.
#
# Environment:
#   AEROSLS_HTTP_BASE            port base (default 3000)
#   AEROSLS_TOKEN                bearer token (default: aeroslsctl's demo
#                                DB_ADMIN token, which the kernel accepts)
#   AEROSLS_GC_FAKE=1            test-only: allow relaunching a follower
#                                whose argv[0] is not QEMU (the smoke's
#                                fake nodes). Refused by default, because
#                                killing and relaunching something
#                                unidentified is how you take down the
#                                wrong process.
#   AEROSLS_GC_FAST=1            scale every wait down to seconds. Used by
#                                the smoke; never for a real cluster.
#   AEROSLS_LOG_DIR               where node<id>.log serial logs live
#                                (default: cluster/). The smoke points this
#                                at its scratch state dir so the fake nodes'
#                                logs cannot touch a real cluster's.
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
FAKE="${AEROSLS_GC_FAKE:-0}"
FAST="${AEROSLS_GC_FAST:-0}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="cluster"
LOG_DIR="${AEROSLS_LOG_DIR:-$CLUSTER_DIR}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-gc-$$"                     # unique per run, so a stale persisted
                                       # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_FLUSH=6; WAIT_BOOT=8; WAIT_GC=8
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_FLUSH=60; WAIT_BOOT=240; WAIT_GC=200
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
# owner check matters: a row is only the ghost if it still claims the
# leader's ownership, not if it came back under someone else's id.
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

# last_line <file> <pattern> -- the line number of the LAST match, or ''.
last_line() {
    grep -n "$2" "$1" 2>/dev/null | tail -n1 | cut -d: -f1
}

# ─── 1. the cluster must exist ────────────────────────────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it. (A node that was DOWN when its
       owner destroyed a partition must collect the ghost row from the
       owned-set reconciliation -- that is the property being checked, and
       it cannot be checked without a cluster.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."

# ─── 2. wait for formation: every node answering, a leader with quorum ────
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
[ -n "$follower" ] || die "no follower found -- the owned-set GC test needs at
       least two nodes. Start a bigger cluster."
echo "   leader node $leader, follower node $follower"

L_PORT=$((HTTP_BASE + leader))
F_PORT=$((HTTP_BASE + follower))
F_LOG="$LOG_DIR/node$follower.log"

# ─── 3. create partition A on the leader; the follower must learn it ───────
# The pre-reboot gate. If the mutation-driven announce is broken, nothing
# after this point is meaningful.
echo "==> creating partition '$NAME' on node $leader (both nodes up)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"

echo "==> waiting for node $follower to learn '$NAME' from the create announce (up to ${WAIT_LEARN}s)"
learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$F_PORT" /api/partitions; then
        learned="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
        [ -n "$learned" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $follower never learned partition '$NAME' from the
       create announce within ${WAIT_LEARN}s. DSPP_PARTITION_ANNOUNCE
       replication is broken -- the owned-set GC test cannot proceed. The
       follower's list held:
$(dump_rows "$FETCH_BODY")"
echo "   node $follower learned $learned"

# ─── 4. the ghost must be ON DISK before we kill the node ─────────────────
# The whole test is about a row that a reboot brings back from disk. If the
# follower's learn never flushed, the row is not on disk, and a rebooted
# node has nothing to collect -- the test would pass vacuously (or fail for
# the wrong reason). So gate on the flush evidence: persist_partitions()
# is the only site that prints "Partitions snapshot written", and it must
# appear AFTER the learn line in the follower's own log.
echo "==> waiting for the learned row to flush to the follower's disk (up to ${WAIT_FLUSH}s)"
flushed=""
learn_line=""
deadline=$(( $(date +%s) + WAIT_FLUSH ))
while :; do
    # Recompute both every pass: the serial log can lag the /api list, so
    # the learn line may not exist the first time we look for it.
    learn_line="$(last_line "$F_LOG" "PARTITION] sync.*$NAME")"
    snap_line="$(last_line "$F_LOG" "PERSIST] Partitions snapshot written")"
    if [ -n "$learn_line" ] && [ -n "$snap_line" ] && [ "$snap_line" -gt "$learn_line" ]; then
        flushed=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$flushed" ] || fail "node $follower learned '$NAME' but never flushed it to
       disk (no 'Partitions snapshot written' after the sync line in
       $F_LOG within ${WAIT_FLUSH}s). The owned-set GC test needs the ghost
       ON DISK -- without that, a rebooted node has nothing to collect and
       the test would prove nothing."
echo "   ghost is on disk (learn at line $learn_line, flush after)"

# ─── 5. capture the follower's identity BEFORE touching it ────────────────
F_PID="$(awk -v n="$follower" '$1==n{print $2}' "$PID_FILE")"
[ -n "$F_PID" ] || die "no pid for node $follower in $PID_FILE"
kill -0 "$F_PID" 2>/dev/null || die "node $follower (pid $F_PID) is not running"

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

# ─── 6. kill the follower ─────────────────────────────────────────────────
echo "==> SIGKILL node $follower (pid $F_PID)"
kill -9 "$F_PID" 2>/dev/null || true
gone=0
for _ in $(seq 1 50); do
    kill -0 "$F_PID" 2>/dev/null || { gone=1; break; }
    sleep 0.1
done
[ "$gone" -eq 1 ] || die "node $follower (pid $F_PID) survived SIGKILL"
sleep 1

# ─── 7. destroy partition A on the leader WHILE the follower is down ──────
# The tooth of this guard. The withdraw broadcasts once; the follower is
# down for it and can never hear the row's absence again. Nothing else
# announces a missing row -- only the owner's next full owned-set can tell
# the rebooted node to collect it.
get200 "$L_PORT" /api/partitions || die "node $leader did not answer /api/partitions"
aid="$(python3 - "$FETCH_BODY" "$NAME" "$leader" <<'PY'
import json, sys
name, owner = sys.argv[2], sys.argv[3]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name and str(p.get("owner_node")) == owner:
        print(p.get("id")); break
PY
)"
[ -n "$aid" ] || die "could not read partition id for '$NAME' from the leader's list"
echo "==> destroying partition $aid ('$NAME') on node $leader while node $follower is DOWN"
rc="$(curl -s --max-time 5 "${CURL_EXTRA[@]}" -H "Content-Type: application/json" \
          -d "{\"partition_id\": $aid}" \
          "http://127.0.0.1:$L_PORT/api/partition/destroy" 2>/dev/null)"
case "$rc" in
    *'"ok":"true"'*) echo "   partition $aid destroyed (follower never heard the withdraw)" ;;
    *) die "destroy on node $leader returned: $rc" ;;
esac

# ─── 8. relaunch it with the same argv ────────────────────────────────────
# setsid: the relaunched node must outlive this script. It is the contract
# of this guard that the follower rejoins the operator's cluster and stays
# there. setsid detaches it into its own session; the pid it reports is the
# node's.
setsid "${F_ARGV[@]}" </dev/null >/dev/null 2>"$LOG_DIR/node$follower.stderr" &
NEW_PID=$!
echo "   relaunched as pid $NEW_PID"
awk -v n="$follower" -v p="$NEW_PID" \
    '$1==n {print n, p; next} {print}' "$PID_FILE" \
    > "$PID_FILE.new" && mv "$PID_FILE.new" "$PID_FILE"

# ─── 9. wait for the rebooted node to answer ──────────────────────────────
echo "==> waiting for node $follower to come back up (up to ${WAIT_BOOT}s)"
up=0
deadline=$(( $(date +%s) + WAIT_BOOT ))
while :; do
    if get200 "$F_PORT" /api/cluster; then up=1; break; fi
    kill -0 "$NEW_PID" 2>/dev/null || die "node $follower died during boot. stderr:
$(sed 's/^/       /' "$LOG_DIR/node$follower.stderr" | tail -5)"
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$up" -eq 1 ] || die "node $follower never answered on port $F_PORT within ${WAIT_BOOT}s"
echo "   node $follower is up"

# ─── 10. the owned-set reconciliation must collect the ghost ──────────────
# Three things, in order, all in the follower's own serial log:
#   1. the ghost came BACK from disk at boot -- "Partition ownership
#      restored from NVMe" (only persist.c prints it) -- so the row really
#      was durable, and
#   2. AFTER that restore, the owned-set frame collected it -- "no longer
#      owned -- collected from the owned-set" (only partition_sync_ownedset
#      prints it) -- so the absence we are about to see in the list is the
#      reconciliation, not a vanished learn, and
#   3. the live list no longer holds the row under the leader's ownership.
echo "==> waiting for the rebooted node to collect '$NAME' from the owned-set (up to ${WAIT_GC}s; the owned-set sends every 10 re-announce periods)"
gc=0
deadline=$(( $(date +%s) + WAIT_GC ))
while :; do
    restore_line="$(last_line "$F_LOG" "Partition ownership restored from NVMe")"
    collect_line="$(last_line "$F_LOG" "no longer owned -- collected from the owned-set")"
    still=""
    if get200 "$F_PORT" /api/partitions; then
        still="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
    fi
    if [ -z "$still" ] && [ -n "$restore_line" ] && [ -n "$collect_line" ] \
       && [ "$collect_line" -gt "$restore_line" ]; then
        gc=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done

if [ -n "$still" ]; then
    fail "node $follower still lists $still after ${WAIT_GC}s -- the owned-set
       reconciliation did not collect it. The row was on disk (flushed
       before the kill), restored at boot, and never re-announced (the
       leader destroyed it while this node was down); a remaining row means
       partition_sync_ownedset() never ran or never removed it. The list
       held:
$(dump_rows "$FETCH_BODY")"
fi
if [ -z "$collect_line" ]; then
    fail "the row left node $follower's list, but no 'collected from the
       owned-set' line appeared in $F_LOG within ${WAIT_GC}s -- so the
       absence was NOT the reconciliation. Either the ghost was never on
       disk (flush gate above should have caught that) or the boot restore
       dropped it. Restore line: ${restore_line:-<none>}."
fi
if [ -z "$restore_line" ]; then
    fail "the row is gone and the owned-set collected it, but the boot never
       restored it ('Partition ownership restored from NVMe' never appeared
       in $F_LOG) -- the ghost was not durable, so this test proved nothing."
fi

collect_ev="$(grep -h "no longer owned -- collected from the owned-set" "$F_LOG" 2>/dev/null | tail -1)"
restore_ev="$(grep -h "Partition ownership restored from NVMe" "$F_LOG" 2>/dev/null | tail -1)"
echo
echo "PASS  node $follower was DOWN for the destroy and still collected '$NAME' after"
echo "      a reboot -- the ghost came back from disk and the owner's full owned-set"
echo "      reconciliation removed it."
echo "      evidence:"
printf '        %s\n' "$restore_ev"
printf '        %s\n' "$collect_ev"
exit 0
