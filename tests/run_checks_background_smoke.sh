#!/usr/bin/env bash
# tests/run_checks_background_smoke.sh — prove the background wrapper
# propagates the child's status, not just a status file.
#
# tests/run_checks_background.sh exists because a launcher can expand `$?`
# outside the shell that ran the suite and record 0 after a real failure. A
# wrapper is easy to "fix" in the file while still exiting 0, or vice versa:
# the file can say 7 while the process exits 0. This smoke drives both a
# failing and a passing fake child through the committed wrapper and requires
# BOTH the process exit status and the recorded status to match the child.
#
# The child is fake on purpose. Running the real suite here would be slow and
# would couple this status-propagation contract to every guard in the tree;
# the wrapper's parent path is exercised by copying it into a temporary root
# with a minimal tests/run_checks.sh. Source-only: bash and coreutils.
#
# Run via tests/run_source_smokes.sh (the tests/*_smoke.sh glob). Exit: 0 if
# both cases preserve their status, 1 otherwise.
set -eu
cd "$(dirname "$0")/.." || exit 1

WRAPPER="$PWD/tests/run_checks_background.sh"
[ -x "$WRAPPER" ] || { echo "ABORT: $WRAPPER is missing or not executable" >&2; exit 2; }

tmp="$(mktemp -d "${TMPDIR:-/tmp}/run-checks-background-smoke.XXXXXX")" || {
    echo "ABORT: could not create a temporary directory" >&2
    exit 2
}
trap 'rm -rf "$tmp"' EXIT
fails=0

make_root() {
    local root="$1" rc="$2"
    mkdir -p "$root/tests"
    cp "$WRAPPER" "$root/tests/run_checks_background.sh"
    cat > "$root/tests/run_checks.sh" <<EOF
#!/usr/bin/env bash
echo "fake run_checks child"
exit $rc
EOF
    chmod +x "$root/tests/run_checks_background.sh"
}

run_case() {
    local root="$1" want="$2" label="$3"
    local log="$root/run.log" status="$root/run.rc" got got_status
    if ( cd "$root" && bash tests/run_checks_background.sh "$log" "$status" ); then
        got=0
    else
        got=$?
    fi
    got_status="$(cat "$status" 2>/dev/null || echo missing)"
    if [ "$got" -eq "$want" ] && [ "$got_status" = "$want" ]; then
        echo "ok:   $label — exit $got, recorded $got_status"
    else
        echo "FAIL: $label — exit $got, recorded $got_status; expected $want"
        fails=$((fails + 1))
    fi
}

make_root "$tmp/failing" 7
make_root "$tmp/passing" 0
run_case "$tmp/failing" 7 "failing child"
run_case "$tmp/passing" 0 "passing child"

echo "run_checks_background_smoke: done, $fails failed"
[ "$fails" -eq 0 ]
