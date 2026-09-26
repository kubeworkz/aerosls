#!/usr/bin/env bash
# tests/guard_port_band_smoke.sh — proves tests/guard_port_band_check.sh's
# teeth bite: the default band back on the live band, an override that is
# ignored, a band that overlaps and is not refused, a malformed band silently
# defaulted, a probe removed (fail-open), a literal host port planted in a
# check, and a CI QEMU with no allocator must each turn it red; the
# reservation itself must lose its lock test, its holder, its TTL and its
# missing-flock refusal; and a missing python3 must make it ABORT rather than
# pass.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# guard_port_band_check.sh is the thing that keeps test boots out of the live
# REST band, which is the property whose absence cost a red 53-minute CI job
# on 2026-09-24 (a local QEMU on :3001, and webapp_served_check.sh judging
# that kernel instead of the deployed one). A rule-enforcer whose own teeth
# were never proven is one bad edit away from passing while inspecting
# nothing — and this one has an especially quiet way to do that: it drives
# tests/free_port.sh, so a mutation in the ALLOCATOR (not in the check) is
# what makes the rule stop holding. Half these teeth are planted there.
#
# ─── How it plants them, and why it never touches the repo ─────────────────
# The check reads four things: tests/free_port.sh, net/consensus.h,
# .github/workflows/ci.yml and Makefile, plus the tests/*_check.sh glob. So
# this smoke builds a scratch tree with exactly those, copies the check in,
# and mutates the COPY. The real worktree is never written to, which matters
# because unlike commands_doc_smoke.sh this one has no mutation that must be
# undone in place: every arm is a file in $T.
#
# Every mutation is an exact-string replacement that fails loudly if the
# needle is absent or not unique, so a tooth cannot degrade into "the
# mutation never applied, and the check was green anyway" — and each arm
# asserts how MANY failure lines it moved (not just that the check exited
# non-zero), so a mutation that cascades into unrelated clauses is a failure
# too. File counts are asserted, not membership.
#
# The reservation teeth (10-14) use assert_run2: the same discipline, plus a
# total ^FAIL: count of the WHOLE check, because one mutation in the allocator
# reds several clauses in rule 6 by design (there is one lock and four arms
# read it). Naming the family AND fixing the total keeps a cascade visible.
#
# ─── Measured (2026-09-25, on this host: 14 teeth bit, 0 failed, 45 s) ────
# Each tooth re-runs the whole check (24 clauses, ~3 s), which is where the
# 45 s goes; the check's own arms are the slow part, not this file.
#   baseline (pristine scratch tree)      -> pass, 24 ok
#   default band onto the live band       -> red, 1 line  'contains live port'
#   AEROSLS_FREE_PORT_RANGE ignored       -> red, 1 line  'expected the override to move the band' (default-arm clause still green)
#   overlap refused nowhere               -> red, 3 lines 'was not refused|did not follow'
#   malformed band silently defaulted     -> red, 2 lines 'was not refused (rc=0)'
#   probe removed (fail-open)             -> red, 1 line  'did not fail closed'
#   literal host port in a check          -> red, 1 line  'literal host port'
#   CI QEMU with no allocator             -> red, 1 line  'but 2 free_port.sh allocation'
#   no python3 on PATH                    -> ABORT exit 2 'python3 not found'
#   silence from --print-band             -> red, 1 line  'not the declared default'
#   no lock test (probe-only allocation)  -> red, 4 FAILs 'not reserved|without a
#                                            held reservation' (the two
#                                            reservation-held arms and the two
#                                            arms that read a held lock)
#   holder never starts (lock released    -> red, 4 FAILs, same family
#     when the allocator exits)
#   TTL ten times too long                -> red, 1 FAIL 'did not bound the reservation'
#   reserved port handed out anyway       -> red, 1 FAIL 'a second allocation …
#                                            (probe-only allocation)'
#   flock missing degrades to probe-only  -> red, 1 FAIL 'did not refuse with flock named'
#   restore (pristine copy re-run)        -> pass, 24 ok, and the repo's own
#                                            check is byte-identical to the
#                                            copy this smoke started from
#
# Three of these teeth earned their place while this smoke was written, which
# is the argument for having it: 'overlap refused nowhere' only bites if the
# WHOLE refusal condition goes (the production-port clause is an independent
# guard), 'override ignored' only bites if the declaration line stays readable
# (the check reads the default out of it, statically), and the fail-open probe
# needle is a 4-space-indented line, not 8.
#
# Source-only: bash, python3, grep and sed, nothing built. Auto-included by
# run_source_smokes.sh (verify job, every push) and run_guard_smokes.sh via
# the tests/*_smoke.sh glob, so it is NOT in run_source_smokes.sh's
# BUILD_SMOKES list.
#
# Exit: 0 all teeth bit, 1 a tooth did not, 2 a prerequisite is missing.
set -u
cd "$(dirname "$0")/.."   # repo root

