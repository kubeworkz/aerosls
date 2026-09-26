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
# ─── The write-lease layer (Multi-Node Partition Scaling Roadmap Phase 4) ──
# The partition table is only HALF the ownership story. Writes are actually
# gated by a per-partition write LEASE: partition_holds_write_lease() is the
# exact check dspp_page_write_allowed() makes before a node may modify a
# partition's pages. In a 2-node cluster the lease quorum is the SAME 2-of-2
# majority as the election quorum (cluster_recompute_quorum() feeds both),
# so the same math that denies a lone survivor the leadership also denies it
# the lease -- with one extra, page-level guarantee worth pinning:
#
#   - the survivor must STRIP write permission when it starts campaigning
#     (update_page_table_permissions_for_partition(pid, 1), logged as
#     `[MMU-LEASE] partition <pid>: page permissions force_read_only=1`) --
#     reads-only must hold at the page-permission call site, not just in
#     the table;
#   - it must NEVER restore (force_read_only=0) -- a restore only fires on
#     lease quorum-achieved, which a lone 2-node survivor can never reach.
#
# The guard therefore acquires the write lease BEFORE the kill (the only
# live trigger is the shell `partition lease acquire <pid>`; a lease row is
# created by the first campaign), gates that the leader won it while both
# nodes were alive AND that the survivor learned the row (without a row,
# the strip gate below would be vacuous), then after the kill asserts
# partition_holds_write_lease() stays false (holds_lease=0 in /api/
# partitions the whole window) and the survivor's log shows the strip with
# never a restore.
#
# The row reaches the survivor over a packet hop, so the guard does not
# accuse the RX path off ONE bounded wait: it re-drives the campaign (up to
# AEROSLS_FAILOVER_LEASE_ATTEMPTS, each attempt a fresh WAIT_LEASE window)
# while the leader still holds the lease, and only then fails. A lost hop
# costs a retry; a broken RX path fails every attempt. See step 4.6.
#
# The same rule now covers the two PRE-KILL learn gates (steps 3 and 4),
# because an announce is a hop too. The create announce is broadcast ONCE
# (announce-on-change) and the checkpoint carrying the row is broadcast
# periodically, by the leader alone; a survivor that misses the one-shot
# announce waits for the owner's next periodic re-announce --
# partition_reannounce_tick() (kernel/partition.c, called from the BSP
# sweep) re-broadcasts the rows a node OWNS every 1000 ticks (~10 s), and
# there is NO command that fires it: user/shell.c has no announce command at
# all (its partition commands are create/list/assign/destroy/pause/resume/
# lease acquire/migrate plus quota bookkeeping, and none of them announces a
# row). So the guard's re-drive IS that period: each
# gate may wait out up to AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS fresh windows,
# and an extra window is spent only while the leader is still answering AND
# still holds the row -- the exact state the periodic re-announce (and the
# leader's checkpoint broadcast) needs to fire again, the same evidence-
# first rule step 4.6 follows for the lease. A lost announce costs a printed
# retry instead of a RED; a broken RX path fails every window.
#
# ─── What this guard does NOT do ─────────────────────────────────────────
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
#   AEROSLS_FAILOVER_LEASE_ATTEMPTS
#                                how many campaigns the lease-row gate may
#                                drive before it accuses the RX path
#                                (default 3; 1 = the pre-2026-09-25 single
#                                window, kept for the smoke's control tooth).
#   AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS
#                                how many periodic re-announce periods each
#                                of the two pre-kill learn gates (the create
#                                announce in step 3, the checkpoint in step
#                                4) may wait out before it accuses the RX
#                                path (default 3; 1 = the pre-change single
#                                window, kept for the smokes' control teeth).
#   AEROSLS_LOG_DIR               where node<id>.log serial logs live
#                                (default: cluster/).
#   AEROSLS_CLUSTER_DIR          where cluster.pids lives (default: cluster/,
#                                run-cluster.sh's). The smoke points this at
#                                a private temp dir, so it can run beside a
#                                live cluster without touching its pid file.
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
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
LOG_DIR="${AEROSLS_LOG_DIR:-$CLUSTER_DIR}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-2node-$$"             # unique per run, so a stale persisted
                                  # row can never be mistaken for it

