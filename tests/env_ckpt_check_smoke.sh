#!/usr/bin/env bash
# tests/env_ckpt_check_smoke.sh — the TEETH for tests/env_ckpt_check.sh.
#
# ─── What a teeth check is for, and why this one is source-only ────────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE wiring invariant of the P1a environment checkpoint in
# a throwaway copy of the tree and requires the guard to (a) exit 1 and (b)
# name THAT invariant. Requiring the right named clause is the half that
# matters: a guard that reddens on everything, or on the wrong clause, would
# pass a tooth that only checked the exit code.
#
# The last tooth is the vacuity control — the guard must be GREEN on the real,
# unmutated tree. Without it, a guard that always failed would pass every other
# tooth in this file.
#
# The hermetic seam is the guard's own optional root argument: the guard
# inspects a repository root, defaulting to its own parent. So this smoke
# builds a minimal root containing exactly the files the guard reads, mutates
# one line, and runs the real guard against it — no network, no build, no
# NVMe, no boot. It is in tests/run_source_smokes.sh's set by construction (it
# needs only bash/sed/cp/python3), so its teeth are proven on every push.
#
# Exit: 0 every tooth bit, 1 a tooth did not.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_ckpt_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads. Kept in one list so a tooth can never pass because
# the copy it mutated was missing something the guard needed.
SEED_FILES=(
    Makefile
    kernel/env_ckpt.h
    kernel/env_ckpt.c
    kernel/persist.c
    kernel/persist.h
    kernel/checkpoint_delta.h
    kernel/checkpoint_mgr.c
    kernel/env_service.c
    kernel/stream.h
    tests/partition_host_stubs.h
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    # Invariant M scans tests/*_host_test.c, so the copy needs them too.
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

echo "env_ckpt_check_smoke — teeth for the P1a wiring guard"
echo "====================================================="
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
seed "$W/a"; rm -f "$W/a/kernel/env_ckpt.c"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   A. a missing env_ckpt.c aborts (exit 2) instead of reporting the wiring sound"
    passed=$((passed + 1))
else
    echo "FAIL: A. a missing file did not abort (exit $rc)"; echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── B. Not built ──────────────────────────────────────────────────────────
seed "$W/b"
sed -i '/^[[:space:]]*kernel\/env_ckpt\.c /d' "$W/b/Makefile"
tooth "B. env_ckpt.c dropped from the Makefile" "B" "$W/b"

# ── C. The region bit and the mask ────────────────────────────────────────
seed "$W/c1"
sed -i '/^#define CKPT_REGION_ENV /d' "$W/c1/kernel/checkpoint_delta.h"
tooth "C. CKPT_REGION_ENV removed" "C" "$W/c1"

seed "$W/c2"
sed -i 's/^#define CKPT_NUM_REGIONS .*$/#define CKPT_NUM_REGIONS          17/' "$W/c2/kernel/checkpoint_delta.h"
tooth "C. the mask not grown past the new bit (18 -> 17)" "C" "$W/c2"

# ── D. LBA discipline ─────────────────────────────────────────────────────
seed "$W/d1"
sed -i 's/^#define PERSIST_ENV_CKPT_ENT_LBA .*$/#define PERSIST_ENV_CKPT_ENT_LBA        7698ULL/' "$W/d1/kernel/persist.h"
tooth "D. the entry array overlapping its own header" "D" "$W/d1"

seed "$W/d2"
sed -i 's/^#define PERSIST_ENV_CKPT_ENT_LBA .*$/#define PERSIST_ENV_CKPT_ENT_LBA        8190ULL/' "$W/d2/kernel/persist.h"
tooth "D. the entry frame pushed past STREAM_DIR_LBA" "D" "$W/d2"

seed "$W/d3"
sed -i 's/^#define PERSIST_ENV_CKPT_HDR_LBA .*$/#define PERSIST_ENV_CKPT_HDR_LBA        7600ULL/' "$W/d3/kernel/persist.h"
tooth "D. the region moved before the TLS anchor" "D" "$W/d3"

# ── E. A duplicated region magic ──────────────────────────────────────────
seed "$W/e"
printf '#define PERSIST_MAGIC_ALIASED      0xCAFE000000000012ULL\n' >> "$W/e/kernel/persist.h"
tooth "E. two regions sharing a magic" "E" "$W/e"

# ── F. The writer stops using the protocol ────────────────────────────────
seed "$W/f"
sed -i '/persist_write_array(env_ckpt_table, bytes, PERSIST_ENV_CKPT_ENT_LBA);/d' "$W/f/kernel/persist.c"
tooth "F. the writer no longer writes the array it checksums" "F" "$W/f"

# The commit clause is aimed at persist_environments()'s OWN commit by line
# range, not by text substitution: persist_region_commit() appears once per
# writer in this file, and rewriting the first one would mutate persist_catalog()
# and leave the clause under test green -- a tooth that bit the wrong thing.
seed "$W/f3"
python3 - "$W/f3/kernel/persist.c" <<'PY'
import sys
p = sys.argv[1]
src = open(p).read()
lines = src.split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("void persist_environments(void) {"))
end = next(i for i in range(start, len(lines)) if lines[i] == "}")
for i in range(start, end):
    if lines[i].strip() == "persist_region_commit();":
        lines[i] = "    /* commit removed by the tooth */"
        break
