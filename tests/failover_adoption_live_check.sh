#!/usr/bin/env bash
# tests/failover_adoption_live_check.sh — when the leader dies, a survivor
# must adopt its partitions from the held checkpoint, and the owner handoff
# must replicate to the other survivor.
#
# ─── Why this is the gate that matters ─────────────────────────────────────
# The failover subsystem (kernel/failover.c) was inert in the running
# kernel until the Step-5 wiring landed: failover_init/failover_tick/
# failover_note_heartbeat had zero live callers, no checkpoint ever flowed
# (dspp_ckpt_send had no callers), and the adoption loop checked the
# survivor's LOCAL partition table -- which never contains another node's
# partitions -- so live clusters never adopted anything. Only the host test,
# which pre-populates the survivor's table, saw the check pass. The wiring
# fixed all of that and it was verified once by hand; before this guard,
# nothing made it repeatable. That is exactly how the entropy path rotted
# unseen here (entropy_init was never called and every host test passed).
#
# The property, end to end, live:
#   1. the leader broadcasts its state tree every 100 ticks, and a follower
#      holds the last COMPLETE checkpoint it received;
#   2. when the leader dies (silent >= FAILOVER_DEAD_TICKS = 300 ticks), the
#      NEWLY ELECTED leader (election timeout 150 < 300, so the new leader is
#      already in place) declares it DEAD and calls failover_recover_from()
#      -- only the leader recovers, so exactly one survivor adopts;
#   3. failover_recover_from() deserializes the held checkpoint, verifies it
#      came from the dead node (hdr.node_id), adopts each partition node it
#      contains (partition_set_owner_node -> owner = this node), and the
#      owner handoff ANNOUNCES over DSPP, so the OTHER survivor learns the
#      row's new owner from the periodic/mutation announce;
#   4. RELAUNCHING the dead leader mid-adoption is safe: the resurrected
#      owner restores its stale row (it still owns the adopted partition on
#      disk) and re-announces it, and the kernel's claimed-by-both conflict
#      path (partition_sync_upsert) rejects the stale claim -- the adopted
#      partition does NOT flap back to the node that lost it -- and the
#      leader's own periodic re-announce teaches the resurrected node the
#      new owner (it converges).
#
# The evidence pins the PATH, not just the outcome. Seven strings, each
# emitted by exactly one code site:
#   * `[DSPP-CKPT] RX: COMPLETE`      -- net/dspp_checkpoint.c, only by the
#     checkpoint RX; gates that the create reached the held checkpoint.
#   * `[FAILOVER] Node <L> declared DEAD` -- kernel/failover.c, only by
#     failover_tick() when silence >= 300 ticks.
#   * `[FAILOVER] Adopted partition <id> from dead node <L>` -- only by
#     failover_recover_from(); proves the adoption, not just a role flip.
#   * `[PARTITION] sync: partition <id> '<name>' (owner node <M>) learned
#     from node <M>` -- only by partition_sync_upsert() on announce RX;
#     proves the owner handoff reached the other survivor.
#   * `[PERSIST] Partition ownership restored from NVMe.` -- persist.c,
#     boot restore; the relaunched leader brought the stale row back from
#     disk (only the restore prints it).
#   * `keeping node <M> (live owner), rejecting the stale claim` --
#     partition_sync_upsert()'s reject branch; the conflict path logged AND
#     resolved in the failover-safe direction.
#   * `leader node <M>'s claim wins, applying` -- partition_sync_upsert()'s
#     leader-wins branch on the resurrected node; the leader's periodic
#     re-announce taught it the new owner (the stale row converges away).
#
# ─── What this guard does, step by step ───────────────────────────────────
# It kills the leader, waits for a survivor to adopt, verifies the handoff
# and the loser's role, and then RELAUNCHES the dead leader from its saved
# argv. That used to be unsafe: the old "last announce wins" conflict rule
# meant the resurrected owner's stale re-announce would steal the adopted
# partition back (flap). partition_sync_upsert now resolves the conflict by
# claim class -- the leader's own claim wins, an owner-initiated transfer
# applies, and any other claim against a live owner is REJECTED -- so the
# relaunch is the point of the test, not a hazard: it proves the conflict
# path logs and resolves without flapping. The guard leaves the relaunched
# leader running, rejoined to the cluster as a follower; the operator stops
# the cluster afterwards (./run-cluster.sh --stop).
#
# Step 11 then proves the same claim-class resolution on the MIGRATE path
# (Multi-Node Phase 4/6), with the write-lease layer pinned end to end: the
# adopter holds the write lease (2-of-3 quorum), migrates the partition
# back to the original leader (the destination step 10 resurrected, whose
# table still holds the stale learned row), the owner-initiated transfer
# out-resolves that stale row on the destination and the observer, the
# destination is killed mid-flight and relaunched and must restore the
# TRANSFERRED row, the cluster converges with no flap, and the lease is
# relinquished at the migrate (holds_lease 0 everywhere) and re-acquired
# on the new owner only through a fresh 2-of-3 quorum.
#
# Step 12 then pins the same claim-class resolution on the SERVICE registry
# (kernel/service_registry.c): the guard registers a service twin on the
# owner before the kill, re-registers it on the adopter after the adoption,
# and -- once the resurrected owner is back -- has it re-announce the SAME
# name (its persisted registry restores it at boot; the guard's declare
# stands in for that re-announce deterministically). service_remote_learn()
# must REJECT the stale claim the way partition_sync_upsert() rejected the
# stale row: the observer's cache keeps the adopter, every node still
# resolves the name to the adopter, and the kernel logs the rejection. The
# old rule -- "last announce wins" -- would hand the adopted name back to
# the node that lost the partition.
#
# ─── Why it does not launch a cluster ─────────────────────────────────────
# This guard KILLS the leader of the cluster it runs against, so it must
# only ever touch a cluster its operator started deliberately -- the same
# contract as entropy_boot_diversity_check.sh. That is also what makes it
# safe to run from run_checks.sh: a runtime guard that booted a cluster of
# its own would do that in the middle of a deploy gate.
#
# ─── Usage ────────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 3          # in another shell (>= 3 nodes)
#   tests/failover_adoption_live_check.sh
#
# The guard will SIGKILL the leader it picks, wait for the adoption, then
# RELAUNCH it from the argv captured in /proc and leave it running (rejoined
# to your cluster as a follower). It does not stop the cluster.
#
# Environment:
#   AEROSLS_HTTP_BASE            port base (default 3000)
#   AEROSLS_TOKEN                bearer token (default: aeroslsctl's demo
#                                DB_ADMIN token, which the kernel accepts)
#   AEROSLS_FAILOVER_FAKE=1      test-only: allow killing a leader whose
#                                argv[0] is not QEMU (the smoke's fake
#                                nodes). Refused by default, because killing
#                                something unidentified is how you take down
#                                the wrong process.
#   AEROSLS_FAILOVER_FAST=1      scale every wait down to seconds. Used by
#                                the smoke; never for a real cluster.
#   AEROSLS_LOG_DIR               where node<id>.log serial logs live
#                                (default: cluster/). The smoke points this
#                                at its scratch state dir.
#   AEROSLS_CLUSTER_DIR          where cluster.pids lives (default: cluster/,
#                                run-cluster.sh's). The smoke points this at
#                                a private temp dir, so it can run beside a
#                                live cluster without touching its pid file.
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no pid file, < 3 nodes).
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
FAKE="${AEROSLS_FAILOVER_FAKE:-0}"
FAST="${AEROSLS_FAILOVER_FAST:-0}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
LOG_DIR="${AEROSLS_LOG_DIR:-$CLUSTER_DIR}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-failover-$$"             # unique per run, so a stale persisted
SVC="svc-$NAME"                      # the service-registry twin: registered on the
                                     # owner pre-kill, re-registered on the adopter
                                     # after adoption; the resurrected owner
                                     # stale re-announce must be REJECTED (step 12)
                                     # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_CKPT=6; WAIT_ADOPT=10; WAIT_HANDOFF=6; WAIT_OBSERVER=5; WAIT_BOOT=10; WAIT_CLAIM=8; WAIT_CONV=8; WAIT_MIGRATE=8; WAIT_TRANSFER=8; WAIT_LEASE=8
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_CKPT=60; WAIT_ADOPT=240; WAIT_HANDOFF=120; WAIT_OBSERVER=60; WAIT_BOOT=240; WAIT_CLAIM=240; WAIT_CONV=180; WAIT_MIGRATE=120; WAIT_TRANSFER=120; WAIT_LEASE=120
fi

