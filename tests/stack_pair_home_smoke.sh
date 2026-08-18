#!/usr/bin/env bash
# tests/stack_pair_home_smoke.sh — proves the stack-pair-home guard's teeth
# bite, by planting the exact drift it exists to catch — a host test that
# reintroduces its own local stack_bottom/stack_top copies — and asserting
# the guard fails on it, then restores the tree and proves the guard passes
# again.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/stack_pair_home_check.sh asserts the bootstrap-stack stub pair is
# defined only inside tests/process_host_stubs.h. Its teeth were proven
# once, by inspection, after the fourteen-file consolidation. Nothing ever
# made it fail automatically, so a regression that made the guard blind —
# a broken pattern, a dropped allowlist path, a path-prefix mismatch — would
# pass every push unnoticed. A check that has never been seen to fail is
# half a check; this smoke is the other half.
#
# ─── How the tooth is planted ─────────────────────────────────────────────
# The guard reads the WORKTREE (grep over the tree), so the tooth is a real
# file: tests/zz_stack_pair_tooth.c carrying the two-line local-copy form
# the drift took, then rm'd. The name matches nothing else — run_all.sh
# globs *_host_test.c, run_checks.sh globs *_check.sh, run_source_smokes.sh
# globs *_smoke.sh — so the tooth cannot collide with another runner even if
# one fired mid-smoke. The trap removes it on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the worktree on purpose — it must never run in
# the local guard gate or a deploy gate. It IS source-only, so CI runs it on
# every push: run_source_smokes.sh globs tests/*_smoke.sh, and only the five
# build-needing smokes are excluded by name — a new source-only smoke is
# covered automatically.
#
# Exit: 0 if the tooth bit and the restore is clean, 1 otherwise.
set -u
cd "$(dirname "$0")/.."   # repo root
guard=tests/stack_pair_home_check.sh
TOOTH=tests/zz_stack_pair_tooth.c
fails=0

trap 'rm -f "$TOOTH"' EXIT

# The drift class: a host test pasting the pair back in because that is what
# the old tests did. Two lines, exactly like the header's own form.
printf '/* tooth: a host test reintroducing its own copy of the pair. */\nchar stack_bottom[16];\nchar stack_top[16];\n' > "$TOOTH"

out="$(bash "$guard" 2>&1)"
rc=$?
rm -f "$TOOTH"

if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$TOOTH"; then
    echo "TOOTH OK   stray local copy — guard failed and named the file"
    printf '%s\n' "$out" | grep -F "FAIL:" | sed 's/^/           /'
else
    echo "TOOTH FAIL stray local copy — guard rc=$rc, expected failure naming $TOOTH"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Restore check — prove the genuine tree still passes. This is also the
# false-positive check: the real tree contains `extern char stack_bottom[]`
# (kernel/frame_pool.c, kernel/stubs.c) and asm `stack_bottom:` labels
# (arch/x86/boot.asm), so a pass proves the anchor excludes them. A guard
# that cannot pass on the real tree protects nothing and must be caught.
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — tree clean after tooth, guard passes (extern decls and boot.asm not flagged)"
else
    echo "RESTORE FAIL — guard failed on the clean tree:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "stack_pair_home_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
