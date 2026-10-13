#!/usr/bin/env bash
# tests/env_migrate_confirm_live_check.sh — the confirmation round trip,
# LIVE: when a partition carrying a real environment migrates, the
# destination rebuilds it from the wire, and the source tears its half
# down ONLY on the destination's own word — so no record on the source
# describes an environment that moved.
#
# ─── Why this is the gate that matters ────────────────────────────────
# POSIX-Environments P3 moved the boundary in three increments: the
# payload before the ownership handoff (3), the destination rebuilding
# the environment from the staged slot (4), and the source letting go
# only when the destination says it is LIVE WITH ITS CONTENTS there (5:
# LIVE_REQ/LIVE_ACK on the sweep's period, teardown by name, the
# deferred frame reclamation). tests/env_migrate_host_test.c (Phases
# A–H) proves all three against the REAL wire encode/decode, the REAL
# record layer and two simulated nodes in one process. What no host
# test can see is the whole path live: two booted kernels on one L2
# segment, two inits, two NVMe disks, real sidecar create/kill round
# trips on both ends, and the BSP sweep actually driving the poll and
# the teardown. This guard is that arm — the same live-cluster gap the
# roadmap addendum names as "deliberately NOT here".
#
# ─── The evidence, each string from exactly one code site ─────────────
#   * `[ENV] create partition=.. index=.. -> ok env_id=..` — the shell's
#     E6 create (user/shell.c): the environment really existed on the
#     source before the migrate, so nothing below can be vacuous.
#   * `1 environment(s) sent and confirmed by the destination` AND
#     `frames are NOT reclaimed yet` in the migrate RESPONSE —
#     partition_migrate(): Step 3c moved the environment and Step 5
#     deferred its reclamation (the deferral line is emitted ONLY when
#     envs_moved > 0, so it cannot appear on an empty migrate).
#   * `[ENV_MIGRATE] restore: env (partition .., index ..) is LIVE on
#     node <dest>` — env_migrate.c's restore success tail, only after
#     the pour completed WITH CONTENTS (and it is the line that sets
#     the restored marker the destination's word rests on).
#   * `[ENV_MIGRATE] teardown (partition .., index ..): node <dest>
#     confirmed the restore, so env .. is ended here` — the
#     confirmation walk's destroy-OK branch: the teardown ran on the
#     destination's WORD, not on the bytes having been sent.
#   * `[ENV_MIGRATE] teardown (partition ..): every environment is
#     confirmed gone from this node -- the deferred reclamation
#     completes` — the walk completing what Step 5 deferred, and only
#     after the records are gone.
#   * `[ENV] list partition=.. live=0` on the source with `live=1` (the
#     same env) on the destination — the operator-visible half of "no
#     records describe an environment that moved": a destroyed
#     environment's console unregisters when its sidecar peer dies
#     (kernel/env_console.h), so the source's registry must empty.
#
# ─── Why it does not launch a cluster ─────────────────────────────────
# The same contract as partition_reannounce_live_check.sh: this guard
# drives (and never kills) its cluster's nodes, so it only ever touches
# a cluster its operator started deliberately, and it is safe to run
# beside a deploy gate. It cleans up after itself (env + partition
# destroyed on the destination) and leaves the cluster running.
#
# ─── Usage ────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (TCG is fine)
#   tests/env_migrate_confirm_live_check.sh
# Two nodes suffice and that is deliberate: the migrate needs both ends
# alive, and nothing here needs a third (adoption's 3-of-3 quorum is a
# different property with its own guard).
#
# Environment:
#   AEROSLS_HTTP_BASE     port base (default 3000)
#   AEROSLS_TOKEN         bearer token (default: aeroslsctl's demo
#                         DB_ADMIN token, which the kernel accepts)
#   AEROSLS_CLUSTER_DIR   where cluster.pids and node<id>.log live
#                         (default: cluster/, run-cluster.sh's)
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no leader formed, the environment never
# existed on the source).
#
# GUARD-KIND: runtime
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-envconfirm-$$"           # unique per run, never stale

POLL=2
WAIT_NODES=300      # TCG boot + election, same generosity as the reannounce guard
WAIT_LEARN=30       # the create announce hop
WAIT_RESTORE=150    # the destination's sweep period (~10s at 1000 ticks) + create RTT
WAIT_TEARDOWN=180   # the source's poll + destroy RTT + reclamation
WAIT_ENVLIST=60     # the console registry emptying on the source

