#!/usr/bin/env bash
# workflow_yaml_check_smoke.sh — proves the workflow-load guard's teeth bite.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/workflow_yaml_check.sh asserts that every .github/workflows/*.yml
# parses and carries a loadable job graph. Its teeth were proven once, by
# reproducing the ci.yml bug by hand. Nothing in CI ever makes it fail, so a
# regression that made it blind — a dropped structural requirement, a glob that
# stopped globbing, a parser that swallowed every error — would pass every push
# unnoticed. This smoke plants one failure of each KIND and asserts the guard
# exits 1 naming it, because a guard is only worth the failures it can still
# produce.
#
# ─── The teeth, one per separable job ──────────────────────────────────────
#   1. syntax        the original regression: an unquoted `: ` inside a plain
#                    scalar. This is the exact shape that cost a whole
#                    branch's CI ("mapping values are not allowed here"), so it
#                    is the tooth that matters most.
#   2. not-a-mapping a document whose top level is not a workflow at all.
#   3. no-jobs       a file with no `jobs:` — GitHub's other zero-jobs load
#                    failure.
#   4. no-runs-on    a job that cannot be scheduled (neither `runs-on` nor
#                    `uses`).
#   5. no-on         a workflow with no trigger.
#   6. duplicate-key a key repeated in one mapping. PyYAML accepts this and
#                    keeps the last value; GitHub does not, so the guard must
#                    reject it — at the top level and nested inside a job.
#   7. merge-key     the control for that: `<<: *anchor` with an overriding key
#                    is NOT a duplicate and must still load, or the duplicate
#                    check would reject legitimate YAML anchors.
#   8. all-files     a clean dir PASSES, and then one broken file among good
#                    ones still reddens it — the guard must reach every file,
#                    and must be able to say yes, or "always fails" would look
#                    like a passing tooth.
#   9. vacuity       an empty directory is an ABORT, not a pass: a guard that
#                    inspects nothing protects nothing.
#  10. broken-parser a python3 that cannot import yaml must ABORT (exit 2) —
#                    and, with no GUARD-KIND marker, run_checks.sh turns that
#                    into a failure, so a missing parser can never read as a
#                    green run.
#  11. real-tree     the committed workflows still load — the anti-false-
#                    positive anchor, run last.
#
# ─── How the teeth are planted, without touching the worktree ──────────────
# The guard takes the workflows directory as an optional argument, so every
# tooth is built in a fresh mktemp -d and the real .github/workflows is never
# written to. The trap removes the temp dir on any exit; there is nothing to
# restore and nothing to leave dirty.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on that same set;
# run_source_smokes.sh globs tests/*_smoke.sh, so this is picked up there
# automatically — the failure that protects against is a new smoke that only
# ever runs on a build host and so never runs at all.
#
# Exit: 0 if every tooth bit, 1 if any did not.
set -u
cd "$(dirname "$0")/.." || exit 1

guard=tests/workflow_yaml_check.sh
[ -f "$guard" ] || { echo "ABORT: $guard missing" >&2; exit 2; }

fails=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
wf="$tmp/wf"

# fresh — start each tooth from an empty workflows directory.
fresh() { rm -rf "$wf"; mkdir -p "$wf"; }
# plant <filename> <content> — write one file into the current tooth's dir;
# %b so '\n' in the content is a newline.
plant() { printf '%b' "$2" > "$wf/$1"; }

VALID='name: sample\non: push\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - run: echo hi\n'

# expect <label> <want-rc> <expected-substring> — run the guard against $wf and
# assert the exit code and a substring of its output.
expect() {
    label="$1"; want="$2"; expected="$3"
    out="$(bash "$guard" "$wf" 2>&1)"; rc=$?
    if [ "$rc" -eq "$want" ] && printf '%s' "$out" | grep -qF "$expected"; then
        echo "TOOTH OK   $label — guard rc=$rc with: $expected"
    else
        echo "TOOTH FAIL $label — guard rc=$rc, wanted rc=$want with '$expected'"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
    fi
}

