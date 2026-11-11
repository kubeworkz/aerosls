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
# ─── The test seam ─────────────────────────────────────────────────────────
# E7_EVIDENCE_DIR overrides the recording directory. It exists so the guard's
# failure side can be exercised against a copied recording (for example, flip
# facts' payloads=1 to 0 and require this script to exit 1); the committed
# default is the only path CI uses.
#
# Exit: 0 pass, 1 fail. It never exits 2: a missing recording is a failure of
# the evidence it is supposed to guard, not a reason to skip the check.
set -u
cd "$(dirname "$0")/.."   # repo root, so the harness's relative paths resolve

ART="${E7_EVIDENCE_DIR:-evidence/e7_gate_drill_adaptive}"

for f in boot1.log boot2.log facts; do
    if [ ! -f "$ART/$f" ]; then
        echo "FAIL  e7_gate_evidence_check: $ART/$f is missing — the committed recording is incomplete"
        exit 1
    fi
done

out="$(bash tests/e7_gate_check.sh --replay "$ART" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$out"
    echo "FAIL  e7_gate_evidence_check: the recording at $ART no longer replays green (exit $rc)"
    exit 1
fi

if ! printf '%s\n' "$out" | grep -qF -- "e7_gate_check: the recorded run in $ART passes the gate's clauses"; then
    printf '%s\n' "$out"
    echo "FAIL  e7_gate_evidence_check: --replay exited 0 without the green verdict — a tooth recording is not a passing gate"
    exit 1
fi

echo "e7_gate_evidence_check: $ART replays green — the committed gate evidence is still accepted by its clauses"
