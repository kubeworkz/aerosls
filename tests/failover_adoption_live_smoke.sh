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
#   migrate_lost1 -> PASS (the FIRST step-11 lease campaign's packet is
#                         lost on the wire for the OBSERVER: it only ever
#                         sees the row of the second campaign, so the
#                         lease-row gate must re-drive the campaign and
#                         pass -- its control is the SAME mode with the
#                         re-drive disabled, which must still FAIL the
#                         gate)
#   migrate_norow -> FAIL (the observer's lease RX path is broken: the
#                         campaign never reaches it at all, so even the
#                         re-driven campaign leaves no row -- the gate
#                         must still bite after spending every attempt)
#   svc_stable  -> PASS  (step 12: the guard registers a service twin on the
#                         owner, re-registers it on the adopter, and when the
#                         resurrected owner re-announces the SAME name the
#                         claim-class resolver in service_remote_learn()
#                         REJECTS it -- the observer's cache keeps the
#                         adopter, every node still resolves to the adopter,
#                         and the kernel logs the rejection)
#   svc_flap    -> FAIL  (the survivors APPLY the stale service claim -- the
#                         old "last announce wins" bug -- and the name flaps
#                         back to the resurrected owner; the step-12 no-flap
#                         gate must bite)
#   svc_nostale -> FAIL  (the stale service re-announce never fires, so the
#                         reject never logs; the step-12 reject gate must bite)
#   silent      -> ABORT (no cluster.pids at all; the guard must say how to
#                         start one)
#   latecreate  -> PASS  (the fakes are idled past EVERY wait the old fakes
#                         carried before the guard is even started, and the
#                         create announce still reaches both followers)
#   leadergap   -> PASS  (the leader is UNREACHABLE for a bounded window
#                         right after the create -- a busy leader, not a
#                         dead one -- and no survivor may declare it dead)
#
# The last five matter as much as the first: a guard that cannot fail is a
# guard that has never been seen to work. And a PASS that never killed the
# leader would prove nothing, so the adopted tooth asserts the leader's
# fake pid is actually dead after the run.
#
# The latecreate tooth exists because the FAKES were deciding verdicts. The
# fakes used to bound each wait with a fixed iteration count (200 x 0.05s =
# 10s to learn the create announce, 600 x 0.05s = 30s for the death/claim/
# migrate events, 900 x 0.05s = 45s for the service declares). Those clocks
# start when the FAKE process starts and run alongside the guard's own
# FAST-scaled gates, and the learn one was exactly the guard's WAIT_NODES
# pre-create budget (10s) -- so on 2026-09-20 CI failed nockpt and lateflip
# with "nodes 2/3 never learned partition": exactly one follower empty, the
# other learned, the mutation's own gate never reached. Nothing about the
# kernel path was wrong; a fake-side timer expired during the guard's own
# cluster-forming budget. The fake's waits are now life-bounded (see the
# rule at the top of failover_adoption_smoke_nodes.py), and this tooth is
# the control for it: it idles the fakes ${LATE_IDLE}s -- past every old cap,
# including the 45s one -- before running the guard in the plain adopted
# mode, so with a fake-side budget restored it fails at that same learn
# gate, and with none it passes for its own reason. A run that wants to skip
# the idle (a tight local loop) can set AEROSLS_FAILOVER_SMOKE_LATE_IDLE=0.
#
# The leadergap tooth exists because TIMEOUTS were deciding verdicts -- the
# second face of the same rule. The fakes' servers were single-threaded, so
# concurrent pollers queued behind each other; every client has a short
# timeout (the survivor's own 0.5s leader poll, the guard's curl --max-time
# 3, this smoke's --max-time 1), and a survivor read a few late answers as
# DEATH -- three failed polls, ~2.3s -- then transitioned, which made the
# guard read its row as "never learned". That is not hypothetical: it is
# what CI hit on 2026-09-20, and what three smokes at once reproduced here.
# Two changes fix it, and this tooth is the control for both: the fakes now
# serve concurrently (ThreadingHTTPServer, so nothing queues up to time out
# behind) and death needs DEAD_SILENCE=4s of CONTINUOUS unreachability,
# matching the property being modelled (failover_tick's "silent >=
# FAILOVER_DEAD_TICKS", a duration). The tooth makes the leader answer
# /api/health only ${LEADER_GAP}s after the create: longer than the old
# 3-strikes burst (~2.3s) and shorter than the silence floor, so it fails
# with the old rule and passes with the new one by construction.
# AEROSLS_FAILOVER_SMOKE_LEADER_GAP tunes the window.
#
# Why migrate_lost1/migrate_norow exist (the same false RED the 2-node
# sibling paid for on 2026-09-25): the step-11 lease row reaches the
# observer over a packet hop, and the lease-row gate used to spend a single
# bounded window (WAIT_LEASE, 8s in FAST) on it before declaring "node 3
# never created a lease row" -- a message that implicates the campaign's
# RX path for a hop that had not landed. The fakes had a genuine bug in the
# same place -- a lease row caught mid-write killed the observer's RX
# thread, and a dead thread never logs the row -- fixed in
# failover_adoption_smoke_nodes.py (whole-row os.replace write, the row
# carries the campaign term, retry-tolerant read exactly like
# read_created()'s rule). The guard side now re-drives instead of guessing:
# up to AEROSLS_FAILOVER_LEASE_ATTEMPTS fresh WAIT_LEASE windows, each
# re-drive admitted only while the adopter still HOLDS the write lease, so
# the same trigger is repeated and the scenario is not changed. The pair of
# teeth pins that this is a fix and not a blindfold: migrate_lost1 passes
# only because the re-drive happened (the tooth asserts the guard printed
# it), with AEROSLS_FAILOVER_LEASE_ATTEMPTS=1 -- the pre-fix behaviour --
# the same lost hop is still the gate's honest FAIL, and migrate_norow (a
# truly broken RX path) still fails the gate with the re-drive on.
#
# Runs on a port base far from any real cluster and never touches a running
# one: the guard's pid file lives in a private temp dir (AEROSLS_CLUSTER_DIR),
# so it runs beside a live cluster, and it asserts at the end that the repo's
# cluster/cluster.pids is unchanged.
#
# Exit: 0 all teeth bit, 1 a tooth failed, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