open(p, "w").write("\n".join(lines))
PY
tooth "F. the writer skipping its checksum commit" "F" "$W/f3"

# ── G. The deferred drain forgets the region ──────────────────────────────
seed "$W/g"
sed -i '/if (pend & PERSIST_PEND_ENV)/d' "$W/g/kernel/persist.c"
tooth "G. the deferred drain not flushing the region" "G" "$W/g"

# ── H. The checksum span no longer matches the array ──────────────────────
seed "$W/h1"
sed -i '/{ PERSIST_ENV_CKPT_HDR_LBA, PERSIST_MAGIC_ENV_CKPT,/d' "$W/h1/kernel/persist.c"
tooth "H. no p_region_specs entry for the region" "H" "$W/h1"

seed "$W/h2"
sed -i 's/1, { { PERSIST_ENV_CKPT_ENT_LBA, (uint32_t)sizeof(env_ckpt_table) } } },/1, { { PERSIST_ENV_CKPT_ENT_LBA, (uint32_t)sizeof(struct EnvCkptRecord) } } },/' "$W/h2/kernel/persist.c"
tooth "H. the spec span narrowed to one record" "H" "$W/h2"

# ── I. The restore path stops being all-or-nothing ────────────────────────
seed "$W/i1"
sed -i 's/persist_read_array(p_env_staging, (uint32_t)sizeof(p_env_staging),/persist_read_array(env_ckpt_table, (uint32_t)sizeof(env_ckpt_table),/' "$W/i1/kernel/persist.c"
tooth "I. the snapshot read straight into the live table" "I" "$W/i1"

seed "$W/i2"
sed -i 's/env_ckpt_adopt(p_env_staging,/env_ckpt_adopt(env_ckpt_table,/' "$W/i2/kernel/persist.c"
tooth "I. the restore block no longer validating a staging copy" "I" "$W/i2"

# ── J. The checkpoint walk skips the region ───────────────────────────────
seed "$W/j"
python3 - "$W/j/kernel/checkpoint_mgr.c" <<'PY'
import sys
p = sys.argv[1]
src = open(p).read()
src = src.replace("if (dirty & (1u << CKPT_REGION_ENV))         persist_environments();",
                  "if (dirty & (1u << CKPT_REGION_VIEWS))       persist_environments();")
open(p, "w").write(src)
PY
tooth "J. checkpoint_trigger() not writing the region" "J" "$W/j"

# ── K. The frame-fit invariant is no longer a build error ─────────────────
seed "$W/k"
sed -i '/_Static_assert(sizeof(struct EnvCkptRecord) \* ENV_CKPT_MAX <= 4096,/d' "$W/k/kernel/env_ckpt.c"
tooth "K. the single-frame _Static_assert removed" "K" "$W/k"

# ── L. adopt() stops clearing before it validates ─────────────────────────
seed "$W/l"
python3 - "$W/l/kernel/env_ckpt.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
guard = next(i for i, l in enumerate(lines) if "env_ckpt_valid(&staging[i])" in l)
keep = [l for i, l in enumerate(lines)
        if not (i < guard and "ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));" in l)]
open(p, "w").write("\n".join(keep))
PY
tooth "L. adopt() validating before it clears the table" "L" "$W/l"

# ── M. A host test left without the new translation unit ──────────────────
seed "$W/m"
sed -i 's/kernel\/persist\.c kernel\/env_ckpt\.c/kernel\/persist.c/' "$W/m/tests/sql_exec_host_test.c"
tooth "M. a persist.c-linking test not linking env_ckpt.c" "M" "$W/m"

# ── N. The quiesce API loses an end ───────────────────────────────────────
seed "$W/n1"
sed -i 's/^void env_ckpt_quiesce_for_capture(/void env_ckpt_quiesce_for_capture_DISABLED(/' "$W/n1/kernel/env_ckpt.c"
tooth "N. the quiesce entry point defined nowhere" "N" "$W/n1"

