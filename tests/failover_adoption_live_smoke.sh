#!/usr/bin/env bash
# tests/failover_adoption_live_smoke.sh — the failover adoption live guard
# can FAIL, and can PASS for the right reason.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/failover_adoption_live_check.sh is a runtime guard: it needs a live
# cluster, so CI and build hosts report it as owed and never run it. A guard
# that only ever runs by hand is a guard that rots unseen -- the exact
# failure this repo already paid for twice (script_conventions_check stopped
# inspecting anything and stayed green; entropy_boot_diversity_check passed
# nothing for days because entropy_init was never called).
#
# So this stands up FAKE nodes and drives the guard through its verdicts,
# with every wait scaled down by AEROSLS_FAILOVER_FAST. The fake nodes
# implement only the handful of routes the guard touches, plus the log
# strings the guard's evidence gates on (learn, checkpoint RX, death
# declaration, adoption, handoff -- each printed by exactly one real kernel
# site) and the one piece of real behavior the property depends on: the
# follower polls the leader's port, and when it dies (the guard SIGKILLs
# it) the follower transitions -- exactly how the real kernel detects death.
#
# The teeth:
#   adopted     -> PASS  (node 2 becomes leader, declares the death, adopts
#                         the row; node 3 observes the handoff; AND the
#                         guard really killed the leader -- its pid is dead)
#   notadopted  -> FAIL  (node 2 leads and declares but never adopts; the
#                         adoption poll must run out and say so)
#   nolearn     -> FAIL  (followers never learn the create; the pre-kill
#                         learn gate must bite)
#   nockpt      -> FAIL  (followers learn but no checkpoint flows; the
#                         checkpoint gate must bite -- without it the held
#                         checkpoint predates the create)
#   splitbrain  -> FAIL  (BOTH survivors become leader at once; the guard
#                         must detect the split and fail loudly)
#   observeradopts -> FAIL (the observer ALSO prints the Adopted line
#                         while staying FOLLOWER -- a follower that
#                         recovered; the step-9 never-adopt check must bite,
#                         pinning cluster_is_leader())
#   lateflip    -> FAIL  (the observer stays FOLLOWER through the adoption,
#                         then flips to LEADER; the step-9 stays-FOLLOWER
#                         watch must catch the late split-brain)
#   resurrect_stable -> PASS (the guard RELAUNCHES the killed leader
#                         mid-adoption; its stale re-announce is rejected by
#                         both survivors, the partition does not flap, and
#                         the leader converges to the new owner)
#   resurrect_flap    -> FAIL  (the survivors APPLY the stale claim -- the
#                         old "last announce wins" bug -- and the partition
#                         flaps back to the resurrected owner)
#   resurrect_nostale -> FAIL  (the relaunched leader never restores its
#                         stale row, so the conflict never manifests)
#   migrate_stable -> PASS  (step 11: the adopter migrates the partition
#                         back to the resurrected original leader; the
#                         owner-initiated transfer out-resolves its stale
#                         learned row on the destination AND the observer,
#                         the destination is killed mid-flight and
#                         relaunched, the cluster converges to owner 1 with
#                         no flap, the adopter's write lease is relinquished
#                         at the migrate and re-acquired on the new owner by
#                         a fresh 2-of-3 quorum)
#   migrate_flap -> FAIL  (the transfer is applied to the wrong owner -- the
#                         flap -- and the step-11 convergence gate must bite:
#                         the receivers report owner = the adopter)
#   migrate_nolease -> FAIL  (the adopter's pre-migrate `partition lease
#                         acquire` never holds; the lease-held gate must bite)
#   migrate_noapply -> FAIL  (the transfer announce never reaches the
#                         receivers; the step-11 transfer gate must bite)
#   migrate_nostale -> FAIL  (the step-11 relaunch of the destination never
#                         restores its row; the step-11 restore gate must bite)
#   migrate_noreacquire -> FAIL  (the destination's re-acquire never holds;
#                         the step-11 re-acquire gate must bite)
#   silent      -> ABORT (no cluster.pids at all; the guard must say how to
#                         start one)
#
# The last five matter as much as the first: a guard that cannot fail is a
# guard that has never been seen to work. And a PASS that never killed the
# leader would prove nothing, so the adopted tooth asserts the leader's
# fake pid is actually dead after the run.
#
# Runs on a port base far from any real cluster and never touches a running
# one: it refuses to start if cluster/cluster.pids names any live process,
# and it restores/removes the pid file it writes.
#
# Exit: 0 all teeth bit, 1 a tooth failed, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

