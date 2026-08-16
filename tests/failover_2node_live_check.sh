#!/usr/bin/env bash
# tests/failover_2node_live_check.sh — in a 2-node cluster, when the leader
# dies the sole survivor must NOT become a second leader and must NOT adopt
# the dead leader's partitions.
#
# ─── Why the property is the NEGATIVE ─────────────────────────────────────
# The 3-node guard (failover_adoption_live_check.sh) asserts the positive:
# two survivors form a 2-of-2 quorum (majority of 3 is 2), one is elected
# LEADER, and it adopts the dead leader's partitions from the held
# checkpoint. This guard asserts the OTHER side of the same majority rule.
#
# The quorum is derived from the roster, never from liveness:
# cluster_recompute_quorum() (net/consensus.c) computes
#     stable_quorum_threshold = (active_nodes_count / 2) + 1
# and nothing in the kernel ever removes a dead peer from the roster --
# cluster_roster[i].active is set to 1 on registration and cleared only by
# cluster_init()'s full reset. A 2-node cluster therefore sits at
# active_nodes_count=2, stable_quorum_threshold=2 for its whole life.
#
# When the leader dies, the sole survivor has exactly one vote (its own)
# and can NEVER reach quorum 2. Its election times out (150+25*id ticks),
# it campaigns, accumulates 1 vote, and re-campaigns forever -- term
# climbing, role cycling CANDIDATE, never LEADER. And because
# failover_tick() only calls failover_recover_from() under
# cluster_is_leader(), the recovery path is gated off entirely: the
# survivor has the checkpoint, the partitions are adoptable, and it still
# must not -- and does not -- take them.
#
# That is the safety property, not a gap: the majority rule that lets two
# 3-node survivors elect a leader and adopt is the SAME rule that denies a
# lone 2-node survivor a quorum, so a 2-node cluster is one node-failure
# away from a read-only, no-leader state BY DESIGN. There is no window in
# which the survivor could unilaterally take over -- which is exactly what
# prevents a split-brain. A 2-node cluster trades availability (one node
# down = cluster stalls) for safety (never two owners).
#
# ─── What the guard gates on ──────────────────────────────────────────────
# The evidence pins the PATH, not just the outcome. The survivor must:
#   1. observe the death: `[FAILOVER] Node <L> declared DEAD` -- only
#      failover_tick() prints it, and it proves the failure detector ran
#      on the survivor (not that failover is inert);
#   2. hold a checkpoint carrying the row BEFORE the kill:
#      `[DSPP-CKPT] RX: COMPLETE` after the sync line -- so the refusal is
#      the quorum gate, not a missing checkpoint (adoption was
#      data-possible and still correctly refused);
#   3. NEVER report LEADER for the whole watch window -- the quorum gate.
#      FOLLOWER and CANDIDATE are both acceptable: the survivor is allowed
#      to keep campaigning forever, it is just never allowed to win;
#   4. NEVER print the Adopted line or a recovery rc= line AFTER its
#      declaration -- the cluster_is_leader() gate. A follower that
#      recovered is a unilateral takeover: two owners of one partition;
#   5. keep the partition owned by the DEAD leader in its list -- the
#      adoption never happened (owner unchanged, not this node).
#
# Fail modes, each with its own message: survivor flips to LEADER
# (split-brain -- a lone 2-node survivor must never be elected), survivor
# prints the Adopted line (recovery gate bypassed), survivor never
# declares the death (failover_tick inert), row adopted (owner = survivor),
# nolearn (create announce broken), nockpt (checkpoint pipeline broken).
#
# ─── What this guard does NOT do ──────────────────────────────────────────
# Exactly one node is killed and it is left dead. The operator stops the
# cluster afterwards (./run-cluster.sh --stop). It does not RELAUNCH the
# dead leader the way failover_adoption_live_check.sh now does -- there is
# no adoption here to protect (the survivor never adopts at all), and the
# resurrected-owner resolution itself is pinned by the 3-node guard's step
# 10, so relaunching would add nothing this guard is about.
#
# ─── Why it does not launch a cluster ─────────────────────────────────────
# This guard KILLS the leader of the cluster it runs against, so it must
# only ever touch a cluster its operator started deliberately -- the same
# contract as entropy_boot_diversity_check.sh and the 3-node failover
# guard. That is also what makes it safe to run from run_checks.sh: a
# runtime guard that booted a cluster of its own would do that in the
# middle of a deploy gate.
#
# ─── Usage ────────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (exactly 2)
#   tests/failover_2node_live_check.sh
#
# Environment:
#   AEROSLS_HTTP_BASE            port base (default 3000)
#   AEROSLS_TOKEN                bearer token (default: aeroslsctl's demo
#                                DB_ADMIN token, which the kernel accepts)
#   AEROSLS_FAILOVER_FAKE=1      test-only: allow killing a leader whose
#                                argv[0] is not QEMU (the smoke's fake
#                                nodes). Refused by default.
#   AEROSLS_FAILOVER_FAST=1      scale every wait down to seconds. Used by
#                                the smoke; never for a real cluster.
#   AEROSLS_LOG_DIR               where node<id>.log serial logs live
#                                (default: cluster/).
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no pid file, wrong node count).
#
# GUARD-KIND: runtime
#
# That marker is read by tests/run_checks.sh: this guard needs a RUNNING
# CLUSTER, which a build host and a deploy gate do not have, and
# --require-all deliberately must not force it. A green build gate is
# therefore NOT evidence that it passes -- the guard is reported as owed
# there, loudly, and this file is what you run against a started cluster.
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
NAME="guard-2node-$$"             # unique per run, so a stale persisted
                                  # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_CKPT=6; WAIT_OBSERVER=8
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_CKPT=60; WAIT_OBSERVER=90
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

