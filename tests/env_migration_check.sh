#!/usr/bin/env bash
# tests/env_migration_check.sh — guards the WIRING of POSIX-Environments v0.2
# Phase P3 increments 3–5 (the environment step inside partition_migrate()'s
# critical section, the restore on the destination, and the confirmation-
# gated teardown on the source), not its logic.
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
# ─── P3 increment 4: the restore half ────────────────────────────────────────
# The wire fills a staging slot on the destination; env_migrate_restore()
# turns it back into a live environment. tests/env_migrate_host_test.c
# (Phase G) proves its BEHAVIOUR — the pour's byte fidelity, every refusal,
# the slot-retention rule. The wiring a host test cannot notice is here:
#
#   K. The BSP sweep calls env_migrate_tick(): staged bytes that nothing
#      restores are frames held forever — the wire delivers, the sweep
#      makes live.
#   L. The restore gates OWNERSHIP before it creates: a follower building
#      an environment in a partition served elsewhere is a second,
#      unaccounted tenant.
#   M. Refusal over partial application on the create paths: a create that
#      failed or came back different DROPS the stored record (and the
#      identity-mismatch path ends the environment the create made).
#   N. The slot-retention rule: the first recv_drop appears AFTER the
#      create — a pre-create refusal keeps the slot (the state is
#      pre-attempt, the sweep may retry), and the restore drops the slot
#      itself on the paths where retrying cannot help.
#   O. The pour is validated WHOLE before a byte: the placement refusal is
#      decided before the first memcpy.
#   P. The partition goes back to the state the record recorded: the
#      PARTITION_PAUSED flag across the wire gates a partition_pause on
#      the destination.
#   Q. The sweep's retry is bounded: a period and an attempts cap both
#      exist, so a create that can never succeed stops instead of spinning.
#   R. The host test still exercises the restore (calls it AND the tick) —
#      a test that stops checking is how a property becomes decoration.
#
# ─── Teeth ────────────────────────────────────────────────────────────────
# tests/env_migration_check_smoke.sh mutates a copy of the tree once per
# invariant and requires this guard to go red naming THAT clause — the
# `P3_TOOTH=ownership-first` tooth among them — then requires it green on
# the untouched tree.
#
# ─── P3 increment 5: the confirmation, then the gated teardown ──────────────
# One sentence, the roadmap's rule applied to an environment that moved:
# no record may describe it afterwards. Increments 3 and 4 moved the bytes
# and rebuilt them on the destination, and both deliberately left the
# source's records, sidecars and frames alive (their boundary, named then).
# This closes it, and the closing is GATED: the source tears its half down
# only on the destination's own word that the environment is live with its
# contents there — never on the bytes having been sent.
#
# tests/env_migrate_host_test.c (Phase H) proves the BEHAVIOUR — what the
# word is grounded in, every gate that keeps the records, the one that
# destroys by name, the deferred reclamation. The wiring a host test
# cannot notice:
#
#   S. The wire pair exists end to end: both opcodes exist, the dispatch
#      range reaches LIVE_ACK, the walk actually SENDS the poll, and
#      dspp.c answers from env_migrate_live_query and records through
#      env_migrate_live_ack. A missing leg leaves the source asking a
#      question that can never be answered — or answering one never asked.
#   T. The destination's word is grounded in its own state: live_query is
#      gated on the restored marker AND the marker is set in exactly one
#      place — the pour's success tail. A marker set anywhere else
#      confirms a source off an environment that was never poured.
#   U. The teardown is gated: in env_migrate.c's walk the not-confirmed
#      branch comes BEFORE env_service_destroy(), and a partition this
#      node owns again disarms. A destroy reachable without the word is
#      this increment's defect verbatim.
#   V. The arm happens exactly once, at the end of a fully-confirmed send,
#      and pending confirmations are countable (env_migrate_pending_count).
#      Armed on an abort, it would destroy environments that never left.
#   W. The frames wait for the records: partition.c defers its Step 5
#      reclaim when environments moved, and env_migrate.c's walk completes
#      it only when no record of the partition remains. Reclaiming earlier
#      frees frames a live sidecar still runs on — the double-ownership
#      defect this increment exists to remove.
#   X. Every host test linking net/dspp.c without the real env_migrate.c
#      defines the confirmation pair — clause I's link class, for dspp.c.
#   Y. The host test still exercises the confirmation (Phase H: query,
#      pending count) — a test that stops checking is how a property
#      becomes decoration.
#
# What this guard deliberately does NOT claim: the live clauses the
# roadmap's verification plan names (the environment live on the
# destination, no residue on the source, the lease moved, an attached
# operator seeing the destination's announcement) need a RUNNING CLUSTER
# and land in this file's live arm when one is available. Asserting them
# now would be asserting a boot nobody has run — the restore's wiring
# (clauses K–R) is what a source-only guard can hold.
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
         kernel/env_ckpt.c kernel/env_ckpt.h Makefile net/http.c \
         net/dspp.c net/dspp.h kernel/failover.c user/shell.c \
         tests/env_migration_check.sh \
         tests/env_migrate_host_test.c \
         tests/partition_migrate_phase6_host_test.c \
         tests/partition_migrate_default_path_host_test.c; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P3 increment 3/4/5 wiring at all" >&2
        exit 2
    fi