die()  { echo; echo "ABORT: $*" >&2; exit 2; }
fail() { echo; echo "FAIL  $*"; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 is needed to parse the /api JSON."
command -v curl    >/dev/null 2>&1 || die "curl not found."
[ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

TOKEN_ARGS=()
[ -n "$TOKEN" ] && TOKEN_ARGS=(--token "$TOKEN")
CURL_EXTRA=()
[ -n "$TOKEN" ] && CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")

# ─── transport: curl with a bound, like the reannounce guard ──────────
# A down QEMU hostfwd port accepts the TCP connect and then times out,
# so every probe needs --max-time. FETCH_STATUS/BODY/RC carry the result
# for the caller's poll loop and for the FAIL diagnostics.
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

jget() {   # $1 = json, $2 = field
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    d = {}
print(d.get(sys.argv[2], ""))
PY
}

row_if() {   # $1 = json, $2 = name, $3 = owner -> "partition <id> '<name>' (owner node <o>)"
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

dump_rows() {   # $1 = json -- every row, for the FAIL diagnostics
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

# new_bytes <file> <offset> -- only what the file gained since the offset,
# so every log assertion below is scoped to THIS migrate and cannot pass
# on a line some earlier run left behind.
new_bytes() {
    tail -c "+$(( $2 + 1 ))" "$1" 2>/dev/null
}

# ─── 1. the cluster must exist ────────────────────────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it. (The confirmation round trip
       needs two live kernels: one to migrate FROM, one that must say the
       environment is live with its contents THERE before the source may
       let go.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."

# ─── 2. wait for formation: a leader with quorum and one other node ───
leader=""; other=""
echo "==> waiting for the cluster to form (up to ${WAIT_NODES}s)..."
deadline=$(( $(date +%s) + WAIT_NODES ))
while :; do
    up=""; leader=""; other=""
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
        [ "$id" != "$leader" ] && [ -z "$other" ] && other="$id"
    done
    [ -n "$leader" ] && [ -n "$other" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$leader" ] || die "no leader emerged within ${WAIT_NODES}s. The nodes that
       answered:${up:- none}. Election needs a quorum -- are all nodes on
       the same segment?"
[ -n "$other" ] || die "no second node found -- the migrate needs a live
       destination. Start a bigger cluster."
echo "   source node $leader, destination node $other"

L_PORT=$((HTTP_BASE + leader))
D_PORT=$((HTTP_BASE + other))
ADP_LOG="$CLUSTER_DIR/node$leader.log"
DST_LOG="$CLUSTER_DIR/node$other.log"
[ -f "$ADP_LOG" ] || die "$ADP_LOG not found -- $CLUSTER_DIR is not this cluster's dir (AEROSLS_CLUSTER_DIR)."
[ -f "$DST_LOG" ] || die "$DST_LOG not found."

# ─── 3. create the partition on the source; the destination learns it ─
echo "==> creating partition '$NAME' on node $leader"
create_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" 2>&1)" \
    || die "partition create failed on node $leader (aeroslsctl output above)"
case "$create_resp" in
    *"(FAILED)"*) die "partition create failed on node $leader: $create_resp" ;;
esac
PART_ID="$(printf '%s' "$create_resp" | sed -n "s/.*create '$NAME' -> id=\([0-9]*\).*/\1/p" | head -1)"
[ -n "$PART_ID" ] || die "could not parse the created partition id from: $(printf '%s' "$create_resp" | tail -2)"
echo "   partition id $PART_ID"

echo "==> waiting for node $other to learn partition $PART_ID (up to ${WAIT_LEARN}s)"
learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$D_PORT" /api/partitions; then
        learned="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
        [ -n "$learned" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $other never learned partition $PART_ID ('$NAME') from the
       create announce within ${WAIT_LEARN}s -- DSPP announce replication is
       broken and the migrate below would race an empty destination table.
       Its list held:
$(dump_rows "$FETCH_BODY")"
echo "   $learned"

# ─── 4. create the environment on the source ──────────────────────────
# The pre-gate for everything below: an env create that fails here would
# make every later "the environment moved" line vacuous.
echo "==> creating an environment in partition $PART_ID on node $leader"
env_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell env create "$PART_ID" 2>&1)" \
    || die "env create failed on node $leader (aeroslsctl output above)"
case "$env_resp" in
    *"-> ok env_id="*)
        ENV_ID="$(printf '%s' "$env_resp" | sed -n 's/.*ok env_id=\([0-9]*\).*/\1/p' | head -1)"
        ;;
    *)  die "the environment was not created on node $leader (no '-> ok env_id=' in the
       response): $(printf '%s' "$env_resp" | tail -5 | sed 's/^/       /')
       Without a live environment on the source there is no confirmation
       round trip to prove." ;;
esac
echo "   environment env_id=$ENV_ID lives in partition $PART_ID on node $leader"

echo "==> waiting for the environment's console to register on the source"
live_src=""
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_src="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_src" in
        *"env partition=$PART_ID env_id=$ENV_ID"*) live_src=1; break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$live_src" ] || die "the environment's console never registered on node $leader
       ('[ENV] env partition=$PART_ID env_id=$ENV_ID' absent from env list after
       ${WAIT_ENVLIST}s). env list reads the kernel's console registry; without a
       registered console the env-list half of the evidence cannot be
       produced, and this guard refuses to pass on log lines alone when it
       promised the operator-visible half too. Last output:
$(printf '%s' "$list_src" | tail -3 | sed 's/^/       /')"
echo "   the source lists env_id=$ENV_ID as live"

# ─── 5. migrate, and capture the pre-state of both logs ───────────────
ADP_OFF="$(wc -c < "$ADP_LOG")"
DST_OFF="$(wc -c < "$DST_LOG")"
echo "==> migrating partition $PART_ID from node $leader to node $other"
# --timeout 600: a live environment's payload is ~1400 pages, each 4 wire
# fragments with an ACK round trip; on TCG that is minutes, and
# aeroslsctl's default 15s would abandon the WAIT while the migrate is
# still running server-side (the command would then read as a failure
# that never was).
mig_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" --timeout 600 \
    shell partition migrate "$PART_ID" "$other" 2>&1)" \
    || die "partition migrate $PART_ID -> $other failed on node $leader (aeroslsctl output above)"

case "$mig_resp" in
    *"migrated partition $PART_ID: node $leader -> node $other"*) ;;
    *) fail "the migrate never ran on node $leader (the success line is absent
       from the response): $(printf '%s' "$mig_resp" | tail -3 | sed 's/^/       /')" ;;
esac
case "$mig_resp" in
    *"1 environment(s) sent and confirmed by the destination"*) ;;
    *) fail "the migrate response does not report the environment as confirmed by
       the destination (no '1 environment(s) sent and confirmed by the
       destination'). The payload step did not move the environment:
$(printf '%s' "$mig_resp" | grep -m1 'environment(s)' | sed 's/^/       /')" ;;
esac
case "$mig_resp" in
    *"frames are NOT reclaimed yet"*) ;;
    *) fail "the migrate response does not carry the Step-5 deferral ('frames are
       NOT reclaimed yet'). With an environment moved, the source's frames
       MUST stay held until the destination's word -- a migrate that
       reclaimed them immediately would put two owners on one frame." ;;
esac
echo "   migrate confirmed: environment moved, reclamation deferred on the source"

# ─── 6. the destination rebuilt it (the restore line, its own log) ────
echo "==> waiting for node $other to restore the environment (up to ${WAIT_RESTORE}s; the sweep period is ~10s)"
restore_line=""
deadline=$(( $(date +%s) + WAIT_RESTORE ))
while :; do
    restore_line="$(new_bytes "$DST_LOG" "$DST_OFF" \
        | grep -F "[ENV_MIGRATE] restore: env (partition $PART_ID, index 1) is LIVE on node $other" \
        | tail -1)"
    [ -n "$restore_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$restore_line" ] || fail "node $other never logged the restore of (partition $PART_ID,
       index 1) within ${WAIT_RESTORE}s. The staged bytes crossed (the migrate was
       byte-confirmed) but the destination's sweep never poured them into a
       live environment. The destination's new log bytes hold:
$(new_bytes "$DST_LOG" "$DST_OFF" | grep -F "ENV_MIGRATE" | tail -8 | sed 's/^/       /')"
echo "   destination rebuilt it: $(printf '%s' "$restore_line" | sed 's/^ *//')"