die()  { echo; echo "ABORT: $*" >&2; exit 2; }
fail() { echo; echo "FAIL  $*"; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 is needed to parse the /api JSON."
command -v curl    >/dev/null 2>&1 || die "curl not found."
[ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

TOKEN_ARGS=()
[ -n "$TOKEN" ] && TOKEN_ARGS=(--token "$TOKEN")
CURL_EXTRA=()
[ -n "$TOKEN" ] && CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")

# ─── transport ────────────────────────────────────────────────────────────
FETCH_STATUS=""; FETCH_BODY=""; FETCH_RC=0
fetch() {   # $1 = port, $2 = path
    local out
    out="$(curl -s --max-time 3 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    FETCH_RC=$?
    FETCH_STATUS="$(printf '%s' "$out" | tail -n1)"
    FETCH_BODY="$(printf '%s' "$out" | sed '$d')"
}

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
# node <owner>)" when a row with BOTH the name and the owner exists.
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

last_line() {
    grep -n "$2" "$1" 2>/dev/null | tail -n1 | cut -d: -f1
}

row_id() {
    python3 - "$1" "$2" <<'PY'
import json, sys
name = sys.argv[2]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name:
        print(p.get("id", ""))
        break
PY
}

# svc_node <json> -- prints the node_id a service resolves to on this node
# (empty when it does not resolve). Reads /api/service/resolve responses.
svc_node() {
    python3 - "$1" <<'PY'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    d = {}
if d.get("ok") == "true":
    print(d.get("node_id", ""))
PY
}

holds_lease_row() {   # $1 = json, $2 = name; exits 0 when holds_lease=1
    python3 - "$1" "$2" <<'PY'
import json, sys
name = sys.argv[2]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name and str(p.get("holds_lease")) == "1":
        sys.exit(0)
sys.exit(1)
PY
}

# ─── 1. the cluster must exist with >= 3 nodes ────────────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 3
       then re-run this guard against it. (A dead leader's partitions must
       be adopted by a survivor from the held checkpoint -- that is the
       property being checked, and it cannot be checked without a cluster.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."
COUNT=$(echo "$NODE_IDS" | wc -w)
[ "$COUNT" -ge 3 ] || die "failover needs at least 3 nodes: a leader to kill,
       a survivor to adopt, and a second survivor to observe the handoff.
       $PID_FILE names $COUNT ($NODE_IDS). Start a bigger cluster
       (./run-cluster.sh --nodes 3)."

# ─── 2. wait for formation: a leader with quorum and two followers ────────
leader=""; f1=""; f2=""
echo "==> waiting for the cluster to form (up to ${WAIT_NODES}s)..."
deadline=$(( $(date +%s) + WAIT_NODES ))
while :; do
    up=""; leader=""; f1=""; f2=""
    for id in $NODE_IDS; do
        port=$((HTTP_BASE + id))
        if get200 "$port" /api/cluster; then
            role="$(jget "$FETCH_BODY" role)"
            active="$(jget "$FETCH_BODY" active_nodes)"
            up="$up $id"
            if [ -z "$leader" ] && [ "$role" = "LEADER" ] \
               && [ "${active:-0}" -ge 3 ] 2>/dev/null; then
                leader="$id"
            fi
        fi
    done
    for id in $up; do
        [ "$id" != "$leader" ] && { [ -z "$f1" ] && f1="$id" || { [ -z "$f2" ] && f2="$id"; }; }
    done
    [ -n "$leader" ] && [ -n "$f1" ] && [ -n "$f2" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$leader" ] || die "no leader emerged within ${WAIT_NODES}s. The nodes that
       answered:${up:- none}. Election needs a quorum -- are all nodes on the
       same segment?"
[ -n "$f1" ] && [ -n "$f2" ] || die "fewer than two followers found -- the failover
       test needs a leader to kill and TWO survivors (one adopts, one observes).
       The nodes that answered:${up:- none}."
echo "   leader node $leader, followers $f1 and $f2"

L_PORT=$((HTTP_BASE + leader))
F1_PORT=$((HTTP_BASE + f1))
F2_PORT=$((HTTP_BASE + f2))
F1_LOG="$LOG_DIR/node$f1.log"
F2_LOG="$LOG_DIR/node$f2.log"

# ─── 3. create a fresh partition; both followers must learn it ────────────
# The pre-kill gate. If the mutation-driven announce is broken, nothing
# after this point is meaningful. Both survivors must learn it: the one
# that becomes leader adopts it, and the one that observes needs the
# pre-kill row to see the owner handoff against.
echo "==> creating partition '$NAME' on node $leader (all three nodes up)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"

echo "==> waiting for nodes $f1 and $f2 to learn '$NAME' (up to ${WAIT_LEARN}s)"
learned1=""; learned2=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$F1_PORT" /api/partitions; then
        learned1="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
    fi
    if get200 "$F2_PORT" /api/partitions; then
        learned2="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
    fi
    [ -n "$learned1" ] && [ -n "$learned2" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned1" ] && [ -n "$learned2" ] || fail "nodes $f1/$f2 never learned partition
       '$NAME' from the create announce within ${WAIT_LEARN}s (learned1='$learned1'
       learned2='$learned2'). DSPP_PARTITION_ANNOUNCE replication is broken --
       the failover test cannot proceed."
echo "   $f1 learned $learned1"
echo "   $f2 learned $learned2"

# ─── 3.5. register the service twin on the owner; all three resolve ───────
# service_resolve() derives the node from the partition owner for local
# entries and from the announcing node for remote ones, so the pre-kill
# registration must resolve to $leader everywhere. This is the baseline
# step 12 compares against: the same name must STILL resolve to the
# adopter after the resurrected owner re-announces it.
SVC_PORT=9999
PART_ID="$(get200 "$L_PORT" /api/partitions && row_id "$FETCH_BODY" "$NAME")"
[ -n "$PART_ID" ] || die "could not read the partition id for '$NAME' from node $leader"
echo "==> registering service '$SVC' on node $leader (the partition owner)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    services declare --name "$SVC" --partition "$PART_ID" --kind tcp --port "$SVC_PORT" >/dev/null \
    || die "service declare failed on node $leader (aeroslsctl output above)"
echo "==> waiting for all three nodes to resolve '$SVC' to node $leader (up to ${WAIT_LEARN}s)"
svc_base_ok=0
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    r_ok=1
    for pn in "$L_PORT" "$F1_PORT" "$F2_PORT"; do
        if get200 "$pn" "/api/service/resolve/$SVC"; then
            [ "$(svc_node "$FETCH_BODY")" = "$leader" ] || r_ok=0
        else
            r_ok=0
        fi
    done
    [ "$r_ok" -eq 1 ] && { svc_base_ok=1; break; }
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$svc_base_ok" -eq 1 ] || fail "the service registration '$SVC' never resolved to node
       $leader on all three nodes within ${WAIT_LEARN}s. The registration or its DSPP
       replication is broken -- the service tooth cannot proceed."
echo "   '$SVC' resolves to node $leader on all three nodes"

# ─── 4. the checkpoint must carry the row before the leader dies ──────────
# failover_recover_from() adopts what is in the held checkpoint, and the
# leader broadcasts its state tree every 100 ticks. If we killed the leader
# before a checkpoint containing the new row flowed, the adoption would
# find nothing to adopt and fail for the wrong reason. Gate on the RX
# evidence: `[DSPP-CKPT] RX: COMPLETE` must appear AFTER the learn line in
# each survivor's log.
echo "==> waiting for a checkpoint carrying '$NAME' to reach both survivors (up to ${WAIT_CKPT}s)"
ckpt_ok=0
deadline=$(( $(date +%s) + WAIT_CKPT ))
while :; do
    learn1="$(last_line "$F1_LOG" "PARTITION] sync.*$NAME")"
    learn2="$(last_line "$F2_LOG" "PARTITION] sync.*$NAME")"
    ck1="$(last_line "$F1_LOG" "DSPP-CKPT] RX: COMPLETE")"
    ck2="$(last_line "$F2_LOG" "DSPP-CKPT] RX: COMPLETE")"
    if [ -n "$learn1" ] && [ -n "$ck1" ] && [ "$ck1" -gt "$learn1" ] \
       && [ -n "$learn2" ] && [ -n "$ck2" ] && [ "$ck2" -gt "$learn2" ]; then
        ckpt_ok=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$ckpt_ok" -eq 1 ] || fail "no checkpoint carrying '$NAME' reached both survivors
       within ${WAIT_CKPT}s ('DSPP-CKPT] RX: COMPLETE' must appear after the sync
       line in $F1_LOG and $F2_LOG). Without it the held checkpoint predates the
       create and the adoption would find nothing to adopt -- the test would
       fail for the wrong reason."
echo "   checkpoint carrying '$NAME' held by both survivors"

# ─── 5. capture the leader's identity BEFORE killing it ───────────────────
L_PID="$(awk -v n="$leader" '$1==n{print $2}' "$PID_FILE")"
[ -n "$L_PID" ] || die "no pid for node $leader in $PID_FILE"
kill -0 "$L_PID" 2>/dev/null || die "node $leader (pid $L_PID) is not running"
# The FULL argv, captured now: step 10 relaunches the killed leader with
# exactly these arguments, so its NVMe disk (the stale partition row), its
# serial logfile and its node identity all survive the restart -- the same
# pattern partition_ownedset_gc_live_check.sh already uses.
mapfile -t -d '' L_ARGV < "/proc/$L_PID/cmdline" \
    || die "cannot read /proc/$L_PID/cmdline"
[ "${#L_ARGV[@]}" -gt 3 ] || die "node $leader's argv came back with only ${#L_ARGV[@]} element(s)"
if [ "$FAKE" != "1" ]; then
    case "${L_ARGV[0]}" in
        *qemu*) ;;
        *) die "pid $L_PID does not look like QEMU (argv[0]='${L_ARGV[0]}') --
       refusing to kill and relaunch something unidentified." ;;
    esac