done
ok "A. every file the environment step's, restore's and confirmation's wiring lives in is present"

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

# ── K. The sweep restores what the wire staged ────────────────────────────
if ! has net/http.c '#include "../kernel/env_migrate.h"'; then
    bad "K. net/http.c no longer includes env_migrate.h — the sweep cannot reach the restore"
elif ! has net/http.c "env_migrate_tick(kernel_tick_counter);"; then
    bad "K. the BSP sweep no longer calls env_migrate_tick() — staged bytes that nothing restores are partition-charged frames held forever; the wire delivers, the sweep makes live"
else
    ok "K. the BSP sweep drives the restore (env_migrate_tick, rate-limited internally)"
fi

# ── L. Ownership is gated BEFORE the create ───────────────────────────────
owner_line=$(grep -nE "partition_get_owner_node\(partition_id\) != cluster_local_node_id\(\)" kernel/env_migrate.c | head -1 | cut -d: -f1)
create_line=$(grep -nE "env_service_create\(partition_id, index" kernel/env_migrate.c | head -1 | cut -d: -f1)
if [ -z "${owner_line:-}" ] || [ -z "${create_line:-}" ]; then
    bad "L. could not locate both the ownership gate and the create in env_migrate.c (gate=${owner_line:-absent}, create=${create_line:-absent}) — a restore with no ownership gate builds a second, unaccounted tenant on a follower"
elif [ "$owner_line" -ge "$create_line" ]; then
    bad "L. the ownership gate (line $owner_line) no longer precedes the create (line $create_line) — a follower could rebuild an environment for a partition it does not serve"
else
    ok "L. the restore gates OWNERSHIP (line $owner_line) before it creates (line $create_line)"
fi

# ── M. A refused create leaves NOTHING behind ─────────────────────────────
drop_count=$(grep -c "env_ckpt_drop(partition_id, index);" kernel/env_migrate.c)
if ! has kernel/env_migrate.c "env_ckpt_restore_refuse(partition_id, index, ENV_CKPT_REFUSE_CREATE"; then
    bad "M. the create-failure path no longer records its refusal in the record layer — the refusal is a log line a grep could miss, not a queryable fact"
elif [ "${drop_count:-0}" -lt 2 ]; then
    bad "M. env_ckpt_drop(partition_id, index) appears only ${drop_count:-0} time(s) — both the resume-refusal and the create-failure paths must drop the stored record, or a refused restore leaves a record describing an environment that does not exist"
else
    ok "M. both pre-create refusal paths drop the stored record (${drop_count} drops), refusal over partial application"
fi

# ── N. The slot-retention rule: dropped only once the environment exists ──
first_drop=$(grep -n "env_migrate_recv_drop(partition_id, index);" kernel/env_migrate.c | head -1 | cut -d: -f1)
if [ -z "${first_drop:-}" ] || [ -z "${create_line:-}" ]; then
    bad "N. could not locate the restore's slot drops (first=${first_drop:-absent}) or the create (${create_line:-absent})"
elif [ "$first_drop" -le "$create_line" ]; then
    bad "N. the first slot drop (line $first_drop) is no longer AFTER the create (line $create_line) — a pre-create refusal would release staged bytes that a retry could still have used"
