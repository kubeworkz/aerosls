#!/usr/bin/env bash
# tests/failover_2node_deadline_smoke.sh — teeth for the observer deadline
# computed by both live failover guards:
#   tests/failover_2node_live_check.sh
#   tests/failover_adoption_live_check.sh
#
# The live guards must wait long enough for the kernel to declare a peer dead.
# That window is not a literal: it is
#
#   ceil((FAILOVER_RELAUNCH_GRACE_TICKS + FAILOVER_DECLARE_TICKS)
#        / CONSENSUS_TICK_HZ) + 60s
#
# read from kernel/failover.h and net/consensus.h. A timeout copied into a
# guard would drift when the kernel graces move, and the guard would then stop
# the cluster just before the declaration and blame the failure detector (the
# original live failure: a 90s window against a 120s worst case).
#
# This smoke perturbs temporary copies of those real headers and runs both
# guards' own deadline paths. It does not start a cluster, boot QEMU, or touch
# the source headers. The guards' test seam is
# AEROSLS_FAILOVER_PRINT_DEADLINE=1, which exits after the same derivation the
# live runs use.
#
# Teeth, run once per guard:
#   baseline              -> 180  (6000 + 6000 over 100 Hz, plus 60s margin)
#   perturbed failover    -> 213  (9001 relaunch, 401 dead, 5801 silence ->
#                                  declare 6202; 15203 ticks -> 153s + 60)
#   perturbed tick rate   -> 365  (same grace, 50 Hz: 15203/50 -> 305 + 60)
#   missing constant      -> ABORT (2), naming FAILOVER_RELAUNCH_GRACE_TICKS;
#                                  no silent fallback to a literal timeout
#
# Each mutation is checked before the guards run, so a tooth whose sed matched
# nothing fails as a mutation failure instead of passing against the pristine
# header.
#
# Source-only: bash, sed, awk and python3 (already a guard prerequisite), so
# run_source_smokes.sh picks it up on every push.
#
# Exit: 0 every tooth held, 1 a tooth failed, 2 a prerequisite is missing.
set -u
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
GUARD_2="$ROOT/tests/failover_2node_live_check.sh"
GUARD_3="$ROOT/tests/failover_adoption_live_check.sh"
FAILOVER_H="$ROOT/kernel/failover.h"
CONSENSUS_H="$ROOT/net/consensus.h"

[ -f "$GUARD_2" ] || { echo "ABORT: $GUARD_2 not found." >&2; exit 2; }
[ -f "$GUARD_3" ] || { echo "ABORT: $GUARD_3 not found." >&2; exit 2; }
[ -f "$FAILOVER_H" ] || { echo "ABORT: $FAILOVER_H not found." >&2; exit 2; }
[ -f "$CONSENSUS_H" ] || { echo "ABORT: $CONSENSUS_H not found." >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 is needed by the guards' header parser." >&2
    exit 2
}
command -v sed >/dev/null 2>&1 || { echo "ABORT: sed not found." >&2; exit 2; }
command -v awk >/dev/null 2>&1 || { echo "ABORT: awk not found." >&2; exit 2; }

W="$(mktemp -d)" || { echo "ABORT: mktemp -d failed." >&2; exit 2; }
cleanup() { rm -rf "$W"; }
trap cleanup EXIT

fails=0

# run_deadline <guard> <failover-header> <consensus-header>
#   A guard's test-only print mode: it performs the same live derivation and
#   exits 0 before it looks for a cluster.
run_deadline() {
    AEROSLS_FAILOVER_FAST=0 \
    AEROSLS_FAILOVER_PRINT_DEADLINE=1 \
    AEROSLS_FAILOVER_HEADER="$2" \
    AEROSLS_CONSENSUS_HEADER="$3" \
    bash "$1" 2>&1
}

# set_define <file> <NAME> <value>
#   Replace the numeric value of one simple `#define NAME <n>u` line. The
#   headers' other constants (including FAILOVER_DECLARE_TICKS' expression)
#   remain untouched, so the guard must follow the reference itself.
set_define() {
    local file="$1" name="$2" value="$3"
    sed -i -E \
        "s/^(#[[:space:]]*define[[:space:]]+${name}[[:space:]]+)[0-9]+[uUlL]+/\1${value}u/" \
        "$file"
}

# define_value <file> <NAME>
#   Read back a numeric #define for the vacuity check.
define_value() {
    awk -v name="$2" '
        $1 == "#define" && $2 == name {
            value = $3
            sub(/[uUlL]+$/, "", value)
            print value
            exit
        }
    ' "$1"
}

