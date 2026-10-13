#!/usr/bin/env bash
# tests/env_migration_check.sh — guards the WIRING of POSIX-Environments v0.2
# Phase P3 increment 3 (the environment step inside partition_migrate()'s
# critical section), not its logic.
#
# ─── Why this guard exists, and what kind of thing it checks ────────────────
# tests/env_migrate_host_test.c (Phase F) proves the step's BEHAVIOUR — the
# freeze, the per-environment send, the thaw on both paths, every refusal —
# and tests/partition_migrate_phase6_host_test.c's Scenario 7 proves the
# orchestration's ORDERING at runtime. Neither can notice the wiring being
# quietly REMOVED: a step that is correct, tested, and no longer called is
# the exact failure kernel/persist.c's own comments record twice ("existed,
# was correct as far as anyone could tell, and was called from nowhere").
#
# So this guard is source-only and reads the tree: it needs no build, no
# NVMe and no boot, which is also why it runs in CI's verify job on every
# push.
#
# The subject is one sentence, and the roadmap states it as a rule: never
# move ownership without the data (v0.2 §7's bullet 4, learned from a live
# cluster that handed ownership over while page 0 was refused). P3's
# `ownership-first` tooth is that rule applied to an environment instead of
# a stream. Everything below exists because "the call is there, it is before
# the handoff, and a shortfall stops it" can each be deleted independently.
#
# ─── The invariants, and what breaks if one is dropped ────────────────────
#   A. Files exist (else exit 2 — the guard could not be evaluated at all).
#   B. partition.c both includes env_migrate.h and CALLS
#      env_migrate_partition() — the wiring existing at all. This is
#      roadmap Appendix B's own named gap ("partition_migrate() does not
#      move an environment").
#   C. ORDER: the call precedes partition_set_owner_node() in the file. The
#      handoff is what makes the destination authoritative; a payload step
#      after it is the defect, not the fix.
#   D. The abort: a non-constant condition comparing the step's result and
#      its counts sits between the call and the handoff, and returns from
#      inside that block. Without it the counts are decoration — the live
#      cluster's own bug, where 0 sent against an expectation still handed
#      ownership over and reported OK.
#   E. The default path is gated on cluster_local_node_id() != 0, exactly
#      as the stream and context steps are: with no real cluster identity
#      there is nowhere to send an environment, and the step must be a
#      no-op rather than a wire attempt to nowhere.
#   F. env_migrate.c freezes BEFORE it sends anything and releases on BOTH
#      paths out (a release inside the failure branch and one after the
#      loop). A freeze with one release is a tenant left marked frozen
#      after an abort — the `leak-unpause` shape, on the migrate side.
#   G. env_ckpt.c's partition-scoped quiesce writes both flag bits and the
#      release restores the PRIOR flags. Restoring is what keeps an aborted
#      migration from leaving a running partition described as frozen.
#   H. The pending-record gate is wired end to end (declared in env_ckpt.h,
#      defined in env_ckpt.c, called in env_migrate.c): a record this boot
#      has not replayed describes an environment that does not exist here,
#      and sending it would read region bases nothing has allocated.
#   I. env_ckpt.h declares what env_ckpt.c defines (quiesce/release
#      partition), and partition.c's new dependency is satisfied in EVERY
#      host test that links kernel/partition.c — by linking
#      kernel/env_migrate.c or by defining the stand-in. The repo has hit
#      this class of omission before ("one test gained the stubs and
#      another did not, and the suite went red at the linker").
#   J. The two tests that own this property still ASSERT it: the orchestration
#      test records the owner at the instant the step runs (Scenario 7's
#      `env_step_saw_owner`), and the default-path test asserts the step
#      never runs without a cluster. A test that stops checking is how a
#      property becomes decoration.
#
# ─── Teeth ────────────────────────────────────────────────────────────────
# tests/env_migration_check_smoke.sh mutates a copy of the tree once per
# invariant and requires this guard to go red naming THAT clause — the
# `P3_TOOTH=ownership-first` tooth among them — then requires it green on
# the untouched tree.
#
# What this guard deliberately does NOT claim: the live clauses the
# roadmap's verification plan names (the environment live on the
# destination, no residue on the source, the lease moved, an attached
# operator seeing the destination's announcement) need increment 4's
# restore half, and land in this file's live arm then. Asserting them now
# would be asserting a boot nobody has run.
#
# Optional argument: the repository root to inspect (the smoke's hermetic
# seam; every real caller passes nothing and gets this script's own parent).
#
# Exit: 0 all invariants hold, 1 one failed, 2 precondition missing.
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }

# has <file> <fixed string> -> 0 when present
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

# ── A. Preconditions ──────────────────────────────────────────────────────
for f in kernel/partition.c kernel/env_migrate.c kernel/env_migrate.h \
         kernel/env_ckpt.c kernel/env_ckpt.h Makefile \
         tests/env_migration_check.sh \
         tests/env_migrate_host_test.c \
         tests/partition_migrate_phase6_host_test.c \
         tests/partition_migrate_default_path_host_test.c; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P3 increment 3 wiring at all" >&2
        exit 2
    fi