if [ "$FAST" = "1" ]; then
    POLL=0.5; WAIT_NODES=10; WAIT_LEARN=6; WAIT_CKPT=6; WAIT_OBSERVER=8; WAIT_LEASE=6
else
    POLL=2;   WAIT_NODES=240; WAIT_LEARN=30; WAIT_CKPT=60; WAIT_OBSERVER=90; WAIT_LEASE=30
fi
# How many campaigns the lease-row gate may drive before it accuses the RX
# path (see step 4.6). One acquire is one campaign; a lost/slow hop costs a
# retry, a broken RX path (the nolearnlease tooth) eats every attempt and
# still fails the gate. AEROSLS_FAILOVER_LEASE_ATTEMPTS=1 restores the old
# single-window behaviour (the smoke's control tooth pins that).
LEASE_ATTEMPTS="${AEROSLS_FAILOVER_LEASE_ATTEMPTS:-3}"
# How many periodic re-announce periods the two pre-kill learn gates may
# wait out before they accuse the RX path (see steps 3 and 4). The re-drive
# is the kernel's OWN periodic re-announce / checkpoint broadcast -- no
# command fires either -- so an attempt is a fresh window, and it is spent
# only while the leader still answers and still holds the row. A lost
# announce costs a retry; a broken RX path (the nolearn/nockpt teeth) eats
# every attempt and still fails its gate.
# AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS=1 restores the old single-window
# behaviour (the smokes' learnlost1/ckptlost1 control teeth pin that).
ANNOUNCE_ATTEMPTS="${AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS:-3}"

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

holds_lease_of() {   # $1 = json, $2 = name -- prints 1/0 for the row
    python3 - "$1" "$2" <<'PY'
import json, sys
name = sys.argv[2]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name:
        print(p.get("holds_lease", "?"))
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
# The pre-kill gate, and an announce hop like the lease row below: the
# create announce is broadcast ONCE, so a single bounded wait cannot tell a
# lost/slow hop from a broken RX path. The re-drive is the kernel's own
# periodic re-announce (there is no command for it -- see the header), so
# each attempt is a fresh WAIT_LEARN window, admitted only while the leader
# is still answering AND still holds the row: the exact state
# partition_reannounce_tick() needs to re-broadcast. A genuinely broken RX
# path (the smoke's nolearn) fails every window and still fails the gate.
echo "==> creating partition '$NAME' on node $leader (both nodes up)"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" >/dev/null \
    || die "partition create failed on node $leader (aeroslsctl output above)"

echo "==> waiting for node $survivor to learn '$NAME' (up to ${WAIT_LEARN}s, up to ${ANNOUNCE_ATTEMPTS} re-announce period(s))"
learned=""
attempt=0
redrives=0
while :; do
    attempt=$((attempt + 1))
    deadline=$(( $(date +%s) + WAIT_LEARN ))
    while :; do
        if get200 "$S_PORT" /api/partitions; then
            learned="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
        fi
        [ -n "$learned" ] && break
        [ "$(date +%s)" -ge "$deadline" ] && break
        sleep "$POLL"
    done
    [ -n "$learned" ] && break
    [ "$attempt" -ge "$ANNOUNCE_ATTEMPTS" ] && break
    # Evidence first: the periodic re-announce can only fire for a row its
    # owner still holds, on a node that is still up. A leader that stopped
    # answering, or one that lost the row, is the announce path regressing
    # -- retrying that would paper over a real find, so the gate falls
    # through to its FAIL.
    if ! get200 "$L_PORT" /api/partitions; then
        echo "   note: node $leader is not answering /api/partitions; not waiting out another re-announce period"
        break
    fi
    if [ -z "$(row_if "$FETCH_BODY" "$NAME" "$leader")" ]; then
        echo "   note: node $leader no longer holds '$NAME'; not waiting out another re-announce period (the row's owner is the only re-announcer -- this is not a lost hop)"
        break
    fi
    echo "   note: '$NAME' has not reached $survivor after ${WAIT_LEARN}s -- the periodic re-announce is the re-drive; re-driving the announce (attempt $((attempt + 1)) of ${ANNOUNCE_ATTEMPTS})"
    redrives=$((redrives + 1))
done
if [ -z "$learned" ]; then
    # The FAIL must say which side of the hop is implicated, with the
    # evidence the gate actually collected -- never blame the RX path for a
    # walk it did not take.
    if [ "$redrives" -gt 0 ]; then
        hop_note="A lost/slow announce is ruled out: the gate waited out ${redrives} extra
       re-announce period(s) while node $leader still held '$NAME' -- so the survivor's
       announce RX was given fresh periodic broadcasts and still learned nothing. The
       announce RX path (partition_sync_upsert) is broken that way."
    elif [ "$ANNOUNCE_ATTEMPTS" -le 1 ]; then
        hop_note="The re-drive was disabled (AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS=$ANNOUNCE_ATTEMPTS),
       so the gate spent its single window and nothing more (the smoke's learnlost1
       control runs this shape to pin that the re-drive is what rescues a lost announce)."
    else
        hop_note="No extra re-announce period was waited out (the leader stopped answering, or
       stopped holding the row), so this may be the announce path rather than a lost hop."
    fi
    fail "node $survivor never learned partition '$NAME' from the
       create announce (no row owned by node $leader in its list after ${attempt}
       window(s) of ${WAIT_LEARN}s). DSPP_PARTITION_ANNOUNCE replication is
       broken -- the 2-node test cannot proceed. ${hop_note}"
