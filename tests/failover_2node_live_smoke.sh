#!/usr/bin/env bash
# tests/failover_2node_live_smoke.sh — drive tests/failover_2node_live_check.sh
# through every verdict WITHOUT a real cluster, via fake nodes
# (tests/failover_2node_smoke_nodes.py).
#
# The property: in a 2-node cluster, when the leader dies the sole survivor
# must NOT become a second leader, must NOT adopt, and must NOT hold the
# partition's write lease. The kernel's majority quorum for 2 nodes is
# 2-of-2; nothing shrinks the roster on death; and failover recovery is
# gated on cluster_is_leader() — so a lone survivor can never be elected,
# never recovers, and can never win the lease quorum (the same
# stable_quorum_threshold feeds both the election and the per-partition
# lease). The guard gates on the survivor observing the death (its own
# declaration line), holding a checkpoint that carried the row (adoption
# was data-possible), never reporting LEADER for the whole watch window,
# never printing an Adopted/recovery line, serving the row still owned by
# the dead leader, AND — the write-lease layer — holding a lease row
# before the kill, keeping holds_lease=0 (partition_holds_write_lease()
# false) through the window, logging the [MMU-LEASE] strip on campaign,
# and never logging a restore (only lease quorum-achieved restores, and a
# lone 2-node survivor can never reach it).
#
# Teeth (each a distinct failure the guard must tell apart):
#   staysfollower -> PASS  (node 2 declares the death, stays FOLLOWER,
#                           never adopts, holds_lease stays 0, the
#                           MMU-LEASE strip is logged and never restored;
#                           the leader was really killed)
#   flipsleader   -> FAIL  (node 2 flips to LEADER after a delay — a late
#                           second leader; the never-LEADER watch must hold
#                           the full window to catch it)
#   adopts        -> FAIL  (node 2 prints the Adopted + recovery lines
#                           while staying FOLLOWER — a follower that
#                           recovered; the never-adopt log check must bite,
#                           pinning cluster_is_leader())
#   nolearn       -> FAIL  (the create announce never arrived)
#   nockpt        -> FAIL  (the checkpoint never carried the row)
#   nolease       -> FAIL  (the leader's `partition lease acquire` never
#                           holds — the lease layer is dead; the lease-held
#                           gate must bite, not silently skip the layer)
#   nolearnlease  -> FAIL  (the survivor never created its lease row — the
#                           lease-learn gate must bite, so the strip/restore
#                           gates cannot be vacuous)
#   leasestrip    -> FAIL  (the survivor never logged the MMU-LEASE strip
#                           — reads-only never engaged at the page-
#                           permission call site)
#   leaserestore  -> FAIL  (the survivor logged the strip AND a restore —
#                           write permission re-enabled without a lease
#                           quorum; the page-permission split-brain)
#   leasehold     -> FAIL  (the survivor reports holds_lease=1 after the
#                           death — partition_holds_write_lease() true;
#                           the page-level gate bypassed)
#   no-cluster    -> ABORT (no fakes at all — the guard must say how to
#                           start a cluster, exit 2)
#   latecreate    -> PASS  (the fakes are idled past EVERY wait the old
#                           fakes carried before the guard is even started,
#                           and the guard still drives the whole
#                           no-leader refusal for its own reason)
#
# The latecreate tooth exists because the FAKES were deciding verdicts. The
# fakes used to bound each wait with a fixed iteration count (200 x 0.05s =
# 10s for the create announce and the lease row, 600 x 0.05s = 30s for the
# death poll). Those clocks start when the FAKE process starts and run
# alongside the guard's own FAST-scaled gates (WAIT_NODES=10s, WAIT_LEARN=6s,
# WAIT_CKPT=6s, WAIT_OBSERVER=8s), and the learn one was exactly the guard's
# WAIT_NODES pre-create budget -- so on 2026-09-20 the sibling smoke failed
# its nockpt and lateflip teeth with "nodes 2/3 never learned partition"
# (exactly one follower empty, the other learned) while the mutations' own
# gates still had budget. Nothing about the kernel path was wrong: a
# fake-side timer expired during the guard's own cluster-forming budget. The
# fake's waits are now life-bounded (see the rule at the top of
# failover_2node_smoke_nodes.py); this tooth is the control for that: it
# idles the fakes past every old cap -- including the 30s one -- before
# running the guard in the plain staysfollower mode, so with a fake-side
# budget restored it fails at the learn gate, and with none it passes for
# its own reason (including the strip/restore assertions a PASS must carry).
# A run that wants to skip the idle (a tight local loop) can set
# AEROSLS_FAILOVER_SMOKE_LATE_IDLE=0.
#
# Source-only: needs nothing built — the fakes are python3 and the guard is
# bash + curl, so this runs in CI's verify job on every push via
# run_source_smokes.sh's glob.
set -u
cd "$(dirname "$0")/.."