for tool in python3 grep sed timeout; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ABORT: $tool not found — the teeth need it." >&2
        exit 2
    }
done

CHECK=tests/guard_port_band_check.sh
for f in "$CHECK" tests/free_port.sh net/consensus.h .github/workflows/ci.yml Makefile; do
    [ -f "$f" ] || { echo "ABORT: $f missing — the check reads it, so a scratch tree cannot be built." >&2; exit 2; }
done

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
PRISTINE="$W/pristine"
T="$W/tree"
mkdir -p "$PRISTINE/tests" "$PRISTINE/net" "$PRISTINE/.github/workflows"
cp "$CHECK" tests/free_port.sh "$PRISTINE/tests/"
cp net/consensus.h "$PRISTINE/net/"
cp .github/workflows/ci.yml "$PRISTINE/.github/workflows/"
cp Makefile "$PRISTINE/"
REPO_CHECK_SHA="$(sha256sum "$CHECK" | cut -d' ' -f1)"

restore() {   # back to the tree this smoke started from, planted files and all
    rm -rf "$T"
    cp -a "$PRISTINE" "$T"
}

# plant <file> <old> <new> — exact, unique, or it fails (never a silent no-op)
plant() {
    FILE="$1" OLD="$2" NEW="$3" python3 - <<'PY' || return 1
import os, sys
path, old, new = os.environ['FILE'], os.environ['OLD'], os.environ['NEW']
s = open(path, encoding='utf-8').read()
n = s.count(old)
if n != 1:
    sys.stderr.write("mutation needle found %d time(s) (wanted exactly 1) in %s: %r\n"
                     % (n, path, old[:70]))
    sys.exit(1)
open(path, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
}

run_check() { (cd "$T" && bash "$CHECK" 2>&1); }

fails=0
teeth=0

# assert_run <name> <expected-rc> <regex> <expected-count> [<unmoved-regex> <count>]
assert_run() {
    name="$1"; want_rc="$2"; pat="$3"; want_n="$4"
    out="$(run_check)"; rc=$?
    got="$(printf '%s\n' "$out" | grep -cE "$pat" || true)"
    extra_ok=1
    if [ "$#" -ge 6 ]; then
        got2="$(printf '%s\n' "$out" | grep -cE "$5" || true)"
        [ "$got2" = "$6" ] || extra_ok=0
    fi
    if [ "$rc" = "$want_rc" ] && [ "$got" = "$want_n" ] && [ "$extra_ok" = 1 ]; then
        echo "TOOTH OK   $name — exit $rc, '$pat' x$got"
        teeth=$((teeth + 1))
    else
        echo "TOOTH FAIL $name — exit $rc (wanted $want_rc), '$pat' seen $got (wanted $want_n)"
        [ "$extra_ok" = 1 ] || echo "           and the unmoved clause moved: '$5' seen $got2 (wanted $6)"
        printf '%s\n' "$out" | grep -E '^(ok|FAIL|ABORT)' | sed 's/^/           /'
        fails=$((fails + 1))
    fi
    restore
}

# ── baseline: a pristine scratch tree passes ───────────────────────────────
restore
out="$(run_check)"; rc=$?
n_ok="$(printf '%s\n' "$out" | grep -c '^ok:' || true)"
if [ "$rc" = 0 ] && [ "$n_ok" -ge 20 ]; then
    echo "BASELINE OK — a pristine scratch tree passes ($n_ok ok, exit 0)"
else
    echo "BASELINE FAIL — the check is red (or vacuous) before any mutation:"
    printf '%s\n' "$out" | sed 's/^/      /'
    fails=$((fails + 1))
fi

# ── 1. the default band back on the live band ──────────────────────────────
plant "$T/tests/free_port.sh" 'AEROSLS_FREE_PORT_RANGE:-32001-32020' 'AEROSLS_FREE_PORT_RANGE:-3001-3020' \
    || echo "           (needle: the default band in free_port.sh)"
assert_run "default band back on the live band" 1 'contains live port' 1

# ── 2. the override is ignored ─────────────────────────────────────────────
# The declaration line stays byte-intact (the check reads the default out of
# it statically) and a second assignment makes the env var reach nothing: the
# override is ignored without breaking where the default points.
plant "$T/tests/free_port.sh" 'RANGE="${AEROSLS_FREE_PORT_RANGE:-32001-32020}"' \
    'RANGE="${AEROSLS_FREE_PORT_RANGE:-32001-32020}"
RANGE=32001-32020   # mutation: the override is not consulted' \
    || echo "           (needle: the RANGE assignment in free_port.sh)"
# The default-band clause must stay green: this tooth moves the override only.
assert_run "AEROSLS_FREE_PORT_RANGE ignored" 1 'expected the override to move the band' 1 \
    'default guard band 32001-32020 excludes' 1

# ── 3. a band that overlaps the live band is not refused ───────────────────
# The WHOLE refusal condition goes, not just its first clause: the `|| { ... }`
# half is an independent guard for production's port, so dropping only
# `overlaps` leaves two of the three refusal arms green for the right reason.
OVERLAP_OLD='if overlaps "$START" "$END" "$LIVE_LO" "$LIVE_HI" || { [ "$START" -le "$PROD" ] && [ "$PROD" -le "$END" ]; }; then'
plant "$T/tests/free_port.sh" "$OVERLAP_OLD" 'if false; then' \
    || echo "           (needle: the overlap refusal in free_port.sh)"
assert_run "overlap not refused" 1 'was not refused|did not follow AEROSLS_HTTP_BASE' 3

# ── 4. a malformed band silently becomes the default ───────────────────────
# Quoted heredocs for needles that carry both quote kinds and an em dash:
# nothing is expanded, and the bytes are readable next to the line they match.
MALFORMED_OLD="$(cat <<'EOF'
        echo "ABORT: AEROSLS_FREE_PORT_RANGE='$RANGE' is not START-END (or a single port) — e.g. 32001-32020." >&2
        exit 1
EOF
)"
plant "$T/tests/free_port.sh" "$MALFORMED_OLD" '        RANGE=32001-32020; START=32001; END=32020' \
    || echo "           (needle: the malformed-band refusal in free_port.sh)"