fi
echo "   $survivor learned $learned"

# ─── 4. the checkpoint must carry the row before the leader dies ──────────
# The refusal this guard asserts is the quorum gate, so we must first prove
# the OTHER prerequisites were in place: the survivor held a checkpoint
# carrying the row, i.e. adoption was data-possible and still refused.
#
# The gate wants a `[DSPP-CKPT] RX: COMPLETE` AFTER the sync line, and only
# the leader broadcasts checkpoints (failover_live_checkpoint_broadcast,
# every 100 ticks), so a frame lost on the hop -- or a sync line that landed
# late, on the re-announce step 3 waited out -- can leave the gate on a
# window that has already spent itself. The re-drive is that same periodic
# broadcast: fresh WAIT_CKPT windows, admitted only while the leader is
# still answering AND still holds the row, exactly like step 3.
echo "==> waiting for a checkpoint carrying '$NAME' to reach the survivor (up to ${WAIT_CKPT}s, up to ${ANNOUNCE_ATTEMPTS} broadcast period(s))"
ckpt_ok=0
attempt=0
redrives=0
while :; do
    attempt=$((attempt + 1))
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
    [ "$ckpt_ok" -eq 1 ] && break
    [ "$attempt" -ge "$ANNOUNCE_ATTEMPTS" ] && break
    if ! get200 "$L_PORT" /api/partitions; then
        echo "   note: node $leader is not answering /api/partitions; not waiting out another checkpoint period"
        break
    fi
    if [ -z "$(row_if "$FETCH_BODY" "$NAME" "$leader")" ]; then
        echo "   note: node $leader no longer holds '$NAME'; not waiting out another checkpoint period (the leader is the only broadcaster -- this is not a lost frame)"
        break
    fi
    echo "   note: no checkpoint carrying '$NAME' on $survivor after ${WAIT_CKPT}s -- the leader's periodic broadcast is the re-drive; re-driving the checkpoint broadcast (attempt $((attempt + 1)) of ${ANNOUNCE_ATTEMPTS})"
    redrives=$((redrives + 1))
