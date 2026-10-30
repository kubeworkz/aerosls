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
# The label is switchable: the mid-suite settle (below) fails through this
# same single-exit path without calling itself the warm-up.
BAD_LABEL="warm-up"
bad_once() {
    echo "BAD  $BAD_LABEL: $*"
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
[ -f "$PID_FILE" ] || bad_once "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh (then re-run this suite against it)."
[ -f "$CTL" ] || bad_once "$CTL not found -- run from the repo root."
command -v curl    >/dev/null 2>&1 || bad_once "curl not found."
command -v python3 >/dev/null 2>&1 || bad_once "python3 is needed to read the /api JSON."
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || bad_once "$PID_FILE is empty -- nothing to test."

# ─── the three probes the warm-up gate and every settle share ────────────
# leader → partition → one env create per node, written once so the two
# cannot drift apart: the warm-up runs them before guard 1, the settle runs
# them again after a guard that killed a node or loaded the host.

# find_leader: the node currently LEADER with quorum (active >= 2), printed.
# Re-asked every time — the adoption guard kills the leader mid-suite, so a
# leader id found earlier is stale by the next probe.
find_leader() {
    local id role active
    for id in $NODE_IDS; do
        fetch $((HTTP_BASE + id)) /api/cluster
        [ "$FETCH_STATUS" = 200 ] || continue
        role="$(jget "$FETCH_BODY" role)"
        active="$(jget "$FETCH_BODY" active_nodes)"
        if [ "$role" = "LEADER" ] && [ "${active:-0}" -ge 2 ] 2>/dev/null; then
            printf '%s' "$id"
            return 0
        fi
    done
    return 1
}

# make_partition <leader> <name> <deadline>: retry the create until it answers
# or the deadline passes. Sets PART_ID ("" = timed out) and LAST_REPLY — a
# command substitution would take both into a subshell, so this is called for
# its globals, not its stdout.
PART_ID=""
LAST_REPLY=""
make_partition() {
    PART_ID=""
    while :; do
        [ "$(date +%s)" -ge "$3" ] && break
        LAST_REPLY="$("$CTL" --host "localhost:$((HTTP_BASE + $1))" "${TOKEN_ARGS[@]}" \
            shell partition create "$2" 2>&1)"
        PART_ID="$(printf '%s' "$LAST_REPLY" \
            | sed -n "s/.*create '$2' -> id=\([0-9]*\).*/\1/p" | head -1)"
        [ -n "$PART_ID" ] && break
        sleep "$POLL"
    done
}

# probe_nodes <part> <deadline> <budget> <context>: one env create per node,
# retried until each manager answers or the deadline passes, then every env it
# created is swept. Fails ONCE through bad_once, naming the node and its last
# reply — the guards would otherwise report one cause several times over.
probe_nodes() {
    local part="$1" deadline="$2" budget="$3" ctx="$4"
    local id port eid pair
    for id in $NODE_IDS; do
        port=$((HTTP_BASE + id))
        eid=""
        while :; do
            [ "$(date +%s)" -ge "$deadline" ] && break
            LAST_REPLY="$("$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                shell env create "$part" 2>&1)"
            case "$LAST_REPLY" in
                *"-> ok env_id="*)
                    eid="$(printf '%s' "$LAST_REPLY" | sed -n 's/.*ok env_id=\([0-9]*\).*/\1/p' | head -1)"
                    break ;;
            esac
            sleep "$POLL"
        done
        # Two different faults wear the same shape at a glance, and only one
        # of them is the starvation this probe exists for. A manager that
        # never ANSWERED (the client deadline expiring: fresh-boot / post-
        # relaunch saturation) is the window; a manager that answered with a
        # refusal is a different, reported cause — and its own words say
        # which (measured: a stale per-id pause flag on one follower read
        # `frame pool exhausted`, because ENV_ERR_NOMEM maps that way even
        # when the kernel's own line is `CAP_EPERM: target partition 1 is
        # paused`). Both retry to the deadline first, because a refusal can
        # be transient too; the verdict, not the retry, is what differs.
        [ -n "$eid" ] || {
            case "$LAST_REPLY" in
                *"-> "*)
                    bad_once "node $id's environment manager REFUSED an env create for ${budget}s
       ($ctx).
       Last reply: $(oneline "$LAST_REPLY")" ;;
                *)
                    bad_once "node $id's environment manager never answered an env create
       within ${budget}s ($ctx).
       Last reply: $(oneline "$LAST_REPLY")" ;;
            esac
        }
        echo "   node $id: env create answered -> ok env_id=$eid"

        # Hand the frames straight back, and sweep rather than trust the last
        # eid: a create whose CLIENT deadline expired can still have succeeded
        # node-side, so a retried window may have stacked more than one env in
        # this partition. Same list-and-destroy shape as the migrate guard's
        # residue sweep, env_id 0 excluded (E3 boot spawns are not ours).
        for pair in $("$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                          shell env list 2>/dev/null \
                      | sed -n "s/.*env partition=$part env_id=\([1-9][0-9]*\).*/$part\/\1/p"); do
            "$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                shell env destroy "${pair%/*}" "${pair#*/}" >/dev/null 2>&1 || true
        done
    done
}

WARM_NAME="guard-warmup-$$"      # unique per run, never stale
WARM_LEADER=""
WARM_PART=""
echo "=== warm-up: a leader, a partition create, and one env create per node (up to ${WAIT_WARM}s per phase)"

