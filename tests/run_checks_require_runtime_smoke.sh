#!/usr/bin/env bash
# tests/run_checks_require_runtime_smoke.sh — prove REQUIRE_RUNTIME changes
# exactly the runtime-abort classification it is meant to change.
#
# tests/run_checks.sh normally files a GUARD-KIND: runtime guard's exit 2 as
# an owed live-cluster skip, even under --require-all. That is right for a
# guard that needs a running cluster, and wrong for a guard whose prerequisite
# is a build artifact: CI builds the E3 ISO and names e3_multi_env_boot_check.sh
# in REQUIRE_RUNTIME, so a missing ISO or QEMU must fail the step rather than
# leave the E3 regression green without a boot.
#
# Three cases pin the contract:
#   1. default          -> runtime abort is a SKIP, runner exits 0
#   2. named            -> the same abort is a FAIL, runner exits 1 (and the
#                          guard is not also reported as a skip)
#   3. unknown name     -> the runner ABORTS (exit 2); a renamed/deleted guard
#                          must not make the requirement a silent no-op
#
# The guard is fake on purpose: the real suite would couple this classification
# contract to every guard in the tree, and the point is the runner's branch.
# Source-only: bash and coreutils. Run via tests/run_source_smokes.sh (the
# tests/*_smoke.sh glob); it is not in that runner's BUILD_SMOKES list.
#
# Exit: 0 if all three cases behave as named, 1 otherwise, 2 on a missing
# prerequisite.
set -u
cd "$(dirname "$0")/.." || exit 1

RUNNER="$PWD/tests/run_checks.sh"
[ -f "$RUNNER" ] || { echo "ABORT: $RUNNER is missing" >&2; exit 2; }

tmp="$(mktemp -d "${TMPDIR:-/tmp}/require-runtime-smoke.XXXXXX")" || {
    echo "ABORT: could not create a temporary directory" >&2
    exit 2
}
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/tests"
cp "$RUNNER" "$tmp/tests/run_checks.sh"
cat > "$tmp/tests/runtime_abort_check.sh" <<'EOF'
#!/usr/bin/env bash
# GUARD-KIND: runtime
echo "ABORT: fake runtime prerequisite missing" >&2
exit 2
EOF
chmod +x "$tmp/tests/runtime_abort_check.sh"

fails=0

# run_case <label> <expected-rc> <required-guard-or-empty>
run_case() {
    local label="$1" want_rc="$2" req="$3"
    local log="$tmp/$label.log" rc=0
    (
        cd "$tmp" || exit 2
        if [ -n "$req" ]; then
            export REQUIRE_RUNTIME="$req"
        else
            unset REQUIRE_RUNTIME
        fi
        bash tests/run_checks.sh --require-all
    ) > "$log" 2>&1 || rc=$?

    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: $label — exit $rc, expected $want_rc"
        sed 's/^/      /' "$log"
        fails=$((fails + 1))
    fi
}

# Assert a line is present in the case log, and (optionally) another absent.
expect() {
    local label="$1" want="$2" forbid="${3:-}"
    local log="$tmp/$label.log"
    if ! grep -qF "$want" "$log"; then
        echo "FAIL: $label — missing '$want'"
        sed 's/^/      /' "$log"
        fails=$((fails + 1))
        return
    fi
    if [ -n "$forbid" ] && grep -qF "$forbid" "$log"; then
        echo "FAIL: $label — unexpectedly saw '$forbid'"
        sed 's/^/      /' "$log"
        fails=$((fails + 1))
        return
    fi
    echo "ok:   $label — saw '$want'${forbid:+ and not '$forbid'}"
}

# 1. Default: the runtime abort is an owed skip, not a failure.
run_case default 0 ""
expect default 'SKIP  runtime_abort_check.sh (runtime guard -- needs a live cluster, not a build)'
expect default '0 passed, 0 failed, 1 skipped'

# 2. Named: the same abort fails, and it is not double-counted as a skip.
run_case named 1 "runtime_abort_check.sh"
expect named 'FAIL  runtime_abort_check.sh (runtime guard aborted, and REQUIRE_RUNTIME names it)' \
    'SKIP  runtime_abort_check.sh'
expect named '0 passed, 1 failed, 0 skipped'

# 3. A name that matches no guard is a runner abort, not a silent no-op.
run_case unknown 2 "no_such_check.sh"
expect unknown 'ABORT: REQUIRE_RUNTIME names tests/no_such_check.sh, but no such guard exists.'

echo "run_checks_require_runtime_smoke: done, $fails failed"
[ "$fails" -eq 0 ]