# Only the two non-numeric bands are affected; 0-5, 70000-… and START>END are
# still caught by the out-of-range clause.
assert_run "malformed band silently defaulted" 1 'was not refused \(rc=0\)' 2

# ── 5. the probe removed: fail-open on a taken port ────────────────────────
# The probe line carries both quote kinds, so it is built once, quoted the way
# bash needs it, rather than inline.
PROBE_OLD="$(cat <<'EOF'
    if ! timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$p" 2>/dev/null; then
EOF
)"
plant "$T/tests/free_port.sh" "$PROBE_OLD" '    if [ -n "$p" ]; then' \
    || echo "           (needle: the bounded probe in free_port.sh)"
assert_run "probe removed (fail-open)" 1 'did not fail closed' 1

# ── 6. a literal host port in the machinery ────────────────────────────────
cat > "$T/tests/literal_hostfwd_check.sh" <<'SH'
#!/usr/bin/env bash
set -u
qemu-system-x86_64 -cdrom sls_operating_system.iso \
    -netdev user,id=net0,hostfwd=tcp::3001-:3000 \
    -display none -m 4G -boot d
SH
assert_run "literal host port in a check" 1 'literal host port' 1

# ── 7. a CI QEMU that allocates nothing ────────────────────────────────────
# Anchored on the decoder step's token line, which is unique: the two hostfwd
# blocks in ci.yml are otherwise byte-identical, so a needle aimed at one of
# them cannot be required to be unique.
CI_ANCHOR='TOK=deadbeef01234567cafebabe76543210   # dave, DB_ADMIN'
plant "$T/.github/workflows/ci.yml" "$CI_ANCHOR" "$CI_ANCHOR
          -netdev user,id=net9,hostfwd=tcp:127.0.0.1:\$PORT-:3000 \\" \
    || echo "           (needle: the decoder token line in ci.yml)"
assert_run "CI QEMU with no allocator" 1 'but 2 free_port\.sh allocation' 1

# assert_run2 <name> <family-regex> <total-FAIL-count>
# Like assert_run, for a mutation that reds a family of clauses: the family
# must bite at least once, and the check's TOTAL ^FAIL: count must be exactly
# the measured number, so an extra cascade still fails the tooth.
assert_run2() {
    name="$1"; pat="$2"; want_nfail="$3"
    out="$(run_check)"; rc=$?
    got="$(printf '%s\n' "$out" | grep -cE "$pat" || true)"
    nfail="$(printf '%s\n' "$out" | grep -c '^FAIL:' || true)"
    if [ "$rc" = 1 ] && [ "$got" -ge 1 ] && [ "$nfail" = "$want_nfail" ]; then
        echo "TOOTH OK   $name — exit 1, '$pat' x$got, $nfail FAIL clause(s) in the check"
        teeth=$((teeth + 1))
    else
        echo "TOOTH FAIL $name — exit $rc (wanted 1), '$pat' seen $got (wanted >=1), $nfail FAIL clause(s) (wanted $want_nfail)"
        printf '%s\n' "$out" | grep -E '^(ok|FAIL|ABORT)' | sed 's/^/           /'
        fails=$((fails + 1))
    fi
    restore
}

