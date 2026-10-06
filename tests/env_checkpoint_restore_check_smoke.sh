#!/usr/bin/env bash
# tests/env_checkpoint_restore_check_smoke.sh — the TEETH for
# tests/env_checkpoint_restore_check.sh, and the teeth v0.2 §4 names plus the
# payload group the sixth increment added:
#
#   P1A_TOOTH=no-restore        — the replay runs, the payload does not (the
#                                 create makes an environment; the CONTENTS
#                                 are withheld)
#   P1A_TOOTH=stale-descriptor  — a record that no longer describes the
#                                 environment the create made is accepted
#   P1A_TOOTH=leak-unpause      — a pause a restore stepped out of is not put
#                                 back (or the snapshot's pause is, too early)
#   P1A_TOOTH=dirty-capture     — a record captured without a quiesce is
#                                 replayed anyway
#   P1A_TOOTH=payload           — the payload's own wiring: captured outside
#                                 the frozen window, poured before the repause
#                                 or with a yield in it, the metadata-only knob
#                                 or the payloads report missing, a host test
#                                 left unresolvable, an entry point undriven
#
# ─── What a teeth smoke is for, and why this one is source-level ──────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE property of the restore path in a throwaway copy of
# the tree and requires the guard to (a) exit 1 and (b) name THAT clause.
# Requiring the right named clause is the half that matters: a guard that
# reddened on everything, or on the wrong clause, would pass a tooth that only
# checked the exit code.
#
# The teeth are source mutations rather than boot-time ones so that every push
# proves them with no QEMU and no build: the replay and its four refusals, the
# payload's capture/pour wiring, and — since the fifth increment — the BOOT
# ARM's artifact half as well: the guard's --replay validator is run here over
# a synthesized well-formed recorded run with exactly one mutation at a time,
# so B1-B11 are each proven to bite without a boot. That fixture is where
# P1A_TOOTH=no-restore is proven too: the recorded run is a REPLAYED
# environment whose payload was withheld (restore.json payloads=0,
# payload_skips=1, and a contents_post that read nothing back), where B8 must
# stay GREEN and B10/B11 must be the only clauses red — the tooth's split,
# on every push, with no QEMU and no build. What is still owed is the
# boot-side mutations of the other tooth names; the names stay.
#
# The guard has TWO halves and this smoke has a section for each:
#
#   * the SOURCE clauses (T1-T7) — proven by mutating a throwaway copy of the
#     tree that contains exactly the files the guard reads;
#   * the BOOT arm's validation (`--replay`) — proven against a synthesized
#     artifact set, the same one a live run records (identity.json, the two
#     boot logs, the checkpoint/pause/restore answers, the three HTTP
#     snapshots and the two CONTENTS transcripts). That is where every B
#     clause, the CONTENTS ones included, is proven to bite.
#
# The hermetic seam for the source half is the guard's own optional root
# argument: it inspects a repository root, defaulting to its own parent. So
# this smoke builds a minimal root containing exactly the files the guard
# reads, applies one mutation, and runs the real guard against it — no network,
# no build, no NVMe, no boot.
#
# The last arm is the vacuity control: the guard must be GREEN on the real,
# unmutated tree. Without it, a guard that failed unconditionally would pass
# every tooth in this file.
#
# Usage:  bash tests/env_checkpoint_restore_check_smoke.sh [P1A_TOOTH=<name>]
#         P1A_TOOTH=<name> bash tests/env_checkpoint_restore_check_smoke.sh
#         (default: all four groups, in the order above)
#
# Exit: 0 every tooth bit, 1 a tooth did not, 2 misuse/precondition missing.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_checkpoint_restore_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

TOOTH_SET="${1:-${P1A_TOOTH:-}}"
case "$TOOTH_SET" in
    ""|no-restore|stale-descriptor|leak-unpause|dirty-capture|payload) ;;
    *) echo "ABORT: unknown P1A_TOOTH='$TOOTH_SET' — expected no-restore, stale-descriptor, leak-unpause, dirty-capture or payload" >&2; exit 2 ;;
