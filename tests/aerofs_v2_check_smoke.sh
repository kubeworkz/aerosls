#!/usr/bin/env bash
# tests/aerofs_v2_check_smoke.sh — the TEETH for tests/aerofs_v2_check.sh.
#
# ─── What a teeth check is for, and why this one is source-only ────────────
# A guard that has never been seen to fail is a guard nobody can trust. The
# guard's subject is the aerofs-lite v2 format's RULE — which version is read,
# which is written, what a third gets, and where the layout moved — so every
# tooth below breaks ONE of those in a throwaway copy of the tree and requires
# the guard to (a) exit 1 and (b) name THAT clause. Requiring the right named
# clause is the half that matters: a guard that reddens on everything, or on
# the wrong clause, would pass a tooth that only looked at the exit code.
#
# The teeth are exactly the edits a future change might make believing it is
# harmless:
#
#   * A — the writer's version falls back to v1 (every store silently
#     read-only), or the unknown-version refusal is dropped;
#   * B — the second indirect pointer is removed (the ceiling collapses back
#     under 71,168 B), or the two pointers' offsets move;
#   * C — the image builder starts writing v2 (every existing image, the
#     system rootfs included, becomes a writable store);
#   * D — the read-only gate stops consulting the version, a read starts
#     parsing the wrong inode layout, or one mutating entry point quietly
#     loses its own v1 guard;
#   * E — format-on-empty stops sizing the map to the device or reverts to
#     the v1 builder image;
#   * F — a test that carries the end-to-end teeth is renamed away.
#
# The last two arms are the controls the other guards' smokes use: a missing
# input is an ABORT, not a pass, and the guard must be GREEN on the untouched
# tree — without that one, a guard that failed unconditionally would pass
# every tooth above.
#
# The hermetic seam is the guard's own optional root argument, so this smoke
# builds a minimal root containing exactly the three files the guard reads,
# mutates one line, and runs the REAL guard against it — no network, no boot.
# The only tooth that needs the user workspace is the vacuity control, which
# runs the guard on the real tree (and therefore its cargo half).
#
# Exit: 0 every tooth bit, 1 a tooth did not.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/aerofs_v2_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads. Kept in one list so a tooth can never pass
# because the copy it mutated was missing something the guard needed.
SEED_FILES=(
    user/vfs/src/aerofs.rs
    user/vfs/src/vfs.rs
    user/vfs/tests/vfs_tests.rs
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/user/vfs/src" "$1/user/vfs/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
}

