#!/usr/bin/env bash
# tests/e7_gate_evidence_check.sh — replay the committed E7 gate recording.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# `tests/e7_gate_check.sh --replay DIR` already re-runs the live arm's own
# assertion function over a recording, but nothing ran it on a push: the
# default `tests/*_check.sh` sweep only ran the harness's G1–G7 source clauses.
# The adaptive-wait run is committed as evidence, and evidence that nothing
# re-reads is a transcript, not a guard. This file closes that gap without a
# boot: it replays `evidence/e7_gate_drill_adaptive/` through the same
# assertions the live run uses, so B1–B7 and D1–D2 are re-derived from the
# committed boot logs and `facts` on every guard sweep.
#
# ─── Why it requires the verdict line, not just exit 0 ─────────────────────
# `--replay` exits 0 for a tooth recording too: a tooth is EXPECTED to redden
# its named clauses, and the validator reports that as a legitimate outcome.
# This guard is about the committed ADAPTIVE run, which must be a passing gate.
# It therefore requires the exact green verdict, so a recording that turned
# into a tooth run (or a vacuous replay that printed nothing) cannot pass.
#
# ─── The counterexample is part of every run ───────────────────────────────
# A guard that only proves the good recording passes can rot into a guard that
# accepts everything. Every default run therefore copies the recording, flips
# `payloads=1` to `payloads=0`, and requires the replay to fail with B1 red.
# The manual proof we ran once was not enough: the failure side now has to
# survive every edit to this file, the harness, or the recording.
#
# E7_EVIDENCE_DIR overrides the recording for a single external check. The
# self-test is skipped then, because the caller is usually supplying a bad
# copy to prove this guard still exits 1 on it.
#
# Exit: 0 pass, 1 fail. It never exits 2: a missing recording is a failure of
# the evidence it is supposed to guard, not a reason to skip the check.
set -u
cd "$(dirname "$0")/.."   # repo root, so the harness's relative paths resolve

ART="${E7_EVIDENCE_DIR:-evidence/e7_gate_drill_adaptive}"

fail() {
    echo "FAIL  e7_gate_evidence_check: $*" >&2
    exit 1
}

# check_recording DIR EXPECT
#   EXPECT=pass: exit 0 and the exact green verdict line.
#   EXPECT=fail: nonzero exit and B1 named in the failure output.
check_recording() {
    dir="$1"
    expect="$2"
    for f in boot1.log boot2.log facts; do
        [ -f "$dir/$f" ] || fail "$dir/$f is missing — the recording is incomplete"
    done

    out="$(bash tests/e7_gate_check.sh --replay "$dir" 2>&1)"
    rc=$?

    if [ "$expect" = "pass" ]; then
        if [ "$rc" -ne 0 ]; then
            printf '%s\n' "$out"
            fail "the recording at $dir no longer replays green (exit $rc)"
        fi
        printf '%s\n' "$out" |
            grep -qF -- "e7_gate_check: the recorded run in $dir passes the gate's clauses" ||
            {
                printf '%s\n' "$out"
                fail "--replay exited 0 without the green verdict — a tooth recording is not a passing gate"
            }
    else
        if [ "$rc" -eq 0 ]; then
            printf '%s\n' "$out"
            fail "the payloads=0 counterexample was accepted as a pass (exit 0)"
        fi
        printf '%s\n' "$out" | grep -qF -- "FAIL: B1." ||
            {
                printf '%s\n' "$out"
                fail "the payloads=0 counterexample did not redden B1"
            }
    fi
}

if [ -n "${E7_EVIDENCE_DIR:-}" ]; then
    check_recording "$ART" pass
    echo "e7_gate_evidence_check: $ART replays green — the recording is still accepted by its clauses"
    exit 0
fi

check_recording "$ART" pass

tmp="$(mktemp -d "evidence/.e7_gate_bad.XXXXXX")" ||
    fail "could not create the counterexample directory"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$ART/boot1.log" "$ART/boot2.log" "$ART/facts" "$tmp/" ||
    fail "could not copy the recording into $tmp"
sed 's/^payloads=1$/payloads=0/' "$ART/facts" > "$tmp/facts" ||
    fail "could not write the counterexample facts file"
grep -qx 'payloads=0' "$tmp/facts" ||
    fail "the counterexample facts file did not flip payloads to 0"

check_recording "$tmp" fail
rm -rf "$tmp"
trap - EXIT HUP INT TERM

echo "e7_gate_evidence_check: $ART replays green; its payloads=0 counterexample reddens B1"