# ── 8. a missing prerequisite ABORTs, never passes ─────────────────────────
# A PATH that still resolves dirname (used before the python3 check) but has
# no python3 at all.
FAKE="$W/fakebin"
mkdir -p "$FAKE"
ln -s "$(command -v dirname)" "$FAKE/dirname"
out="$(cd "$T" && env PATH="$FAKE" "$(command -v bash)" "$CHECK" 2>&1)"; rc=$?
if [ "$rc" = 2 ] && printf '%s\n' "$out" | grep -q 'python3 not found'; then
    echo "TOOTH OK   no python3 on PATH — ABORT exit 2, 'python3 not found'"
    teeth=$((teeth + 1))
else
    echo "TOOTH FAIL no python3 on PATH — exit $rc, expected 2 with 'python3 not found':"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi
restore

# ── 9. a silent --print-band cannot pass vacuously ─────────────────────────
plant "$T/tests/free_port.sh" '    echo "$START-$END"
    exit 0' '    echo ""
    exit 0' || echo "           (needle: the --print-band output in free_port.sh)"
assert_run "--print-band silent (vacuity)" 1 'not the declared default' 1

# ── 10. no lock test: the reservation excludes nothing ─────────────────────
# The lock is the whole mechanism: without it an allocation is a probe again,
# which is what the rule exists to forbid. Reds every arm that reads the lock.
plant "$T/tests/free_port.sh" '    if ! flock -n "$fd" 2>/dev/null; then' '    if false; then' \
    || echo "           (needle: the lock test in free_port.sh's reserve_port)"
assert_run2 "no lock test (probe-only allocation)" 'not reserved|without a held reservation' 4

# ── 11. no holder: the lock dies with the allocator ────────────────────────
# The allocator exits as soon as it has printed the port, so unless something
# outlives it the reservation lasts microseconds — exactly the window the probe
# could not cover. 'wait' in the check's first arm makes this deterministic.
plant "$T/tests/free_port.sh" '        if [ "$HOLD" = "1" ]; then
            hold_until_bound "$p" "$HOLD_S" >/dev/null 2>&1 &
        fi' '        :' \
    || echo "           (needle: the holder spawn in free_port.sh)"
assert_run2 "holder never starts" 'not reserved|without a held reservation' 4

# ── 12. the TTL is ten times what it says ──────────────────────────────────
plant "$T/tests/free_port.sh" 'ticks=$(( $2 * 5 ))' 'ticks=$(( $2 * 50 ))' \
    || echo "           (needle: the holder's tick budget in free_port.sh)"
assert_run2 "TTL ten times too long" 'did not bound the reservation' 1

# ── 13. a reserved port is handed out anyway ───────────────────────────────
# Skipping a locked port without probing it is what makes two concurrent runs
# mutually exclusive. With the skip gone the port is free (nothing has bound
# it yet) and the second run takes it: the collision, restored.
plant "$T/tests/free_port.sh" '        reserve_port "$p" || continue' '        reserve_port "$p" || true' \
    || echo "           (needle: the reserved-port skip in free_port.sh)"
assert_run2 "reserved port handed out anyway" '\(probe-only allocation\)' 1

# ── 14. no flock quietly degrades to probe-only ────────────────────────────
FLOCK_OLD="$(cat <<'EOF'
    command -v flock >/dev/null 2>&1 || {
        echo "ABORT: flock not found, so a guard port cannot be reserved until QEMU" >&2
        echo "       binds it: two concurrent guard runs could both pass the probe and" >&2
        echo "       then collide on the bind. flock ships with util-linux. Install it," >&2
        echo "       or accept probe-only allocation with AEROSLS_FREE_PORT_HOLD=0." >&2
        exit 1
    }
EOF
)"
plant "$T/tests/free_port.sh" "$FLOCK_OLD" '    :   # mutation: without flock the allocator probes only' \
    || echo "           (needle: the flock prerequisite in free_port.sh)"
assert_run2 "flock missing degrades to probe-only" 'did not refuse with flock named' 1

# ── restore check: the pristine copy still passes, and the repo is untouched ─
restore
out="$(run_check)"; rc=$?
n_ok="$(printf '%s\n' "$out" | grep -c '^ok:' || true)"
now_sha="$(sha256sum "$CHECK" | cut -d' ' -f1)"
if [ "$rc" = 0 ] && [ "$n_ok" -ge 20 ] && [ "$now_sha" = "$REPO_CHECK_SHA" ]; then
    echo "RESTORE OK — pristine copy passes again ($n_ok ok) and $CHECK is byte-identical to the copy this smoke started from"
else
    echo "RESTORE FAIL — rc=$rc, $n_ok ok, repo check sha $now_sha vs $REPO_CHECK_SHA at entry:"
    printf '%s\n' "$out" | sed 's/^/      /'
    fails=$((fails + 1))
fi

echo
echo "guard_port_band_smoke: $teeth teeth bit, $fails failed"
[ "$fails" -eq 0 ]
