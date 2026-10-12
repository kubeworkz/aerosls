#!/usr/bin/env bash
# tests/env_migration_check_smoke.sh — the TEETH for tests/env_migration_check.sh.
#
# ─── What a teeth check is for, and why this one is source-only ────────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE wiring invariant of P3 increment 3 (the environment
# step inside partition_migrate()'s critical section) in a throwaway copy of
# the tree and requires the guard to (a) exit 1 and (b) name THAT invariant.
# Requiring the right named clause is the half that matters: a guard that
# reddened on everything, or on the wrong clause, would pass a tooth that
# only checked the exit code.
#
# The first teeth are the ones worth stating in prose, because they are the
# defect the increment exists for:
#
#   * `ownership-first` — the environment step is moved AFTER
#     partition_set_owner_node(). The payload then cannot stop the handoff,
#     which is v0.2 §7's rule ("never move ownership without the data")
#     reproduced at the environment level.
#   * the abort neutered — the step stays before the handoff but a shortfall
#     no longer returns, so the counts are decoration and ownership moves
#     anyway: the live-cluster bug, verbatim.
#
# The last tooth is the vacuity control — the guard must be GREEN on the
# real, unmutated tree. Without it, a guard that always failed would pass
# every other tooth in this file.
#
# The hermetic seam is the guard's own optional root argument: this smoke
# builds a minimal root containing exactly the files the guard reads, mutates
# one line, and runs the real guard against it — no network, no build, no
# NVMe, no boot. It is in tests/run_source_smokes.sh's set by construction
# (bash/sed/python3 only), so its teeth are proven on every push.
#
# Exit: 0 every tooth bit, 1 a tooth did not.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_migration_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads, kept in one list so a tooth can never pass
# because the copy it mutated was missing something the guard needed.
SEED_FILES=(
    Makefile
    kernel/partition.c
    kernel/env_migrate.c
    kernel/env_migrate.h
    kernel/env_ckpt.c
    kernel/env_ckpt.h
    tests/env_migration_check.sh
    tests/env_migrate_host_test.c
    tests/partition_migrate_phase6_host_test.c
    tests/partition_migrate_default_path_host_test.c
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    # Invariant I scans every host test's own build command, so the copy
    # needs them all.
    cp "$ROOT"/tests/*_host_test.c "$1/tests/" || return 1
    return 0
}

passed=0
failed=0
tooth() {   # tooth <name> <expected-clause> <dir>; the mutation has already run
    local name="$1" clause="$2" dir="$3" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   $name bites (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: $name did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "env_migration_check_smoke — teeth for the P3 increment 3 wiring guard"
echo "======================================================================"
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
seed "$W/a"; rm -f "$W/a/kernel/env_migrate.c"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   A. a missing env_migrate.c aborts (exit 2) instead of reporting the wiring sound"
    passed=$((passed + 1))
else
    echo "FAIL: A. a missing file did not abort (exit $rc)"; printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── B. The wiring dropped entirely ─────────────────────────────────────────
seed "$W/b"
sed -i '/#include "env_migrate.h"/d' "$W/b/kernel/partition.c"
tooth "B. the environment step's include removed from partition.c" "B" "$W/b"

# ── C. THE TOOTH: the payload moved after the handoff ──────────────────────
# Cut the step's own statement out of its position and re-insert it AFTER
# partition_set_owner_node(), so the guard must fail on the ORDER rather
# than on the step's mere presence.
seed "$W/c"
python3 - "$W/c/kernel/partition.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
# the three lines of the gated call expression
start = next(i for i, l in enumerate(lines)
             if "int envs_rc = (cluster_local_node_id() != 0)" in l)
end = next(i for i in range(start, len(lines)) if lines[i].strip() == ": 0;")
call = lines[start:end + 1]
del lines[start:end + 1]
handoff = next(i for i, l in enumerate(lines)
               if "partition_set_owner_node(partition_id, dest_node_id)" in l)
# re-insert IMMEDIATELY AFTER the handoff line, so the step's own text is
# intact and only its position has changed — the guard must fail on the
# ORDER, which is the tooth's whole subject.
lines[handoff + 1:handoff + 1] = ["    env_migrate_partition(partition_id, dest_node_id, &envs_expected, &envs_moved);"] + call
open(p, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
PY
tooth "C. ownership-first: the environment step moved AFTER the ownership handoff" "C" "$W/c"

# ── D. The abort condition neutered ────────────────────────────────────────
# The step stays where it is; a shortfall just stops stopping anything. This
# is the live-cluster defect itself (0 of N sent, ownership moved, OK
# reported) with the environment in the stream's place.
seed "$W/d"
sed -i 's/if (envs_rc != 0 || envs_moved < envs_expected)/if (0 \&\& (envs_rc != 0 || envs_moved < envs_expected))/' "$W/d/kernel/partition.c"
tooth "D. the shortfall abort neutered (ownership moves regardless)" "D" "$W/d"

# ── E. The cluster gate dropped ────────────────────────────────────────────
seed "$W/e"
sed -i 's/int envs_rc = (cluster_local_node_id() != 0)/int envs_rc = (1)/' "$W/e/kernel/partition.c"
tooth "E. the environment step taken without a real cluster identity" "E" "$W/e"

# ── F. The freeze released on only one path ────────────────────────────────
seed "$W/f"
sed -i '/env_ckpt_release_partition(&freeze);/d' "$W/f/kernel/env_migrate.c"
tooth "F. the freeze never released (an aborted migration leaves its records marked)" "F" "$W/f"

# ── G. The prior flags not restored ────────────────────────────────────────
seed "$W/g"
sed -i '/env_ckpt_table\[i\].flags = q->prior_flags\[i\];/d' "$W/g/kernel/env_ckpt.c"
tooth "G. the release stops restoring each record's prior flags" "G" "$W/g"

# ── H. The not-yet-replayed gate unwired ───────────────────────────────────
seed "$W/h"
sed -i '/if (!env_ckpt_restore_pending_record(partition_id/d' "$W/h/kernel/env_migrate.c"
tooth "H. the pending-record gate no longer called before anything is read" "H" "$W/h"

# ── I. A partition.c-linking test left without the new dependency ──────────
# partition_host_test.c specifically: it is the file that header-phrase
# search missed in the first pass (its header says "built and run directly:"
# rather than "Build and run:"), so the suite went red at the linker. The
# tooth removes its stand-in and requires clause I to see THAT file.
seed "$W/i"
python3 - "$W/i/tests/partition_host_test.c" <<'PY'
import sys
p = sys.argv[1]
src = open(p, encoding="utf-8").read()
a = src.index("/* POSIX-Environments P3 increment 3:")
b = src.index("\n}\n", a) + 3
open(p, "w", encoding="utf-8", newline="\n").write(src[:a] + src[b:])
PY
tooth "I. a host test linking partition.c with neither the real step nor a stand-in (partition_host_test.c, the file the phrase search missed)" "I" "$W/i"

# ── J. The tests stop asserting what they own ──────────────────────────────
seed "$W/j1"
sed -i '/env_step_saw_owner == 1/d' "$W/j1/tests/partition_migrate_phase6_host_test.c"
tooth "J. the orchestration test stops asserting ownership was local when the step ran" "J" "$W/j1"

seed "$W/j2"
sed -i '/CHECK(env_step_calls == 0/d' "$W/j2/tests/partition_migrate_default_path_host_test.c"
tooth "J. the default-path test stops asserting the step never runs" "J" "$W/j2"

# ── The vacuity control: green on the real tree ────────────────────────────
out="$(bash "$GUARD" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   vacuity control — the guard is GREEN on the untouched tree"
    passed=$((passed + 1))
else
    echo "FAIL: vacuity control — the guard failed on the real tree (exit $rc); every tooth above is meaningless"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "---- $passed teeth bit, $failed did not"
[ "$failed" -eq 0 ]