fi
echo "   node $leader = pid $L_PID, ${#L_ARGV[@]} argv elements, ${L_ARGV[0]##*/}"

# ─── 6. kill the leader ───────────────────────────────────────────────────
echo "==> SIGKILL node $leader (pid $L_PID)"
kill -9 "$L_PID" 2>/dev/null || true
gone=0
for _ in $(seq 1 50); do
    kill -0 "$L_PID" 2>/dev/null || { gone=1; break; }
    sleep 0.1
done
[ "$gone" -eq 1 ] || die "node $leader (pid $L_PID) survived SIGKILL"

# ─── 7. wait for the adoption: a survivor leads, declares, recovers ───────
# The new leader is whichever survivor becomes LEADER with quorum after the
# kill. Only the leader adopts (cluster_is_leader() in failover_tick), so
# exactly one survivor should ever print the Adopted line. We wait for:
#   * a survivor to report LEADER (role flip on the surviving pair), and
#   * its log to show the death declaration THEN the adoption, and
#   * its partition list to hold '$NAME' with owner = that survivor.
echo "==> waiting for a survivor to adopt '$NAME' from the dead leader (up to ${WAIT_ADOPT}s; death at 300 ticks of silence)"
adopter=""; observer=""
deadline=$(( $(date +%s) + WAIT_ADOPT ))
while :; do
    adopt1=""; adopt2=""
    if get200 "$F1_PORT" /api/cluster; then
        r1="$(jget "$FETCH_BODY" role)"
        [ "$r1" = "LEADER" ] && adopt1=1
    fi
    if get200 "$F2_PORT" /api/cluster; then
        r2="$(jget "$FETCH_BODY" role)"
        [ "$r2" = "LEADER" ] && adopt2=1
    fi
    if [ -n "$adopt1" ] && [ -n "$adopt2" ]; then
        fail "both survivors ($f1 and $f2) report LEADER after node $leader died --
       split-brain: a partition must have exactly one owner, and
       failover_tick() only lets the leader recover. The cluster is in an
       unrecoverable state -- stop it and inspect the consensus logs."
    fi
    if [ -n "$adopt1" ] || [ -n "$adopt2" ]; then
        can=""; ob=""
        if [ -n "$adopt1" ]; then can="$f1"; ob="$f2"; else can="$f2"; ob="$f1"; fi
        CAN_LOG="$LOG_DIR/node$can.log"
        dead_line="$(last_line "$CAN_LOG" "FAILOVER] Node $leader declared DEAD")"
        adopt_line="$(last_line "$CAN_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
        rec_line="$(last_line "$CAN_LOG" "FAILOVER] recovery for dead node $leader: rc=0")"
        owned=""
        if get200 "$((HTTP_BASE + can))" /api/partitions; then
            owned="$(row_if "$FETCH_BODY" "$NAME" "$can")"
        fi
        if [ -n "$dead_line" ] && [ -n "$adopt_line" ] \
           && [ "$adopt_line" -gt "$dead_line" ] && [ -n "$rec_line" ] \
           && [ -n "$owned" ]; then
            adopter="$can"; observer="$ob"; break
        fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done

if [ -z "$adopter" ]; then
    # Distinguish "nobody became leader" from "leader but no adoption".
    any_leader=""
    get200 "$F1_PORT" /api/cluster && [ "$(jget "$FETCH_BODY" role)" = "LEADER" ] && any_leader=1
    get200 "$F2_PORT" /api/cluster && [ "$(jget "$FETCH_BODY" role)" = "LEADER" ] && any_leader=1
    if [ -z "$any_leader" ]; then
        fail "neither survivor ($f1, $f2) became LEADER within ${WAIT_ADOPT}s after node
       $leader died. The election did not converge -- is the cluster's
       consensus heartbeating? (The adoption needs a leader.)"
    fi
    fail "a survivor became leader but never adopted '$NAME' within ${WAIT_ADOPT}s.
       Check for 'FAILOVER] Node $leader declared DEAD' and 'FAILOVER] Adopted
       partition ... from dead node $leader' (rc=0) in $LOG_DIR/node*.log. A
       leader that never recovers means failover_recover_from() is broken or
       the held checkpoint did not carry the row."
fi
echo "   $adopter became leader, declared node $leader DEAD, and adopted $owned"

# ─── 7.5. the adopter re-registers the service twin ───────────────────────
# The adoption moved ownership; the service twin moves with it. The adopter
# re-registers the same name (standing in for the adoption path re-issuing
# its services) and every up node must resolve it to the adopter. This is
# the LIVE entry step 12's stale claim must not displace.
echo "==> re-registering service '$SVC' on node $adopter (the new owner)"
"$CTL" --host "localhost:$((HTTP_BASE + adopter))" "${TOKEN_ARGS[@]}" \
    services declare --name "$SVC" --partition "$PART_ID" --kind tcp --port "$SVC_PORT" >/dev/null \
    || die "service re-declare failed on node $adopter (aeroslsctl output above)"
echo "==> waiting for the survivors to resolve '$SVC' to node $adopter (up to ${WAIT_LEARN}s)"
svc_adopt_ok=0
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    r_ok=1
    for pn in "$((HTTP_BASE + adopter))" "$((HTTP_BASE + observer))"; do
        if get200 "$pn" "/api/service/resolve/$SVC"; then
            [ "$(svc_node "$FETCH_BODY")" = "$adopter" ] || r_ok=0
        else
            r_ok=0
        fi
    done
    [ "$r_ok" -eq 1 ] && { svc_adopt_ok=1; break; }
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$svc_adopt_ok" -eq 1 ] || fail "the service '$SVC' never resolved to the adopter (node
       $adopter) after its re-registration within ${WAIT_LEARN}s. The owner-initiated
       re-announce did not replicate -- the service tooth cannot proceed."
