#!/usr/bin/env bash
# tests/script_conventions_smoke.sh — proves the script-conventions guard's
# teeth bite, by staging four deliberate violations and asserting the guard
# fails on each one.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/script_conventions_check.sh asserts four properties: every .sh is
# 100755, every .sh has a shebang, no text blob is stored CRLF, and no
# tracked .sh is CRLF on disk. Its teeth were proven once, by hand, on a
# local clone. Nothing in CI ever made it fail, so a regression that made
# the guard blind — a botched loop, a reordered check, a path filter, an
# attribute change — would pass every push unnoticed. This smoke runs in
# CI (ci.yml, the step after "Guard scripts") and deliberately plants one
# violation per rule, then asserts the guard exits 1 and names the
# violation. The guard step above proves the guards pass; this step proves
# they can fail.
#
# ─── How the teeth are planted ────────────────────────────────────────────
# For the index rules a tooth is two git calls: `git hash-object -w` writes
# the violating blob, `git update-index --cacheinfo` adds the index entry
# for it, and `git update-index --force-remove` removes it afterwards. The
# worktree stays untouched, and the staged entries are exactly what the
# guard inspects — the same way it catches a violation in a local index
# before it is committed. The fourth tooth is the inverse: the index entry
# is clean LF and the CRLF exists only in the checked-out bytes, because
# that is the state a normalised index cannot reveal.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the index on purpose — it must never run in the
# local guard gate or a deploy gate. The *_smoke.sh name keeps it out of
# both; only ci.yml invokes it.
#
# Exit: 0 if all three teeth bit, 1 if any did not.
set -u

cd "$(dirname "$0")/.."   # repo root, so the git index paths resolve
guard=tests/script_conventions_check.sh
fails=0

# drop <path> — remove a staged tooth from the index (worktree untouched;
# the tooth file never exists on disk, only as an index entry + blob).
drop() {
    git update-index --force-remove "$1" 2>/dev/null || true
}
trap 'drop .ci-teeth/tooth_mode.sh; drop .ci-teeth/tooth_shebang.sh; drop .ci-teeth/tooth_crlf.txt; drop .ci-teeth/tooth_worktree.sh; rm -f .ci-teeth/tooth_worktree.sh' EXIT

# tooth <name> <expected-substring> <mode> <path> <content> — plant one
# violation, run the guard, assert it exits 1 and names the violation.
# Content goes through printf '%b' so \r in the CRLF tooth is interpreted.
tooth() {
    name="$1"; expected="$2"; mode="$3"; path="$4"; content="$5"
    sha="$(printf '%b' "$content" | git hash-object -w --stdin)"
    git update-index --add --cacheinfo "$mode,$sha,$path"
    out="$(bash "$guard" 2>&1)"
    rc=$?
    drop "$path"
    if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
        echo "TOOTH OK   $name — guard failed with: $expected"
    else
        echo "TOOTH FAIL $name — guard rc=$rc, expected failure '$expected'"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
    fi
}

tooth "mode"    "tooth_mode.sh: committed mode 100644, expected 100755" \
    100644 .ci-teeth/tooth_mode.sh '#!/usr/bin/env bash
echo hi
'

tooth "shebang" "tooth_shebang.sh: no shebang" \
    100755 .ci-teeth/tooth_shebang.sh 'echo hi
'

tooth "crlf"    "tooth_crlf.txt: committed blob is stored CRLF" \
    100644 .ci-teeth/tooth_crlf.txt 'line1\r\nline2\r\n'

# Worktree tooth — index clean LF, file CRLF on disk. This is the exact
# state tests/env_catalog_live_check.sh was in: git status reported nothing,
# the index scan passed, and executing the file failed with 'bash\r'.
wt=.ci-teeth/tooth_worktree.sh
sha="$(printf '#!/usr/bin/env bash\necho hi\n' | git hash-object -w --stdin)"
git update-index --add --cacheinfo "100755,$sha,$wt"
mkdir -p .ci-teeth
printf '#!/usr/bin/env bash\r\necho hi\r\n' > "$wt"
out="$(bash "$guard" 2>&1)"
rc=$?
rm -f "$wt"
drop "$wt"
rmdir .ci-teeth 2>/dev/null || true
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$wt: working-tree file is CRLF"; then
    echo "TOOTH OK   worktree-crlf — guard failed with: $wt: working-tree file is CRLF"
else
    echo "TOOTH FAIL worktree-crlf — guard rc=$rc, expected failure '$wt: working-tree file is CRLF'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Final restore check — the teeth are dropped above; prove the index is
# clean again, so this smoke can never leave a footprint for a later step
# (or a local run) to trip over.
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — index clean after teeth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the teeth were dropped:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "script_conventions_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
