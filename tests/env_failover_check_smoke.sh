#!/usr/bin/env bash
# tests/env_failover_check_smoke.sh — the TEETH for tests/env_failover_check.sh.
#
# ─── What a teeth check is for ───────────────────────────────────────────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE wiring invariant of P3's failover half (destination
# (b): adopt the partition, report the unrestored environments BY NAME) in a
# throwaway copy of the tree and requires the guard to (a) exit 1 and (b) name
# THAT invariant. Requiring the right named clause is the half that matters: a
# guard that reddened on everything, or on the wrong clause, would pass a tooth
# that only checked the exit code.
#
# The first tooth is worth stating in prose, because it is this increment's
# defect verbatim:
#
#   * `silent-failover` — the adoption completes and says NOTHING about the
#     environments. This is Appendix B's own named gap ("a dead node's
#     environments vanish with no name in the report"), reproduced
#     deliberately so its regression test is an environment-shaped one. An
#     operator sees "Adopted partition 3" and has no way to learn that
#     partition 3's environment index 2 went with the dead node.
#
# The second worth-stating tooth is increment 11's defect verbatim:
#
#   * `refused-as-restored` — the replay REFUSES a record and the report
#     calls it restored. Both outcomes clear the pending bit, so a caller
#     that reads only pending cannot tell them apart; the per-slot refused
#     mark is the only thing that can, and deleting its one query is the
#     whole defect. It lies about a dead node's data in the optimistic
#     direction, which is the direction nobody questions.
#
# The supporting cast, each a way the report can be present but wrong:
#
#   * `unscoped-report` — the owner check removed, so the report names
#     environments in partitions this node does not own. Worse than silence
#     in one specific way: it is confident, and it is about someone else's
#     work.
#   * `stale-count` / `stale-restored-count` — a counter reset moved out of
#     function entry, so every early return leaves the PREVIOUS recovery's
#     number standing (the gap, and the success).
#   * `names-not-emitted` — the tree stops producing environment nodes, so
#     the walk has nothing to find. The wiring-removed failure, one level up.
#   * `names-before-partitions` — the emission order flipped, so each
#     environment's parent lookup misses its partition and it lands on root.
#   * `version-not-bumped` — v2's node type without v2's version, so a v1
#     tree is misread instead of refused.
#   * `no-durable-restore` / `no-replay-driver` — the restore half present
#     but inert: records armed and never drained, or neither armed nor
#     driven. The report still succeeds; nothing comes back.
#   * `restored-not-pinned` — the host test asserts only >= 0, so a stale
#     restored count passes every assertion that remains.
#   * `tracker-shift-broken` — env_ckpt.c's drop() stops shifting the
#     refused mark, so after a drop it points at a different environment
#     and a healthy one is reported refused.
#   * the six N teeth — destination (a)'s promotion wired out one link at
#     a time (the lookup, the newer-wins ordering, the promotion call, the
#     replay call) and its classification unpinned from the host test twice
#     (the hole-to-replica restore, the fallback's dropped slot).
#
# The vacuity control is last and mandatory: the guard must be GREEN on the
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
GUARD="$ROOT/tests/env_failover_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads, kept in one list so a tooth can never pass
# because the copy it mutated was missing something the guard needed.
SEED_FILES=(
    kernel/state_tree.h
    kernel/state_tree.c
    kernel/failover.c
    kernel/failover.h
    kernel/env_ckpt.c
    kernel/env_ckpt.h
    kernel/env_service.h
    tests/checkpoint_recovery_host_test.c
    tests/env_failover_check.sh
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
}