echo "   '$SVC' resolves to node $adopter on the survivors"

# ─── 8. the owner handoff must replicate to the other survivor ────────────
# partition_set_owner_node() announces the new owner, and the observer's
# partition_sync_upsert() logs the learn. The observer never adopted (only
# the leader does), so a learned row with owner = adopter is the handoff.
OBS_LOG="$LOG_DIR/node$observer.log"
echo "==> waiting for the owner handoff to reach node $observer (up to ${WAIT_HANDOFF}s)"
handoff=""
deadline=$(( $(date +%s) + WAIT_HANDOFF ))
while :; do
    if get200 "$((HTTP_BASE + observer))" /api/partitions; then
        handoff="$(row_if "$FETCH_BODY" "$NAME" "$adopter")"
        [ -n "$handoff" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$handoff" ] || fail "node $observer never saw the owner handoff for '$NAME'
       (owner = node $adopter) within ${WAIT_HANDOFF}s. The adoption happened on
       $adopter but its owner-change announce did not reach $observer -- the
       replication of the handoff is broken. $observer's list held:
$(get200 "$((HTTP_BASE + observer))" /api/partitions && dump_rows "$FETCH_BODY")"

# ─── 9. the loser must stay FOLLOWER and never adopt ──────────────────────
# The step-7 loop breaks the moment ONE survivor's adoption evidence is
# complete, so a misbehaviour that lands after that -- a follower that ALSO
# recovers, or a late flip to LEADER -- would otherwise slip past. Both are
# the other half of the split-brain property, and they pin the two gates:
#   * cluster_is_leader() in failover_tick(): every node that observes the
#     death DECLARES it (the observer's log must carry its own declaration
#     -- it observed), but only the leader calls failover_recover_from().
#     An Adopted or recovery line on the OBSERVER, after its declaration,
#     means a follower recovered too: two owners of one partition.
#   * the election: the loser's consensus role must stay FOLLOWER. A flip
#     to LEADER after the adoption window is a late split-brain -- two
#     leaders, two owners.
OBS_PORT=$((HTTP_BASE + observer))
echo "==> watching node $observer stay FOLLOWER and never adopt (up to ${WAIT_OBSERVER}s)"
# The watch HOLDS THE FULL WINDOW: it must not break on the first clean
# poll, or a late misbehaviour -- a follower that recovers after the
# adoption, or a flip to LEADER that lands a moment later -- would slip
# past. It passes only if EVERY poll observed the loser as FOLLOWER and no
# Adopted/recovery line ever appeared after the declaration. A transient
# fetch failure does NOT fail the watch -- only a 200 that says LEADER, or
# a late Adopted line, does.
loser_ok=1
loser_why=""
decl_seen=0
deadline=$(( $(date +%s) + WAIT_OBSERVER ))
while :; do
    # 9a. role: must be FOLLOWER at EVERY poll. Once flipped, the loser is
    # out -- a second LEADER means a second owner of '$NAME'.
    if get200 "$OBS_PORT" /api/cluster; then
        [ "$(jget "$FETCH_BODY" role)" = "FOLLOWER" ] || { loser_ok=0; loser_why="flipped to LEADER"; }
    fi
    # 9b. log: the observer must have declared the death (it observed), and
    # must NOT have printed an Adopted or recovery line AFTER that
    # declaration. (A stale line from a previous run of this guard against
    # the same cluster would sit BEFORE this run's declaration -- last_line
    # takes the LAST match, so the comparison is scoped to this run.)
    obs_decl="$(last_line "$OBS_LOG" "FAILOVER] Node $leader declared DEAD")"
    obs_adopt="$(last_line "$OBS_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
    obs_rec="$(last_line "$OBS_LOG" "FAILOVER] recovery for dead node $leader")"
    if [ -n "$obs_decl" ]; then
        decl_seen=1
        [ -z "$obs_adopt" ] || [ "$obs_adopt" -lt "$obs_decl" ] || { loser_ok=0; loser_why="printed its own Adopted line"; }
        [ -z "$obs_rec" ] || [ "$obs_rec" -lt "$obs_decl" ] || { loser_ok=0; loser_why="printed its own recovery line"; }
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$decl_seen" -eq 1 ] || { loser_ok=0; loser_why="never observed the death (no declaration in $OBS_LOG)"; }
if [ "$loser_ok" -ne 1 ]; then
    # Which gate failed? Report the specific violation.
    if get200 "$OBS_PORT" /api/cluster && [ "$(jget "$FETCH_BODY" role)" != "FOLLOWER" ]; then
        fail "node $observer flipped to $(jget "$FETCH_BODY" role) after node $adopter
       adopted -- a late split-brain. The surviving pair must elect exactly
       one leader and the loser stays FOLLOWER; a second leader means two
       owners of '$NAME'. Stop the cluster and inspect the consensus logs."
    fi
    obs_adopt="$(last_line "$OBS_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
    obs_rec="$(last_line "$OBS_LOG" "FAILOVER] recovery for dead node $leader")"
    if [ -n "$obs_adopt" ] || [ -n "$obs_rec" ]; then
        fail "node $observer ALSO recovered from dead node $leader -- a follower
       must never adopt (cluster_is_leader() in failover_tick gates recovery
       to the leader). The observer's log shows:
$(grep -h "FAILOVER] Adopted partition.*dead node $leader\|FAILOVER] recovery for dead node $leader" "$OBS_LOG" 2>/dev/null | tail -2 | sed 's/^/         /')
       Two Adopted lines means two owners of '$NAME'."
    fi
    fail "node $observer neither stayed FOLLOWER nor observed the death cleanly
       (role check / declaration check failed: ${loser_why:-unknown}) within
       ${WAIT_OBSERVER}s. Inspect $OBS_LOG."
fi
echo "   node $observer stayed FOLLOWER and never adopted (observed the death only)"

# ─── 10. the resurrected owner: the conflict must log and resolve ────────
# The partition id is needed for the claim-class evidence below: the
# conflict lines carry the partition id, not the name (partition_sync_upsert
# logs the claimed-by pair before the name is in scope).
PART_ID="$(get200 "$((HTTP_BASE + adopter))" /api/partitions && row_id "$FETCH_BODY" "$NAME")"
[ -n "$PART_ID" ] || die "could not read the partition id for '$NAME' from node $adopter"
# Relaunch the killed leader MID-adoption. It restores its stale row (it
# still owns the adopted partition on disk), re-announces it, and the
# claimed-by-both conflict path must REJECT the stale claim -- the adopted
# partition must not flap back to the node that lost it -- and the leader's
# own periodic re-announce must teach the resurrected node the new owner.
# Four evidence gates, each scoped to THIS run by line-number ordering
# against the adoption evidence:
#   1. the relaunched leader restored the stale row ("Partition ownership
#      restored from NVMe", only persist.c prints it) -- without the row
#      there is nothing to conflict over;
#   2. BOTH survivors log the reject ("keeping node ... , rejecting the
#      stale claim", only partition_sync_upsert's reject branch prints it)
#      AFTER their adoption evidence -- proving the claimed-by-both path
#      ran and resolved in the failover-safe direction;
#   3. the owner does NOT flap: no "learned from node $leader" for '$NAME'
#      after the adoption, and the live lists on BOTH survivors still name
#      the adopter;
#   4. the resurrected node CONVERGES: the leader's periodic re-announce
#      reaches it ("leader node $adopter's claim wins, applying" then the
#      learn) AFTER the boot restore -- the stale row does not survive.
CAN_LOG="$LOG_DIR/node$adopter.log"
adopt_line="$(last_line "$CAN_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
handoff_line="$(last_line "$OBS_LOG" "PARTITION] sync.*$NAME.*learned from node $adopter")"
L_LOG="$LOG_DIR/node$leader.log"
L_PORT=$((HTTP_BASE + leader))

