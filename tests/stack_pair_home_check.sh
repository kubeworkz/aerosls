#!/usr/bin/env bash
# tests/stack_pair_home_check.sh — the bootstrap-stack stub pair
# (char stack_bottom / char stack_top) may be DEFINED only inside
# tests/process_host_stubs.h.
#
# ─── The drift this locks down ─────────────────────────────────────────────
# Every host test that links frame_pool.c must supply the pair, because
# frame_pool_init() reserves [stack_bottom, stack_top) by name instead of
# trusting _kernel_image_end to cover it. Each host test used to carry its
# own copy — fourteen files at the peak — until they were consolidated into
# tests/process_host_stubs.h. The copies are the drift class: a new test
# pastes the two lines in because that is what every old test did, and the
# "one copy, one place" guarantee silently dies the way it always does in
# C — one file at a time, each edit locally reasonable. This guard makes
# the consolidation enforced rather than aspirational: a future test that
# reintroduces a local copy fails the suite at the source instead of
# re-starting the drift.
#
# ─── What "defined" means here ─────────────────────────────────────────────
# The scan matches lines that BEGIN a C definition of either symbol:
#
#     ^[[:space:]]*(static[[:space:]]+)?char[[:space:]]+(stack_bottom|stack_top)
#
# That anchor deliberately excludes every legitimate non-definition:
#
#   - `extern char stack_bottom[], stack_top[];` (kernel/frame_pool.c,
#     kernel/stubs.c) — a declaration, not a definition; the line starts
#     with `extern`, so the anchor never matches it. The guard must NOT
#     flag these, or it would fail on the genuine kernel;
#   - `global stack_bottom` / `stack_bottom: resb 4096 * 256`
#     (arch/x86/boot.asm) — asm, and the REAL provider. The guard exists
#     to police the host-test stub pair, never the production symbol;
#   - every comment, doc, and nm/readelf reference elsewhere in the tree.
#
# ─── The one sanctioned exception ──────────────────────────────────────────
# tests/tls_cert_oracle_stubs.c defines `char stack_bottom[1], stack_top[1];`
# on a single line. That is NOT a host-test copy: the TLS oracle is a
# standalone host link of kernel/stubs.c (which references the pair), built
# by tests/tls_cert_oracle_smoke.sh with no host test and no boot.asm in the
# link. The pair there is one of the oracle's deliberately-local plumbing
# one-liners — the same philosophy as every other stub in that file — and
# its `[1]` sizing makes the link-satisfaction purpose explicit. It is
# allowlisted by full path, with this reason, following the
# makefile_sources_check.sh EXCLUDED-with-a-reason pattern: anything NOT in
# the allowlist is a finding, and this is the only place a "we meant to
# leave that there" can hide.
#
# Run via tests/run_checks.sh (the tests/*_check.sh glob) and therefore in
# CI's "Guard scripts" step and deploy.sh's guard gate. Source-only: needs
# nothing but grep and the tree, so it never skips — a guard that inspects
# nothing protects nothing.
#
# Exit: 0 pass, 1 fail (never 2 — there is no prerequisite that could be
# missing).
set -u
cd "$(dirname "$0")/.."   # repo root, so the relative paths below resolve

HOME_FILE=tests/process_host_stubs.h
EXCEPTION=tests/tls_cert_oracle_stubs.c   # see header: oracle link-satisfaction stub
PAT='^[[:space:]]*(static[[:space:]]+)?char[[:space:]]+(stack_bottom|stack_top)'

pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

echo "=== stack_bottom/stack_top may be defined only in tests/process_host_stubs.h ==="
echo

# ─── positive: the designated home still carries the pair ─────────────────
# "Only in the header" is vacuously true if nobody defines them anywhere.
# If the header ever loses the pair, the host tests that include it fail at
# the link — loud but late — and the guard should say so at the source.
for sym in stack_bottom stack_top; do
    if grep -qE "^[[:space:]]*char[[:space:]]+${sym}" "$HOME_FILE"; then
        ok "$HOME_FILE still defines $sym"
    else
        bad "$HOME_FILE no longer defines $sym -- the designated home lost the pair"
    fi
done

# ─── sweep: every definition site in the tree ──────────────────────────────
# vendor/ is excluded for the same reason script_conventions_check.sh
# excludes it: vendored code is verified by provenance, not by house style.
viol=0
while IFS=: read -r file line rest; do
    file="${file#./}"    # grep -r prints ./-prefixed paths; allowlist is bare
    case "$file" in
        "$HOME_FILE"|"$EXCEPTION") continue ;;
    esac
    bad "$file:$line defines the pair: $rest"
    viol=$((viol+1))
done < <(grep -rnE -I --exclude-dir=.git --exclude-dir=vendor "$PAT" . 2>/dev/null)

if [ "$viol" -eq 0 ]; then
    ok "*** no definition outside the shared header (and its one named exception) ***"
fi

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
