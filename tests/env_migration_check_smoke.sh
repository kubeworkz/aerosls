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
# The restore half (P3 increment 4) adds its own worth-stating pair:
#
#   * `L` — the ownership gate moved AFTER the create: a follower could
#     rebuild an environment for a partition it does not serve.
#   * `N` — a pre-create refusal releases the staged slot: bytes a retry
#     could still have used are freed on a path where nothing was created.
#
# The confirmation half (P3 increment 5, the gated teardown) adds the one
# that is this increment's defect verbatim, plus its supporting cast:
#
#   * `U` — the not-confirmed branch deleted: a teardown reachable on the
#     ARM alone, before the destination has said a word. The source would
#     destroy its records for an environment whose fate there is unknown.
#   * `T` — the restored marker no longer set exactly once, so the word no
#     longer rests on what the destination actually poured.
#   * `S`/`V`/`W` — a leg of the confirmation wire removed, the arm
#     duplicated beyond the fully-confirmed send, and the frames reclaimed
#     without the records-remain gate (two owners, one frame).
#   * `X`/`Y` — a dspp.c-linking host test left without the stand-in, and
#     the host test itself stopped exercising the confirmation.
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
    kernel/failover.c
    kernel/failover.h
    kernel/env_service.c
    kernel/env_payload.c
    tests/env_payload_host_test.c
    net/http.c
    net/dspp.c
    net/dspp.h
    user/shell.c
    tests/env_migration_check.sh
    tests/env_migrate_host_test.c
    tests/partition_migrate_phase6_host_test.c
    tests/partition_migrate_default_path_host_test.c
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/net" "$1/tests" "$1/user"
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

echo "env_migration_check_smoke — teeth for the P3 increments 3–5 wiring guard"
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
# Every call, not just the send-side one: increment 4 added a second gate
# (the restore's own live check), and a tooth that left any one of them in
# place would leave clause H green and prove nothing.
sed -i '/env_ckpt_restore_pending_record(/d' "$W/h/kernel/env_migrate.c"
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

# ══ P3 increment 4 teeth: the restore half ═══════════════════════════════

# ── K. The sweep stops restoring what the wire staged ─────────────────────
seed "$W/k"
sed -i '/env_migrate_tick(kernel_tick_counter);/d' "$W/k/net/http.c"
tooth "K. the BSP sweep no longer drives the restore (staged bytes held forever)" "K" "$W/k"

# ── L. The ownership gate moved AFTER the create ──────────────────────────
# The two lines of the gate are cut and re-inserted after the create, so the
# clause must fail on the ORDER rather than on the gate's mere presence.
seed "$W/l"
python3 - "$W/l/kernel/env_migrate.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
start = next(i for i, l in enumerate(lines)
             if "partition_get_owner_node(partition_id) != cluster_local_node_id()" in l)
gate = lines[start:start + 2]
del lines[start:start + 2]
create = next(i for i, l in enumerate(lines)
              if "int rc = env_service_create(partition_id, index" in l)