GUARD="tests/failover_2node_live_check.sh"
FAKE="tests/failover_2node_smoke_nodes.py"
BASE=59600
# The latecreate tooth's idle, in seconds. 50 > every wait the pre-fix fakes
# could hold (the longest was 600 x 0.05s = 30s), so the tooth's point holds
# by construction; 0 disables the idle (it does NOT disable the tooth, which
# then still proves the plain staysfollower path).
LATE_IDLE="${AEROSLS_FAILOVER_SMOKE_LATE_IDLE:-50}"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found or not executable."; exit 2; }
[ -f "$FAKE" ]  || { echo "ABORT: $FAKE not found."; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 needed to stand up fake nodes."; exit 2; }
command -v curl    >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

# The guard's cluster dir is a PRIVATE temp dir, never the repo's cluster/.
# The guard kills whatever cluster.pids names, and the deploy gate runs this
# smoke on the host where the real cluster is live -- with a shared
# cluster/cluster.pids the only safe move was to refuse, and a smoke that
# cannot run proves nothing (the 2026-09-13 deploy gate failed on exactly
# that). AEROSLS_CLUSTER_DIR is exported so EVERY guard call below, the
# silent no-cluster tooth included, resolves its pid file here and can never
# read a real cluster's.
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
    [ -n "$FAKE_PIDS" ] && kill $FAKE_PIDS 2>/dev/null || true
    FAKE_PIDS=""
    if [ -f "$PID_FILE" ]; then
        while read -r _ pid; do
            [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null || true
        done < "$PID_FILE"
        rm -f "$PID_FILE"
    fi
    wait 2>/dev/null
    return 0
}
cleanup() { cleanup_fakes; [ -n "$STATE" ] && rm -rf "$STATE"; rm -rf "$SMOKE_CLUSTER_DIR"; return 0; }
trap cleanup EXIT

# $1 = mode, $2 = expected exit, $3 = a phrase the output must contain,
# $4 = label, $5 = optional seconds to idle the fakes before the guard runs
#      (the latecreate tooth's control: nothing about the kernel path
#      changes, so the tooth can only fail if a FAKE-side clock decided it)
tooth() {
    local mode="$1" want_rc="$2" want_txt="$3" label="$4" delay="${5:-0}"
    local out rc
    cleanup_fakes
    [ -n "$STATE" ] && rm -rf "$STATE"
    STATE="$(mktemp -d)"
    echo "$mode" > "$STATE/mode"
    echo 1 > "$STATE/leader"   # node 1 is the leader

    # A slow python cold start on a loaded runner must not abort a tooth:
    # retry the pair (up to 3 times), keep each attempt's stderr in $STATE
    # so a real fake crash is diagnosable instead of vanishing to /dev/null.
    local lpid fpid attempt
    FAKE_PIDS=""
    for attempt in 1 2 3; do
        [ -n "$FAKE_PIDS" ] && kill $FAKE_PIDS 2>/dev/null || true
        FAKE_PIDS=""
        python3 "$FAKE" 1 leader   $((BASE+1)) "$STATE" "$STATE" >"$STATE/fake1.err" 2>&1 &
        lpid=$!
        python3 "$FAKE" 2 follower $((BASE+2)) "$STATE" "$STATE" >"$STATE/fake2.err" 2>&1 &
        fpid=$!
        FAKE_PIDS="$lpid $fpid"

        local up=0
        for _ in $(seq 1 60); do
            if curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/health" \
                && curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+2))/api/health"; then
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
    printf '1 %s\n2 %s\n' "$lpid" "$fpid" > "$PID_FILE"

    if [ "$delay" != 0 ]; then
        echo "  note: idling the fakes ${delay}s before the guard runs (the tooth's point)"
        sleep "$delay"
    fi

    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_FAILOVER_FAKE=1 \
           AEROSLS_FAILOVER_FAST=1 AEROSLS_LOG_DIR=$STATE bash "$GUARD" 2>&1)"
    rc=$?

    # The hygiene checks below ask "did this PASS mean what it claims", so
    # they may only run when the guard actually passed. Running them after a
    # failure reports the consequence instead of the cause: an abort at an
    # earlier gate (the learn gate, say) leaves the leader alive, and that
    # would be reported as "guard passed, but the leader was never killed" --
    # hiding the guard's real verdict. The control run for the latecreate
    # tooth hit exactly that, so the gate is explicit here.
    local guard_passed=0
    case "$out" in *PASS*) guard_passed=1 ;; esac
    if [ "$mode" = "staysfollower" ] && [ "$rc" -eq 0 ] && [ "$guard_passed" -eq 1 ]; then
        # A PASS that never killed the leader proves nothing: the whole
        # point is that the refusal happens AFTER the leader dies.
        if kill -0 "$lpid" 2>/dev/null; then
            echo "TOOTH FAIL $label — guard passed, but the leader was never killed (pid $lpid still alive)"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1))
            return
        fi
        # A PASS that never engaged the write-lease layer proves nothing
        # either: the strip must be in the survivor's log, and a restore
        # must not be.
        if ! grep -q "MMU-LEASE] partition 1: page permissions force_read_only=1" \
                "$STATE/node2.log" 2>/dev/null; then
            echo "TOOTH FAIL $label — guard passed, but the survivor never logged the MMU-LEASE strip (the lease layer never engaged)"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1))
            return
        fi
        if grep -q "MMU-LEASE] partition 1: page permissions force_read_only=0" \
                "$STATE/node2.log" 2>/dev/null; then
            echo "TOOTH FAIL $label — guard passed, but the survivor's log contains a lease restore (impossible for a lone 2-node survivor)"
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

