#!/usr/bin/env bash
# tests/tls_key_git_abort_smoke.sh — proves tls_key_containment_check.sh
# aborts HONESTLY when git is unusable, instead of reporting
# "used by NOTHING -- the store is not wired up".
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The guard's structural half reads the COMMITTED index (git grep --cached)
# on purpose -- the containment argument is about what is committed, not what
# happens to be in a worktree. When git cannot resolve the repository, every
# one of those greps comes back EMPTY, and the guard used to translate that
# into a fabricated finding:
#
#     FAIL: PERSIST_TLS_LBA is defined but used by NOTHING -- the store is
#     not wired up
#
# for a store that was fully wired. That happened once, for real -- a
# worktree whose .git gitdir line the local git could not resolve -- and
# sent a diagnostic round hunting for a wiring bug that did not exist. The
# guard now probes git up front and aborts (exit 2) with the true reason.
# This smoke proves that tooth bites, and that the fabricated finding cannot
# come back in the two shapes a broken git can take.
#
# ─── The two teeth ─────────────────────────────────────────────────────────
#   1. git PRESENT but cannot resolve the repo (the real incident): a fake
#      git on PATH that exits 128 for everything. The guard's `command -v
#      git` passes and the probe -- `git rev-parse --git-dir` -- fails,
#      exercising the exact branch that once fabricated the finding. Expect
#      exit 2 and "git cannot resolve".
#   2. git MISSING from PATH entirely. Expect exit 2 and "git not
#      available". The PATH for this tooth is a minimal dir containing only
#      dirname (the one external tool the guard needs before the abort), so
#      "missing" is exactly what it means rather than a guess about where
#      git lives on this host.
# Both teeth also assert the guard does NOT print "used by NOTHING" -- that
# sentence is the lie the probe exists to prevent, and a smoke that checked
# only the exit code would let the guard print it AND abort and still pass.
#
# ─── Why CC is pinned to a no-op ───────────────────────────────────────────
# The guard checks for $CC before it checks for git, and both teeth abort
# before any compilation happens. CC=/bin/true gets the guard past its own
# toolchain check deterministically, so this smoke does not depend on gcc
# being installed -- it is testing the git-abort branch, not the toolchain.
#
# ─── Why the restore check is environment-neutral ──────────────────────────
# The house smokes re-run the guard after their teeth and assert a clean
# pass. Here that would assume the HOST's git works, which is exactly what
# this smoke exists to stop assuming: on a host with a broken git (like the
# one this was written on) the guard LEGITIMATELY aborts, and the smoke must
# not fail because the real world is the thing the guard is honest about. So
# the restore asserts the environment itself is back: the real git is on
# PATH again and runs. Everything the smoke created lives in one temp dir,
# removed on any exit.
#
# Source-only: needs only bash, dirname and a temp dir -- CI runs it on
# every push via tests/run_source_smokes.sh (auto-globbed, no registration).
#
# Exit: 0 if both teeth bit and the environment was restored, 1 otherwise.
set -u
cd "$(dirname "$0")/.."   # repo root

guard=tests/tls_key_containment_check.sh
BASH_BIN="$(command -v bash)"
DIRNAME_BIN="$(command -v dirname)"
ORIG_PATH="$PATH"
T="$(mktemp -d)"
fails=0

trap 'PATH="$ORIG_PATH"; rm -rf "$T"' EXIT

# run_tooth <name> <PATH-for-guard> <expected-substring> — run the guard with
# a sabotaged PATH, assert it exits 2, prints the honest reason, and does NOT
# print the fabricated "used by NOTHING" finding.
run_tooth() {
    local name="$1" path="$2" expected="$3" out rc
    out="$(PATH="$path" CC=/bin/true "$BASH_BIN" "$guard" 2>&1)"
    rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -qF "$expected"; then
        echo "TOOTH OK   $name — guard aborted with: $expected"
        if printf '%s' "$out" | grep -qF "used by NOTHING"; then
            echo "TOOTH FAIL $name — guard printed 'used by NOTHING' in the same run:"
            printf '%s\n' "$out" | grep -F "used by NOTHING" | sed 's/^/           /'
            fails=$((fails + 1))
        fi
    else
        echo "TOOTH FAIL $name — guard rc=$rc, expected exit 2 with '$expected'"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
    fi
}

# Tooth 1: git present but cannot resolve the repository. The fake git
# shadows the real one on PATH; it exists (so `command -v git` passes) and
# fails the probe exactly the way a broken worktree gitdir does.
FAKE="$T/fakebin"
mkdir -p "$FAKE"
printf '#!/usr/bin/env bash\nexit 128\n' > "$FAKE/git"
chmod +x "$FAKE/git"

run_tooth "git cannot resolve the repo" "$FAKE:$ORIG_PATH" "git cannot resolve"

# Tooth 2: git missing from PATH entirely. The minimal dir holds only
# dirname, the one external tool the guard needs before the git check, so
# bash builtins plus $BASH_BIN/$CC absolute paths cover the rest.
MIN="$T/minbin"
mkdir -p "$MIN"
ln -s "$DIRNAME_BIN" "$MIN/dirname"

run_tooth "git missing from PATH" "$MIN" "git not available"

# Restore check — the environment is back to what it was: the real git is
# on PATH again and runs. (It may still fail to resolve THIS repo on a host
# like this one; that is the guard's honest abort, not a smoke footprint.)
PATH="$ORIG_PATH"
if command -v git >/dev/null 2>&1 && git --version >/dev/null 2>&1; then
    echo "RESTORE OK — real git is back on PATH"
else
    echo "RESTORE FAIL — git missing from the restored PATH"
    fails=$((fails + 1))
fi

echo "tls_key_git_abort_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