# ─── 1. the cluster must exist with EXACTLY 2 nodes ───────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it. (The property being checked is
       that a 2-node cluster's sole survivor does NOT adopt -- the majority
       quorum of 2-of-2 cannot be met by one vote, so it can never be
       elected and failover recovery stays leader-gated off.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."
COUNT=$(echo "$NODE_IDS" | wc -w)
[ "$COUNT" -eq 2 ] || die "this guard needs EXACTLY 2 nodes: a leader to kill and
       the sole survivor that must not take over. $PID_FILE names $COUNT
       ($NODE_IDS). For the >= 3-node adoption property, use
       tests/failover_adoption_live_check.sh instead."

# ─── 2. wait for formation: one leader with quorum and one follower ───────
leader=""; survivor=""
echo "==> waiting for the 2-node cluster to form (up to ${WAIT_NODES}s)..."
deadline=$(( $(date +%s) + WAIT_NODES ))
while :; do
    up=""; leader=""; survivor=""
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
        [ "$id" != "$leader" ] && { [ -z "$survivor" ] && survivor="$id"; }
    done
    [ -n "$leader" ] && [ -n "$survivor" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$leader" ] || die "no leader emerged within ${WAIT_NODES}s. The nodes that
       answered:${up:- none}. Election needs a quorum -- are both nodes on the
       same segment?"
[ -n "$survivor" ] || die "no follower found -- the 2-node test needs a leader
       to kill and the sole survivor to watch. The nodes that
       answered:${up:- none}."
echo "   leader node $leader, sole survivor node $survivor"

L_PORT=$((HTTP_BASE + leader))
S_PORT=$((HTTP_BASE + survivor))
S_LOG="$LOG_DIR/node$survivor.log"

# ─── 3. create a fresh partition; the survivor must learn it ──────────────
echo "==> creating partition '$NAME' on node $leader (both nodes up)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"

echo "==> waiting for node $survivor to learn '$NAME' (up to ${WAIT_LEARN}s)"
learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$S_PORT" /api/partitions; then
        learned="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
    fi
    [ -n "$learned" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $survivor never learned partition '$NAME' from the
       create announce within ${WAIT_LEARN}s. DSPP_PARTITION_ANNOUNCE
       replication is broken -- the 2-node test cannot proceed."
echo "   $survivor learned $learned"

# ─── 4. the checkpoint must carry the row before the leader dies ──────────
# The refusal this guard asserts is the quorum gate, so we must first prove
# the OTHER prerequisites were in place: the survivor held a checkpoint
# carrying the row, i.e. adoption was data-possible and still refused.
echo "==> waiting for a checkpoint carrying '$NAME' to reach the survivor (up to ${WAIT_CKPT}s)"
ckpt_ok=0
deadline=$(( $(date +%s) + WAIT_CKPT ))
while :; do
    learn_line="$(last_line "$S_LOG" "PARTITION] sync.*$NAME")"
    ck="$(last_line "$S_LOG" "DSPP-CKPT] RX: COMPLETE")"
    if [ -n "$learn_line" ] && [ -n "$ck" ] && [ "$ck" -gt "$learn_line" ]; then
        ckpt_ok=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$ckpt_ok" -eq 1 ] || fail "no checkpoint carrying '$NAME' reached node $survivor
       within ${WAIT_CKPT}s ('DSPP-CKPT] RX: COMPLETE' must appear after the sync
       line in $S_LOG). Without it the held checkpoint predates the create, and
       the guard could not distinguish 'quorum gate refused adoption' from
       'nothing to adopt'."