lines[create + 1:create + 1] = gate
open(p, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
PY
tooth "L. the ownership gate moved after the create (a follower could rebuild someone else's tenant)" "L" "$W/l"

# ── M. The create-failure path stops dropping the stored record ───────────
seed "$W/m"
python3 - "$W/m/kernel/env_migrate.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
idx = [i for i, l in enumerate(lines) if l.strip() == "env_ckpt_drop(partition_id, index);"]
del lines[idx[-1]]   # the create-failure path's drop
open(p, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
PY
tooth "M. a refused create leaves its stored record behind (half-applied restore)" "M" "$W/m"

# ── N. The slot released BEFORE the environment exists ────────────────────
seed "$W/n"
python3 - "$W/n/kernel/env_migrate.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
inc = next(i for i, l in enumerate(lines)
           if "return env_migrate_refuse(ENV_MIGRATE_REFUSE_INCOMPLETE" in l)
lines[inc:inc] = ["            env_migrate_recv_drop(partition_id, index);"]
open(p, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
PY
tooth "N. a pre-create refusal now releases the staged slot (a retry could still have used the bytes)" "N" "$W/n"

# ── O. The placement gate deleted: the pour runs unvalidated ──────────────
seed "$W/o"
sed -i '/refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT;/d' "$W/o/kernel/env_migrate.c"
tooth "O. the placement rule gone: bytes cross for base X and land wherever" "O" "$W/o"

# ── P. The flag-pause read but never acted on ─────────────────────────────
seed "$W/p"
sed -i '/(void)partition_pause(partition_id);/d' "$W/p/kernel/env_migrate.c"
tooth "P. the staged record's PAUSED flag no longer pauses the destination's partition" "P" "$W/p"

# ── Q. The attempts cap defined but never enforced ────────────────────────
seed "$W/q"
sed -i '/#define ENV_MIGRATE_RESTORE_MAX_TRIES/d' "$W/q/kernel/env_migrate.c"
tooth "Q. the sweep's attempts cap removed (a failing create retries forever)" "Q" "$W/q"

# ── R. The host test stops exercising the restore ────────────────────────
seed "$W/r"
sed -i 's/env_migrate_restore(/env_migrate_restore_MUTATED(/g' "$W/r/tests/env_migrate_host_test.c"
tooth "R. the host test no longer calls the restore (Phase G unproven)" "R" "$W/r"

# ══ P3 increment 5 teeth: the confirmation, then the gated teardown ══════

# ── S. A leg of the confirmation wire removed (the ack never recorded) ────
seed "$W/s"
sed -i '/env_migrate_live_ack(h->partition_id/d' "$W/s/net/dspp.c"
tooth "S. dspp.c stops recording the destination's word (the source polls into the void)" "S" "$W/s"

# ── T. The restored marker no longer set exactly once ─────────────────────
# Removed from the pour's success tail: the marker would never be set, so
# nothing would ever be confirmed — and if it were set ANYWHERE ELSE the
# word would rest on something other than the pour. Either way the count
# must stop being exactly one.
seed "$W/t"
sed -i '/env_migrate_restored_mark(partition_id, index);/d' "$W/t/kernel/env_migrate.c"
tooth "T. the restored marker no longer set by the pour's success tail (the word ungrounded)" "T" "$W/t"

# ── U. THE TOOTH: the not-confirmed gate deleted ──────────────────────────
# The increment's defect verbatim: a teardown reachable on the arm alone,
# before the destination has said the environment is live there. The source
# would destroy the records (and sidecars, and frames) of an environment
# whose fate on the destination is unknown.
seed "$W/u"
sed -i '/if (!c->confirmed) {/d' "$W/u/kernel/env_migrate.c"
tooth "U. confirm-first: the not-confirmed gate deleted (a teardown reachable without the destination's word)" "U" "$W/u"

# ── V. The arm duplicated beyond the fully-confirmed send ─────────────────
seed "$W/v"
sed -i 's/^    env_migrate_arm_confirms(partition_id, dest_node_id);$/    env_migrate_arm_confirms(partition_id, dest_node_id);\n    env_migrate_arm_confirms(partition_id, dest_node_id);/' "$W/v/kernel/env_migrate.c"
tooth "V. the arm runs twice (armed beyond the end of a fully-confirmed send)" "V" "$W/v"

# ── W. The frames reclaimed without the records-remain gate ───────────────
seed "$W/w"
sed -i '/!env_migrate_partition_records_remain(c->partition_id)/d' "$W/w/kernel/env_migrate.c"
tooth "W. the deferred reclamation completed without the records-remain gate (frames under a record still standing)" "W" "$W/w"

# ── X. A dspp.c-linking host test left without the confirmation stand-in ──
# workload_ctx_host_test.c: the file that links net/dspp.c (and so now
# routes LIVE_REQ/LIVE_ACK) but links the real kernel/env_migrate.c
# nowhere. Removing its stand-in must be seen by clause X by name.
seed "$W/x"
sed -i '/env_migrate_live_query(/,+2d' "$W/x/tests/workload_ctx_host_test.c"
tooth "X. a dspp.c-linking host test with neither the real confirmation pair nor a stand-in (workload_ctx_host_test.c)" "X" "$W/x"

# ── Y. The host test stops exercising the confirmation ────────────────────
seed "$W/y"
sed -i 's/env_migrate_live_query(/env_migrate_live_query_MUTATED(/g' "$W/y/tests/env_migrate_host_test.c"
tooth "Y. the host test no longer calls env_migrate_live_query (the word's grounding unproven)" "Y" "$W/y"

# ── Z. The declaration path forgets the peer's announced intent ───────────
# The idle-observer defect verbatim: failover.c declaring the busy source
# DEAD because the only liveness it can see stopped with the blocked sweep,
# then adopting every partition in its checkpoint.
seed "$W/z"
sed -i '/env_migrate_peer_in_flight(peers\[i\].node_id)/d' "$W/z/kernel/failover.c"
tooth "Z. the declaration path no longer consults the silent peer's announced intent (the idle-observer false DEAD)" "Z" "$W/z"

# ── AA. The announce moved AFTER the block it must precede ────────────────
# Ordering is the invariant: a blocked node transmits nothing, so an announce
# after Step 1 announces nothing.
seed "$W/aa"
sed -i '/env_migrate_intent_begin();/d; /Step 1 (LPAR Phase 14)/a\    env_migrate_intent_begin();' "$W/aa/kernel/partition.c"
tooth "AA. partition_migrate announces intent AFTER its Step-1 block begins (an announce that never reaches the wire)" "AA" "$W/aa"

# ── AB. The dispatch stops routing the announce into the migrate RX ───────
seed "$W/ab"
sed -i 's/opcode <= DSPP_MIGRATE_ENV_INTENT/opcode <= DSPP_MIGRATE_ENV_LIVE_ACK/' "$W/ab/net/dspp.c"
tooth "AB. the dispatch no longer routes the intent opcode (the announce falls through unheard)" "AB" "$W/ab"

# ── AC. The silence arithmetic wraps on a stamp ahead of the sample ────────
# The live defect verbatim: last_seen_tick is stamped by the heartbeat RX
# path (timer ISR / AP core) while failover_tick()'s `now` is sampled by the
# BSP sweep, so the stamp can land ahead of the sample and the unsigned
# subtraction wraps to ~2^64 — past FAILOVER_DEAD_TICKS. A DEFERRED line
# merely printed a silly number; on an ungated peer it declares DEAD the very
# sweep that heard it.
seed "$W/ac"
sed -i '/uint64_t silence = (now > peers\[i\]\.last_seen_tick)/c\        uint64_t silence = now - peers[i].last_seen_tick;   /* MUTATED: wraps */' "$W/ac/kernel/failover.c"
sed -i '/? now - peers\[i\]\.last_seen_tick : 0;/d' "$W/ac/kernel/failover.c"
tooth "AC. the silence arithmetic wraps again on a heartbeat stamped ahead of the tick sample (a just-heard peer declared DEAD)" "AC" "$W/ac"

# ── AD. The console no longer follows the environment ──────────────────────
# POSIX-Environments v0.2 P3, §7's "the console and the terminal follow".
# The console is inherited, not reimplemented: the destination's restore
# calls env_service_create(), and inside that round trip init creates the
# POSIX sidecar, so cap.c registers its console from the sidecar's own name
# and env_service_create() binds the fresh env_id to it. Both halves can be
# cut independently, and each leaves an operator's attach answering "no such
# environment" for an environment that IS live on the destination.
#
# AD-a: the restore stops going through create — the sidecar is never
# created on the destination at all, so no console is ever registered.
seed "$W/ada"
sed -i 's/env_service_create(partition_id, index, \&status, \&new_id)/env_service_create_MUTATED(partition_id, index, \&status, \&new_id)/' "$W/ada/kernel/env_migrate.c"
tooth "AD-a. the destination's restore no longer goes through env_service_create (no sidecar, so no console, on the destination)" "AD" "$W/ada"

# AD-b: create stops binding the env_id — the environment is live on the
# destination but not addressable by the id the control plane speaks.
seed "$W/adb"
sed -i 's/if (env_console_bind_env(partition, index, \*out_env_id)) {/if (0) {   \/\* MUTATED: bind removed \*\//' "$W/adb/kernel/env_service.c"
tooth "AD-b. env_service_create no longer binds the new env_id to the console (live on the destination, unreachable by attach)" "AD" "$W/adb"

# ── AE. The pour stops waiting for the destination's identity ──────────────
# Without the wait the captured bytes land on a sidecar still inside its
# first boot, erasing the [env-id] announcement that identifies it — the
# operator's history arrives and the identity it is keyed by never does.
seed "$W/ae"
sed -i 's/env_console_identity_seen(rec->partition_id, rec->index)/1 \/\* MUTATED: identity wait removed \*\//' "$W/ae/kernel/env_payload.c"
tooth "AE. the pour no longer waits for the destination's own [env-id] announcement (the bytes erase the identity they are keyed by)" "AE" "$W/ae"

# ── G (increment 12). The conditional marking removed — the defect verbatim ─
# The live-found regression, reproduced: the quiesce marks
# PARTITION_PAUSED UNCONDITIONALLY again, so every staged record claims an
# operator's pause the migration itself took — and every destination
# restores into a partition nobody paused, leaving the tenant LIVE by the
# records and dead to the scheduler with its console reading input nobody
# would ever drain. (Found by increment 10's console arm on a real 2-node
# cluster; no host test saw it because none of them ran a destination.)
seed "$W/leakpause"
sed -i 's/if (operator_paused)/if (1)   \/\* MUTATED: the operator question ignored \*\//' "$W/leakpause/kernel/env_ckpt.c"
tooth "G (inc 12). the quiesce ignores operator_paused — the migration's own freeze crosses as an operator's pause (the tenant dead on arrival, live-found)" "G" "$W/leakpause"

# ── AF (increment 12). The operator's intent never reaches the step ────────
# The other direction: partition_migrate() still KNOWS (it captured
# was_paused_before_migrate before its own pause) but stops TELLING — a
# constant 0 at the call. An operator-paused partition then migrates and
# comes back RUNNING on the destination, the operator's pause silently
# dropped at the one boundary where it was supposed to travel.
seed "$W/nointent"
sed -i 's/env_migrate_partition(partition_id, dest_node_id, was_paused_before_migrate,/env_migrate_partition(partition_id, dest_node_id, 0,   \/\* MUTATED \*\//' "$W/nointent/kernel/partition.c"
tooth "AF (inc 12). was_paused_before_migrate never reaches the environment step (an operator's pause is dropped at the boundary)" "AF" "$W/nointent"

# ── AG. The announce is never released — the live-found 300 s blind spot ───
# Found on a 3-node cluster (2026-10-14): an env create finished in
# seconds, the node was killed right after, and both survivors deferred the
# declaration for the announce's full 300 s budget — the adoption guard's
# 240 s wait lost to a blind spot the create had already finished with.
# Four independent cuts, one per face of the release.

# AG-a: the peer's release branch removed — budget 0 falls into the clamp
# and becomes a 1-tick ANNOUNCE instead of ending one.
seed "$W/agrecv"
sed -i 's/if (budget_ticks == 0) {/if (0) {   \/\* MUTATED: the release is latched as a 1-tick announce \*\//' "$W/agrecv/kernel/env_migrate.c"
tooth "AG-a. recv_intent no longer treats budget 0 as the release (a release frame latches instead of ending the latch)" "AG" "$W/agrecv"

# AG-b: end() keeps the local stamp — this node stays undeclarable for the
# rest of the budget even though the peers were told.
seed "$W/agstamp"
sed -i '/^void env_migrate_intent_end/,/^}/s/env_migrate_intent_tick = 0;/env_migrate_intent_tick = kernel_tick_counter;   \/\* MUTATED \*\//' "$W/agstamp/kernel/env_migrate.c"
tooth "AG-b. env_migrate_intent_end() no longer clears the local gate (this node stays undeclarable DEAD after its phase resolved)" "AG" "$W/agstamp"

# AG-c: partition_migrate's releases deleted — the exact live-found shape
# on the migrate route: success and every refusal keep every peer blind.
seed "$W/agpart"
sed -i '/env_migrate_intent_end();/d' "$W/agpart/kernel/partition.c"
tooth "AG-c. partition_migrate() no longer releases the intent on any of its four exits (the 300 s blind spot after a migrate)" "AG" "$W/agpart"

# AG-d: the sweep's restore-loop release deleted — every restore period
# re-arms a full budget on the restoring node.
seed "$W/agsweep"
sed -i '/int rc = env_migrate_restore/,/if (rc == ENV_MIGRATE_OK)/{/env_migrate_intent_end();/d}' "$W/agsweep/kernel/env_migrate.c"
tooth "AG-d. the sweep's restore loop no longer releases the intent after each restore attempt" "AG" "$W/agsweep"

# ── AH. The blocked sweep keeps beating ────────────────────────────────────
# AH-a: the RPC wait's pump deleted — the exact live-found shape: the
# create round trip silences the leader's heartbeat stream and observers
# depose it mid-create (then, as a quiet follower, misdeclare it dead).
seed "$W/ahsvc"
sed -i 's/failover_sweep_keepalive();/\/* MUTATED *\//' "$W/ahsvc/kernel/env_service.c"
tooth "AH-a. the create/destroy RPC wait no longer pumps the keepalive (the create round trip silences the leader)" "AH" "$W/ahsvc"

# AH-b: the keepalive's body gutted — the calls remain, pumping nothing.
seed "$W/ahfo"
sed -i '/^void failover_sweep_keepalive/,/^}/{/check_consensus_heartbeat_tick(now);/d; /failover_live_checkpoint_broadcast(now);/d}' "$W/ahfo/kernel/failover.c"
tooth "AH-b. failover_sweep_keepalive() no longer pumps both streams (calls remain, they transmit nothing)" "AH" "$W/ahfo"

# AH-c: EVERY pump in env_migrate.c removed (0 < 4: the ACK waits and the
# pour loops all fell out of the keepalive's reach). All of them, not the
# first — the module grew two more pour-loop pumps with the image-window
# tail (2026-10-23), and a single-site mutation then left 5 >= 4 and the
# clause stayed green, proving nothing (the count is a FLOOR, so the tooth
# has to reach below it).
seed "$W/ahmig"
sed -i "s/failover_sweep_keepalive();/\\/\\* MUTATED \\*\\//g" "$W/ahmig/kernel/env_migrate.c"
tooth "AH-c. env_migrate.c no longer pumps the keepalive at any of its blocking sites" "AH" "$W/ahmig"

# ── AI. An intent frame refreshes liveness, never creates a watch entry ────
# AI-a: the peer-status gate deleted — every announcing peer gains a
# death-watch entry, and a quiet follower is declared DEAD 300 ticks after
# its own release (the second half of the 2026-10-23 live find).
seed "$W/aigate"
sed -i 's/if (failover_peer_status(node_id) >= 0)//' "$W/aigate/kernel/env_migrate.c"
tooth "AI-a. recv_intent stamps every announcing peer's heartbeat (creating watch entries for followers)" "AI" "$W/aigate"

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
