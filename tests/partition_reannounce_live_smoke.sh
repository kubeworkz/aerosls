#!/usr/bin/env bash
# tests/partition_reannounce_live_smoke.sh — the re-announce live guard can
# FAIL, and can PASS for the right reason.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# partition_reannounce_live_check.sh is a runtime guard: it needs a live
# cluster, so CI and build hosts report it as owed and never run it. A guard
# that only ever runs by hand is a guard that rots unseen -- the exact
# failure this repo already paid for twice (script_conventions_check stopped
# inspecting anything and stayed green; entropy_boot_diversity_check passed
# nothing for days because entropy_init was never called).
#
# So this stands up FAKE nodes and drives the guard through its verdicts,
# with every wait scaled down by AEROSLS_REANNOUNCE_FAST. The fake nodes
# implement only the handful of routes the guard touches, plus one piece of
# real state: the leader "announces" a created partition to a row file the
# follower "learns" from, and the follower counts its own boots so it can
# behave differently AFTER the guard kills and relaunches it. The guard's
# scenario is shaped for the persistence change: it creates row A while
# both nodes are up (learn gate), kills the follower, creates row B WHILE
# it is down, and relaunches -- so the follower's second boot serves B only
# if the re-announce delivered it, exactly like the real kernel (B was
# never learned and is on nobody's disk but the creator's).
#
# The teeth:
#   converged     -> PASS  (follower serves row B after the relaunch)
#   notconverged  -> FAIL  (follower never serves B; the convergence poll
#                           must run out and say so)
#   nolearn       -> FAIL  (follower does not even learn row A from the
#                           create announce; the pre-reboot gate must bite)
#   silent        -> ABORT (no cluster.pids at all; the guard must say how
#                           to start one)
#
# The last three matter as much as the first: a guard that cannot fail is a
# guard that has never been seen to work. And a PASS that skipped the reboot
# would prove nothing, so the converged tooth also asserts the follower's
# boot counter reached 2 -- the guard really did kill and relaunch it.
#
# Runs on a port base far from any real cluster and never touches a running
# one: it refuses to start if cluster/cluster.pids names any live process,
# and it restores/removes the pid file it writes.
#
# Exit: 0 all teeth bit, 1 a tooth failed, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

GUARD=tests/partition_reannounce_live_check.sh
FAKE=tests/partition_reannounce_smoke_nodes.py
BASE=58900
PID_FILE=cluster/cluster.pids

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found or not executable."; exit 2; }
[ -f "$FAKE" ]  || { echo "ABORT: $FAKE not found."; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 needed to stand up fake nodes."; exit 2; }
command -v curl    >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

# A real cluster must not be running: this smoke writes cluster/cluster.pids
# and the guard kills whatever that file names. Refuse rather than guess.
if [ -s "$PID_FILE" ]; then
    while read -r _ pid; do
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            echo "ABORT: $PID_FILE names live pid $pid -- a real cluster is running." >&2
            echo "       Stop it first (./run-cluster.sh --stop) -- this smoke uses" >&2
            echo "       fake nodes on port base $BASE and must not touch it." >&2
            exit 2
        fi
    done < "$PID_FILE"
    echo "note: $PID_FILE was stale (no live pids); removing it." >&2
    rm -f "$PID_FILE"
fi
mkdir -p cluster

fails=0
STATE=""
FAKE_PIDS=""

cleanup_fakes() {
    # Kill every pid the CURRENT pid file names -- the guard may have
    # replaced the follower's entry with its relaunch, so the file is the
    # only place the new pid is recorded. Then the originals, and the state.
    if [ -f "$PID_FILE" ]; then
        while read -r _ pid; do
            [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null || true
        done < "$PID_FILE"
        rm -f "$PID_FILE"
    fi
    [ -n "$FAKE_PIDS" ] && kill $FAKE_PIDS 2>/dev/null || true
    wait 2>/dev/null   # silence the "Killed" job notices for the SIGKILLed fakes
    return 0
}
cleanup() { cleanup_fakes; [ -n "$STATE" ] && rm -rf "$STATE"; return 0; }
trap cleanup EXIT

# $1 = mode, $2 = expected exit, $3 = a phrase the output must contain,
# $4 = label
tooth() {
    local mode="$1" want_rc="$2" want_txt="$3" label="$4"
    local out rc
    cleanup_fakes
    [ -n "$STATE" ] && rm -rf "$STATE"
    STATE="$(mktemp -d)"
    echo "$mode" > "$STATE/mode"
    echo 0 > "$STATE/bootcount-1"
    echo 0 > "$STATE/bootcount-2"

    python3 "$FAKE" 1 leader $((BASE+1)) "$STATE" >/dev/null 2>&1 &
    local lpid=$!
    python3 "$FAKE" 2 follower $((BASE+2)) "$STATE" >/dev/null 2>&1 &
    local fpid=$!
    FAKE_PIDS="$lpid $fpid"

    # Wait for both ports to answer rather than sleeping a guess.
    for _ in $(seq 1 40); do
        curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/health" \
            && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+2))/api/health" \
            && break
        sleep 0.1
    done
    printf '1 %s\n2 %s\n' "$lpid" "$fpid" > "$PID_FILE"

    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_REANNOUNCE_FAKE=1 \
           AEROSLS_REANNOUNCE_FAST=1 bash "$GUARD" 2>&1)"
    rc=$?

    if [ "$mode" = "converged" ]; then
        # A PASS that never rebooted the follower proves nothing: the whole
        # point is that a node which booted AFTER the create converges.
        b2="$(cat "$STATE/bootcount-2" 2>/dev/null || echo 0)"
        if [ "$b2" != "2" ]; then
            echo "TOOTH FAIL $label — guard passed, but the follower was never rebooted (bootcount-2=$b2)"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1))
            return
        fi
    fi
    cleanup_fakes

    if [ "$rc" != "$want_rc" ]; then
        echo "TOOTH FAIL $label — guard exit $rc, expected $want_rc"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"$want_txt"*) echo "TOOTH OK   $label — exit $rc, said: $want_txt" ;;
        *)
            echo "TOOTH FAIL $label — exit $rc was right, but the output did not mention: $want_txt"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1)) ;;
    esac
}

tooth converged    0 "PASS"      "converged -> PASS (and the follower was rebooted)"
tooth notconverged 1 "never converged" "notconverged -> FAIL (the re-announce did not arrive)"
tooth nolearn      1 "never learned"   "nolearn -> FAIL (the create announce did not arrive)"

# The silent tooth: no cluster at all. The guard must abort (2) and say how
# to start one -- not pass, and not blame the wrong layer.
cleanup_fakes
out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_REANNOUNCE_FAST=1 bash "$GUARD" 2>&1)"
rc=$?
if [ "$rc" != "2" ]; then
    echo "TOOTH FAIL no cluster -> ABORT — guard exit $rc, expected 2"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
else
    case "$out" in
        *"Start one first"*) echo "TOOTH OK   no cluster -> ABORT naming how to start one" ;;
        *)
            echo "TOOTH FAIL no cluster -> ABORT — exit 2 was right, but the message did not mention: Start one first"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1)) ;;
    esac
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "partition_reannounce_live_smoke: done, 0 teeth failed"
    exit 0
fi
echo "partition_reannounce_live_smoke: done, $fails teeth failed"
exit 1