elif [ "$(grep -c "env_migrate_recv_drop(partition_id, index);" kernel/env_migrate.c)" -lt 4 ]; then
    bad "N. the restore drops the slot on fewer than 4 paths — success, the parked refusal, the validation refusal and the extent-IO failure each must release the frames (a slot that survives an unappliable pour is a frame leak with a name)"
else
    ok "N. the slot is kept through the pre-create refusals and dropped on the paths where the environment exists (first drop line $first_drop, after the create)"
fi

# ── O. The pour is validated WHOLE before a byte ──────────────────────────
place_line=$(grep -n "refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT" kernel/env_migrate.c | head -1 | cut -d: -f1)
pour_line=$(grep -n "dst\[b\] = src\[b\]" kernel/env_migrate.c | head -1 | cut -d: -f1)
if [ -z "${place_line:-}" ] || [ -z "${pour_line:-}" ]; then
    bad "O. could not locate the placement decision (${place_line:-absent}) or the pour (${pour_line:-absent}) — a pour with no placement gate writes bytes that cross for base X into whatever now lives there"
elif [ "$place_line" -ge "$pour_line" ]; then
    bad "O. the placement refusal (line $place_line) is no longer BEFORE the first memcpy (line $pour_line) — refusal over partial application requires every destination validated before any byte"
else
    ok "O. the placement rule is decided (line $place_line) before the first byte of the pour (line $pour_line)"
fi

# ── P. The record's pause comes back with it ──────────────────────────────
if ! has kernel/env_migrate.c "staged.flags & ENV_CKPT_FLAG_PARTITION_PAUSED"; then
    bad "P. the restore no longer reads ENV_CKPT_FLAG_PARTITION_PAUSED off the staged record — the destination would leave a partition running that the source recorded as paused, losing the operator's intent across the wire"
elif ! has kernel/env_migrate.c "partition_pause(partition_id)"; then
    bad "P. the restore no longer calls partition_pause() — the flag is read but not acted on, and a successful migration leaves its partition RUNNING on the destination instead of paused for its operator"
else
    ok "P. the staged record's PAUSED flag gates a partition_pause on the destination"
fi

# ── Q. The sweep's retry is bounded on both axes ──────────────────────────
if ! has kernel/env_migrate.c "#define ENV_MIGRATE_RESTORE_PERIOD_TICKS"; then
    bad "Q. the restore tick has no PERIOD — it would run on every sweep, retrying a failing create at sweep rate (a sweep is load-dependent and unbounded)"
elif ! has kernel/env_migrate.c "#define ENV_MIGRATE_RESTORE_MAX_TRIES"; then
    bad "Q. the restore tick has no ATTEMPTS cap — a create that can never succeed would retry forever, logging every period, with the slot's frames held silently"
elif ! has kernel/env_migrate.c "s->restore_tries >= ENV_MIGRATE_RESTORE_MAX_TRIES"; then
    bad "Q. the attempts cap is defined but never CHECKED — the sweep would still retry forever"
else
    ok "Q. the sweep's retry is bounded: a period, a cap, and the cap actually checked"
fi

# ── R. The host test still exercises the restore ──────────────────────────
if ! has tests/env_migrate_host_test.c "env_migrate_restore("; then
    bad "R. env_migrate_host_test.c no longer calls env_migrate_restore() — Phase G is gone and the restore's behaviour is unproven"
elif ! has tests/env_migrate_host_test.c "env_migrate_tick("; then
    bad "R. env_migrate_host_test.c no longer drives env_migrate_tick() — the sweep's bounds (period, cap, ownership wait) are unproven"
else
    ok "R. the host test still drives both the restore and its sweep"
fi

# ══ P3 increment 5 clauses: the confirmation, then the gated teardown ══

# ── S. The wire pair exists end to end ────────────────────────────────────
if ! has net/dspp.h "DSPP_MIGRATE_ENV_LIVE_REQ"; then
    bad "S. net/dspp.h no longer defines the LIVE_REQ opcode — the source has no question to ask"
elif ! has net/dspp.h "DSPP_MIGRATE_ENV_LIVE_ACK"; then
    bad "S. net/dspp.h no longer defines the LIVE_ACK opcode — the destination has no word to give"