GUARD=tests/failover_adoption_live_check.sh
FAKE=tests/failover_adoption_smoke_nodes.py
BASE=59200
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
    echo 1 > "$STATE/leader"   # node 1 is the leader

    python3 "$FAKE" 1 leader   $((BASE+1)) "$STATE" "$STATE" >/dev/null 2>&1 &
    local lpid=$!
    python3 "$FAKE" 2 follower $((BASE+2)) "$STATE" "$STATE" >/dev/null 2>&1 &
    local f1pid=$!
    python3 "$FAKE" 3 follower $((BASE+3)) "$STATE" "$STATE" >/dev/null 2>&1 &
    local f2pid=$!
    FAKE_PIDS="$lpid $f1pid $f2pid"

    for _ in $(seq 1 40); do
        curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/health" \
            && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+2))/api/health" \
            && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+3))/api/health" \
            && break
        sleep 0.1
    done
    printf '1 %s\n2 %s\n3 %s\n' "$lpid" "$f1pid" "$f2pid" > "$PID_FILE"

    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_FAILOVER_FAKE=1 \
           AEROSLS_FAILOVER_FAST=1 AEROSLS_LOG_DIR=$STATE bash "$GUARD" 2>&1)"
    rc=$?

    if [ "$mode" = "adopted" ]; then
        # A PASS that never killed the leader proves nothing: the whole
        # point is that the ADOPTION happens after the leader dies.
        if kill -0 "$lpid" 2>/dev/null; then
            echo "TOOTH FAIL $label — guard passed, but the leader was never killed (pid $lpid still alive)"
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

tooth adopted          0 "PASS"      "adopted -> PASS (and the leader was really killed)"
tooth notadopted       1 "never adopted" "notadopted -> FAIL (the leader leads but the adoption never fires)"
tooth nolearn          1 "never learned" "nolearn -> FAIL (the create announce did not arrive)"
tooth nockpt           1 "no checkpoint" "nockpt -> FAIL (the checkpoint never carried the row)"
tooth splitbrain       1 "split-brain"   "splitbrain -> FAIL (both survivors claim LEADER at once)"
tooth observeradopts   1 "ALSO recovered" "observeradopts -> FAIL (a follower that recovered must be caught by step 9)"
tooth lateflip         1 "flipped to LEADER" "lateflip -> FAIL (the late flip to LEADER must be caught by step 9)"
tooth resurrect_stable 0 "PASS"   "resurrect_stable -> PASS (stale claim rejected by both survivors, no flap, leader converged)"
tooth resurrect_flap   1 "FLAPPED" "resurrect_flap -> FAIL (the old last-wins apply of the stale claim must be caught)"
tooth resurrect_nostale 1 "never restored" "resurrect_nostale -> FAIL (a leader whose stale row never came back must be caught)"
tooth migrate_stable 0 "PASS"   "migrate_stable -> PASS (step 11: the owner-initiated transfer out-resolves the destination's stale row, mid-flight relaunch converges, lease relinquished + re-acquired)"
tooth migrate_flap   1 "converge to owner" "migrate_flap -> FAIL (step 11's transfer applied to the wrong owner must be caught -- the receivers report owner = the adopter, so the step-11 convergence gate bites)"
tooth migrate_nolease 1 "never held the write lease" "migrate_nolease -> FAIL (the adopter's pre-migrate lease acquire never holds must be caught)"
tooth migrate_noapply 1 "logged the owner-initiated transfer" "migrate_noapply -> FAIL (a transfer the destination never receives must be caught)"
tooth migrate_nostale 1 "never restored its row" "migrate_nostale -> FAIL (a step-11 relaunch that loses the transferred row must be caught)"
tooth migrate_noreacquire 1 "never re-acquired the write lease" "migrate_noreacquire -> FAIL (the destination's re-acquire never holds must be caught)"

# The silent tooth: no cluster at all. The guard must abort (2) and say how
# to start one -- not pass, and not blame the wrong layer.
cleanup_fakes
out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_FAILOVER_FAST=1 bash "$GUARD" 2>&1)"
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
    echo "failover_adoption_live_smoke: done, 0 teeth failed"
    exit 0
fi
echo "failover_adoption_live_smoke: done, $fails teeth failed"
exit 1
