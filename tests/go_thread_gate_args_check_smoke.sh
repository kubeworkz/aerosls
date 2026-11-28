#!/usr/bin/env bash
# tests/go_thread_gate_args_check_smoke.sh — teeth for
# tests/go_thread_gate_args_check.sh, without Go or strace.
#
# The guard accepts GO_THREAD_GATE_ARGS_ARTIFACT so a temp copy can be
# mutated, and GO_THREAD_GATE_FIXTURE so the fixture tree can be drifted.
# This smoke proves seven independent teeth:
#   1. clone rows that drop a measured flag (and add CHILD_CLEARTID);
#   2. clone rows that gain an extra namespace flag;
#   3. a futex row that uses non-private WAIT;
#   4. an epoll_pwait probe with a non-zero timeout;
#   5. openat calls that do not use AT_FDCWD (the same rows also clear the
#      two measured input/output shape clauses, so all three must fire);
#   6. an rt_sigaction install that gains SA_NODEFER;
#   7. a fixture file that no longer hashes to the artifact's source-sha256.
#
# A tooth is green only when the guard newly fails on the clause that mutation
# names. Exit 1 alone is not enough: the guard also reconciles the tree (table
# rows, sidecar arms, the gate doc), and a tree-phase failure must not stand in
# for an artifact tooth. So the smoke records the guard's FAIL lines on the
# pristine artifact and subtracts them; what remains must be exactly the
# tooth's named failure(s) — no named clause missing, and no extra failure.
#
# Run:  bash tests/go_thread_gate_args_check_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ART="${GO_THREAD_GATE_ARGS_ARTIFACT:-tools/go_thread_gate_args_census.txt}"
GUARD=tests/go_thread_gate_args_check.sh
FIXTURE="${GO_THREAD_GATE_FIXTURE:-tools/go_thread_gate}"

if [ ! -f "$ART" ]; then
    echo "ABORT: $ART missing — run tools/go_thread_gate_census.sh first" >&2
    exit 2
fi
if [ ! -f "$FIXTURE/main.go" ] || [ ! -f "$FIXTURE/go.mod" ]; then
    echo "ABORT: the fixture sources are missing from $FIXTURE" >&2
    exit 2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/go-thread-gate-args-smoke.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
copy="$tmp/artifact.txt"
drift="$tmp/fixture"
fixture="$FIXTURE"