elif ! has net/dspp.c "opcode >= DSPP_MIGRATE_ENV_BEGIN_REQ && opcode <= DSPP_MIGRATE_ENV_INTENT"; then
    bad "S. the ENV dispatch range no longer reaches LIVE_ACK (the range's written terminator must be at least DSPP_MIGRATE_ENV_INTENT=24, which passes LIVE_ACK=23) — the destination's word would be silently dropped on arrival"
elif ! has net/dspp.h "dspp_env_migrate_send_live("; then
    bad "S. net/dspp.h no longer declares the LIVE_REQ sender — the source could not ask even if it wanted to"
elif ! has kernel/env_migrate.c "dspp_env_migrate_send_live("; then
    bad "S. env_migrate.c's walk no longer sends the LIVE_REQ poll — the source would never ask, and every teardown would wait a bounded forever while the destination's word went unrequested"
elif ! has net/dspp.c "env_migrate_live_query(h->partition_id"; then
    bad "S. dspp.c no longer answers LIVE_REQ from env_migrate_live_query() — the destination's word would come from the packet instead of its own state"
elif ! has net/dspp.c "env_migrate_live_ack(h->partition_id"; then
    bad "S. dspp.c no longer records LIVE_ACK through env_migrate_live_ack() — the source would poll into the void and never tear down"
else
    ok "S. the confirmation pair is wired end to end (both opcodes, the dispatch range, the poll, the query and the ack)"
fi

# ── T. The word is grounded in what the destination actually restored ────
marker_set=$(grep -cF "env_migrate_restored_mark(partition_id, index);" kernel/env_migrate.c)
if [ "${marker_set:-0}" -ne 1 ]; then
    bad "T. the restored marker is set ${marker_set:-0} time(s), not exactly once — it must be set ONLY by the pour's success tail; a marker set anywhere else confirms a source off an environment that was never poured, and one set nowhere means nothing is ever confirmed"
elif ! has kernel/env_migrate.c "env_migrate_restored_here("; then
    bad "T. env_migrate_live_query() no longer consults the restored marker — the word would rest on the record's existence, which a refused pour also leaves behind"
else
    ok "T. the destination answers from the restored marker, set exactly once (the pour's success tail)"
fi

# ── U. The teardown is gated on the destination's word ────────────────────
unconfirmed_line=$(grep -nE "if \(!c->confirmed\)" kernel/env_migrate.c | head -1 | cut -d: -f1)
destroy_line=$(grep -nE "env_service_destroy\(env_id" kernel/env_migrate.c | head -1 | cut -d: -f1)
if [ -z "${unconfirmed_line:-}" ] || [ -z "${destroy_line:-}" ]; then
    bad "U. could not locate both the not-confirmed branch and the teardown in env_migrate.c's walk (gate=${unconfirmed_line:-absent}, destroy=${destroy_line:-absent}) — a walk with no gate reaches env_service_destroy() on the arm alone (P3_TOOTH=confirm-first)"
elif [ "$unconfirmed_line" -ge "$destroy_line" ]; then
    bad "U. the not-confirmed branch (line $unconfirmed_line) no longer precedes the teardown (line $destroy_line) — a confirmation not yet received could reach env_service_destroy() (P3_TOOTH=confirm-first)"
elif ! has kernel/env_migrate.c "partition_get_owner_node(c->partition_id) == cluster_local_node_id()"; then
    bad "U. the walk no longer disarms when this node owns the partition again — records that are the truth again (an aborted migration, a return) would be destroyed"
else
    ok "U. the walk gates the teardown: not-confirmed before destroy (line $unconfirmed_line < $destroy_line), and local ownership disarms"
fi

# ── V. The arm: exactly once, after every payload confirmed ───────────────
arm_calls=$(grep -cF "env_migrate_arm_confirms(partition_id, dest_node_id);" kernel/env_migrate.c)
if [ "${arm_calls:-0}" -ne 1 ]; then
    bad "V. the confirmation arm runs ${arm_calls:-0} time(s), not exactly once — armed anywhere but the end of a fully-confirmed send it could destroy environments that never left"
