#!/usr/bin/env bash
# tests/go_thread_gate_args_check_smoke.sh — teeth for
# tests/go_thread_gate_args_check.sh, without Go or strace.
#
# The guard accepts GO_THREAD_GATE_ARGS_ARTIFACT so a temp copy can be
# mutated. This smoke proves six independent teeth:
#   1. clone rows that drop a measured flag (and add CHILD_CLEARTID);
#   2. clone rows that gain an extra namespace flag;
#   3. a futex row that uses non-private WAIT;
#   4. an epoll_pwait probe with a non-zero timeout;
#   5. an openat call that does not use AT_FDCWD;
#   6. an rt_sigaction install that gains SA_NODEFER.
#
# Each mutation must make the guard exit 1 (violated), not exit 2 (missing
# artifact) and not pass. The unmutated copy must pass.
#
# Run:  bash tests/go_thread_gate_args_check_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ART="${GO_THREAD_GATE_ARGS_ARTIFACT:-tools/go_thread_gate_args_census.txt}"
GUARD=tests/go_thread_gate_args_check.sh

if [ ! -f "$ART" ]; then
    echo "ABORT: $ART missing — run tools/go_thread_gate_census.sh first" >&2
    exit 2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/go-thread-gate-args-smoke.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
copy="$tmp/artifact.txt"

pass=0
fail=0
ok()  { echo "ok:   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }

run_guard() {
    GO_THREAD_GATE_ARGS_ARTIFACT="$copy" bash "$GUARD" >"$tmp/guard.out" 2>&1
}

expect_pass() {
    if run_guard; then
        ok "$1"
    else
        bad "$1 (guard rejected the baseline)"
        sed 's/^/      /' "$tmp/guard.out"
    fi
}

expect_tooth() {
    # VACUITY GUARD. A sed whose row pattern stopped matching leaves the copy
    # pristine and the guard passes; the tooth would then blame the guard for
    # the miss. Prove the mutation landed first.
    if cmp -s "$copy" "$ART"; then
        bad "$1 — mutation did not apply: the artifact copy is unchanged, so the guard would see the pristine rows"
        return
    fi
    set +e
    run_guard
    rc=$?
    set -e
    if [ "$rc" -eq 1 ]; then
        ok "$1 (guard exited 1)"
    elif [ "$rc" -eq 0 ]; then
        bad "$1 (guard accepted the mutation)"
        sed 's/^/      /' "$tmp/guard.out"
    else
        bad "$1 (guard exited $rc; expected 1)"
        sed 's/^/      /' "$tmp/guard.out"
    fi
}

cp "$ART" "$copy"
expect_pass "baseline artifact"

# Tooth 1: clone rows lose CLONE_SETTLS and gain CHILD_CLEARTID.
cp "$ART" "$copy"
sed -i '/^clone(/s/CLONE_SETTLS/CLONE_CHILD_CLEARTID/' "$copy"
expect_tooth "clone shape mutation"

# Tooth 2: clone rows gain a flag outside the measured thread shape.
cp "$ART" "$copy"
sed -i '/^clone(/s/CLONE_SETTLS/CLONE_SETTLS|CLONE_NEWPID/' "$copy"
expect_tooth "clone extra-flag mutation"

# Tooth 3: a futex row stops being the private WAIT operation.
cp "$ART" "$copy"
sed -i '/^futex(/s/FUTEX_WAIT_PRIVATE/FUTEX_WAIT/' "$copy"
expect_tooth "futex non-private mutation"

# Tooth 4: epoll_pwait rows no longer have timeout 0.
cp "$ART" "$copy"
sed -i '/^epoll_pwait(/s/, 0, NULL, 0)/, 1, NULL, 0)/' "$copy"
expect_tooth "epoll timeout mutation"

# Tooth 5: the first openat row no longer uses AT_FDCWD.
cp "$ART" "$copy"
sed -i '/^openat(/s/AT_FDCWD/5/' "$copy"
expect_tooth "openat dirfd mutation"

# Tooth 6: an rt_sigaction install gains a flag outside the measured shape.
cp "$ART" "$copy"
sed -i '/^rt_sigaction(/s/sa_flags=SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO/sa_flags=SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO|SA_NODEFER/' "$copy"
expect_tooth "rt_sigaction flags mutation"

# The pristine copy must still pass after the teeth.
cp "$ART" "$copy"
expect_pass "restored artifact"

printf 'go_thread_gate_args_check_smoke: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