pass=0
fail=0
ok()  { echo "ok:   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }

run_guard() {
    GO_THREAD_GATE_ARGS_ARTIFACT="$copy" \
    GO_THREAD_GATE_FIXTURE="$fixture" \
        bash "$GUARD" >"$tmp/guard.out" 2>&1
}

# The guard's named failures, one per line, with the "FAIL: " prefix removed.
guard_failures() {
    awk '/^FAIL: / { sub(/^FAIL: /, ""); print }' "$tmp/guard.out"
}

expect_pass() {
    if run_guard; then
        ok "$1"
    else
        bad "$1 (guard rejected the baseline)"
        sed 's/^/      /' "$tmp/guard.out"
    fi
}

# judge_guard <label> <expected-failure-ERE>...
# The guard must exit 1, and the failures the mutation introduces (the FAIL
# lines that were not already there for the pristine artifact) must match the
# given patterns exactly: every pattern present, every new failure explained.
judge_guard() {
    local label="$1"
    shift
    local rc=0 missing=0 unexpected=0 p line matched

    set +e
    run_guard
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        bad "$label (guard accepted the mutation)"
        sed 's/^/      /' "$tmp/guard.out"
        return
    fi
    if [ "$rc" -ne 1 ]; then
        bad "$label (guard exited $rc; expected 1)"
        sed 's/^/      /' "$tmp/guard.out"
        return
    fi

    guard_failures > "$tmp/actual.fail"
    if [ -s "$tmp/baseline.fail" ]; then
        grep -Fxv -f "$tmp/baseline.fail" "$tmp/actual.fail" > "$tmp/new.fail" || true
    else
        cp "$tmp/actual.fail" "$tmp/new.fail"
    fi

    for p in "$@"; do
        grep -Eq "$p" "$tmp/new.fail" || {
            missing=$((missing + 1))
            echo "      missing named clause: /$p/"
        }
    done
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        matched=0
        for p in "$@"; do
            if printf '%s\n' "$line" | grep -Eq "$p"; then
                matched=1
                break
            fi
        done
        if [ "$matched" -eq 0 ]; then
            unexpected=$((unexpected + 1))
            echo "      failure outside the tooth's clauses: $line"
        fi
    done < "$tmp/new.fail"

    if [ "$missing" -eq 0 ] && [ "$unexpected" -eq 0 ] && [ -s "$tmp/new.fail" ]; then
        ok "$label (guard newly failed on its named clause only)"
    else
        bad "$label (guard exited 1, but not only for its named clause: $missing named clause(s) missing, $unexpected extra)"
        sed 's/^/      /' "$tmp/guard.out"
    fi
}

expect_tooth() {
    local label="$1"
    shift

    # VACUITY GUARD. A sed whose row pattern stopped matching leaves the copy
    # pristine and the guard passes; the tooth would then blame the guard for
    # the miss. Prove the mutation landed first.
    if cmp -s "$copy" "$ART"; then
        bad "$label — mutation did not apply: the artifact copy is unchanged, so the guard would see the pristine rows"
        return
    fi
    judge_guard "$label" "$@"
}

expect_fixture_tooth() {
    local label="$1"
    shift

    # The drift tooth mutates the fixture, not the artifact: prove the copy it
    # hands the guard differs from the committed sources before judging it.
    if cmp -s "$drift/main.go" "$FIXTURE/main.go" && cmp -s "$drift/go.mod" "$FIXTURE/go.mod"; then
        bad "$label — drift did not apply: the fixture copy is unchanged, so the guard would see the measured sources"
        return
    fi
    judge_guard "$label" "$@"
}

cp "$ART" "$copy"
expect_pass "baseline artifact"
guard_failures > "$tmp/baseline.fail"

# Tooth 1: clone rows lose CLONE_SETTLS and gain CHILD_CLEARTID.
cp "$ART" "$copy"
sed -i '/^clone(/s/CLONE_SETTLS/CLONE_CHILD_CLEARTID/' "$copy"
expect_tooth "clone shape mutation" \
    '^a clone row does not have exactly the measured seven-flag thread shape$'

# Tooth 2: clone rows gain a flag outside the measured thread shape.
cp "$ART" "$copy"
sed -i '/^clone(/s/CLONE_SETTLS/CLONE_SETTLS|CLONE_NEWPID/' "$copy"
expect_tooth "clone extra-flag mutation" \
    '^a clone row does not have exactly the measured seven-flag thread shape$'

# Tooth 3: a futex row stops being the private WAIT operation.
cp "$ART" "$copy"
sed -i '/^futex(/s/FUTEX_WAIT_PRIVATE/FUTEX_WAIT/' "$copy"
expect_tooth "futex non-private mutation" \
    "^futex ops are '.*', expected 'FUTEX_WAIT_PRIVATE;FUTEX_WAKE_PRIVATE'$"

# Tooth 4: epoll_pwait rows no longer have timeout 0.
cp "$ART" "$copy"
sed -i '/^epoll_pwait(/s/, 0, NULL, 0)/, 1, NULL, 0)/' "$copy"
expect_tooth "epoll timeout mutation" \
    '^an epoll_pwait row has a non-zero timeout or a sigmask$'

# Tooth 5: openat rows no longer use AT_FDCWD. The dirfd clause and the two
# measured shape clauses read the same rows, so all three must go red.
cp "$ART" "$copy"
sed -i '/^openat(/s/AT_FDCWD/5/' "$copy"
expect_tooth "openat dirfd mutation" \
    '^an openat row does not use AT_FDCWD$' \
    "^the fixture's input openat shape is missing$" \
    "^the fixture's output openat shape is missing$"

# Tooth 6: an rt_sigaction install gains a flag outside the measured shape.
cp "$ART" "$copy"
sed -i '/^rt_sigaction(/s/sa_flags=SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO/sa_flags=SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO|SA_NODEFER/' "$copy"
expect_tooth "rt_sigaction flags mutation" \
    '^an rt_sigaction install lacks the measured SA_.* flags or restorer$'

# Tooth 7: the fixture drifts from the artifact's source-sha256.
cp "$ART" "$copy"
rm -rf "$drift"
cp -R "$FIXTURE" "$drift"
printf '\n// drift: this is not the measured fixture\n' >> "$drift/main.go"
fixture="$drift"
expect_fixture_tooth "fixture source drift" \
    "^fixture/artifact drift: artifact pins 'main\\.go=.*' but the tree hashes to 'main\\.go=.*'$"
fixture="$FIXTURE"

# The pristine copy must still pass after the teeth.
cp "$ART" "$copy"
expect_pass "restored artifact"

printf 'go_thread_gate_args_check_smoke: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