expect_define() {
    local label="$1" file="$2" name="$3" want="$4" got
    got="$(define_value "$file" "$name")"
    if [ "$got" != "$want" ]; then
        echo "TOOTH FAIL $label — mutation of $name did not land (got '${got:-<empty>}', wanted $want)"
        fails=$((fails + 1))
        return 1
    fi
    return 0
}

expect_deadline() {
    local label="$1" want="$2" guard="$3" failover="$4" consensus="$5" out rc
    out="$(run_deadline "$guard" "$failover" "$consensus")"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "TOOTH FAIL $label — guard exit $rc, expected 0"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
        return
    fi
    if [ "$out" != "$want" ]; then
        echo "TOOTH FAIL $label — guard computed '$out', expected '$want'"
        fails=$((fails + 1))
        return
    fi
    echo "TOOTH OK   $label — computed $out"
}

expect_abort() {
    local label="$1" want="$2" guard="$3" failover="$4" consensus="$5" out rc
    out="$(run_deadline "$guard" "$failover" "$consensus")"
    rc=$?
    if [ "$rc" -ne 2 ]; then
        echo "TOOTH FAIL $label — guard exit $rc, expected 2"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"$want"*) echo "TOOTH OK   $label — aborted and said: $want" ;;
        *)
            echo "TOOTH FAIL $label — exit 2 was right, but the output did not mention: $want"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1)) ;;
    esac
}

# ─── 1. prepare the perturbed headers once, for both guards ────────────────
# FAILOVER_DECLARE_TICKS is deliberately left as the header's expression:
# each guard must resolve FAILOVER_DEAD_TICKS + FAILOVER_SILENCE_GRACE_TICKS
# itself, not carry a second copy of 6000.
cp "$FAILOVER_H" "$W/failover.h" || exit 2
set_define "$W/failover.h" FAILOVER_RELAUNCH_GRACE_TICKS 9001
set_define "$W/failover.h" FAILOVER_DEAD_TICKS 401
set_define "$W/failover.h" FAILOVER_SILENCE_GRACE_TICKS 5801
expect_define "failover mutation" "$W/failover.h" FAILOVER_RELAUNCH_GRACE_TICKS 9001 || true
expect_define "failover mutation" "$W/failover.h" FAILOVER_DEAD_TICKS 401 || true
expect_define "failover mutation" "$W/failover.h" FAILOVER_SILENCE_GRACE_TICKS 5801 || true

cp "$CONSENSUS_H" "$W/consensus-50.h" || exit 2
set_define "$W/consensus-50.h" CONSENSUS_TICK_HZ 50
expect_define "tick mutation" "$W/consensus-50.h" CONSENSUS_TICK_HZ 50 || true

cp "$FAILOVER_H" "$W/failover-missing.h" || exit 2
sed -i -E '/^#[[:space:]]*define[[:space:]]+FAILOVER_RELAUNCH_GRACE_TICKS/d' \
    "$W/failover-missing.h"
if grep -qE '^#[[:space:]]*define[[:space:]]+FAILOVER_RELAUNCH_GRACE_TICKS' \
    "$W/failover-missing.h"; then
    echo "TOOTH FAIL missing constant — the deletion did not land"
    fails=$((fails + 1))
    missing_ok=0
else
    missing_ok=1
fi

# ─── 2. both guards read the same constants the same way ───────────────────
check_guard() {
    local guard="$1" label="$2"
    expect_deadline "$label baseline" 180 "$guard" "$FAILOVER_H" "$CONSENSUS_H"
    expect_deadline "$label perturbed failover graces" 213 \
        "$guard" "$W/failover.h" "$CONSENSUS_H"
    expect_deadline "$label perturbed tick rate" 365 \
        "$guard" "$W/failover.h" "$W/consensus-50.h"
    if [ "${missing_ok:-0}" -eq 1 ]; then
        expect_abort "$label missing relaunch grace" "FAILOVER_RELAUNCH_GRACE_TICKS" \
            "$guard" "$W/failover-missing.h" "$CONSENSUS_H"
    fi
}

check_guard "$GUARD_2" "2-node"
check_guard "$GUARD_3" "3-node"

echo
if [ "$fails" -eq 0 ]; then
    echo "failover_2node_deadline_smoke: done, 0 teeth failed"
else
    echo "failover_2node_deadline_smoke: done, $fails teeth failed"
fi
exit $(( fails > 0 ))