GUARD=tests/failover_adoption_live_check.sh
FAKE=tests/failover_adoption_smoke_nodes.py
BASE=59200
# The latecreate tooth's idle, in seconds. 50 > every wait the pre-fix fakes
# could hold (the longest was 900 x 0.05s = 45s), so the tooth's point holds
# by construction; 0 disables the idle (it does NOT disable the tooth, which
# then still proves the plain adopted path).
LATE_IDLE="${AEROSLS_FAILOVER_SMOKE_LATE_IDLE:-50}"
# The leadergap tooth's unreachability window, in seconds. It must be
# LONGER than the "3 failed polls" burst the fake's old death rule fired on
# (~2.3s: four 0.5s timeouts) and SHORTER than the sustained-silence floor
# that replaced it (DEAD_SILENCE=4s in the fake), so it fails with the old
# rule and passes with the new one by construction.
LEADER_GAP="${AEROSLS_FAILOVER_SMOKE_LEADER_GAP:-3}"

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

# A failing tooth must carry its own diagnosis: a fake that cracked shows up
# in its stderr, which would otherwise be deleted with $STATE unread. Never
# printed on a PASS.
dump_fake_err() {
    local f
    for f in "$STATE/fake1.err" "$STATE/fake2.err" "$STATE/fake3.err"; do
        [ -s "$f" ] || continue
        echo "           --- $(basename "$f") (last 20 lines) ---"
        tail -20 "$f" | sed 's/^/           /'
    done
    return 0
}