echo "   checkpoint carrying '$NAME' held by the survivor"

# ─── 5. capture the leader's identity BEFORE killing it ───────────────────
L_PID="$(awk -v n="$leader" '$1==n{print $2}' "$PID_FILE")"
[ -n "$L_PID" ] || die "no pid for node $leader in $PID_FILE"
kill -0 "$L_PID" 2>/dev/null || die "node $leader (pid $L_PID) is not running"
if [ "$FAKE" != "1" ]; then
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

# ─── 7. the sole survivor must NOT take over ──────────────────────────────
# Hold the FULL window, exactly like the 3-node guard's loser watch: a
# late flip to LEADER, or a recovery that lands a moment after the death
# declaration, would otherwise slip past. Passes only if EVERY poll saw a
# non-LEADER role (FOLLOWER or CANDIDATE -- campaigning forever is allowed,
# winning is not), the death was declared, and no Adopted/recovery line
# ever appeared after the declaration. The row must also still be owned by
# the DEAD leader -- never by the survivor.
echo "==> watching node $survivor stay non-LEADER and never adopt (up to ${WAIT_OBSERVER}s)"
survivor_ok=1
survivor_why=""
decl_seen=0
deadline=$(( $(date +%s) + WAIT_OBSERVER ))
while :; do
    # 7a. role: must never be LEADER. FOLLOWER and CANDIDATE are both fine
    # -- the point is the survivor can never WIN. A transient fetch failure
    # does NOT fail the watch; a 200 that says LEADER does.
    if get200 "$S_PORT" /api/cluster; then
        r="$(jget "$FETCH_BODY" role)"
        [ "$r" != "LEADER" ] || { survivor_ok=0; survivor_why="flipped to LEADER (was $r)"; }
    fi
    # 7b. log: the survivor must have declared the death (it observed --
    # every node runs failover_tick, only the leader recovers), and must
    # NOT have printed an Adopted or recovery line AFTER that declaration.
    obs_decl="$(last_line "$S_LOG" "FAILOVER] Node $leader declared DEAD")"
    obs_adopt="$(last_line "$S_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
    obs_rec="$(last_line "$S_LOG" "FAILOVER] recovery for dead node $leader")"
    if [ -n "$obs_decl" ]; then
        decl_seen=1
        [ -z "$obs_adopt" ] || [ "$obs_adopt" -lt "$obs_decl" ] || { survivor_ok=0; survivor_why="printed its own Adopted line"; }
        [ -z "$obs_rec" ] || [ "$obs_rec" -lt "$obs_decl" ] || { survivor_ok=0; survivor_why="printed its own recovery line"; }
    fi
    # 7c. row: still owned by the DEAD leader, never by the survivor. If it
    # vanished that would also be wrong (a withdraw would need a mutation
    # from somewhere).
    [ "$survivor_ok" -eq 1 ] || { [ "$(date +%s)" -ge "$deadline" ] && break; sleep "$POLL"; continue; }
    if get200 "$S_PORT" /api/partitions; then
        if [ -n "$(row_if "$FETCH_BODY" "$NAME" "$survivor")" ]; then
            survivor_ok=0; survivor_why="serves the row with owner = itself (adopted)"
        fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$decl_seen" -eq 1 ] || { survivor_ok=0; survivor_why="never observed the death (no declaration in $S_LOG)"; }