seed "$W/n2"
sed -i '/env_ckpt_apply_restored_pauses(void);/d' "$W/n2/kernel/env_ckpt.h"
tooth "N. the restore-pause entry point undeclared" "N" "$W/n2"

# ── O. The capture stops quiescing, or stops releasing ────────────────────
seed "$W/o1"
sed -i '/env_ckpt_release_capture(&q);/d' "$W/o1/kernel/persist.c"
tooth "O. the capture never releasing what it froze" "O" "$W/o1"

# The leak-unpause tooth: a bail-out added between the quiesce and its release.
# This is the shape v0.2 §4 calls out -- a failed capture that leaves a tenant
# frozen with nothing on disk to show for it.
seed "$W/o2"
python3 - "$W/o2/kernel/persist.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
i = next(i for i, l in enumerate(lines) if "env_ckpt_quiesce_for_capture(&q);" in l)
lines[i + 1:i + 1] = ["    if (q.n_dropped) {", "        return;", "    }"]
open(p, "w").write("\n".join(lines))
PY
tooth "O. an early return between the quiesce and its release (the leak-unpause tooth)" "O" "$W/o2"

seed "$W/o3"
python3 - "$W/o3/kernel/persist.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
q = next(i for i, l in enumerate(lines) if "env_ckpt_quiesce_for_capture(&q);" in l)
call = lines.pop(q)
c = next(i for i, l in enumerate(lines) if l.strip() == "persist_region_commit();")
lines.insert(c, call)
open(p, "w").write("\n".join(lines))
PY
tooth "O. the capture quiescing after it has already written" "O" "$W/o3"

# ── P. The flag bits stop being two distinct facts ────────────────────────
seed "$W/p1"
sed -i 's/^#define ENV_CKPT_FLAG_PARTITION_PAUSED 0x0002u/#define ENV_CKPT_FLAG_PARTITION_PAUSED 0x0001u/' "$W/p1/kernel/env_ckpt.h"
tooth "P. the two flag bits collapsed onto one" "P" "$W/p1"

seed "$W/p2"
python3 - "$W/p2/kernel/env_ckpt.c" <<'PY'
import sys
p = sys.argv[1]
keep = [l for l in open(p).read().split("\n") if "rec->flags & ~(ENV_CKPT_FLAG_QUIESCED" not in l]
open(p, "w").write("\n".join(keep))
PY
tooth "P. the unknown-flag refusal removed from the gate" "P" "$W/p2"

seed "$W/p3"
sed -i '/case ENV_CKPT_REFUSE_BAD_FLAGS:/d' "$W/p3/kernel/env_ckpt.c"
tooth "P. the unknown-flag refusal with no rendering" "P" "$W/p3"

# ── Q. The restore forgets the pauses it recorded ─────────────────────────
seed "$W/q1"
sed -i 's/uint32_t repaused = env_ckpt_apply_restored_pauses();/uint32_t repaused = 0;/' "$W/q1/kernel/persist.c"
tooth "Q. the restore never re-applying the recorded pauses" "Q" "$W/q1"

seed "$W/q2"
python3 - "$W/q2/kernel/persist.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
a = next(i for i, l in enumerate(lines) if "env_ckpt_after_restore();" in l)
b = next(i for i, l in enumerate(lines) if "env_ckpt_apply_restored_pauses();" in l and "uint32_t" in l)
lines[a], lines[b] = lines[b], lines[a]
open(p, "w").write("\n".join(lines))
PY
tooth "Q. the pauses re-applied before the live set is settled" "Q" "$W/q2"

# ── R. A destroy stops dropping the record ────────────────────────────────
seed "$W/r"
sed -i 's/if (env_ckpt_drop_env(partition, env_id) == 0) {/if (0) {/' "$W/r/kernel/env_service.c"
tooth "R. a destroyed environment's record outliving it" "R" "$W/r"

# ── S. A host test left without the quiesce stubs ─────────────────────────
seed "$W/s"
sed -i '/tests\/partition_host_stubs\.h/d' "$W/s/tests/sql_exec_host_test.c"
tooth "S. an env_ckpt.c-linking test without partition.c and without the stubs" "S" "$W/s"

# ── The vacuity control ───────────────────────────────────────────────────
# The guard must be GREEN on the untouched tree. Without this arm, a guard that
# failed unconditionally would pass every tooth above.
out="$(bash "$GUARD" "$ROOT" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control. the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: control. the untouched tree does NOT pass — the guard is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