# $1 = mode, $2 = expected exit, $3 = a phrase the output must contain,
# $4 = label, $5 = optional seconds to idle the fakes before the guard runs
#      (the latecreate tooth's control: nothing about the kernel path
#      changes, so the tooth can only fail if a FAKE-side clock decided it)
# $6 = optional seconds of leader unreachability after the create (the
#      leadergap tooth's control: a bounded episode a real death rule
#      survives, but a "few failed polls" rule does not)
# $7 = optional extra environment for the guard call (the migrate_lost1
#      control runs the same mode with the re-drive disabled)
tooth() {
    local mode="$1" want_rc="$2" want_txt="$3" label="$4" delay="${5:-0}" gap="${6:-0}" extra_env="${7:-}"
    local out rc
    cleanup_fakes
    [ -n "$STATE" ] && rm -rf "$STATE"
    STATE="$(mktemp -d)"
    echo "$mode" > "$STATE/mode"
    echo 1 > "$STATE/leader"   # node 1 is the leader

    # A slow python cold start on a loaded runner must not abort a tooth:
    # retry the trio (up to 3 times), keep each attempt's stderr in $STATE
    # so a real fake crash is diagnosable instead of vanishing to /dev/null.
    local lpid f1pid f2pid attempt
    FAKE_PIDS=""
    for attempt in 1 2 3; do
        [ -n "$FAKE_PIDS" ] && kill $FAKE_PIDS 2>/dev/null || true
        FAKE_PIDS=""
        python3 "$FAKE" 1 leader   $((BASE+1)) "$STATE" "$STATE" >"$STATE/fake1.err" 2>&1 &
        lpid=$!
        python3 "$FAKE" 2 follower $((BASE+2)) "$STATE" "$STATE" >"$STATE/fake2.err" 2>&1 &
        f1pid=$!
        python3 "$FAKE" 3 follower $((BASE+3)) "$STATE" "$STATE" >"$STATE/fake3.err" 2>&1 &
        f2pid=$!
        FAKE_PIDS="$lpid $f1pid $f2pid"

        local up=0
        for _ in $(seq 1 60); do
            if curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/health" \
                && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+2))/api/health" \
                && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+3))/api/health"; then
                up=1
                break
            fi
            sleep 0.2
        done
        if [ "$up" -eq 1 ]; then
            break
        fi
        echo "  note: fake nodes not up on attempt $attempt (retrying)" >&2
    done
    printf '1 %s\n2 %s\n3 %s\n' "$lpid" "$f1pid" "$f2pid" > "$PID_FILE"

    if [ "$delay" != 0 ]; then
        echo "  note: idling the fakes ${delay}s before the guard runs (the tooth's point)"
        sleep "$delay"
    fi
    if [ "$gap" != 0 ]; then
        echo "  note: the leader will go unreachable for ${gap}s after the create (the tooth's point)"
        echo "$gap" > "$STATE/health_gap"
    fi

    # env with an empty $extra_env is a plain pass-through (the common case);
    # the migrate_lost1 control passes one assignment here.
    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_FAILOVER_FAKE=1 \
           AEROSLS_FAILOVER_FAST=1 AEROSLS_LOG_DIR=$STATE \
           env $extra_env bash "$GUARD" 2>&1)"
    rc=$?

    # The hygiene checks below ask "did this PASS mean what it claims", so
    # they may only run when the guard actually passed. Running them after a
    # failure reports the consequence instead of the cause: an abort at an
    # earlier gate (the learn gate, say) leaves the leader alive, and that
    # would be reported as "guard passed, but the leader was never killed" --
    # hiding the guard's real verdict.
    local guard_passed=0
    case "$out" in *PASS*) guard_passed=1 ;; esac
    # The PASS teeth: adopted and svc_stable (the plain paths) and
    # migrate_lost1 (the same step-11 path behind a flipped bit in the
    # wire). All must mean what they claim, so all carry the checks below.
    if { [ "$mode" = "adopted" ] || [ "$mode" = "svc_stable" ] \
            || [ "$mode" = "migrate_lost1" ]; } \
            && [ "$rc" -eq 0 ] && [ "$guard_passed" -eq 1 ]; then
        # A PASS that never killed the leader proves nothing: the whole
        # point is that the ADOPTION happens after the leader dies.
        if kill -0 "$lpid" 2>/dev/null; then
            echo "TOOTH FAIL $label — guard passed, but the leader was never killed (pid $lpid still alive)"
            printf '%s\n' "$out" | sed 's/^/           /'
            dump_fake_err
            fails=$((fails + 1))
            return
        fi
        # migrate_lost1's PASS must mean the re-drive really happened: if
        # the fake had logged the row on the FIRST campaign the tooth would
        # be vacuous (it would pass on the pre-re-drive guard too), so the
        # guard's own re-drive note has to be in its output.
        if [ "$mode" = "migrate_lost1" ] \
                && ! printf '%s\n' "$out" | grep -q "re-driving the campaign"; then
            echo "TOOTH FAIL $label — guard passed, but it never re-drove the campaign (the lost first campaign was not modelled, so the tooth proves nothing)"
            printf '%s\n' "$out" | sed 's/^/           /'
            dump_fake_err
            fails=$((fails + 1))
            return
        fi
    fi
    cleanup_fakes

    if [ "$rc" != "$want_rc" ]; then
        echo "TOOTH FAIL $label — guard exit $rc, expected $want_rc"
        printf '%s\n' "$out" | sed 's/^/           /'
        dump_fake_err
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"$want_txt"*) echo "TOOTH OK   $label — exit $rc, said: $want_txt" ;;
        *)
            echo "TOOTH FAIL $label — exit $rc was right, but the output did not mention: $want_txt"
            printf '%s\n' "$out" | sed 's/^/           /'
            dump_fake_err
            fails=$((fails + 1)) ;;
    esac
}