tooth staysfollower  0 "PASS"      "staysfollower -> PASS (the sole survivor stayed non-LEADER, never adopted, never held the write lease; the leader was really killed)"
tooth staysfollower  0 "PASS"      "latecreate -> PASS (the fakes idled ${LATE_IDLE}s first — past every wait the old fakes carried — and the guard still drove the whole no-leader refusal)" "$LATE_IDLE"
tooth flipsleader    1 "became LEADER" "flipsleader -> FAIL (a late flip to LEADER must be caught by the never-LEADER watch)"
tooth adopts         1 "ALSO recovered" "adopts -> FAIL (a follower that recovered must be caught by the never-adopt check)"
tooth nolearn        1 "never learned"  "nolearn -> FAIL (the create announce did not arrive)"
tooth nockpt         1 "no checkpoint"  "nockpt -> FAIL (the checkpoint never carried the row)"
tooth nolease        1 "never HOLD the write lease" "nolease -> FAIL (the lease layer must be live before the kill; a dead lease layer is a broken guard, not a pass)"
tooth nolearnlease   1 "never created a lease row" "nolearnlease -> FAIL (the survivor must hold a lease row before the kill, or the strip/restore gates are vacuous)"
tooth leasestrip     1 "never stripped write permission" "leasestrip -> FAIL (the survivor must log the MMU-LEASE strip on campaign — reads-only at the page-permission call site)"
tooth leaserestore   1 "RESTORED write permission" "leaserestore -> FAIL (a restore on a lone 2-node survivor is the page-permission split-brain)"
tooth leasehold      1 "holds_lease=1" "leasehold -> FAIL (partition_holds_write_lease() must stay false on the survivor)"

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
    echo "failover_2node_live_smoke: done, 0 teeth failed"
else
    echo "failover_2node_live_smoke: done, $fails teeth failed"
fi
exit $(( fails > 0 ))