echo "==> relaunching the dead leader node $leader to prove the resurrected-owner conflict resolves"
setsid "${L_ARGV[@]}" </dev/null >/dev/null 2>"$LOG_DIR/node$leader.stderr" &
NEW_L_PID=$!
echo "   relaunched as pid $NEW_L_PID"
awk -v n="$leader" -v p="$NEW_L_PID" \
    '$1==n {print n, p; next} {print}' "$PID_FILE" \
    > "$PID_FILE.new" && mv "$PID_FILE.new" "$PID_FILE"

echo "==> waiting for node $leader to come back up (up to ${WAIT_BOOT}s)"
up=0
deadline=$(( $(date +%s) + WAIT_BOOT ))
while :; do
    if get200 "$L_PORT" /api/cluster; then up=1; break; fi
    kill -0 "$NEW_L_PID" 2>/dev/null || die "node $leader died during relaunch boot. stderr:
$(sed 's/^/       /' "$LOG_DIR/node$leader.stderr" | tail -5)"
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$up" -eq 1 ] || die "relaunched node $leader never answered on port $L_PORT within ${WAIT_BOOT}s"
echo "   node $leader is back up"

echo "==> waiting for the resurrected-owner conflict to log and resolve (up to ${WAIT_CLAIM}s; the re-announce period is 1000 ticks)"
restored=0; rej_s=0; rej_o=0; lw=0
deadline=$(( $(date +%s) + WAIT_CLAIM ))
while :; do
    # Each gate latches independently -- they land on different polls.
    restore_line="$(last_line "$L_LOG" "Partition ownership restored from NVMe")"
    [ -n "$restore_line" ] && restored=1
    # Path (a): the stale claim escaped -- both survivors rejected it.
    rej_s_line="$(last_line "$CAN_LOG" "rejecting the stale claim")"
    rej_o_line="$(last_line "$OBS_LOG" "rejecting the stale claim")"
    [ -n "$rej_s_line" ] && [ "$rej_s_line" -gt "$adopt_line" ] && rej_s=1
    [ -n "$rej_o_line" ] && [ "$rej_o_line" -gt "$handoff_line" ] && rej_o=1
    # Path (b): the leader's claim reached the resurrected node before its
    # own stale re-announce fired (the common live case -- the leader's
    # re-announce tick is partway while the rebooted node's starts at
    # zero) -- the conflict logged on the RESURRECTED node, resolved
    # leader-wins, scoped to $NAME by the partition id in the line.
    lw_line="$(last_line "$L_LOG" "PARTITION] sync: partition $PART_ID claimed by node $leader and node $adopter -- leader node $adopter's claim wins")"
    [ -n "$lw_line" ] && [ -n "$restore_line" ] && [ "$lw_line" -gt "$restore_line" ] && lw=1
    if [ "$restored" -eq 1 ] && { { [ "$rej_s" -eq 1 ] && [ "$rej_o" -eq 1 ]; } || [ "$lw" -eq 1 ]; }; then
        break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$restored" -eq 1 ] || fail "the relaunched leader never restored its stale row
       ('Partition ownership restored from NVMe' absent from $L_LOG within
       ${WAIT_CLAIM}s). Without the stale row there is nothing to conflict
       over -- the resurrected-owner resolution cannot be exercised."

# no-flap: the owner must still be the adopter on BOTH survivors, and no
# learn from the dead leader may have arrived after the adoption.
flap_s="$(last_line "$CAN_LOG" "PARTITION] sync.*$NAME.*learned from node $leader")"
flap_o="$(last_line "$OBS_LOG" "PARTITION] sync.*$NAME.*learned from node $leader")"
still_s=""; still_o=""
if get200 "$((HTTP_BASE + adopter))" /api/partitions; then
    still_s="$(row_if "$FETCH_BODY" "$NAME" "$adopter")"
fi
if get200 "$OBS_PORT" /api/partitions; then
    still_o="$(row_if "$FETCH_BODY" "$NAME" "$adopter")"
fi
if [ -n "$flap_s" ] && [ "$flap_s" -gt "$adopt_line" ]; then
    fail "the adopted partition FLAPPED: node $adopter re-learned '$NAME' from the
       resurrected node $leader (learn line at $flap_s, after the adoption).
       The conflict path must reject the stale claim, not apply it."
fi
if [ -n "$flap_o" ] && [ "$flap_o" -gt "$handoff_line" ]; then
    fail "the adopted partition FLAPPED: node $observer re-learned '$NAME' from the
       resurrected node $leader (learn line at $flap_o, after the handoff).
       The conflict path must reject the stale claim, not apply it."
fi
[ -n "$still_s" ] && [ -n "$still_o" ] || fail "the survivors no longer agree on the owner of
       '$NAME': adopter list held '${still_s:-<none>}', observer list held
       '${still_o:-<none>}'. The resurrected node's stale claim stole the
       partition -- the conflict path must keep the live owner."

# conflict evidence: the claimed-by-both path must have LOGGED its
# resolution -- either the survivors' rejects (path a: the stale claim
# escaped before the resurrected node converged) or the resurrected
# node's own leader-wins (path b: the leader's re-announce beat it, the
# common live case). The no-flap checks above already ruled out the wrong
# direction; this gate rules out "the conflict never happened at all".
conflict_ok=0
{ [ "$rej_s" -eq 1 ] && [ "$rej_o" -eq 1 ]; } && conflict_ok=1
[ "$lw" -eq 1 ] && conflict_ok=1
[ "$conflict_ok" -eq 1 ] || fail "the claimed-by-both conflict path never logged its
       resolution within ${WAIT_CLAIM}s: neither survivor logged 'rejecting the
       stale claim' after the adoption, nor did the resurrected node log
       'leader node $adopter's claim wins' after its boot restore. Either the
       stale row never escaped (announce broken) or both resolution branches
       are gone."

# convergence: the leader's periodic re-announce teaches the resurrected
# node the new owner.
echo "==> waiting for the resurrected node to converge to the adopter (up to ${WAIT_CONV}s)"
converged=0
deadline=$(( $(date +%s) + WAIT_CONV ))
while :; do
    conv_learn="$(last_line "$L_LOG" "PARTITION] sync.*$NAME.*learned from node $adopter")"
    restore_line="$(last_line "$L_LOG" "Partition ownership restored from NVMe")"
    if [ -n "$conv_learn" ] && [ -n "$restore_line" ] && [ "$conv_learn" -gt "$restore_line" ]; then
        converged=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$converged" -eq 1 ] || fail "the resurrected node $leader never converged to the new
       owner ('learned from node $adopter' for '$NAME' absent after its boot
       restore in $L_LOG within ${WAIT_CONV}s). The leader's periodic
       re-announce must teach the stale node the truth, or the stale row
       survives in its table forever."

# ─── 12. the service twin: the resurrected owner's stale re-announce ──────
# Step 10 resolved the PARTITION claim; the SERVICE registration must
# resolve the same way or the adoption is not durable. The resurrected
# owner's persisted registry (persist_services restored it at boot) holds
# the pre-kill registration and re-announces it; the guard's declare here
# makes that re-announce deterministic. service_remote_learn()'s
# claim-class resolver (mirroring partition_sync_upsert) must REJECT the
# stale claim on the observer -- which holds the adopter's live entry -- so
# the name keeps resolving to the adopter on EVERY node, and the kernel
# logs the rejection. The old rule -- "last announce wins" -- would hand
# the adopted name back to the node that lost the partition.
echo "==> resurrected owner node $leader re-announces service '$SVC' (stale claim)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    services declare --name "$SVC" --partition "$PART_ID" --kind tcp --port "$SVC_PORT" >/dev/null \
    || die "stale service declare failed on node $leader (aeroslsctl output above)"

