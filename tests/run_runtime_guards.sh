#!/usr/bin/env bash
# One-shot: run the runtime guards that run_checks.sh owed, against the
# already-running 3-node cluster. Sequential (they share the cluster), token
# supplied, each exit status recorded.
#
# ─── the warm-up gate, before guard 1 ───────────────────────────────────
# WHY THIS EXISTS, verbatim from what happened: on the first run after a
# clean relaunch (fresh scratch disks) three guards — env_migrate_confirm,
# failover_adoption, env_catalog — all died at their first `env create`,
# which got NO reply from the environment manager, while /api/cluster was
# already answering 200 and the kernel was healthy. Fresh-disk boot is
# saturated for its first minutes (a full checkpoint over the whole dirty
# mask plus partition-sync storms) and init's sidecar misses the shell
# client's deadline; a warm re-run of the same three guards minutes later
# was green. So the window is real, it is transient, and it is not a guard
# defect — which means it belongs HERE: spend it before guard 1 instead of
# letting three guards report it three times.
#
# The gate retries one partition create on the leader and one `env create`
# on EVERY node (each node's manager is its own; every guard that creates
# an environment targets the leader at that moment, and the adoption guard
# kills that leader mid-suite, handing the later guards a former follower
# — the probe that settled this: a follower answers `env create` fine).
# Each phase gets its own budget, because formation can eat all of a
# fresh-boot wait by itself. Only when every manager has answered does
# guard 1 start; a cluster that never gets there fails HERE, once, naming
# the node and its last reply.
set -u
cd "$(dirname "$0")/.."
export AEROSLS_TOKEN=deadbeef01234567cafebabe76543210

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
CTL="tools/aeroslsctl"
POLL=5
WAIT_WARM="${AEROSLS_WARMUP_WAIT:-300}"   # same generosity as the guards' own formation waits

TOKEN_ARGS=()
[ -n "${AEROSLS_TOKEN:-}" ] && TOKEN_ARGS=(--token "$AEROSLS_TOKEN")
CURL_EXTRA=()
[ -n "${AEROSLS_TOKEN:-}" ] && CURL_EXTRA=(-H "Authorization: Bearer $AEROSLS_TOKEN")

# One failure, said once, with the overall line every caller and transcript
# already parses. The guards each re-wait for formation themselves; only
# the manager's answer to `env create` is the thing they cannot wait out.
warm_bad() {
    echo "BAD  warm-up: $*"
    echo "RUNTIME-GUARDS overall: FAIL"
    exit 1
}

# A create reply can be several lines (CAP/ENV preamble); the last two
# carry the verdict, flattened so they sit on one transcript line.
oneline() { printf '%s' "$1" | tail -2 | tr '\n' ' '; }

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

# fetch: bounded curl, status + body carried for the caller's poll loop —
# the same shape every live guard uses (a down hostfwd port accepts the
# connect and then times out, so --max-time is not optional).
FETCH_STATUS=""; FETCH_BODY=""
fetch() {   # $1 = port, $2 = path
    local out
    out="$(curl -s --max-time 3 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    FETCH_STATUS="$(printf '%s' "$out" | tail -n1)"
    FETCH_BODY="$(printf '%s' "$out" | sed '$d')"
}

# ─── the cluster must at least exist on disk ────────────────────────────
PID_FILE="$CLUSTER_DIR/cluster.pids"
[ -f "$PID_FILE" ] || warm_bad "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh (then re-run this suite against it)."
[ -f "$CTL" ] || warm_bad "$CTL not found -- run from the repo root."
command -v curl    >/dev/null 2>&1 || warm_bad "curl not found."
command -v python3 >/dev/null 2>&1 || warm_bad "python3 is needed to read the /api JSON."
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || warm_bad "$PID_FILE is empty -- nothing to test."

WARM_NAME="guard-warmup-$$"      # unique per run, never stale
WARM_LEADER=""
WARM_PART=""
WARM_LAST=""
echo "=== warm-up: a leader, a partition create, and one env create per node (up to ${WAIT_WARM}s per phase)"