done
ok "A. every file the environment step's wiring lives in is present"

# ── B. The wiring exists at all ───────────────────────────────────────────
if ! has kernel/partition.c '#include "env_migrate.h"'; then
    bad "B. kernel/partition.c no longer includes env_migrate.h — the migration cannot reach the environment step"
elif ! grep -qE "env_migrate_partition\(" kernel/partition.c; then
    bad "B. kernel/partition.c does not call env_migrate_partition() — migration moves a partition's streams and ownership but leaves its environments behind (roadmap Appendix B's named gap)"
else
    ok "B. partition.c includes env_migrate.h and calls env_migrate_partition()"
fi

# ── C. ORDER: payload before handoff ──────────────────────────────────────
# The pattern names the ARGUMENTS, not just the callee: partition.c's own
# include line mentions the function in a comment, and a guard that matched
# that comment would place the "call" above every other line in the file and
# pass no matter where the real call went.
env_call_line=$(grep -nE "env_migrate_partition\(partition_id" kernel/partition.c | head -1 | cut -d: -f1)
handoff_line=$(grep -nE "partition_set_owner_node\(partition_id, dest_node_id\)" kernel/partition.c | head -1 | cut -d: -f1)
if [ -z "${env_call_line:-}" ] || [ -z "${handoff_line:-}" ]; then
    bad "C. could not locate both the environment step and the ownership handoff in partition.c (call=${env_call_line:-absent}, handoff=${handoff_line:-absent})"
elif [ "$env_call_line" -ge "$handoff_line" ]; then
    bad "C. the environment step (line $env_call_line) no longer precedes the ownership handoff (line $handoff_line) — ownership could move before the payload lands (P3_TOOTH=ownership-first)"
else
    ok "C. the environment step (line $env_call_line) runs BEFORE the ownership handoff (line $handoff_line)"
fi

# ── D. A shortfall actually stops it ──────────────────────────────────────
abort_line=$(grep -nE "if \(envs_rc != 0 \|\| envs_moved < envs_expected\)" kernel/partition.c | head -1 | cut -d: -f1)
if [ -z "${abort_line:-}" ]; then
    bad "D. the abort condition comparing env_migrate_partition()'s result and counts is gone from partition.c — without it the counts are decoration and ownership moves regardless (the live cluster's own defect, reproduced at the environment level)"
elif [ -n "${env_call_line:-}" ] && [ -n "${handoff_line:-}" ] && \
     { [ "$abort_line" -le "$env_call_line" ] || [ "$abort_line" -ge "$handoff_line" ]; }; then
    bad "D. the abort condition (line $abort_line) is no longer BETWEEN the environment step and the handoff — it must stop the handoff, not merely exist"
else
    # the block after that if must return before Step 4
    after=$((abort_line + 1))
    ret_line=$(awk -v s="$after" 'NR>=s && NR<s+30 && /return 1;/ { print NR; exit }' kernel/partition.c)
    if [ -z "$ret_line" ]; then
        bad "D. the abort condition no longer returns from inside its block (no 'return 1;' within 30 lines after line $abort_line) — the migration would fall through to the ownership handoff anyway"
    elif [ -n "${handoff_line:-}" ] && [ "$ret_line" -ge "$handoff_line" ]; then
        bad "D. the abort's return (line $ret_line) is no longer before the ownership handoff (line $handoff_line)"
    else
        ok "D. a shortfall (envs_rc != 0 or envs_moved < envs_expected) returns at line $ret_line, before the handoff"
    fi
fi

# ── E. The default path never attempts it ─────────────────────────────────
# Checked on the line ABOVE the call rather than anywhere in the file:
# partition.c carries three cluster_local_node_id() branches (streams,
# contexts, environments) and grepping the file as a whole would still pass
# after the environment one was replaced with an unconditional call.
gate_line=$(grep -nF "? env_migrate_partition(" kernel/partition.c | head -1 | cut -d: -f1)
if [ -z "${gate_line:-}" ]; then
    bad "E. the environment step is no longer reached through a cluster-identity gate at all — the default single-node posture would attempt a wire transfer with nowhere to send it"
elif [ "$(sed -n "$((gate_line - 1))p" kernel/partition.c | grep -cF 'cluster_local_node_id() != 0')" -eq 0 ]; then
    bad "E. the environment step (line $gate_line) is no longer gated on cluster_local_node_id() != 0 — with no real cluster identity there is nowhere to send an environment, and the default posture must stay a no-op"
else
    ok "E. the step sits behind cluster_local_node_id() != 0 (line $((gate_line - 1))), like the stream and context steps"
fi