elif ! has kernel/env_migrate.h "env_migrate_pending_count"; then
    bad "V. env_migrate.h no longer declares env_migrate_pending_count() — a confirmation that never arrives must be countable, not suspected"
elif ! has kernel/env_migrate.c "uint32_t env_migrate_pending_count(void)"; then
    bad "V. env_migrate.c no longer defines env_migrate_pending_count() — a declaration with no definition is a link error at best"
else
    ok "V. the arm happens exactly once, at the end of a fully-confirmed send, and pending confirmations are countable"
fi

# ── W. The frames wait for the records ────────────────────────────────────
if ! has kernel/partition.c "if (envs_moved > 0)"; then
    bad "W. partition.c's Step 5 no longer defers the reclamation when environments moved — the source's frames would go back to the pool while its environments' sidecars still run on them (two owners, one frame)"
elif ! has kernel/env_migrate.c "partition_reclaim_all_frames(c->partition_id)"; then
    bad "W. env_migrate.c's walk no longer completes the deferred reclamation — a migrated partition's frames would never come back (a silent, permanent leak)"
elif ! has kernel/env_migrate.c "!env_migrate_partition_records_remain(c->partition_id)"; then
    bad "W. the walk completes the reclamation without the records-remain gate — it could reclaim frames under a record (and sidecars) still standing here, which is the double-ownership defect in miniature"
else
    ok "W. the reclamation is deferred on the move and completed by the walk only when no record of the partition remains"
fi