done
if [ "$ckpt_ok" -eq 0 ]; then
    if [ "$redrives" -gt 0 ]; then
        hop_note="A lost/slow frame is ruled out: the gate waited out ${redrives} extra broadcast
       period(s) while node $leader still held '$NAME' -- so fresh checkpoints went
       out and the survivor still logged none. The checkpoint RX path is broken that way."
    elif [ "$ANNOUNCE_ATTEMPTS" -le 1 ]; then
        hop_note="The re-drive was disabled (AEROSLS_FAILOVER_ANNOUNCE_ATTEMPTS=$ANNOUNCE_ATTEMPTS),
       so the gate spent its single window and nothing more (the smoke's ckptlost1
       control runs this shape to pin that the re-drive is what rescues a lost frame)."
    else
        hop_note="No extra broadcast period was waited out (the leader stopped answering, or
       stopped holding the row), so this may be the checkpoint path rather than a lost
       frame."
    fi
    fail "no checkpoint carrying '$NAME' reached node $survivor
       (no 'DSPP-CKPT] RX: COMPLETE' after the sync line in $S_LOG, after
       ${attempt} window(s) of ${WAIT_CKPT}s). Without it the held checkpoint
       predates the create, and the guard could not distinguish 'quorum gate
       refused adoption' from 'nothing to adopt'. ${hop_note}"
fi
echo "   checkpoint carrying '$NAME' held by the survivor"

# ─── 4.5 the write-lease layer must be LIVE and CONTESTED before the kill ─
# The page-permission gate (step 7d/7e below) is only meaningful if the
# partition actually HAS a write lease that can be contested. A lease row
# is created by the FIRST campaign; the only live trigger is the shell
# `partition lease acquire <pid>`. So, before the kill, the leader acquires
# the lease (winning the 2-of-2 quorum while BOTH nodes are alive -- the
# same quorum that will deny the survivor after the kill), and the guard
# gates that BOTH sides went live:
#   - leader holds_lease=1 in /api/partitions: partition_holds_write_lease()
#     returned true -- the exact gate dspp_page_write_allowed() checks;
#   - survivor logged `partition <pid> lease initialised`: it RXed the
#     campaign and created its own row. Without a row the strip gate would
#     be vacuous (no row = no campaign = no strip and no restore), so the
#     guard must prove the row exists before it may trust the absence of a
#     restore.
echo "==> acquiring the write lease for '$NAME' on node $leader"
PART_ID="$(get200 "$L_PORT" /api/partitions && row_id "$FETCH_BODY" "$NAME")"
[ -n "$PART_ID" ] || die "could not read the partition id for '$NAME' from node $leader"
echo "   partition '$NAME' is id $PART_ID"
"$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition lease acquire "$PART_ID" >/dev/null \
    || die "partition lease acquire $PART_ID failed on node $leader (shell command output above)"

echo "==> waiting for node $leader to HOLD the write lease (up to ${WAIT_LEARN}s)"
held=0
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$L_PORT" /api/partitions; then
        if python3 - "$FETCH_BODY" "$NAME" <<'PY'
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
        then held=1; break; fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$held" -eq 1 ] || fail "node $leader never HOLD the write lease for '$NAME' within
       ${WAIT_LEARN}s (holds_lease stayed 0 in /api/partitions). The lease
       campaign never won -- but with both nodes alive the 2-of-2 quorum IS
       reachable, so a no-win means the lease layer is broken (not the
       quorum gate): either `partition lease acquire` is inert or the vote
       exchange never completed. A guard that cannot make the lease layer
       live cannot assert it stays read-only."
echo "   node $leader holds the write lease for '$NAME'"