passed=0
failed=0
tooth() {   # tooth <name> <expected-clause> <dir> <file-that-must-differ> <pristine-copy>
    # VACUITY GUARD. A tooth whose sed did not apply is worse than no tooth:
    # it runs the guard against the UNMUTATED tree, the guard is green (as it
    # should be), and the smoke reports "did not bite" — pointing the reader
    # at the GUARD when the fault was the mutation. That is exactly what
    # happened to the first three teeth written here (one typoed the wrong
    # identifier, one renamed a line to a string that still prefix-matched
    # the guard's own lookup, one deleted a static initializer instead of the
    # reset). So every tooth now PROVES its mutation landed first.
    local name="$1" clause="$2" dir="$3" mutated="$4" pristine="$5" out rc
    if cmp -s "$dir/$mutated" "$pristine"; then
        echo "FAIL: $name did NOT mutate $mutated — the sed applied nothing, so the guard was run against the pristine tree and this tooth proves nothing"
        failed=$((failed + 1))
        return
    fi
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

echo "env_failover_check_smoke — teeth for the P3 failover (b) wiring guard"
echo "====================================================================="
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
rm -f "$W/kernel/failover.c"
if bash "$GUARD" "$W" >/dev/null 2>&1; then
    echo "FAIL: the guard PASSED with kernel/failover.c missing — it cannot have evaluated the wiring it claims to hold"
    failed=$((failed + 1))
else
    rc=$?
    if [ "$rc" -eq 2 ]; then
        echo "ok:   a missing failover.c aborts (exit 2) instead of reporting the wiring sound"
        passed=$((passed + 1))
    else
        echo "FAIL: a missing failover.c exited $rc; expected 2 (cannot evaluate)"
        failed=$((failed + 1))
    fi
fi

# ── THE TOOTH: silent-failover — the adoption says nothing ─────────────────
# The defect verbatim: the loop that names the unrestored environments is
# deleted. The adoption still flips ownership and still succeeds, so nothing
# else in the system notices — the environments simply stop being mentioned.
seed "$W/silent"
sed -i '/nodes\[i\].type != ST_NODE_ENVIRONMENT/d' "$W/silent/kernel/failover.c"
tooth "silent-failover: the environment walk deleted (a dead node's environments vanish with no name in the report)" "D" "$W/silent" kernel/failover.c "$ROOT/kernel/failover.c"

# ── unscoped-report: the owner check removed ──────────────────────────────
seed "$W/unscoped"
sed -i '/partition_get_owner_node(part_id) != my_id/d' "$W/unscoped/kernel/failover.c"
tooth "unscoped-report: the adoption names environments of partitions it does not own" "E" "$W/unscoped" kernel/failover.c "$ROOT/kernel/failover.c"

# ── stale-count: the reset moved out of function entry ─────────────────────
# Anchored to the INDENTED reset inside failover_recover_from(). The static
# initializer at file scope matches a looser pattern, and deleting IT is
# harmless — statics zero-init — so a tooth that deleted it instead was
# proving nothing. That is what the first draft did.
seed "$W/stale"
sed -i '/^    last_unrestored_envs = 0;$/d' "$W/stale/kernel/failover.c"
tooth "stale-count: the count no longer resets at entry (a later recovery reads the previous one's gap)" "F" "$W/stale" kernel/failover.c "$ROOT/kernel/failover.c"

# ── names-not-emitted: the tree stops producing the nodes ─────────────────
seed "$W/names"
sed -i 's/nodes\[n\].type         = ST_NODE_ENVIRONMENT;/nodes[n].type         = ST_NODE_STREAM;   \/* MUTATED *\/\//' "$W/names/kernel/state_tree.c"
tooth "names-not-emitted: the tree produces no environment node (the walk finds nothing to name)" "B" "$W/names" kernel/state_tree.c "$ROOT/kernel/state_tree.c"

# ── names-before-partitions: the emission order flipped ───────────────────
# A DECOY environment emission inserted BEFORE the partition block, so the
# guard's own ordering lookup finds an environment node ahead of any
# partition — the real ordering branch. Renaming the partition line instead
# (the first draft) was vacuous: the renamed string still PREFIX-MATCHED the
# guard's /type         = ST_NODE_PARTITION/ lookup, so the ordering claim
# was unaffected and the tooth proved nothing.
seed "$W/order"
sed -i '/type         = ST_NODE_ROOT;/a\        nodes[n].type         = ST_NODE_ENVIRONMENT;   /* MUTATED decoy */' "$W/order/kernel/state_tree.c"
tooth "names-before-partitions: an environment node emitted before any partition (each lands parented to root)" "C" "$W/order" kernel/state_tree.c "$ROOT/kernel/state_tree.c"

# ── version-not-bumped: v2's node type without v2's version ───────────────
seed "$W/ver"
sed -i 's/#define ST_VERSION  2/#define ST_VERSION  1/' "$W/ver/kernel/state_tree.h"
tooth "version-not-bumped: a tree carrying environment nodes still claims to be v1 (misread instead of refused)" "H" "$W/ver" kernel/state_tree.h "$ROOT/kernel/state_tree.h"

# ── accessor-not-pinned: the host test stops asserting the count ──────────
seed "$W/pin"
sed -i 's/failover_last_unrestored_envs() == 2/failover_last_unrestored_envs() >= 0/' "$W/pin/tests/checkpoint_recovery_host_test.c"
sed -i 's/failover_last_unrestored_envs() == 0/failover_last_unrestored_envs() >= 0/' "$W/pin/tests/checkpoint_recovery_host_test.c"
tooth "accessor-not-pinned: the host test asserts only >= 0 (a stale count would pass every assertion)" "G" "$W/pin" tests/checkpoint_recovery_host_test.c "$ROOT/tests/checkpoint_recovery_host_test.c"

# ── no-durable-restore: the arm call renamed away ─────────────────────────
# Increment 11's wiring-removed failure: the report still walks, still
# scopes, still counts — it just never asks the durable store for anything.
seed "$W/noarm"
sed -i 's/env_ckpt_restore_arm(part_id, env_idx)/env_ckpt_ARM_GONE(part_id, env_idx)/' "$W/noarm/kernel/failover.c"
tooth "no-durable-restore: the records are never armed for replay (the restore half is present but inert)" "I" "$W/noarm" kernel/failover.c "$ROOT/kernel/failover.c"

# ── no-replay-driver: armed, never drained ────────────────────────────────
# Arming without driving leaves every record pending forever — the attempt
# exists, the create round trip never happens, and nothing in the report
# says which half died.
seed "$W/nodrive"
sed -i 's/env_service_restore_pending(&rep)/env_service_DRIVER_GONE(\&rep)/' "$W/nodrive/kernel/failover.c"
tooth "no-replay-driver: the armed records are never drained through env_service_restore_pending()" "J" "$W/nodrive" kernel/failover.c "$ROOT/kernel/failover.c"

# ── refused-as-restored: the one query that separates them ────────────────
# THE increment-11 tooth. Both a settle and a refusal clear the pending bit,
# so the per-slot refused mark is the only fact that tells a restored
# environment from a refused one. Delete its query and the report calls a
# REFUSED record restored — the lie, in the direction nobody questions.
seed "$W/liar"
sed -i '/} else if (env_ckpt_restore_refused_record(p, idx)) {/d' "$W/liar/kernel/failover.c"
tooth "refused-as-restored: the refused mark is never consulted (a REFUSED record is reported restored)" "K" "$W/liar" kernel/failover.c "$ROOT/kernel/failover.c"

# ── stale-restored-count: the success counter survives an early return ────
seed "$W/rst"
sed -i '/^    last_restored_envs = 0;$/d' "$W/rst/kernel/failover.c"
tooth "stale-restored-count: the restored count no longer resets at entry (a later recovery reads the previous one's success)" "L" "$W/rst" kernel/failover.c "$ROOT/kernel/failover.c"

# ── restored-not-pinned: the host test stops asserting the success ────────
seed "$W/rpin"
sed -i 's/failover_last_restored_envs() == 2/failover_last_restored_envs() >= 0/' "$W/rpin/tests/checkpoint_recovery_host_test.c"
sed -i 's/failover_last_restored_envs() == 0/failover_last_restored_envs() >= 0/' "$W/rpin/tests/checkpoint_recovery_host_test.c"
tooth "restored-not-pinned: the host test asserts the restored count only >= 0 (a stale success passes everything)" "L" "$W/rpin" tests/checkpoint_recovery_host_test.c "$ROOT/tests/checkpoint_recovery_host_test.c"

# ── tracker-shift-broken: drop() stops moving the refused mark ────────────
seed "$W/shift"
sed -i '/ec_restore_refused_slot\[i\] = ec_restore_refused_slot\[i + 1\];/d' "$W/shift/kernel/env_ckpt.c"
tooth "tracker-shift-broken: drop() no longer shifts the refused mark (after a drop it describes a different environment)" "M" "$W/shift" kernel/env_ckpt.c "$ROOT/kernel/env_ckpt.c"

# ── N: destination (a)'s third increment — the replica fills the hole ──────
# Four ways the promotion can be present in the source and absent from the
# wiring, plus two ways its CLASSIFICATION can quietly stop being a property.
# Each reddens clause N and nothing about the older (b) clauses, which is
# the point: (a) landed on top of (b), and a reader deleting (a) must be
# told they deleted (a).

seed "$W/nocons"
sed -i '/env_replica_lookup(part_id, env_idx, \&rep)/d' "$W/nocons/kernel/failover.c"
tooth "replica-not-consulted: the survivor never looks in its replica band (a fillable hole is named 'no durable record' again)" "N" "$W/nocons" kernel/failover.c "$ROOT/kernel/failover.c"

seed "$W/noorder"
sed -i 's/(uint64_t)rec->sequence < rep.seq/(uint64_t)0 < rep.seq/' "$W/noorder/kernel/failover.c"
tooth "newer-wins-gone: the capture-seq comparison between record and replica removed (the older source could be applied over the newer)" "N" "$W/noorder" kernel/failover.c "$ROOT/kernel/failover.c"

seed "$W/nopromote"
sed -i 's/env_replica_promote(part_id, env_idx)/env_promote_GONE(part_id, env_idx)/' "$W/nopromote/kernel/failover.c"
tooth "no-promotion: a found replica is never staged (destination (a) without its third increment)" "N" "$W/nopromote" kernel/failover.c "$ROOT/kernel/failover.c"

seed "$W/noreplay"
sed -i 's/env_migrate_restore(part_id, env_idx)/env_migrate_RESTORE_GONE(part_id, env_idx)/' "$W/noreplay/kernel/failover.c"
tooth "no-replay-of-replica: the promoted replica is never replayed through env_migrate_restore() (staged bytes, no classifier)" "N" "$W/noreplay" kernel/failover.c "$ROOT/kernel/failover.c"

seed "$W/rpin2"
sed -i 's/both hole environments came back from the REPLICA/both hole environments came back somehow/' "$W/rpin2/tests/checkpoint_recovery_host_test.c"
tooth "replica-restore-not-pinned: the host test no longer pins the hole-to-replica restore (the count is a serial claim again)" "N" "$W/rpin2" tests/checkpoint_recovery_host_test.c "$ROOT/tests/checkpoint_recovery_host_test.c"

seed "$W/fbpin"
sed -i 's/the staged slot was dropped on the way/the staged slot was left standing/' "$W/fbpin/tests/checkpoint_recovery_host_test.c"
tooth "fallback-not-pinned: the fallback arm stops asserting the staged slot was dropped (a double-drive under (b) reads as a plain gap)" "N" "$W/fbpin" tests/checkpoint_recovery_host_test.c "$ROOT/tests/checkpoint_recovery_host_test.c"

# ── The vacuity control: green on the real tree ───────────────────────────
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