# ── X. Every dspp.c-linking test stands in for the confirmation pair ──────
missing=""
for t in tests/*_host_test.c; do
    cmd=$(awk '/\* *gcc /{grab=1}
               grab { line=$0
                      gsub(/\r/, "", line)
                      sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
                      print line
                      if (line !~ /\\[[:space:]]*$/) exit }' "$t" | tr '\n' ' ')
    case "$cmd" in
        *net/dspp.c*) ;;
        *) continue ;;
    esac
    case "$cmd" in
        *kernel/env_migrate.c*) continue ;;   # the real definitions are linked
    esac
    if ! grep -qE "env_migrate_live_query\(" "$t"; then
        missing="$missing $t"
    fi
done
if [ -n "$missing" ]; then
    bad "X. these host tests link net/dspp.c (which now routes LIVE_REQ/LIVE_ACK through env_migrate_live_query/live_ack) without linking kernel/env_migrate.c and without a stand-in, so they fail at LINK time:$missing"
else
    ok "X. every host test linking net/dspp.c links the real confirmation pair or defines a stand-in for it"
fi

# ── Y. The host test still exercises the confirmation ────────────────────
if ! has tests/env_migrate_host_test.c "env_migrate_live_query("; then
    bad "Y. env_migrate_host_test.c no longer calls env_migrate_live_query() — what the destination's word is grounded in is unproven"
elif ! has tests/env_migrate_host_test.c "env_migrate_pending_count("; then
    bad "Y. env_migrate_host_test.c no longer checks env_migrate_pending_count() — the gated teardown (armed, waiting, completed) is unproven"
else
    ok "Y. the host test still drives the confirmation pair and the pending count"
fi

# ── Z. A silent peer's announced migration intent defers its declaration ──
# The idle-observer arm (docs/AeroSLS-Idle-Observer-Adoption-Finding-
# v0.1.md): the local in_flight gate covers only nodes that HOLD migration
# state, so a healthy third node declared the busy source DEAD and adopted
# its partitions mid-migration — every node alive, the migrating environment
# destroyed. The declaration path must ask the silent PEER's latch.
if ! has kernel/failover.c "env_migrate_peer_in_flight(peers[i].node_id)"; then
    bad "Z. failover.c's declaration path no longer consults env_migrate_peer_in_flight() — the idle-observer false 'declared DEAD' (a healthy observer adopting a busy source's partitions mid-migration) comes straight back"
elif ! has kernel/env_migrate.h "int env_migrate_peer_in_flight(uint32_t node_id)"; then
    bad "Z. env_migrate.h no longer declares env_migrate_peer_in_flight() — the gate failover.c asks has no contract"
elif ! has kernel/env_migrate.c "env_migrate_recv_intent(uint32_t node_id, uint32_t budget_ticks)"; then
    bad "Z. env_migrate.c no longer defines the intent latch (env_migrate_recv_intent) — no peer's announce can ever be remembered, and every observer is blind again"
else
    ok "Z. a silent peer's announced migration intent defers its declaration, through a declared, defined, clamped latch"
fi

# ── AA. The announce precedes the block at every operator entry ────────────
# Ordering IS the invariant: a blocked node transmits nothing, so an announce
# after the blocking call announces nothing. Three operator-reachable entries
# block the BSP sweep — partition_migrate()'s capture+send, the HTTP
# env-create route's RPC, the shell's 'env create' — and on the consensus
# leader that sweep carries the only heartbeat stream.
aa_pc_b=$(grep -nF 'env_migrate_intent_begin();' kernel/partition.c | head -1 | cut -d: -f1)
aa_pc_s=$(grep -nF 'Step 1 (LPAR Phase 14)' kernel/partition.c | head -1 | cut -d: -f1)
aa_ht_b=$(grep -nF 'env_migrate_intent_begin();' net/http.c | head -1 | cut -d: -f1)
aa_ht_c=$(grep -nF 'int rc = env_service_create' net/http.c | head -1 | cut -d: -f1)
aa_sh_b=$(grep -nF 'env_migrate_intent_begin();' user/shell.c | head -1 | cut -d: -f1)
aa_sh_c=$(grep -nF 'int r = env_service_create' user/shell.c | head -1 | cut -d: -f1)
if [ -z "$aa_pc_b" ] || [ -z "$aa_pc_s" ] || [ "$aa_pc_b" -ge "$aa_pc_s" ]; then
    bad "AA. partition_migrate() no longer announces migration intent BEFORE its Step-1 pause/capture/send — the announce would go out after the sweep already blocked, which is no announce at all"
elif [ -z "$aa_ht_b" ] || [ -z "$aa_ht_c" ] || [ "$aa_ht_b" -ge "$aa_ht_c" ]; then
    bad "AA. the env-create route no longer announces migration intent BEFORE its create RPC — a sidecar boot + 30 s parked-posture wait undeferred on every observer"
elif [ -z "$aa_sh_b" ] || [ -z "$aa_sh_c" ] || [ "$aa_sh_b" -ge "$aa_sh_c" ]; then
    bad "AA. the shell's 'env create' no longer announces migration intent BEFORE its create RPC — the serial-side operator path undeferred"
else
    ok "AA. every operator-reachable blocking entry announces intent while its sweep still runs (before, not after)"
fi

# ── AB. The broadcast is defined, sent, and heard BEFORE the self-filter ───
ab_h=$(awk '/^void dspp_env_migrate_rx/{f=1} f && /env_migrate_recv_intent\(\(uint32_t\)h->node_source_id/{print NR; exit}' net/dspp.c)
ab_f=$(awk '/^void dspp_env_migrate_rx/{f=1} f && /if \(h->node_dest_id != cluster_local_node_id\(\)\) return;/{print NR; exit}' net/dspp.c)
if ! has net/dspp.h "DSPP_MIGRATE_ENV_INTENT"; then
    bad "AB. dspp.h no longer defines the intent opcode — sender and receiver cannot agree on the frame"
elif ! has net/dspp.c "opcode <= DSPP_MIGRATE_ENV_INTENT"; then
    bad "AB. dspp.c's dispatch no longer routes the intent opcode into the migrate RX — the announce falls through the dispatch unheard"
elif ! has kernel/env_migrate.c "dspp_env_migrate_send_intent"; then
    bad "AB. env_migrate.c no longer transmits the announce — a local gate alone cannot spare a THIRD node that holds no migration state"
elif [ -z "$ab_h" ] || [ -z "$ab_f" ] || [ "$ab_h" -ge "$ab_f" ]; then
    bad "AB. the intent RX no longer runs BEFORE the point-to-point self-filter — the announce carries node_dest_id 0 (broadcast), so the filter would drop every peer's announce on every node, leaving exactly the blind spot it exists to close"
else
    ok "AB. the intent announce has an opcode, a sender, and an RX path heard before the self-filter"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "---- env_migration_check: ALL INVARIANTS HOLD"
else
    echo "---- env_migration_check: FAILURES ABOVE"
fi
[ "$fail" -eq 0 ]