if [ "$survivor_ok" -ne 1 ]; then
    # Which gate failed? Report the specific violation.
    if get200 "$S_PORT" /api/cluster && [ "$(jget "$FETCH_BODY" role)" = "LEADER" ]; then
        fail "node $survivor became LEADER after node $leader died -- a 2-node
       cluster's sole survivor must NEVER be elected: the majority quorum is
       2-of-2 and a lone survivor holds 1 vote, so reaching LEADER means the
       quorum rule was bypassed. A second leader is a split-brain: two owners
       of '$NAME'. Stop the cluster and inspect the consensus logs."
    fi
    obs_adopt="$(last_line "$S_LOG" "FAILOVER] Adopted partition.*dead node $leader")"
    obs_rec="$(last_line "$S_LOG" "FAILOVER] recovery for dead node $leader")"
    if [ -n "$obs_adopt" ] || [ -n "$obs_rec" ]; then
        fail "node $survivor ALSO recovered from dead node $leader -- a lone
       survivor must never adopt (failover_recover_from() is gated on
       cluster_is_leader(), and a 2-node survivor can never reach the 2-of-2
       quorum that would elect it). The survivor's log shows:
$(grep -h "FAILOVER] Adopted partition.*dead node $leader\|FAILOVER] recovery for dead node $leader" "$S_LOG" 2>/dev/null | tail -2 | sed 's/^/         /')
       A unilateral takeover is two owners of '$NAME'."
    fi
    if get200 "$S_PORT" /api/partitions && [ -n "$(row_if "$FETCH_BODY" "$NAME" "$survivor")" ]; then
        fail "node $survivor serves '$NAME' with owner = itself -- the adoption
       happened even though no Adopted line was logged. The quorum gate was
       bypassed end to end; '$NAME' now has two owners (the dead leader's
       persisted row and the survivor's)."
    fi
    fail "node $survivor neither stayed a non-leader nor observed the death
       cleanly (${survivor_why:-unknown}) within ${WAIT_OBSERVER}s. Inspect $S_LOG."
fi
echo "   node $survivor stayed non-LEADER and never adopted (observed the death only)"

echo
echo "PASS  node $leader (the leader of a 2-node cluster) was killed; the sole"
echo "      survivor node $survivor held the checkpoint carrying '$NAME', observed"
echo "      the death, and still neither became LEADER nor adopted -- the"
echo "      2-of-2 majority quorum cannot be met by one vote, so recovery"
echo "      stayed leader-gated off, exactly as designed. No split-brain."
echo "      evidence:"
printf '        %s\n' "$(grep -h "FAILOVER] Node $leader declared DEAD" "$S_LOG" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "DSPP-CKPT] RX: COMPLETE" "$S_LOG" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "PARTITION] sync.*$NAME" "$S_LOG" 2>/dev/null | tail -1)"
printf '        %s\n' "$(grep -h "FAILOVER] Adopted partition" "$S_LOG" 2>/dev/null | tail -1 | sed 's/^/adopted lines: /' || echo 'adopted lines: (none)')"
echo "      role now: $(get200 "$S_PORT" /api/cluster && jget "$FETCH_BODY" role)"
exit 0