echo "==> waiting for the survivors to reject the stale claim (up to ${WAIT_CLAIM}s)"
svc_rej=0
svc_flapped=""
deadline=$(( $(date +%s) + WAIT_CLAIM ))
while :; do
    # The rejection must be logged AFTER the step-8 handoff to scope it to
    # this run, and must name OUR service (the line carries the name, so a
    # stale run's reject against the same cluster cannot match).
    rej_svc="$(last_line "$OBS_LOG" "\[SERVICE\].*$SVC.*rejecting the stale claim")"
    [ -n "$rej_svc" ] && [ -n "$handoff_line" ] && [ "$rej_svc" -gt "$handoff_line" ] && svc_rej=1
    # No-flap: EVERY node must still resolve the name to the adopter; the
    # moment any node reports the resurrected owner, the stale claim won.
    r_ok=1
    for pn in "$L_PORT" "$((HTTP_BASE + adopter))" "$((HTTP_BASE + observer))"; do
        if get200 "$pn" "/api/service/resolve/$SVC"; then
            n="$(svc_node "$FETCH_BODY")"
            if [ "$n" = "$leader" ]; then
                svc_flapped=1; r_ok=0
            elif [ "$n" != "$adopter" ]; then
                r_ok=0
            fi
        else
            r_ok=0
        fi
    done
    if [ "$svc_rej" -eq 1 ] && [ "$r_ok" -eq 1 ]; then break; fi
    [ -n "$svc_flapped" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
if [ -n "$svc_flapped" ]; then
    fail "the service registration FLAPPED: a node resolved '$SVC' back to the
       resurrected owner (node $leader) after its stale re-announce. The
       claim-class resolver must keep the live registration (node $adopter) --
       the old 'last announce wins' rule hands the adopted name back to the
       node that lost the partition."
fi
[ "$svc_rej" -eq 1 ] || fail "the stale service re-announce was never rejected: no
       '[SERVICE] ... rejecting the stale claim' for '$SVC' appeared in $OBS_LOG after
       the handoff within ${WAIT_CLAIM}s. Either the stale announce never escaped
       (replication broken) or service_remote_learn()'s claim-class resolver is gone."
echo "   '$SVC' still resolves to node $adopter on all three nodes (stale claim rejected)"

# ─── 11. the migration return: the owner-initiated claim out-resolves ─────
# the resurrected destination's stale row, and the write lease follows the
# new owner only through a fresh quorum election.
#
# Step 10 proved the ADOPTION stays put against a resurrected owner. This
# step proves the same claim-class resolution on the MIGRATE path, with
# the write-lease layer pinned end to end:
#   * the adopter (current owner AND lease holder) migrates the partition
#     back to the original leader -- the destination step 10 resurrected,
#     whose table still holds the stale LEARNED row (owner = the adopter);
#   * the destination is ALIVE for the migrate (it must ACK the stream
#     data), so the transfer announce (owner = leader, source = adopter)
#     reaches it and partition_sync_upsert's owner-initiated branch
#     (source == current owner) OUT-RESOLVES the stale learned row --
#     "claimed by node <adopter> and node <leader> -- owner-initiated
#     transfer, applying" on the destination AND the observer;
#   * the destination is then killed MID-FLIGHT (its apply of the transfer
#     is still settling to NVMe) and relaunched: it must restore the
#     TRANSFERRED row (owner = leader) -- never the stale learned row --
#     and the whole cluster must converge to exactly one owner with no
#     flap back to the adopter;
#   * the lease layer: the adopter holds the write lease before the migrate
#     (2-of-3 quorum; the [MMU-LEASE] strip on campaign and restore on
#     win), relinquishes it at the migrate (holds_lease drops to 0,
#     "Lease relinquished=yes"), and NOBODY holds it while the destination
#     is down (the lease table is runtime-only -- a resurrected node
#     cannot resurrect write permission); the new owner re-acquires it
#     only through a fresh 2-of-3 quorum election, and the other two nodes
#     hold 0.
#
# The kernel change that would make this fail is exactly the unsafe one: an
# ownership transfer the destination can LOSE (it cannot -- the destination
# participates in the stream handshake and the transfer's owner-initiated
# branch applies over its stale row), a lease restore without a quorum, or
# partition_holds_write_lease() true on a node that did not win one.
ADP_LOG="$CAN_LOG"
ADP_PORT=$((HTTP_BASE + adopter))

# 11a. the adopter holds the write lease BEFORE the migrate: the 2-of-3
# quorum is reachable with the resurrected leader back up, so a no-win is
# a broken lease layer, not a quorum gate.
echo "==> acquiring the write lease for '$NAME' (partition $PART_ID) on node $adopter"
"$CTL" --host "localhost:$ADP_PORT" "${TOKEN_ARGS[@]}" \
    shell partition lease acquire "$PART_ID" >/dev/null \
    || die "partition lease acquire $PART_ID failed on node $adopter (aeroslsctl output above)"
adp_held=0
deadline=$(( $(date +%s) + WAIT_LEASE ))
while :; do
    if get200 "$ADP_PORT" /api/partitions; then
        if holds_lease_row "$FETCH_BODY" "$NAME"; then adp_held=1; break; fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$adp_held" -eq 1 ] || fail "node $adopter never held the write lease for '$NAME' before
       the migrate (holds_lease stayed 0 in /api/partitions within ${WAIT_LEASE}s). With
       the resurrected leader back up the 2-of-3 quorum IS reachable -- a no-win means
       the lease layer is broken (acquire inert or the vote exchange incomplete)."
echo "   node $adopter holds the write lease for '$NAME'"
echo "==> waiting for node $observer to LEARN the lease row (up to ${WAIT_LEASE}s)"
lease_init_obs=""
deadline=$(( $(date +%s) + WAIT_LEASE ))
while :; do
    lease_init_obs="$(last_line "$OBS_LOG" "CONSENSUS] partition $PART_ID lease initialised")"
    [ -n "$lease_init_obs" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$lease_init_obs" ] || fail "node $observer never created a lease row for partition
       $PART_ID (no 'partition $PART_ID lease initialised' in $OBS_LOG within ${WAIT_LEASE}s).
       Without a row the absence of a restore on the resurrected node would be vacuous."
echo "   $observer learned the lease row"

# 11b. the migrate itself: adopter -> original leader (the destination step
# 10 resurrected). The migrate line proves the lease was relinquished at
# the handoff (partition_migrate step 2).
#
# EVIDENCE PATH: partition_migrate() prints its success line and the lease
# step-down SYNCHRONOUSLY, inside the shell's capture window
# (kernel_serial_capture_start -- sls_shell_execute diverts every character
# the command produces into the HTTP response and nothing reaches the UART).
# So the migrate evidence lives in the aeroslsctl RESPONSE, never the serial
# log. Grepping $ADP_LOG for it is how a successful migrate once read as
# "inert" on a live cluster (the row moved -- the adopter's table flipped
# to owner = node $leader and announced it -- while the log never showed
# the line). The response is the evidence.
echo "==> migrating '$NAME' (partition $PART_ID) from node $adopter back to node $leader"
mig_resp="$("$CTL" --host "localhost:$ADP_PORT" "${TOKEN_ARGS[@]}" \
    shell partition migrate "$PART_ID" "$leader" 2>&1)" \
    || die "partition migrate $PART_ID -> $leader failed on node $adopter (aeroslsctl output above)"
case "$mig_resp" in
    *"migrated partition $PART_ID: node $adopter -> node $leader"*) ;;
    *) fail "the migrate of partition $PART_ID never ran on node $adopter
       ('migrated partition $PART_ID: node $adopter -> node $leader' absent from the
       shell response: $(printf '%s' "$mig_resp" | tail -3 | sed 's/^/       /')). The
       migrate command is inert or refused." ;;
esac
echo "   migrate confirmed: $(printf '%s' "$mig_resp" | grep -m1 'migrated partition' | sed 's/^/      /')"
case "$mig_resp" in
    *"Lease relinquished=yes"*) ;;
    *) fail "the migrate of partition $PART_ID did NOT relinquish the write lease (no
       'Lease relinquished=yes' in the migrate response). partition_migrate's step 2 must
       step the lease down -- a source that keeps write permission after handing the
       row off is the page-level split-brain." ;;