tooth adopted          0 "PASS"      "adopted -> PASS (and the leader was really killed)"
tooth adopted          0 "PASS"      "latecreate -> PASS (the fakes idled ${LATE_IDLE}s first — past every wait the old fakes carried — and the create announce still reached both followers)" "$LATE_IDLE"
tooth adopted          0 "PASS"      "leadergap -> PASS (the leader was unreachable for ${LEADER_GAP}s after the create — a bounded episode, not silence — and no survivor declared it dead)" 0 "$LEADER_GAP"
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
# The re-drive gate, both sides of it. migrate_lost1 drops the FIRST
# step-11 campaign on the wire for the observer, so its row only exists
# because the guard re-drove -- the sibling 2-node guard went red in CI on
# 2026-09-25 on exactly this shape, treating a hop that had not landed as
# "never created a lease row". The control runs the same mode with the
# re-drive disabled (AEROSLS_FAILOVER_LEASE_ATTEMPTS=1, i.e. the old
# behaviour) and must still FAIL the gate. migrate_norow is the other
# direction with the re-drive ON: a genuinely broken RX path eats every
# attempt and still fails.
tooth migrate_lost1 0 "PASS" "migrate_lost1 -> PASS (the first step-11 campaign's packet was lost on the wire for the observer; the re-drive created its lease row instead of failing the RX path)" 0 0
tooth migrate_lost1 1 "never created a lease row" "migrate_lost1 control -> FAIL with the re-drive disabled (AEROSLS_FAILOVER_LEASE_ATTEMPTS=1: the same lost hop is the gate's honest RED, so the tooth above is not vacuous)" 0 0 "AEROSLS_FAILOVER_LEASE_ATTEMPTS=1"
tooth migrate_norow 1 "never created a lease row" "migrate_norow -> FAIL (the observer's lease RX path is broken: no row appears, however often the guard re-drives, so the gate still bites)" 0 0
tooth svc_stable  0 "PASS"   "svc_stable -> PASS (the service twin survives the resurrected owner's stale re-announce: rejected on the observer, every node still resolves to the adopter)"
tooth svc_flap    1 "service registration FLAPPED" "svc_flap -> FAIL (the old last-wins apply of the stale service claim must be caught by step 12)"
tooth svc_nostale 1 "never rejected" "svc_nostale -> FAIL (a stale service re-announce that never fires must be caught by step 12)"

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
    echo "failover_adoption_live_smoke: done, 0 teeth failed"
    exit 0
fi
echo "failover_adoption_live_smoke: done, $fails teeth failed"
exit 1