# ─── 7. the source tore down ONLY on that word ────────────────────────
echo "==> waiting for the source's confirmation teardown (up to ${WAIT_TEARDOWN}s)"
teardown_line=""
deadline=$(( $(date +%s) + WAIT_TEARDOWN ))
while :; do
    teardown_line="$(new_bytes "$ADP_LOG" "$ADP_OFF" \
        | grep -F "[ENV_MIGRATE] teardown (partition $PART_ID, index 1): node $other confirmed the restore, so env $ENV_ID is ended here" \
        | tail -1)"
    [ -n "$teardown_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$teardown_line" ] || fail "the source node $leader never ended env $ENV_ID of partition
       $PART_ID within ${WAIT_TEARDOWN}s of the migrate (no 'node $other confirmed the
       restore' teardown line). Either the LIVE poll never reached the
       destination or the word never came back -- in both cases the source
       still DESCRIBES an environment that moved, which is the defect this
       guard exists to catch. env_migrate_pending_count() would name it; the
       source's new log bytes hold:
$(new_bytes "$ADP_LOG" "$ADP_OFF" | grep -F "ENV_MIGRATE" | tail -10 | sed 's/^/       /')"
echo "   source ended it on the word: $(printf '%s' "$teardown_line" | sed 's/^ *//')"

reclaim_line=""
deadline=$(( $(date +%s) + WAIT_TEARDOWN ))
while :; do
    reclaim_line="$(new_bytes "$ADP_LOG" "$ADP_OFF" \
        | grep -F "[ENV_MIGRATE] teardown (partition $PART_ID): every environment is confirmed gone from this node" \
        | tail -1)"
    [ -n "$reclaim_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$reclaim_line" ] || fail "the source never completed the deferred reclamation for
       partition $PART_ID (no 'the deferred reclamation completes' line within
       ${WAIT_TEARDOWN}s of the teardown). The records are gone but the frames are
       still held -- Step 5's deferral never got its other half. The source's
       new log bytes hold:
$(new_bytes "$ADP_LOG" "$ADP_OFF" | grep -F "ENV_MIGRATE" | tail -10 | sed 's/^/       /')"
echo "   deferred reclamation completed: $(printf '%s' "$reclaim_line" | sed 's/^ *//')"

# ─── 8. the operator-visible half: source empty, destination listing ──
echo "==> checking the console registries (source must empty, destination must list it)"
list_dst=""
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_dst="$("$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_dst" in
        *"env partition=$PART_ID env_id="*) break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
case "$list_dst" in
    *"env partition=$PART_ID env_id="*)
        echo "   destination lists the rebuilt environment: $(printf '%s' "$list_dst" | grep -m1 "env partition=$PART_ID" | sed 's/^ *//')"
        ;;
    *)  fail "node $other restored the environment and then lost it: 'env list $PART_ID'
       shows no environment there within ${WAIT_ENVLIST}s. The rebuilt environment is
       not reachable. Its output:
$(printf '%s' "$list_dst" | tail -3 | sed 's/^/       /')" ;;
esac

src_listed=1
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_src="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_src" in
        *"env partition=$PART_ID env_id="*) src_listed=0 ;;
        *) src_listed=1; break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$src_listed" -eq 1 ] || fail "the source node $leader STILL lists an environment for
       partition $PART_ID after its teardown completed ('env list' output:
$(printf '%s' "$list_src" | grep "env partition=" | sed 's/^/       /')). The environment
       moved and was ended here, yet its console registry entry survives --
       a record that describes an environment that moved, at the
       operator-visible level."
echo "   the source lists nothing for partition $PART_ID -- no record of it remains"

# ─── 9. cleanup: the migrated pair belongs to the destination now ─────
echo "==> cleaning up: destroying env $ENV_ID and partition $PART_ID on node $other"
"$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" \
    shell env destroy "$PART_ID" "$ENV_ID" >/dev/null 2>&1 || true
"$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" \
    shell partition destroy "$PART_ID" >/dev/null 2>&1 || true

echo
echo "PASS  the confirmation round trip is proven live on a real $(( $(echo "$NODE_IDS" | wc -w) ))-node cluster:"
echo "      node $other rebuilt env $ENV_ID from the wire (restore -> LIVE), and node $leader"
echo "      ended it ONLY on node $other's word (teardown 'confirmed the restore'), then"
echo "      completed the deferred reclamation. The source no longer describes the"
echo "      environment that moved."
exit 0
