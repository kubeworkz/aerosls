#!/usr/bin/env bash
# tests/partition_ownedset_gc_live_smoke.sh — the owned-set GC live guard
# can FAIL, and can PASS for the right reason.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# partition_ownedset_gc_live_check.sh is a runtime guard: it needs a live
# cluster, so CI and build hosts report it as owed and never run it. A guard
# that only ever runs by hand is a guard that rots unseen -- the exact
# failure this repo already paid for twice (script_conventions_check stopped
# inspecting anything and stayed green; entropy_boot_diversity_check passed
# nothing for days because entropy_init was never called).
#
# So this stands up FAKE nodes and drives the guard through its verdicts,
# with every wait scaled down by AEROSLS_GC_FAST. The fake nodes implement
# only the handful of routes the guard touches, plus the three serial-log
# strings the guard's evidence gates on (learn, flush, restore, collect --
# each printed by exactly one real kernel site). The follower counts its own
# boots so it can behave differently AFTER the guard kills and relaunches it:
# boot 1 learns the row and flushes it to disk; boot 2 restores the ghost
# from disk and -- for the collected tooth only -- the fake owned-set frame
# then collects it, exactly like partition_sync_ownedset() on the real RX
# path.
#
# The teeth:
#   collected     -> PASS  (boot 2 restores the ghost, the owned-set frame
#                           collects it, the row leaves /api/partitions;
#                           AND the follower was really rebooted)
#   notcollected  -> FAIL  (boot 2 restores but never collects; the GC poll
#                           must run out and say "still lists")
#   nolearn       -> FAIL  (boot 1 never learns the row; the pre-reboot
#                           learn gate must bite)
#   nopersist     -> FAIL  (boot 1 learns but never flushes; the disk gate
#                           must bite -- without the ghost on disk the test
#                           would prove nothing)
#   silent        -> ABORT (no cluster.pids at all; the guard must say how
#                           to start one)
#
# The last four matter as much as the first: a guard that cannot fail is a
# guard that has never been seen to work. And a PASS that skipped the reboot
# would prove nothing, so the collected tooth also asserts the follower's
# boot counter reached 2.
#
# Runs on a port base far from any real cluster and never touches a running
# one: the guard's pid file lives in a private temp dir (AEROSLS_CLUSTER_DIR),
# so it runs beside a live cluster, and it asserts at the end that the repo's
# cluster/cluster.pids is unchanged.
#
# Exit: 0 all teeth bit, 1 a tooth failed, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

GUARD=tests/partition_ownedset_gc_live_check.sh
FAKE=tests/partition_ownedset_gc_smoke_nodes.py
BASE=59000

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found or not executable."; exit 2; }
[ -f "$FAKE" ]  || { echo "ABORT: $FAKE not found."; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 needed to stand up fake nodes."; exit 2; }
command -v curl    >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

# The guard's cluster dir is a PRIVATE temp dir, never the repo's cluster/.
# The guard kills and relaunches whatever cluster.pids names, and the deploy
# gate runs this smoke on the host where the real cluster is live -- with a
# shared cluster/cluster.pids the only safe move was to refuse, and a smoke
# that cannot run proves nothing (the 2026-09-13 deploy gate failed on
# exactly that). AEROSLS_CLUSTER_DIR is exported so EVERY guard call below,
# the silent no-cluster tooth included, resolves its pid file here and can
# never read a real cluster's.
SMOKE_CLUSTER_DIR="$(mktemp -d)" || { echo "ABORT: mktemp -d failed."; exit 2; }
export AEROSLS_CLUSTER_DIR="$SMOKE_CLUSTER_DIR"
PID_FILE="$SMOKE_CLUSTER_DIR/cluster.pids"
# The repo's own cluster/cluster.pids (a live cluster's, on a deploy host)
# must be unchanged when this smoke ends -- asserted at the bottom.
real_pids_sum() { if [ -f cluster/cluster.pids ]; then cksum < cluster/cluster.pids; else echo absent; fi; }
REAL_PIDS_BEFORE="$(real_pids_sum)"

fails=0
STATE=""
FAKE_PIDS=""

cleanup_fakes() {
    if [ -f "$PID_FILE" ]; then
        while read -r _ pid; do
            [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null || true
        done < "$PID_FILE"
        rm -f "$PID_FILE"
    fi
    [ -n "$FAKE_PIDS" ] && kill $FAKE_PIDS 2>/dev/null || true
    wait 2>/dev/null
    return 0
}
cleanup() { cleanup_fakes; [ -n "$STATE" ] && rm -rf "$STATE"; rm -rf "$SMOKE_CLUSTER_DIR"; return 0; }
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

    python3 "$FAKE" 1 leader $((BASE+1)) "$STATE" "$STATE" >/dev/null 2>&1 &
    local lpid=$!
    python3 "$FAKE" 2 follower $((BASE+2)) "$STATE" "$STATE" >/dev/null 2>&1 &
    local fpid=$!
    FAKE_PIDS="$lpid $fpid"

    for _ in $(seq 1 40); do
        curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/health" \
            && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+2))/api/health" \
            && break
        sleep 0.1
    done
    printf '1 %s\n2 %s\n' "$lpid" "$fpid" > "$PID_FILE"

    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_GC_FAKE=1 \
           AEROSLS_GC_FAST=1 AEROSLS_LOG_DIR=$STATE bash "$GUARD" 2>&1)"
    rc=$?

    if [ "$mode" = "collected" ]; then
        # A PASS that never rebooted the follower proves nothing: the whole
        # point is that a node which booted AFTER the destroy collects the
        # ghost restored from disk.
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

tooth collected    0 "PASS"      "collected -> PASS (and the follower was rebooted)"
tooth notcollected 1 "still lists" "notcollected -> FAIL (the owned-set never collected the ghost)"
tooth nolearn      1 "never learned" "nolearn -> FAIL (the create announce did not arrive)"
tooth nopersist    1 "never flushed" "nopersist -> FAIL (the ghost never reached disk)"

# The silent tooth: no cluster at all. The guard must abort (2) and say how
# to start one -- not pass, and not blame the wrong layer.
cleanup_fakes
out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_GC_FAST=1 bash "$GUARD" 2>&1)"
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

# Isolation: every guard call above used the private dir, so the repo's
# cluster/cluster.pids (a live cluster's, on a deploy host) is untouched.
if [ "$(real_pids_sum)" = "$REAL_PIDS_BEFORE" ]; then
    echo "ISOLATION OK   the repo's cluster/cluster.pids is unchanged (pid files lived in $SMOKE_CLUSTER_DIR)"
else
    echo "ISOLATION FAIL the repo's cluster/cluster.pids changed during the smoke -- a guard call escaped AEROSLS_CLUSTER_DIR"
    fails=$((fails + 1))
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "partition_ownedset_gc_live_smoke: done, 0 teeth failed"
    exit 0
fi
echo "partition_ownedset_gc_live_smoke: done, $fails teeth failed"
exit 1