# The row is created by the survivor's RX path when the leader's campaign
# LANDS -- a packet hop, not a function call, so it can be delayed or lost
# independently of the code under test. A single bounded wait cannot tell a
# lost hop from a broken RX path, and on 2026-09-25 in CI it did not: the
# plain staysfollower arm and the leaserestore arm both failed here with
# "never created a lease row" on a loaded runner while every other arm in
# the same run -- and both arms on the re-run -- passed. That false RED
# blamed process_partition_consensus_packet's find-or-create for a hop that
# had not landed yet (the fakes had a real bug here too: a lease row caught
# mid-write killed the survivor's RX thread, fixed in
# failover_2node_smoke_nodes.py). So the gate now RE-DRIVES the campaign:
# each attempt is a fresh WAIT_LEASE window, and a re-drive is admitted
# only while the leader is answering AND still holds the write lease -- the
# exact state the first acquire won in, so the re-drive is the same
# trigger, not a new scenario (the shell `partition lease acquire` is
# documented as the only live trigger, and partition_lease_trigger_
# election creates the row if missing and re-campaigns). A hop that is
# merely lost/slow now costs a printed retry instead of a RED.
#
# The tooth is intact: a genuinely broken RX path (nolearnlease) never
# creates the row, so every attempt fails it -- and the FAIL below still
# says `never created a lease row`. The smoke pins both sides of that:
# leaselost1->PASS (the first campaign is dropped on the wire: the re-drive
# must rescue it) and the leaselost1 attempts=1 control->FAIL (with the
# re-drive disabled the same loss is still the gate's honest RED).
echo "==> waiting for node $survivor to LEARN the lease row (up to ${WAIT_LEASE}s, up to ${LEASE_ATTEMPTS} campaign attempt(s))"
lease_init=""
attempt=0
redrives=0
while :; do
    attempt=$((attempt + 1))
    deadline=$(( $(date +%s) + WAIT_LEASE ))
    while :; do
        lease_init="$(last_line "$S_LOG" "CONSENSUS] partition $PART_ID lease initialised")"
        [ -n "$lease_init" ] && break
        [ "$(date +%s)" -ge "$deadline" ] && break
        sleep "$POLL"
    done
    [ -n "$lease_init" ] && break
    [ "$attempt" -ge "$LEASE_ATTEMPTS" ] && break
    # Evidence first: re-drive only while the acquire that won can be
    # repeated. A leader that stopped answering, or one that lost the
    # leash it just held, is the lease layer regressing -- retrying that
    # would paper over a real find, so the gate falls through to its FAIL.
    if ! get200 "$L_PORT" /api/partitions; then
        echo "   note: node $leader is not answering /api/partitions; not re-driving the campaign"
        break
    fi
    if [ "$(holds_lease_of "$FETCH_BODY" "$NAME")" != "1" ]; then
        echo "   note: node $leader no longer holds the write lease; not re-driving (the lease layer regressed -- this is not a lost hop)"
        break
    fi
    echo "   note: no lease row on $survivor after ${WAIT_LEASE}s -- re-driving the campaign (attempt $((attempt + 1)) of ${LEASE_ATTEMPTS})"
    redrives=$((redrives + 1))
    "$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
        shell partition lease acquire "$PART_ID" >/dev/null \
        || echo "   note: the re-acquire returned non-zero; spending the remaining attempt(s) on the wait anyway"
done
if [ -z "$lease_init" ]; then
    # The FAIL must say which side of the hop is implicated, with the
    # evidence the gate actually collected -- never blame the RX path for a
    # walk it did not take.
    if [ "$redrives" -gt 0 ]; then
        hop_note="A lost/slow hop is ruled out: the campaign was re-driven ${redrives} time(s),
       each time with the leader still holding the write lease -- so the
       survivor's RX side was given fresh campaigns and still created no row.
       The leader's REQUEST_VOTE RX path (process_partition_consensus_packet's
       find-or-create) is broken that way."
    elif [ "$LEASE_ATTEMPTS" -le 1 ]; then
        hop_note="The re-drive was disabled (AEROSLS_FAILOVER_LEASE_ATTEMPTS=$LEASE_ATTEMPTS),
       so the gate spent its single window and nothing more (the smoke's
       leaselost1 control runs this shape to pin that the re-drive is what
       rescues a lost hop)."
    else
        hop_note="The campaign was NOT re-driven (the leader stopped answering, or stopped
       holding the lease), so this may be the lease layer rather than the RX hop."
    fi
    if get200 "$S_PORT" /api/health; then
        s_note="The survivor's HTTP server is answering, so the node is up and serving."
    else
        s_note="The survivor's HTTP server is NOT answering either -- suspect the node, not the RX path."
    fi
    fail "node $survivor never created a lease row for partition
       $PART_ID (no 'partition $PART_ID lease initialised' line in $S_LOG after
       ${attempt} campaign attempt(s) of ${WAIT_LEASE}s each). Without a row the
       survivor has nothing to contest -- the strip gate below would be vacuous,
       so the guard refuses to run blind. ${hop_note} ${s_note}"