replace() {   # replace <file> <old> <new> — python, so long lines need no escaping
    # ALL occurrences, not the first: a rule stated in a header, a section
    # banner and a doc comment is still the rule, and replacing one copy
    # would leave the tooth biting nothing while looking aimed correctly.
    # A pattern that is not found is fatal — a tooth whose mutation did not
    # apply would be asserted against the unmutated tree and pass by
    # accident, which is the one failure mode this whole file exists to
    # prevent, one level up.
    python3 - "$1" "$2" "$3" <<'PY' || { echo "ABORT: a tooth's mutation did not apply — aborting rather than asserting against an unmutated tree" >&2; exit 2; }
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if old not in s:
    print(f"tooth pattern not found in {p}: {old!r}", file=sys.stderr)
    sys.exit(2)
open(p, "w").write(s.replace(old, new))
PY
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

echo "aerofs_v2_check_smoke — teeth for the writable format's rule guard"
echo "=================================================================="
echo

# ── A. A missing half is a refusal to evaluate, not a pass or a failure ────
seed "$W/miss"; rm -f "$W/miss/user/vfs/src/aerofs.rs"
out="$(bash "$GUARD" "$W/miss" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT: missing:"; then
    echo "ok:   A. a missing aerofs.rs aborts (exit 2) instead of reporting the rule in step"
    passed=$((passed + 1))
else
    echo "FAIL: A. a missing file did not abort (exit $rc)"; echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── A. The version rule ────────────────────────────────────────────────────
seed "$W/a1"
replace "$W/a1/user/vfs/src/aerofs.rs" \
    'pub const AEROFS_VERSION: u32 = AEROFS_VERSION_V2;' \
    'pub const AEROFS_VERSION: u32 = AEROFS_VERSION_V1;'
tooth "A1. the version this build writes falls back to v1" "A" "$W/a1"

seed "$W/a2"
replace "$W/a2/user/vfs/src/aerofs.rs" \
    'found => return Err(SuperblockRefusal::Version { found }),' \
    'found => return Err(SuperblockRefusal::Magic),'
tooth "A2. an unknown version stops being a named refusal" "A" "$W/a2"

seed "$W/a3"
replace "$W/a3/user/vfs/src/aerofs.rs" \
    'read v1, write v2, refuse' \
    'the versions are handled'
tooth "A3. the migration rule stops being written down" "A" "$W/a3"

# ── B. The layout moved ────────────────────────────────────────────────────
seed "$W/b1"
replace "$W/b1/user/vfs/src/aerofs.rs" \
    'pub const NDIRECT_V2: usize = 10;' \
    'pub const NDIRECT_V2: usize = 11;'
tooth "B1. the second indirect pointer is removed (ten direct again)" "B" "$W/b1"

seed "$W/b2"
replace "$W/b2/user/vfs/src/aerofs.rs" \
    'indirect2: u32le(&slot[58..]),' \
    'indirect2: u32le(&slot[62..]),'
tooth "B2. the v2 inode's second pointer offset moves" "B" "$W/b2"

seed "$W/b3"
replace "$W/b3/user/vfs/src/aerofs.rs" \
    'put_u32le(&mut slot[54..], ino.indirect);' \
    'put_u32le(&mut slot[50..], ino.indirect);'
tooth "B3. the v2 inode's first pointer offset moves" "B" "$W/b3"

# ── C. The builder still writes v1 ─────────────────────────────────────────
seed "$W/c1"
replace "$W/c1/user/vfs/src/aerofs.rs" \
    'put_u32le(&mut sb_raw[4..], AEROFS_VERSION_V1);' \
    'put_u32le(&mut sb_raw[4..], AEROFS_VERSION_V2);'
tooth "C1. the image builder starts writing v2" "C" "$W/c1"

# ── D. The fs layer is version-dispatched ──────────────────────────────────
seed "$W/d1"
replace "$W/d1/user/vfs/src/vfs.rs" \
    'Fs::Aerofs(f) => f.read_only(),' \
    'Fs::Aerofs(_) => true,'
tooth "D1. the read-only gate stops consulting the version" "D" "$W/d1"

seed "$W/d2"
replace "$W/d2/user/vfs/src/vfs.rs" \
    'let inode = if self.writable {' \
    'let inode = if true {'
tooth "D2. reads parse the v2 inode layout regardless of the version" "D" "$W/d2"

seed "$W/d3"
replace "$W/d3/user/vfs/src/vfs.rs" \
    'if !self.writable {' \
    'if self.writable {'
tooth "D3. one mutating entry point loses its own v1 guard" "D" "$W/d3"

seed "$W/d4"
replace "$W/d4/user/vfs/src/vfs.rs" \
    'let (indirect, idx) = if self.writable {' \
    'let (indirect, idx) = if true {'
tooth "D4. the block-address walk stops consulting the version" "D" "$W/d4"

# ── E. An empty store formats v2, by the device's size ─────────────────────
seed "$W/e1"
replace "$W/e1/user/vfs/src/vfs.rs" \
    'let total = cache.blocks() as u32;' \
    'let total = 64u32;'
tooth "E1. the format stops being sized by the device's block count" "E" "$W/e1"

seed "$W/e2"
replace "$W/e2/user/vfs/src/vfs.rs" \
    'format_v2(total).ok_or(Errno::EInval)?' \
    'crate::aerofs::ImageBuilder::new().build()'
tooth "E2. an empty store gets the v1 builder image again" "E" "$W/e2"

# ── F. The teeth exist ─────────────────────────────────────────────────────
# The rename removes the name entirely (not a suffix added to it): the guard
# pins "fn name() {", and a suffix-only rename would still contain the old
# name — the substring trap the guard's own header warns the reader about.
seed "$W/f1"
replace "$W/f1/user/vfs/tests/vfs_tests.rs" \
    'fn v2_large_file_survives_a_reboot() {' \
    'fn v2_survival_renamed() {'
tooth "F1. the byte-identity survival test is renamed away" "F" "$W/f1"

seed "$W/f2"
replace "$W/f2/user/vfs/src/aerofs.rs" \
    'fn version_rule_refusals_are_named() {' \
    'fn refusals_renamed() {'
tooth "F2. the named-refusals unit test is renamed away" "F" "$W/f2"

# ── The vacuity control ───────────────────────────────────────────────────
# The guard must be GREEN on the untouched tree — including its cargo half,
# which is why this one arm runs the real repository. Without it, a guard that
# failed unconditionally would pass every tooth above.
echo "--- control: the untouched tree (and its host tests) must pass…"
out="$(bash "$GUARD" "$ROOT" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control. the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:') clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: control. the untouched tree does NOT pass — the guard is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
