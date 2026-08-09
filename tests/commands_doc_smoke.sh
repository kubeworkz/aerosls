#!/usr/bin/env bash
# tests/commands_doc_smoke.sh — proves the commands-doc guard's teeth
# bite, by planting the exact historical doc bug and asserting the guard
# fails on it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/commands_doc_check.sh asserts docs/COMMANDS.md matches the real
# command surface — every shell dispatch and aeroslsctl subcommand
# documented, `raw` with a literal method, the upload body field named
# `hex` (not `data`), and no phantom `stream list` command. Its teeth were
# proven once, by hand, on a local clone. Nothing in CI ever made it fail,
# so a regression that made the guard blind — a broken regex, a dropped
# check, a missing file guard — would pass every push unnoticed.
#
# This smoke runs in CI (ci.yml, after "Guard scripts") and plants the
# check-4 violation, which is the direct inverse of the historical bug
# that started this whole guard: `api_stream_upload()` reads
# json_str(body,"hex",...), and documenting `data` produced "name and hex
# required" at runtime. The tooth rewrites every `"hex"` in COMMANDS.md to
# `"data"` — check 4 is the only check whose input touches that string, so
# exactly one check fails, with a message naming the field.
#
# ─── How the tooth is planted and restored ─────────────────────────────────
# `sed -i 's/"hex"/"data"/g' docs/COMMANDS.md`, then run the guard, then
# `git checkout -- docs/COMMANDS.md` restores the committed blob
# byte-identically. That restore is only safe because the file is clean at
# entry — the pre-check below aborts (exit 2, the run_checks convention
# for a missing prerequisite) if anything has already modified it, so the
# smoke can never clobber a real doc edit. The trap restores on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the worktree on purpose — it must never run in
# the local guard gate or a deploy gate. Only ci.yml invokes it.
#
# Exit: 0 if the tooth bit, 1 if it did not, 2 if the doc was dirty at entry.
set -u

cd "$(dirname "$0")/.."   # repo root
guard=tests/commands_doc_check.sh
fails=0
DOC=docs/COMMANDS.md

if [ -n "$(git status --porcelain "$DOC")" ]; then
    echo "ABORT: $DOC is dirty — the smoke needs a clean checkout to restore it" >&2
    exit 2
fi

trap 'git checkout -- "$DOC" 2>/dev/null || true' EXIT

sed -i 's/"hex"/"data"/g' "$DOC"
out="$(bash "$guard" 2>&1)"
rc=$?
git checkout -- "$DOC"

expected="docs/COMMANDS.md does not document the 'hex' field"
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
    echo "TOOTH OK   hex-vs-data — guard failed with: $expected"
else
    echo "TOOTH FAIL hex-vs-data — guard rc=$rc, expected failure '$expected'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Final restore check — prove the doc is byte-identical again, so this
# smoke can never leave a footprint for a later step (or a local run).
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — doc clean after tooth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the doc was restored:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "commands_doc_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