fi
echo "   $survivor learned the lease row: $(sed -n "${lease_init}p" "$S_LOG" | sed 's/^/      /')"

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
        # 7d. write lease: partition_holds_write_lease() must stay FALSE on
        # the survivor the whole window. holds_lease is the raw output of
        # that function in /api/partitions -- the exact gate
        # dspp_page_write_allowed() checks before a page write. The
        # partition-table check above (7c) pins who OWNS the row; this pins
        # whether the survivor could actually WRITE it. A survivor that
        # keeps ownership but gains the lease is the page-level split-brain
        # this gate exists to catch.
        if python3 - "$FETCH_BODY" "$NAME" <<'PY'
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
        then
            survivor_ok=0; survivor_why="reports holds_lease=1 (partition_holds_write_lease() true)"
        fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$decl_seen" -eq 1 ] || { survivor_ok=0; survivor_why="never observed the death (no declaration in $S_LOG)"; }

# 7e. the page-permission level: the survivor must have STRIPPED write
# permission when it started campaigning ([MMU-LEASE] force_read_only=1)
# and must NEVER have restored it (=0). The strip is logged by
# update_page_table_permissions_for_partition() on every campaign start;
# the restore only fires on lease quorum-achieved, unreachable for a lone
# 2-node survivor. A survivor with the strip but a restore is the page-
# level split-brain; a survivor with NEITHER means the lease layer never
# engaged (vacuous -- caught earlier at the lease-learn gate, but the
# strip is re-checked here against the final log).
strip_n=$(grep -hc "MMU-LEASE] partition $PART_ID: page permissions force_read_only=1" "$S_LOG" 2>/dev/null || true)
restore_n=$(grep -hc "MMU-LEASE] partition $PART_ID: page permissions force_read_only=0" "$S_LOG" 2>/dev/null || true)
if [ "$strip_n" -eq 0 ]; then
    survivor_ok=0; survivor_why="never stripped write permission (no [MMU-LEASE] force_read_only=1 line for partition $PART_ID)"
elif [ "$restore_n" -gt 0 ]; then
    survivor_ok=0; survivor_why="RESTORED write permission after campaigning ([MMU-LEASE] force_read_only=0 line for partition $PART_ID)"
fi

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
    if [ "${survivor_why:-}" = "reports holds_lease=1 (partition_holds_write_lease() true)" ]; then
        fail "node $survivor reports holds_lease=1 for '$NAME' after node $leader died
       -- partition_holds_write_lease() came back TRUE, which is the exact
       check dspp_page_write_allowed() makes. A lone 2-node survivor can
       never reach the 2-of-2 lease quorum, so holding the lease means the
       lease layer was bypassed: writes to '$NAME' would be allowed on a
       node that does not own it. This is the page-permission split-brain."
    fi
    if printf '%s' "${survivor_why:-}" | grep -q "force_read_only=0"; then
        fail "node $survivor RESTORED write permission for partition $PART_ID after
       campaigning (a [MMU-LEASE] force_read_only=0 line is in $S_LOG). The
       restore only fires on lease quorum-achieved, which a lone 2-node
       survivor can never reach -- so this is the page-permission layer
       re-enabling writes without a lease. The survivor correctly stripped
       on campaign, then illegally restored:
$(grep -h "MMU-LEASE] partition $PART_ID" "$S_LOG" 2>/dev/null | tail -3 | sed 's/^/         /')"
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
echo "      write lease: holds_lease=$(get200 "$S_PORT" /api/partitions && holds_lease_of "$FETCH_BODY" "$NAME")"
printf '        %s\n' "$(grep -h "MMU-LEASE] partition $PART_ID" "$S_LOG" 2>/dev/null | tail -2 | sed 's/^/mmu-lease: /' || echo 'mmu-lease: (none)')"
echo "      role now: $(get200 "$S_PORT" /api/cluster && jget "$FETCH_BODY" role)"
exit 0