# 1. The original ci.yml bug, verbatim in shape: the step name's `: ` turns a
#    plain scalar into a mapping key.
fresh
plant bug.yml 'name: broken\non: push\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - name: aerobackup host test (E7 census target: retention prune)\n        run: echo hi\n'
expect "syntax" 1 "does not parse as YAML"

# 2. Top level is a sequence, not a workflow document.
fresh
plant seq.yml '- a\n- b\n'
expect "not-a-mapping" 1 "top level is list, not a mapping"

# 3. No jobs at all.
fresh
plant nojobs.yml 'name: x\non: push\n'
expect "no-jobs" 1 "has no non-empty \`jobs:\` mapping"

# 4. A job with no runs-on and no uses cannot be scheduled.
fresh
plant nojobkey.yml 'name: x\non: push\njobs:\n  a:\n    steps:\n      - run: echo hi\n'
expect "no-runs-on" 1 "has neither \`runs-on\` nor \`uses\`"

# 5. No trigger.
fresh
plant noon.yml 'name: x\njobs:\n  a:\n    runs-on: x\n    steps:\n      - run: echo hi\n'
expect "no-on" 1 "has no \`on:\` trigger"

# 6. A key repeated in one mapping. PyYAML would accept this and keep the last
#    value; GitHub does not, so the guard must reject it — at the top level and
#    nested inside a job.
fresh
plant dup.yml 'name: first\nname: second\non: push\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - run: echo hi\n'
expect "duplicate-key" 1 "duplicate mapping key 'name'"

fresh
plant dupnested.yml 'name: x\non: push\njobs:\n  build:\n    runs-on: ubuntu-latest\n    runs-on: ubuntu-22.04\n    steps:\n      - run: echo hi\n'
expect "duplicate-key-nested" 1 "duplicate mapping key 'runs-on'"

# 7. The control for duplicate-key: a merge key with an overriding explicit key
#    is NOT a duplicate and must still load.
fresh
plant merge.yml 'name: x\non: push\njobs:\n  build:\n    runs-on: ubuntu-latest\n    env: &shared\n      FOO: "1"\n    steps:\n      - run: echo hi\n        env:\n          <<: *shared\n          FOO: "2"\n'
expect "merge-key-not-duplicate" 0 "parses; 1 job(s): build"

# 8. The guard can say yes, and still reaches every file when one is broken.
fresh
plant good.yml "$VALID"
expect "clean-dir-passes" 0 "parses; 1 job(s): build"
plant bad.yml '- a\n- b\n'
expect "one-bad-among-good" 1 "bad.yml: top level is list"

# 9. Vacuity: no workflows found is an ABORT, not a pass.
fresh
expect "empty-dir-aborts" 2 "ABORT: no $wf/*.yml found"

# 10. A python3 that cannot import yaml must ABORT, so a parser-less host fails
#    closed instead of reporting success over files nobody parsed.
fresh
plant good.yml "$VALID"
mkdir -p "$tmp/bin"
printf '#!/bin/sh\nexit 1\n' > "$tmp/bin/python3"
chmod +x "$tmp/bin/python3"
out="$(PATH="$tmp/bin:$PATH" bash "$guard" "$wf" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -qF "ABORT: python3 cannot import yaml"; then
    echo "TOOTH OK   broken-parser — guard rc=2 with: ABORT: python3 cannot import yaml"
else
    echo "TOOTH FAIL broken-parser — guard rc=$rc, wanted rc=2 with 'ABORT: python3 cannot import yaml'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# 11. The real, committed workflows still load (run last: the anti-false-positive
#    anchor — every tooth above would also bite if the guard simply always
#    failed).
out="$(bash "$guard" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -qF "ci.yml" \
   && printf '%s' "$out" | grep -qF "nightly-flake.yml"; then
    echo "REAL OK — the committed workflows load (ci.yml, nightly-flake.yml)"
else
    echo "REAL FAIL — the guard does not accept the committed workflows (rc=$rc):"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "workflow_yaml_check_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