esac
want_tooth() { [ -z "$TOOTH_SET" ] || [ "$TOOTH_SET" = "$1" ]; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads. Kept in one list so a tooth can never pass because
# the copy it mutated was missing something the guard needed.
SEED_FILES=(
    kernel/env_ckpt.h
    kernel/env_ckpt.c
    kernel/env_service.h
    kernel/env_service.c
    kernel/persist.c
    kernel/env_payload.h
    kernel/env_payload.c
    kernel/env_console.h
    kernel/env_console.c
    net/http.c
    tests/partition_host_stubs.h
    tests/payload_host_stubs.h
    Makefile
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/net" "$1/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    # The guard's T5/T6 scan tests/*_host_test.c, so the copy needs them too.
    cp "$ROOT"/tests/*_host_test.c "$1/tests/" || return 1
    return 0
}

passed=0
failed=0
skipped=0
tooth() {   # tooth <P1A_TOOTH-name> <expected-clause> <label> <dir>
    local group="$1" clause="$2" label="$3" dir="$4" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   P1A_TOOTH=$group: $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: P1A_TOOTH=$group: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "env_checkpoint_restore_check_smoke — teeth for the P1a restore guard"
echo "====================================================================="
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
# (Always runs: every tooth group depends on the guard being evaluable at all.)
seed "$W/a"; rm -f "$W/a/kernel/env_service.c"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   a missing env_service.c aborts (exit 2) instead of reporting the restore sound"
    passed=$((passed + 1))
else
    echo "FAIL: a missing file did not abort (exit $rc)"; echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── P1A_TOOTH=no-restore ──────────────────────────────────────────────────
# The tooth v0.2 §4 describes as the vacuity control: with the replay skipped
# the environment does NOT come back, which must read as "not replayed" rather
# than as a clean pass. Each mutation below is one way to skip it.
if want_tooth no-restore; then
    seed "$W/n1"
    sed -i '/"\/api\/env\/restore"/d' "$W/n1/net/http.c"
    tooth no-restore T1 "the route that calls the replay is no longer registered" "$W/n1"

    seed "$W/n2"
    python3 - "$W/n2/kernel/env_service.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
i = next(i for i, l in enumerate(lines) if "env_service_create(p, idx, &status, &new_id);" in l)
c = next(i for i, l in enumerate(lines) if "out->n_replayed++;" in l)
# The counter moves above the create: the pass now claims replays it has not
# made yet -- "an environment that comes back empty must not be described as
# restored", at the source level.
line = lines.pop(c)
lines.insert(i, line)
open(p, "w").write("\n".join(lines))
PY
    tooth no-restore T1 "the pass counts a replay before the create it claims" "$W/n2"

    seed "$W/n3"
    sed -i '/env_ckpt_restore_begin();/d' "$W/n3/kernel/env_ckpt.c"
    tooth no-restore T1 "after_restore() no longer arms the pending set (nothing is ever replayed)" "$W/n3"
else
    skipped=$((skipped + 1))
fi

# ── P1A_TOOTH=stale-descriptor ────────────────────────────────────────────
# A descriptor that no longer describes what the create produced must be
# refused AND leave nothing behind. Three ways to get that wrong: don't
# compare, compare but don't undo, or compare the wrong fields.
if want_tooth stale-descriptor; then
    seed "$W/s1"
    sed -i 's/if (!env_ckpt_restore_same_identity(&w, env_ckpt_find(p, idx))) {/if (0) {/' "$W/s1/kernel/env_service.c"
    tooth stale-descriptor T2 "the identity comparison is short-circuited (a stale descriptor is accepted)" "$W/s1"

    seed "$W/s2"
    sed -i '/(void)env_service_destroy(new_id, p, &dst, &d_id, &d_part);/d' "$W/s2/kernel/env_service.c"
    tooth stale-descriptor T2 "the mismatch is refused but the environment it made is left running" "$W/s2"

    seed "$W/s3"
    python3 - "$W/s3/kernel/env_ckpt.c" <<'PY'
import sys
p = sys.argv[1]
keep = [l for l in open(p).read().split("\n")
        if "want->tasks[i].name[c] != got->tasks[i].name[c]" not in l]
open(p, "w").write("\n".join(keep))
PY
    tooth stale-descriptor T2 "identity stops comparing the sidecar names (a different environment passes)" "$W/s3"
else
    skipped=$((skipped + 1))
fi

# ── P1A_TOOTH=leak-unpause ────────────────────────────────────────────────
# The restore's pause contract, which is the capture's mirrored: step out of a
# restored pause for the create, put it back from the pass's own ledger, and
# have no exit between the two. Break any of those and a partition an operator
# had frozen can come out of a failed restore RUNNING.
if want_tooth leak-unpause; then
    seed "$W/l1"
    python3 - "$W/l1/kernel/env_service.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
i = next(i for i, l in enumerate(lines) if "int can = env_ckpt_restore_resume_for_create(p);" in l)
# A bail-out added between the resume and its repause -- the exact shape the
# capture's clause O refuses.
lines[i + 2:i + 2] = ["        if (can) {", "            return 0;", "        }"]
open(p, "w").write("\n".join(lines))
PY
    tooth leak-unpause T3 "an early return between the resume and the repause" "$W/l1"

    seed "$W/l2"
    python3 - "$W/l2/kernel/env_service.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
c = next(i for i, l in enumerate(lines) if "out->n_repaused  = env_ckpt_restore_repause();" in l)
call = lines.pop(c)
w = next(i for i, l in enumerate(lines) if "while (env_ckpt_restore_pending() > 0) {" in l)
lines.insert(w, call)
open(p, "w").write("\n".join(lines))
PY
    tooth leak-unpause T3 "the pause is put back before the create, not after the loop" "$W/l2"

    seed "$W/l3"
    sed -i 's/uint32_t p = ec_restore_resumed\[i\];/uint32_t p = env_ckpt_table[i].partition_id;/' "$W/l3/kernel/env_ckpt.c"
    tooth leak-unpause T3 "the repause walks the records instead of the ledger it resumed" "$W/l3"

    seed "$W/l4"
    python3 - "$W/l4/kernel/persist.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
a = next(i for i, l in enumerate(lines) if "env_ckpt_after_restore();" in l)
b = next(i for i, l in enumerate(lines) if "env_ckpt_apply_restored_pauses();" in l and "uint32_t" in l)
lines[a], lines[b] = lines[b], lines[a]
open(p, "w").write("\n".join(lines))
PY
    tooth leak-unpause T3 "the snapshot's pauses are re-applied before the live set is settled" "$W/l4"
else
    skipped=$((skipped + 1))
fi

# ── P1A_TOOTH=dirty-capture ───────────────────────────────────────────────
# The pause is the load-bearing step a reader is most likely to treat as
# optional: a record captured while its partition ran describes a moving
# target, and the replay must refuse it -- with a reason, a counter, and its
# record cleared from the pending set.
if want_tooth dirty-capture; then
    seed "$W/d1"
    python3 - "$W/d1/kernel/env_ckpt.c" <<'PY'
import sys
p = sys.argv[1]
src = open(p).read()
src = src.replace("if (!(rec->flags & ENV_CKPT_FLAG_QUIESCED) &&\n        rec->partition_id != PARTITION_SYSTEM)\n        return ENV_CKPT_REFUSE_UNQUIESCED;",
                  "if (0)\n        return ENV_CKPT_REFUSE_UNQUIESCED;")
open(p, "w").write(src)
PY
    tooth dirty-capture T4 "the quiesced-flag gate is short-circuited (an un-frozen capture is replayed)" "$W/d1"

    seed "$W/d2"
    sed -i 's/int why = env_ckpt_restore_admissible(rec);/int why = ENV_CKPT_REFUSE_NONE;/' "$W/d2/kernel/env_ckpt.c"
    tooth dirty-capture T4 "the hand-out stops applying the gate (a caller could forget it)" "$W/d2"

    seed "$W/d3"
    sed -i '/case ENV_CKPT_REFUSE_UNQUIESCED:/d' "$W/d3/kernel/env_ckpt.c"
    tooth dirty-capture T4 "the refusal has a code but no rendering" "$W/d3"

    seed "$W/d4"
    sed -i '/ec_restore_refused_count++;/d' "$W/d4/kernel/env_ckpt.c"
    tooth dirty-capture T4 "a refused replay is no longer counted" "$W/d4"
else
    skipped=$((skipped + 1))
fi

# ── P1A_TOOTH=payload ─────────────────────────────────────────────────────
# The sixth increment's wiring, one broken property each: the capture outside
# the frozen window, the pour out of order (or with a yield or a bail-out
# between the repause and the first memcpy), the metadata-only knob gone from
# the route, the report the validator's B7 reads gone from the response, a host
# test left unable to resolve the payload symbols, the placement refusal
# deleted, the console inject cut, and an entry point nothing drives.
if want_tooth payload; then
    seed "$W/p1"
    sed -i '/(void)env_payload_capture_all();/d' "$W/p1/kernel/persist.c"
    tooth payload T7 "the payload capture is no longer called inside the quiesce interval" "$W/p1"

    seed "$W/p2"
    python3 - "$W/p2/kernel/env_service.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
i = next(i for i, l in enumerate(lines) if "out->n_repaused  = env_ckpt_restore_repause();" in l)
# A yield planted between the repause and the pour — Ring-3 would resume over
# memory the pour is about to overwrite.
lines[i + 1:i + 1] = ["    kernel_yield_to_ring3(1);"]
open(p, "w").write("\n".join(lines))
PY
    tooth payload T7 "a yield between the repause and the pour (the pour is no longer straight-line)" "$W/p2"

    seed "$W/p3"
    python3 - "$W/p3/kernel/env_service.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
i = next(i for i, l in enumerate(lines) if "out->n_repaused  = env_ckpt_restore_repause();" in l)
# A bail-out after the pause is back but before anything is poured: every
# replayed environment would come back empty with a clean report.
lines[i + 1:i + 1] = ["    return 0;"]
open(p, "w").write("\n".join(lines))
PY
    tooth payload T7 "a return between the repause and the pour (replays that are never filled)" "$W/p3"

    seed "$W/p4"
    sed -i '/env_service_set_restore_payload(with_payload);/d' "$W/p4/net/http.c"
    tooth payload T7 "the route lost the metadata-only knob (the tooth could not withhold the payload)" "$W/p4"

    seed "$W/p5"
    sed -i '/jb_uint(&j,"payload_skips", rep.n_payload_skips);/d' "$W/p5/net/http.c"
    tooth payload T7 "the response no longer reports payload_skips (a skip is indistinguishable from a pour)" "$W/p5"

    seed "$W/p6"
    sed -i '/payload_host_stubs.h/d' "$W/p6/tests/mvcc_host_test.c"
    tooth payload T7 "a host test links persist.c with neither env_payload.c nor the stub header (unresolvable symbols)" "$W/p6"

    seed "$W/p7"
    sed -i 's/EP_REFUSE_PLACEMENT/EP_REFUSE_NONE/g' "$W/p7/kernel/env_payload.c"
    tooth payload T7 "the pour no longer refuses a moved placement (bytes would move before the rule)" "$W/p7"

    seed "$W/p8"
    sed -i 's/env_console_inject(/env_console_inject_DISABLED(/g' "$W/p8/kernel/env_payload.c"
    tooth payload T7 "the restore never injects the buffered console bytes under the new env_id" "$W/p8"

    seed "$W/p9"
    rm -f "$W/p9/tests/env_payload_host_test.c"
    tooth payload T7 "the payload entry points are exercised by no host test (a claim, not a feature)" "$W/p9"
else
    skipped=$((skipped + 1))
fi

# ═══ The boot arm's teeth: the artifact validation half, no QEMU ══════════
# The set below is what a live run records (the guard's own file names and
# shapes); a mutation here is the same class of mistake as a wrong boot, and
# the guard must name the clause that catches it.
write_arts() {   # write_arts <dir> — a well-formed recorded run
    local d="$1"
    rm -rf "$d"; mkdir -p "$d"
    cat > "$d/identity.json" <<'JSON'
{"partition": 1, "index": 2, "env_id": 4242, "partition_name": "p1arestore"}
JSON
    cat > "$d/create.json" <<'JSON'
{"ok":"true","env_id":4242,"partition":1}
JSON
    cat > "$d/pause.json" <<'JSON'
{"ok":"true"}
JSON
    cat > "$d/checkpoint.json" <<'JSON'
{"status":0,"seq":3,"count":1}
JSON
    cat > "$d/boot1.log" <<'LOG'
[BOOT] command line: unified=1 node=0
[ENV_CKPT] no snapshot — cold start.
[ENV] checkpoint record registered: env 4242 (partition 1, index 2) regions=3 chans=4 tasks=2 console=2
[PERSIST] Environment checkpoint snapshot written (1 live record(s), 1 frozen, 0 dropped).
LOG
    cat > "$d/boot2.log" <<'LOG'
[BOOT] command line: unified=1 node=0
[ENV_CKPT] Restored 1 environment checkpoint record(s) (1 partition(s) re-paused).
[ENV_RESTORE] record (partition 1, index 2) replayed through create: env 4242 -> 5001, identity intact
[ENV_RESTORE] 1 pending, 1 replayed, 0 refused, 0 still pending (1 partition(s) resumed for a create, 1 re-paused).
LOG
    cat > "$d/restore.json" <<'JSON'
{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":1,"repaused":1,"payloads":1,"payload_skips":0}
JSON
    cat > "$d/partitions_pre.json" <<'JSON'
{"ok":"true","partitions":[{"id":1,"name":"p1arestore","paused":true}]}
JSON
    cat > "$d/partitions_post.json" <<'JSON'
{"ok":"true","partitions":[{"id":1,"name":"p1arestore","paused":true}]}
JSON
    # The shape GET /api/partition/1/env actually answers: the partition at the
    # TOP level (the route is partition-scoped) and (index, env_id) per entry --
    # NOT a per-entry "partition", which the route has never emitted. A fixture
    # that invents a field is a fixture that would have kept the guard green
    # while the live run was red.
    cat > "$d/envlist.json" <<'JSON'
{"ok":"true","partition":1,"envs":[{"env_id":5001,"index":2,"posix_pid":77,"dropped":0}],"live":1}
JSON
    cat > "$d/processes.json" <<'JSON'
{"ok":"true","processes":[{"pid":77,"name":"aerosls.posix.2","partition_id":1}]}
JSON
    # The CONTENTS evidence, exactly what the live arm records: both claims
    # read back through the environment's own console, before and after the
    # reboot. A well-formed run has them EQUAL and non-empty — the pair an
    # empty replay can never produce.
    cat > "$d/contents_pre.json" <<'JSON'
{"file_bytes":"P1A-FILE-7f3a9c01","ok":true,"var_value":"P1A-VAR-b19e0203"}
JSON
    cat > "$d/contents_post.json" <<'JSON'
{"file_bytes":"P1A-FILE-7f3a9c01","ok":true,"var_value":"P1A-VAR-b19e0203"}
JSON
}

boot_tooth() {   # boot_tooth <label> <expected-clause> <dir>
    local label="$1" clause="$2" dir="$3" out rc
    out="$(bash "$GUARD" --replay "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo
# The vacuity control for the validator: a well-formed recorded run passes.
write_arts "$W/art-good"
if out="$(bash "$GUARD" --replay "$W/art-good" 2>&1)"; rc=$?; [ "$rc" -eq 0 ]; then
    echo "ok:   the validator passes a well-formed recorded run ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: the validator REJECTED a well-formed recorded run — it is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── P1A_TOOTH=no-restore: the requested mutant ────────────────────────────
# What the live tooth leaves behind: the WHOLE replay — create, identity,
# liveness, pause — with only the payload withheld (restore.json payloads=0,
# payload_skips=1) and a contents read that saw nothing come back. The split
# that must hold: B10 and B11 (the CONTENTS clauses) red, B8 (liveness) GREEN,
# and nothing else — an attributable tooth, or it proves nothing.
cp -a "$W/art-good" "$W/art-tooth"
printf '{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":1,"repaused":1,"payloads":0,"payload_skips":1}\n' > "$W/art-tooth/restore.json"
# The post-replay console read of a payload-withheld environment: the file and
# the variable are simply not there (empty fields — the live arm's transcript
# would be empty too, and contents_write records exactly this).
printf '{"file_bytes":"","ok":true,"var_value":""}\n' > "$W/art-tooth/contents_post.json"
printf 'no-restore\n' > "$W/art-tooth/tooth.txt"
out="$(bash "$GUARD" --replay "$W/art-tooth" 2>&1)"; rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: B10\." && \
   printf '%s\n' "$out" | grep -q "^FAIL: B11\." && \
   ! printf '%s\n' "$out" | grep -q "^FAIL: B8\." && \
   [ "$(printf '%s\n' "$out" | grep -c '^FAIL: ')" -eq 2 ]; then
    echo "ok:   P1A_TOOTH=no-restore: withholding the payload takes B10 and B11 (the CONTENTS clauses) red while B8 stays green, and nothing else"
    passed=$((passed + 1))
else
    echo "FAIL: P1A_TOOTH=no-restore did NOT bite B10+B11 alone (B8 must stay green) — exit $rc"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── The other boot clauses, one mutation each ─────────────────────────────
cp -a "$W/art-good" "$W/art-b1"; sed -i '/no snapshot/d' "$W/art-b1/boot1.log"
boot_tooth "B1: a first boot that was not cold for the environment region" B1 "$W/art-b1"

cp -a "$W/art-good" "$W/art-b1n"; printf '[NVME] MMIO above 4 GiB (0x100000000) — stream cold start.\n' >> "$W/art-b1n/boot1.log"
boot_tooth "B1: an NVMe stack that never came up (the round trip was a no-op)" B1 "$W/art-b1n"

cp -a "$W/art-good" "$W/art-b3"; printf '{"status":3,"seq":3,"count":1}\n' > "$W/art-b3/checkpoint.json"
boot_tooth "B3: a checkpoint that failed" B3 "$W/art-b3"

cp -a "$W/art-good" "$W/art-b4"; printf '{"ok":"true","partitions":[{"id":1,"name":"p1arestore","paused":false}]}\n' > "$W/art-b4/partitions_pre.json"
boot_tooth "B4: the snapshot's pause did not come back" B4 "$W/art-b4"

cp -a "$W/art-good" "$W/art-b5"; sed -i 's/(1 partition(s) re-paused)/(0 partition(s) re-paused)/' "$W/art-b5/boot2.log"
boot_tooth "B5: the reboot restored records but re-applied no pause" B5 "$W/art-b5"

cp -a "$W/art-good" "$W/art-b6"; sed -i '/replayed through create/d' "$W/art-b6/boot2.log"
boot_tooth "B6: the replay left no identity-intact line (it never re-registered)" B6 "$W/art-b6"

cp -a "$W/art-good" "$W/art-b7"; printf '{"ok":"true","pending":1,"replayed":0,"refused":0,"remaining":1,"resumed":1,"repaused":1,"payloads":1,"payload_skips":0}\n' > "$W/art-b7/restore.json"
boot_tooth "B7: a pass that replayed nothing and says so" B7 "$W/art-b7"

cp -a "$W/art-good" "$W/art-b8"; printf '{"ok":"true","partition":1,"envs":[]}\n' > "$W/art-b8/envlist.json"
boot_tooth "B8: no environment is live in the partition after the reboot" B8 "$W/art-b8"

cp -a "$W/art-good" "$W/art-b8b"; printf '{"ok":"true","processes":[{"pid":77,"name":"aerosls.posix.2","partition_id":9}]}\n' > "$W/art-b8b/processes.json"
boot_tooth "B8: the POSIX sidecar came back in a different partition" B8 "$W/art-b8b"

# The env listing is partition-SCOPED, so the partition the clause compares is
# the response's top-level one. This arm moves ONLY that field: a guard reading
# a per-entry partition (as this one did, against an invented field, until the
# live run caught it) stays green through it and would call an environment
# restored in a partition it is not in.
cp -a "$W/art-good" "$W/art-b8c"; printf '{"ok":"true","partition":9,"envs":[{"env_id":5001,"index":2,"posix_pid":77}]}\n' > "$W/art-b8c/envlist.json"
boot_tooth "B8: the returned environment is listed under a different partition" B8 "$W/art-b8c"

cp -a "$W/art-good" "$W/art-b9"; printf '{"ok":"true","partitions":[{"id":1,"name":"p1arestore","paused":false}]}\n' > "$W/art-b9/partitions_post.json"
boot_tooth "B9: the pass resumed for the create and never put the pause back" B9 "$W/art-b9"

cp -a "$W/art-good" "$W/art-b9b"; printf '{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0,"resumed":0,"repaused":0,"payloads":1,"payload_skips":0}\n' > "$W/art-b9b/restore.json"
boot_tooth "B9: a pass that never stepped out of the recorded pause" B9 "$W/art-b9b"

# ── The CONTENTS clauses, one mutation each ───────────────────────────────
# B10/B11 read the pair of transcripts the live arm records through the
# environment's own console. Each mutation below is one way the claim can be
# false, and each must redden exactly its own clause.
cp -a "$W/art-good" "$W/art-b10"; printf '{"file_bytes":"P1A-FILE-d1fferent","ok":true,"var_value":"P1A-VAR-b19e0203"}\n' > "$W/art-b10/contents_post.json"
boot_tooth "B10: the file came back with different bytes" B10 "$W/art-b10"

cp -a "$W/art-good" "$W/art-b10b"; printf '{"file_bytes":"","ok":true,"var_value":"P1A-VAR-b19e0203"}\n' > "$W/art-b10b/contents_post.json"
boot_tooth "B10: the file came back with nothing at all" B10 "$W/art-b10b"

cp -a "$W/art-good" "$W/art-b10c"; printf '{"file_bytes":"","ok":true,"var_value":"P1A-VAR-b19e0203"}\n' > "$W/art-b10c/contents_pre.json"
boot_tooth "B10: a pre-checkpoint read that recorded no bytes (an empty pair must not pass as equal)" B10 "$W/art-b10c"

cp -a "$W/art-good" "$W/art-b11"; printf '{"file_bytes":"P1A-FILE-7f3a9c01","ok":true,"var_value":"P1A-VAR-0dd0000d"}\n' > "$W/art-b11/contents_post.json"
boot_tooth "B11: the shell variable came back as a different value" B11 "$W/art-b11"

cp -a "$W/art-good" "$W/art-b11b"; printf '{"file_bytes":"P1A-FILE-7f3a9c01","ok":true,"var_value":""}\n' > "$W/art-b11b/contents_post.json"
boot_tooth "B11: the shell variable is not visible after the replay" B11 "$W/art-b11b"

cp -a "$W/art-good" "$W/art-b11c"; rm -f "$W/art-b11c/contents_post.json"
boot_tooth "B11: the run never recorded the contents evidence" B11 "$W/art-b11c"

# ── The vacuity control ───────────────────────────────────────────────────
# The guard must be GREEN on the untouched tree. Without this arm, a guard that
# failed unconditionally would pass every tooth above.
out="$(bash "$GUARD" "$ROOT" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control: the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: control: the untouched tree does NOT pass — the guard is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
[ "$skipped" -gt 0 ] && echo "($skipped tooth group(s) not selected by P1A_TOOTH=$TOOTH_SET)"
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
