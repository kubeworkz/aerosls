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
#      row's new owner from the periodic/mutation announce.
#
# The evidence pins the PATH, not just the outcome. Four strings, each
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
#
# ─── What this guard does NOT do ──────────────────────────────────────────
# It does NOT relaunch the dead leader. A relaunched old leader restores its
# OWN local partition table from disk, where it still owns the adopted
# partition, and its periodic re-announce would claim the same id -- the
# kernel's "claimed by node X and node Y -- taking the newer" conflict path
# (partition_sync_upsert) logs the collision but does not resolve it, so the
# adoption could flap. The honest contract: the guard kills the leader and
# leaves it dead; the operator stops the cluster afterwards
# (./run-cluster.sh --stop).
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
# The guard will SIGKILL the leader it picks and leave it dead. It does not
# stop the cluster.
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
CLUSTER_DIR="cluster"
LOG_DIR="${AEROSLS_LOG_DIR:-$CLUSTER_DIR}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-failover-$$"             # unique per run, so a stale persisted
                                     # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_CKPT=6; WAIT_ADOPT=10; WAIT_HANDOFF=6; WAIT_OBSERVER=5
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_CKPT=60; WAIT_ADOPT=240; WAIT_HANDOFF=120; WAIT_OBSERVER=60
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
if [ "$FAKE" != "1" ]; then
    read -r -d '' _ < "/proc/$L_PID/cmdline" 2>/dev/null
    argv0="$(tr '\0' ' ' < "/proc/$L_PID/cmdline" 2>/dev/null | awk '{print $1}')"
    case "$argv0" in
        *qemu*) ;;
        *) die "pid $L_PID does not look like QEMU (argv[0]='${argv0:-<unreadable>}') --
       refusing to kill something unidentified." ;;
    esac
fi
echo "   node $leader = pid $L_PID"

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

echo
echo "PASS  node $leader was killed; node $adopter adopted '$NAME' from the held"
echo "      checkpoint, the owner handoff replicated to node $observer, and the"
echo "      loser stayed FOLLOWER without adopting."
echo "      evidence:"
printf '        %s\n' "$(grep -h "FAILOVER] Node $leader declared DEAD" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] Adopted partition.*dead node $leader" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] recovery for dead node $leader: rc=0" "$LOG_DIR/node$adopter.log" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "PARTITION] sync.*$NAME.*owner node $adopter.*learned from node $adopter" "$OBS_LOG" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] Node $leader declared DEAD" "$OBS_LOG" 2>/dev/null | tail -1 | sed 's/^/observer: /')"
exit 0