esac
case "$mig_resp" in
    *"voluntarily stepped down from LEADER"*) ;;
    *) fail "the migrate of partition $PART_ID did NOT step the lease row down (no
       'voluntarily stepped down from LEADER' in the migrate response). partition_lease_
       step_down never ran -- the source keeps its lease row over a partition it no
       longer owns." ;;
esac
echo "   lease relinquished at the migrate"
echo "==> waiting for node $adopter's holds_lease to drop to 0 (up to ${WAIT_LEASE}s)"
adp_dropped=0
deadline=$(( $(date +%s) + WAIT_LEASE ))
while :; do
    if get200 "$ADP_PORT" /api/partitions; then
        if ! holds_lease_row "$FETCH_BODY" "$NAME"; then adp_dropped=1; break; fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$adp_dropped" -eq 1 ] || fail "node $adopter still reports holds_lease=1 for '$NAME' after
       the migrate. partition_lease_step_down did not clear the lease -- the source keeps
       write permission over a partition it no longer owns."

# 11c. the owner-initiated claim must out-resolve the destination's stale
# LEARNED row (owner = adopter) BEFORE anything is killed. The destination
# is alive here (it ACKed the stream data), so the transfer announce
# (owner = leader, source = adopter) reaches it, and partition_sync_upsert
# applies it via the source == current-owner branch -- logged on the
# destination AND the observer, scoped AFTER the migrate line.
echo "==> waiting for the owner-initiated transfer to out-resolve the stale row (up to ${WAIT_TRANSFER}s)"
tr_leader=""; tr_obs=""
deadline=$(( $(date +%s) + WAIT_TRANSFER ))
while :; do
    tr_leader="$(last_line "$L_LOG" "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying")"
    tr_obs="$(last_line "$OBS_LOG" "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying")"
    [ -n "$tr_leader" ] && [ -n "$tr_obs" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
lease_init_leader="$(last_line "$L_LOG" "CONSENSUS] partition $PART_ID lease initialised")"
[ -n "$tr_leader" ] && [ -n "$lease_init_leader" ] && [ "$tr_leader" -gt "$lease_init_leader" ]     || fail "the destination node $leader never
       logged the owner-initiated transfer ('claimed by node $adopter and node $leader --
       owner-initiated transfer, applying' absent from $L_LOG after the step-11 lease
       acquire). The stale learned row (owner = node $adopter) was NOT out-resolved on the
       destination."
[ -n "$tr_obs" ] && [ -n "$lease_init_obs" ] && [ "$tr_obs" -gt "$lease_init_obs" ]     || fail "node $observer never logged the
       owner-initiated transfer for '$NAME' after the step-11 lease acquire. The transfer
       claim must reach and apply on every node."
echo "   owner-initiated transfer applied on node $leader and node $observer"
# Settle: the transfer's apply is RX-path (dirty flag, sweep flush). Give
# the sweep a beat before the kill so the destination's NVMe holds the
# TRANSFERRED row -- that is what makes the relaunch below deterministic.
sleep "$POLL"

# 11d. kill the destination MID-FLIGHT and relaunch it, exactly like step
# 10: same argv (node identity, NVMe disk, serial log all survive), same
# pid-file surgery. The destination must restore the TRANSFERRED row
# (owner = leader), never the stale learned row it resurrected in step 10.
D_PID="$NEW_L_PID"
kill -0 "$D_PID" 2>/dev/null || die "destination node $leader (pid $D_PID) is not running"
mapfile -t -d '' D_ARGV < "/proc/$D_PID/cmdline" \
    || die "cannot read /proc/$D_PID/cmdline"
[ "${#D_ARGV[@]}" -gt 3 ] || die "node $leader's argv came back with only ${#D_ARGV[@]} element(s)"
echo "==> SIGKILL node $leader (the migration destination, pid $D_PID) mid-flight"
kill -9 "$D_PID" 2>/dev/null || true
gone=0
for _ in $(seq 1 50); do
    kill -0 "$D_PID" 2>/dev/null || { gone=1; break; }
    sleep 0.1
done
[ "$gone" -eq 1 ] || die "node $leader (pid $D_PID) survived SIGKILL"
setsid "${D_ARGV[@]}" </dev/null >/dev/null 2>"$LOG_DIR/node$leader.stderr" &
NEW_D_PID=$!
echo "   relaunched as pid $NEW_D_PID"
awk -v n="$leader" -v p="$NEW_D_PID" \
    '$1==n {print n, p; next} {print}' "$PID_FILE" \
    > "$PID_FILE.new" && mv "$PID_FILE.new" "$PID_FILE"
echo "==> waiting for node $leader to come back up (up to ${WAIT_BOOT}s)"
up=0
deadline=$(( $(date +%s) + WAIT_BOOT ))
while :; do
    if get200 "$L_PORT" /api/cluster; then up=1; break; fi
    kill -0 "$NEW_D_PID" 2>/dev/null || die "node $leader died during relaunch boot. stderr:
$(sed 's/^/       /' "$LOG_DIR/node$leader.stderr" | tail -5)"
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$up" -eq 1 ] || die "relaunched node $leader never answered on port $L_PORT within ${WAIT_BOOT}s"
echo "   node $leader is back up"

# 11e. the destination must restore the TRANSFERRED row and the whole
# cluster must converge to owner = node $leader, with no flap back to the
# adopter.
echo "==> waiting for the destination to restore the transferred row and converge (up to ${WAIT_CONV}s)"
# SCOPING: the step-11 relaunch TRUNCATES the destination's serial log --
# QEMU's chardev logfile starts a fresh file on every process start, so
# the transfer line latched at 11c (previous generation) is gone by the
# time this boot's restore lands. Comparing line numbers across the
# truncation boundary is meaningless (a successful migrate once read as
# "never restored its row" that way on a live cluster). Scope the restore
# against THIS boot's own generation marker instead: kernel.c prints
# "[AEROSLS BOOT LOGGER V1.0.0 RUNNING]" as line 1 of every boot, so the
# current boot's restore must land after the LAST such marker. The
# convergence gates below then pin WHICH row was restored (the transferred
# one, owner = node $leader) on all three nodes.
restore2=""
deadline=$(( $(date +%s) + WAIT_CONV ))
while :; do
    boot_gen="$(last_line "$L_LOG" "AEROSLS BOOT LOGGER")"
    restore2="$(last_line "$L_LOG" "Partition ownership restored from NVMe")"
    [ -n "$restore2" ] && [ -n "$boot_gen" ] && [ "$restore2" -gt "$boot_gen" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$restore2" ] && [ -n "$boot_gen" ] && [ "$restore2" -gt "$boot_gen" ]     || fail "the relaunched destination node $leader never restored its row
       ('Partition ownership restored from NVMe' absent from $L_LOG within
       ${WAIT_CONV}s after this boot's 'AEROSLS BOOT LOGGER' marker). A step-11
       relaunch that loses the transferred row leaves the destination without
       the row it must converge on."
conv_l=0; conv_a=0; conv_o=0
deadline=$(( $(date +%s) + WAIT_CONV ))
while :; do
    [ "$conv_l" -eq 1 ] || { if get200 "$L_PORT" /api/partitions; then [ -n "$(row_if "$FETCH_BODY" "$NAME" "$leader")" ] && conv_l=1; fi; }
    [ "$conv_a" -eq 1 ] || { if get200 "$ADP_PORT" /api/partitions; then [ -n "$(row_if "$FETCH_BODY" "$NAME" "$leader")" ] && conv_a=1; fi; }
    [ "$conv_o" -eq 1 ] || { if get200 "$OBS_PORT" /api/partitions; then [ -n "$(row_if "$FETCH_BODY" "$NAME" "$leader")" ] && conv_o=1; fi; }
    [ "$conv_l" -eq 1 ] && [ "$conv_a" -eq 1 ] && [ "$conv_o" -eq 1 ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$conv_l" -eq 1 ] && [ "$conv_a" -eq 1 ] && [ "$conv_o" -eq 1 ] || fail "the cluster did not
       converge to owner = node $leader after the migrate + mid-flight relaunch within
       ${WAIT_CONV}s (node $leader:'${conv_l:-no}' node $adopter:'${conv_a:-no}' node
       $observer:'${conv_o:-no}'). The owner-initiated claim must out-resolve the
       resurrected destination's stale row on EVERY node -- a node that keeps the
       adopter's ownership is a split view."
echo "   all three nodes agree: owner = node $leader"

# no-flap: after the transfer, nothing may re-learn the row from the
# adopter. Scope per node against its own transfer-era line (the migrate
# summary on the adopter, the owner-initiated apply on the other two).
#
# The learn pattern is deliberately the SPECIFIC "(owner node $adopter)
# learned from node $adopter", not any "learned from node $adopter": the
# transfer itself is an announce FROM the adopter (owner = node $leader,
# source = node $adopter), so on a node whose row already matches it lands
# as a plain learn -- "(owner node $leader) learned from node $adopter" --
# AFTER the owner-initiated apply. Matching that would flag the transfer
# itself as the flap (a false positive seen live: the transfer learn sat
# one line after the apply and the guard reported FLAPPED on a converged
# cluster). A real flap is the ADOPTER'S OWNERSHIP coming back -- the
# adopter announcing owner = itself after handing the row off.
flap_a="$(last_line "$ADP_LOG" "PARTITION] sync.*$NAME.*(owner node $adopter) learned from node $adopter")"
flap_l="$(last_line "$L_LOG" "PARTITION] sync.*$NAME.*(owner node $adopter) learned from node $adopter")"
flap_o="$(last_line "$OBS_LOG" "PARTITION] sync.*$NAME.*(owner node $adopter) learned from node $adopter")"
tr_l="$(last_line "$L_LOG" "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying")"
tr_o="$(last_line "$OBS_LOG" "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying")"
if [ -n "$flap_a" ] && [ "$flap_a" -gt "$mig_line" ]; then
    fail "the migrated partition FLAPPED: node $adopter re-learned '$NAME' from itself
       (learn line at $flap_a, after the migrate at $mig_line)."