# ── F. The freeze is released on every path out ───────────────────────────
quiesce_calls=$(grep -c "env_ckpt_quiesce_partition(" kernel/env_migrate.c)
release_calls=$(grep -c "env_ckpt_release_partition(&freeze)" kernel/env_migrate.c)
if [ "${quiesce_calls:-0}" -eq 0 ]; then
    bad "F. env_migrate.c no longer freezes the partition's records before sending them — an unfrozen payload is a moving target, which is the gate env_migrate_send() exists to enforce"
elif [ "${release_calls:-0}" -lt 2 ]; then
    bad "F. env_migrate.c calls env_ckpt_release_partition(&freeze) only ${release_calls:-0} time(s) — one on the failure path and one after the loop are both required, or an aborted migration leaves its records marked frozen (the leak-unpause shape)"
else
    ok "F. the step freezes once and releases on ${release_calls} paths out (failure and success)"
fi

# ── G. The freeze marks both bits and the release restores what it found ──
if ! has kernel/env_ckpt.c "env_ckpt_table[i].flags |= ENV_CKPT_FLAG_QUIESCED"; then
    bad "G. env_ckpt_quiesce_partition() no longer sets ENV_CKPT_FLAG_QUIESCED — the record that crosses would be refused by the destination's restore rule 2 (unquiesced)"
elif ! has kernel/env_ckpt.c "ENV_CKPT_FLAG_PARTITION_PAUSED"; then
    bad "G. the partition-scoped quiesce no longer records ENV_CKPT_FLAG_PARTITION_PAUSED — a destination's restore would not put the partition back frozen"
elif ! has kernel/env_ckpt.c "env_ckpt_table[i].flags = q->prior_flags[i]"; then
    bad "G. env_ckpt_release_partition() no longer restores each record's PRIOR flags — an aborted migration would leave a running partition described as paused-and-frozen, which is a lie the capture's own rules exist to prevent"
else
    ok "G. the freeze sets both flag bits and the release restores the prior flags bit for bit"
fi

# ── H. The not-yet-replayed gate, wired end to end ────────────────────────
if ! has kernel/env_ckpt.h "env_ckpt_restore_pending_record"; then
    bad "H. env_ckpt.h no longer declares env_ckpt_restore_pending_record() — a caller cannot ask whether one record's environment exists on this boot"
elif ! has kernel/env_ckpt.c "env_ckpt_restore_pending_record(uint32_t partition_id, uint32_t index)"; then
    bad "H. env_ckpt.c no longer defines env_ckpt_restore_pending_record() — a declaration with no definition is a link error at best"
elif ! has kernel/env_migrate.c "env_ckpt_restore_pending_record("; then
    bad "H. env_migrate.c no longer calls the pending-record gate — a record awaiting this boot's replay has no live regions, and sending it would read whatever now occupies those frames"
else
    ok "H. the pending-record gate is declared, defined, and called before anything is read"
fi

# ── I. Declared where defined, stubbed where the orchestrator is linked ───
missing=""
for t in tests/*_host_test.c; do
    # The build command is read EXACTLY the way tests/run_all.sh reads it —
    # start at the "* gcc" line and keep the backslash continuations — not
    # by looking for the phrase "Build and run:", which is only one way a
    # header states it. tests/partition_host_test.c writes "built and run
    # directly:" instead, and a guard that searched for the phrase would
    # never see that it links partition.c: it would pass while the test
    # failed at the linker, which is the omission this clause exists for.
    cmd=$(awk '/\* *gcc /{grab=1}
               grab { line=$0
                      gsub(/\r/, "", line)
                      sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
                      print line
                      if (line !~ /\\[[:space:]]*$/) exit }' "$t" | tr '\n' ' ')
    case "$cmd" in
        *kernel/partition.c*) ;;
        *) continue ;;
    esac
    case "$cmd" in
        *kernel/env_migrate.c*) continue ;;   # the real definition is linked
    esac
    if ! grep -qE "env_migrate_partition\(" "$t"; then
        missing="$missing $t"
    fi
done
if [ -n "$missing" ]; then
    bad "I. these host tests link kernel/partition.c (which now references env_migrate_partition()) without linking kernel/env_migrate.c and without a stand-in, so they fail at LINK time:$missing"
else
    ok "I. every host test linking kernel/partition.c links the real step or defines a stand-in for it"
fi

# ── J. The tests that own the property still assert it ────────────────────
if ! has tests/partition_migrate_phase6_host_test.c "env_step_saw_owner == 1"; then
    bad "J. partition_migrate_phase6_host_test.c no longer asserts the owner AT THE INSTANT the environment step ran — Scenario 7 would still pass while the step moved behind the handoff, which is the one thing it exists to catch"
elif ! has tests/partition_migrate_default_path_host_test.c "env_step_calls == 0"; then
    bad "J. partition_migrate_default_path_host_test.c no longer asserts the step never runs without a cluster — the default posture is now assumed rather than measured"
else
    ok "J. both orchestration tests still assert their half: ownership local when the step ran, and the step never taken by default"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "---- env_migration_check: ALL INVARIANTS HOLD"
else
    echo "---- env_migration_check: FAILURES ABOVE"
fi
[ "$fail" -eq 0 ]