# ─── phase A: a leader with quorum, then one partition create on it ────
deadline=$(( $(date +%s) + WAIT_WARM ))
while :; do
    [ "$(date +%s)" -ge "$deadline" ] && break
    [ -z "$WARM_LEADER" ] && WARM_LEADER="$(find_leader)" || true
    if [ -n "$WARM_LEADER" ] && [ -z "$WARM_PART" ]; then
        make_partition "$WARM_LEADER" "$WARM_NAME" "$deadline"
        WARM_PART="$PART_ID"
        [ -n "$WARM_PART" ] && break
    fi
    sleep "$POLL"
done
[ -n "$WARM_LEADER" ] || bad_once "no leader with quorum (active >= 2) among nodes $(echo $NODE_IDS)
       within ${WAIT_WARM}s -- the cluster never formed ($PID_FILE names the nodes that
       should be there; every guard would wait for formation again anyway)."
[ -n "$WARM_PART" ] || bad_once "partition create never answered on node $WARM_LEADER within
       ${WAIT_WARM}s. Last reply: $(oneline "$LAST_REPLY")"

# ─── phase B: one env create per node -- every manager must answer ─────
# Fresh budget: formation may have consumed all of phase A's wait, and the
# manager's own window deserves its own (it opens AFTER the APIs answer).
deadline=$(( $(date +%s) + WAIT_WARM ))
probe_nodes "$WARM_PART" "$deadline" "$WAIT_WARM" \
    "fresh-boot saturation — the window that failed three guards on the first post-relaunch run, which the suite must not start into"

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

# ─── settle: the same probe, re-run after a guard that loaded the system ──
# Four guards open that same window in the MIDDLE of the suite, and each of
# them made the NEXT guard fail on a cold pass while a plain re-run minutes
# later was green:
#   partition_reannounce / partition_ownedset_gc  SIGKILL a follower and
#       relaunch it — the relaunched node's catch-up then flooded the next
#       guard's response capture (env_migrate_confirm, 1st failure),
#   e3_multi_env_boot        boots its own 4 GB TCG QEMU on the same host
#       cores while node 1 is still catching up (e3 saw 2/3 rootfs in its
#       120 s window),
#   failover_adoption        kills the LEADER and relaunches it — the guard
#       after it hit `no reply from the environment manager` right after
#       e3's QEMU exited (adoption's own 1st failure came from that window).
# Same transient as the warm-up, opened mid-suite: spend it here, once,
# instead of letting the next guard report it as its own defect.
#
# What it does NOT gate: checkpoint catch-up traffic. A manager that answers
# `env create` is the property the guards need to START; the relaunched
# node's broadcast volume settles on its own afterwards.
SETTLE_AFTER="
partition_reannounce_live_check.sh
partition_ownedset_gc_live_check.sh
e3_multi_env_boot_check.sh
failover_adoption_live_check.sh
"
# A cap, not a delay: a settled cluster passes on the first probe (seconds).
SETTLE_WAIT="${AEROSLS_SETTLE_WAIT:-300}"

settle() {                                   # $1 = the guard that just ran
    local after="$1" leader="" part="" deadline n=0
    echo "--- settle after $after (up to ${SETTLE_WAIT}s per phase)"
    BAD_LABEL="settle (after $after)"
    deadline=$(( $(date +%s) + SETTLE_WAIT ))
    while :; do
        [ "$(date +%s)" -ge "$deadline" ] && break
        [ -z "$leader" ] && leader="$(find_leader)" || true
        if [ -n "$leader" ] && [ -z "$part" ]; then
            n=$((n + 1))
            make_partition "$leader" "guard-settle-$$-$n" "$deadline"
            part="$PART_ID"
            [ -n "$part" ] && break
        fi
        sleep "$POLL"
    done
    [ -n "$leader" ] || bad_once "no leader with quorum (active >= 2) among nodes $NODE_IDS
       within ${SETTLE_WAIT}s -- $after left the cluster without one, and every guard after it
       waits for formation anyway."
    [ -n "$part" ] || bad_once "partition create never answered on node $leader within
       ${SETTLE_WAIT}s. Last reply: $(oneline "$LAST_REPLY")"

    # Fresh budget, as in phase B: the manager's window opens after the APIs answer.
    deadline=$(( $(date +%s) + SETTLE_WAIT ))
    probe_nodes "$part" "$deadline" "$SETTLE_WAIT" \
        "mid-suite saturation right after $after (its relaunched node / side QEMU still loading the host) — the next guard's first env create would die here exactly as a plain re-run minutes later would not"

    "$CTL" --host "localhost:$((HTTP_BASE + leader))" "${TOKEN_ARGS[@]}" \
        shell partition destroy "$part" >/dev/null 2>&1 || true
    echo "OK   settle after $after (leader node $leader, settle partition id $part torn down)"
}

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
    # The kill-heavy four starve whoever runs NEXT, not themselves, so settle
    # before the next guard starts — including after a FAILED one: a guard
    # that died mid-kill leaves the same load behind it.
    case "$SETTLE_AFTER" in
        *"$g"*) settle "$g" ;;
    esac
done
echo "RUNTIME-GUARDS overall: $([ $overall -eq 0 ] && echo PASS || echo FAIL)"
exit $overall