fi
if [ -n "$flap_l" ] && [ -n "$tr_l" ] && [ "$flap_l" -gt "$tr_l" ]; then
    fail "the migrated partition FLAPPED: the destination re-learned '$NAME' from the
       adopter (learn line at $flap_l, after the transfer at $tr_l)."
fi
if [ -n "$flap_o" ] && [ -n "$tr_o" ] && [ "$flap_o" -gt "$tr_o" ]; then
    fail "the migrated partition FLAPPED: node $observer re-learned '$NAME' from the
       adopter (learn line at $flap_o, after the transfer at $tr_o)."
fi

# 11f. the write lease after the dust settles: nobody holds it while the
# destination was down (runtime-only lease table -- a resurrected node has
# no row, and the source stepped down), and the new owner re-acquires it
# ONLY through a fresh 2-of-3 quorum.
for node in "$leader" "$adopter" "$observer"; do
    port=$((HTTP_BASE + node))
    if get200 "$port" /api/partitions && holds_lease_row "$FETCH_BODY" "$NAME"; then
        fail "node $node reports holds_lease=1 for '$NAME' after the migrate + relaunch --
       write permission survived the ownership transfer (or a resurrected node
       resurrected it). partition_holds_write_lease() must be false here until a
       fresh quorum election."
    fi
done
echo "   holds_lease=0 on all three nodes after the migrate + relaunch"

echo "==> re-acquiring the write lease on the new owner node $leader (fresh 2-of-3 quorum)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition lease acquire "$PART_ID" >/dev/null \
    || die "partition lease acquire $PART_ID failed on node $leader (aeroslsctl output above)"
newheld=0
deadline=$(( $(date +%s) + WAIT_LEASE ))
while :; do
    if get200 "$L_PORT" /api/partitions; then
        if holds_lease_row "$FETCH_BODY" "$NAME"; then newheld=1; break; fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$newheld" -eq 1 ] || fail "the new owner node $leader never re-acquired the write lease for
       '$NAME' within ${WAIT_LEASE}s. The migrated owner must be able to win a fresh quorum
       -- a lease the migration cannot re-establish leaves the partition read-only forever."
for node in "$adopter" "$observer"; do
    port=$((HTTP_BASE + node))
    if get200 "$port" /api/partitions && holds_lease_row "$FETCH_BODY" "$NAME"; then
        fail "node $node ALSO holds the write lease for '$NAME' after node $leader re-acquired
       it -- two lease holders is the page-level split-brain."
    fi
done
echo "   node $leader holds the write lease; nodes $adopter and $observer hold 0"

echo
echo
echo "PASS  node $leader was killed; node $adopter adopted '$NAME' from the held"
echo "      checkpoint, the owner handoff replicated to node $observer, and the"
echo "      loser stayed FOLLOWER without adopting. Relaunching node $leader"
echo "      mid-adoption: its stale re-announce was REJECTED by both survivors,"
echo "      the partition did not flap, and the leader converged to the new owner."
echo "      Then node $adopter migrated '$NAME' back to node $leader: the"
echo "      owner-initiated transfer out-resolved the destination's resurrected"
echo "      stale row on every node, the mid-flight relaunch of the destination"
echo "      restored the TRANSFERRED row, the cluster converged with no flap, the"
echo "      write lease was relinquished at the migrate (holds_lease 0 on all"
echo "      three nodes) and re-acquired on the new owner only by a fresh"
echo "      2-of-3 quorum."
echo "      evidence:"
printf '        %s\n' "$(grep -h "FAILOVER] Node $leader declared DEAD" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] Adopted partition.*dead node $leader" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] recovery for dead node $leader: rc=0" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "PARTITION] sync.*$NAME.*owner node $adopter.*learned from node $adopter" "$OBS_LOG" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] Node $leader declared DEAD" "$OBS_LOG" 2>/dev/null | tail -1 | sed 's/^/observer: /')"
printf '        %s\n' "$(grep -h "Partition ownership restored from NVMe" "$L_LOG" 2>/dev/null | tail -1 | sed 's/^/resurrected: /')"
printf '        %s\n' "$(grep -h "rejecting the stale claim" "$CAN_LOG" 2>/dev/null | tail -1 | sed 's/^/adopter: /')"
printf '        %s\n' "$(grep -h "rejecting the stale claim" "$OBS_LOG" 2>/dev/null | tail -1 | sed 's/^/observer: /')"
printf '        %s\n' "$(grep -h "PARTITION] sync.*$NAME.*learned from node $adopter" "$L_LOG" 2>/dev/null | tail -1 | sed 's/^/resurrected: /')"
printf '        %s\n' "$(grep -h "\[SERVICE\].*$SVC.*rejecting the stale claim" "$OBS_LOG" 2>/dev/null | tail -1 | sed 's/^/service: /')"
printf '        %s\n' "$(grep -h "PARTITION] migrated partition $PART_ID: node $adopter -> node $leader" "$ADP_LOG" 2>/dev/null | tail -1 | sed 's/^/migrate: /')"
printf '        %s\n' "$(grep -h "CONSENSUS] partition $PART_ID: node $adopter voluntarily stepped down from LEADER" "$ADP_LOG" 2>/dev/null | tail -1 | sed 's/^/lease: /')"
printf '        %s\n' "$(grep -h "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying" "$L_LOG" 2>/dev/null | tail -1 | sed 's/^/transfer on destination: /')"
printf '        %s\n' "$(grep -h "PARTITION] sync: partition $PART_ID claimed by node $adopter and node $leader -- owner-initiated transfer, applying" "$OBS_LOG" 2>/dev/null | tail -1 | sed 's/^/transfer on observer: /')"
printf '        %s\n' "$(grep -h "CONSENSUS] partition $PART_ID: quorum stable, node $leader elected LEADER (write lease)" "$L_LOG" 2>/dev/null | tail -1 | sed 's/^/re-acquired: /')"
exit 0
