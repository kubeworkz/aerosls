#!/usr/bin/env bash
# tests/makefile_sources_smoke.sh — proves the makefile-sources guard's
# teeth bite, by planting two deliberate violations and asserting the
# guard fails on each.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/makefile_sources_check.sh asserts every kernel .c is in the build:
# every .c under kernel/ net/ drivers/ arch/x86/ must appear in the
# Makefile's X86_C_SRC (or be in its named EXCLUDED list), and every listed
# file must exist. Its teeth were proven once, by hand, on a local clone.
# Nothing in CI ever made it fail, so a regression that made the guard
# blind — a broken python parse, a dropped direction, a wrong SEARCH_DIRS —
# would pass every push unnoticed. This smoke runs in CI (ci.yml, after
# "Guard scripts") and proves both directions independently:
#
#   1. missing — a .c on disk that X86_C_SRC does not list (the historical
#      boot_params.c class: shipped, compiled, linked, and never built);
#   2. stale   — a listed file that no longer exists on disk.
#
# ─── How the teeth are planted ─────────────────────────────────────────────
# Unlike script_conventions_check.sh (index-based), this guard reads the
# WORKTREE: the Makefile and `find` over the source dirs. So each tooth is
# a real file operation, restored byte-identically afterwards:
#
#   1. missing — `echo > kernel/zz_tooth.c` (a one-line file, name chosen
#      to sort last and collide with nothing), then `rm` it;
#   2. stale   — `mv kernel/agent.c kernel/agent.c.tooth` (agent.c is
#      definitely in X86_C_SRC; `.tooth` does not match `*.c`), then `mv`
#      it back. The trap restores both if the script dies mid-tooth.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the worktree on purpose — it must never run in
# the local guard gate or a deploy gate. Only ci.yml invokes it.
#
# Exit: 0 if both teeth bit, 1 if any did not.
set -u

cd "$(dirname "$0")/.."   # repo root
guard=tests/makefile_sources_check.sh
fails=0

# Restore both teeth unconditionally on exit (idempotent no-ops once they
# have been restored by their own tooth).
trap 'rm -f kernel/zz_tooth.c; if [ -f kernel/agent.c.tooth ]; then mv kernel/agent.c.tooth kernel/agent.c; fi' EXIT

# tooth <name> <expected-substring> <plant> <restore> — run the guard
# between plant and restore, assert it exits 1 and names the violation.
tooth() {
    name="$1"; expected="$2"; plant="$3"; restore="$4"
    eval "$plant"
    out="$(bash "$guard" 2>&1)"
    rc=$?
    eval "$restore"
    if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
        echo "TOOTH OK   $name — guard failed with: $expected"
    else
        echo "TOOTH FAIL $name — guard rc=$rc, expected failure '$expected'"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
    fi
}

tooth "missing" "kernel/zz_tooth.c is not in X86_C_SRC" \
    'printf "/* tooth: unlisted source */\n" > kernel/zz_tooth.c' \
    'rm -f kernel/zz_tooth.c'

tooth "stale" "X86_C_SRC lists kernel/agent.c, which does not exist" \
    'mv kernel/agent.c kernel/agent.c.tooth' \
    'mv kernel/agent.c.tooth kernel/agent.c'

# Final restore check — prove the worktree is byte-identical again, so
# this smoke can never leave a footprint for a later step (or a local run).
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — worktree clean after teeth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the teeth were restored:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "makefile_sources_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