# ─── phase A: a leader with quorum, then one partition create on it ────
deadline=$(( $(date +%s) + WAIT_WARM ))
while :; do
    [ "$(date +%s)" -ge "$deadline" ] && break
    if [ -z "$WARM_LEADER" ]; then
        for id in $NODE_IDS; do
            fetch $((HTTP_BASE + id)) /api/cluster
            [ "$FETCH_STATUS" = 200 ] || continue
            role="$(jget "$FETCH_BODY" role)"
            active="$(jget "$FETCH_BODY" active_nodes)"
            if [ "$role" = "LEADER" ] && [ "${active:-0}" -ge 2 ] 2>/dev/null; then
                WARM_LEADER="$id"
                break
            fi
        done
    fi
    if [ -n "$WARM_LEADER" ] && [ -z "$WARM_PART" ]; then
        WARM_LAST="$("$CTL" --host "localhost:$((HTTP_BASE + WARM_LEADER))" "${TOKEN_ARGS[@]}" \
            shell partition create "$WARM_NAME" 2>&1)"
        WARM_PART="$(printf '%s' "$WARM_LAST" \
            | sed -n "s/.*create '$WARM_NAME' -> id=\([0-9]*\).*/\1/p" | head -1)"
        [ -n "$WARM_PART" ] && break
    fi
    sleep "$POLL"
done
[ -n "$WARM_LEADER" ] || warm_bad "no leader with quorum (active >= 2) among nodes $(echo $NODE_IDS)
       within ${WAIT_WARM}s -- the cluster never formed ($PID_FILE names the nodes that
       should be there; every guard would wait for formation again anyway)."
[ -n "$WARM_PART" ] || warm_bad "partition create never answered on node $WARM_LEADER within
       ${WAIT_WARM}s. Last reply: $(oneline "$WARM_LAST")"

# ─── phase B: one env create per node -- every manager must answer ─────
# Fresh budget: formation may have consumed all of phase A's wait, and the
# manager's own window deserves its own (it opens AFTER the APIs answer).
deadline=$(( $(date +%s) + WAIT_WARM ))
for id in $NODE_IDS; do
    port=$((HTTP_BASE + id))
    eid=""
    while :; do
        [ "$(date +%s)" -ge "$deadline" ] && break
        WARM_LAST="$("$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
            shell env create "$WARM_PART" 2>&1)"
        case "$WARM_LAST" in
            *"-> ok env_id="*)
                eid="$(printf '%s' "$WARM_LAST" | sed -n 's/.*ok env_id=\([0-9]*\).*/\1/p' | head -1)"
                break ;;
        esac
        sleep "$POLL"
    done
    [ -n "$eid" ] || warm_bad "node $id's environment manager never answered an env create
       within ${WAIT_WARM}s -- this is the fresh-boot saturation that failed three guards on
       the first post-relaunch run, and starting the suite now would fail them the same way.
       Last reply: $(oneline "$WARM_LAST")"
    echo "   node $id: env create answered -> ok env_id=$eid"

    # Hand the frames straight back, and sweep rather than trust the last
    # eid: a create whose CLIENT deadline expired can still have succeeded
    # node-side, so a retried window may have stacked more than one env in
    # this partition. Same list-and-destroy shape as the migrate guard's
    # residue sweep, env_id 0 excluded (E3 boot spawns are not ours).
    for pair in $("$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                      shell env list 2>/dev/null \
                  | sed -n "s/.*env partition=$WARM_PART env_id=\([1-9][0-9]*\).*/$WARM_PART\/\1/p"); do
        "$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
            shell env destroy "${pair%/*}" "${pair#*/}" >/dev/null 2>&1 || true
    done
done

# ─── teardown: the warm-up leaves nothing for the guards to inherit ────
# Best-effort on purpose: a leftover row cannot fail guards 1-3 (they ran
# green over two pre-existing guard-failover rows), and the migrate guard
# re-sweeps every node itself before its allocator-parity assertion. A
# partition destroy also ends whatever env its list half-cleared.
"$CTL" --host "localhost:$((HTTP_BASE + WARM_LEADER))" "${TOKEN_ARGS[@]}" \
    shell partition destroy "$WARM_PART" >/dev/null 2>&1 || true
echo "OK   warm-up (leader node $WARM_LEADER, warm-up partition id $WARM_PART torn down)"

# failover_2node_live_check.sh is NOT here: it refuses a cluster whose
# cluster.pids names anything but exactly 2 nodes. It runs last, against its
# own 2-node cluster (see the caller's notes), after these finish.
GUARDS="
entropy_boot_diversity_check.sh
partition_reannounce_live_check.sh
partition_ownedset_gc_live_check.sh
env_migrate_confirm_live_check.sh
e3_multi_env_boot_check.sh
failover_adoption_live_check.sh
env_catalog_live_check.sh
"

overall=0
for g in $GUARDS; do
    echo "=== $g"
    if ./tests/$g; then
        echo "OK   $g"
    else
        rc=$?
        echo "BAD  $g (rc=$rc)"
        overall=1
    fi
done
echo "RUNTIME-GUARDS overall: $([ $overall -eq 0 ] && echo PASS || echo FAIL)"
exit $overall
