#!/usr/bin/env bash
# tests/kernel_copy_matrix_smoke.sh — teeth for the §10.184 matrix
# self-check (tests/kernel_copy_matrix_check.sh).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The check proves every script the matrix names in a CHECK cell exists
# and is wired, and every .c it names is referenced by the build/CI; this
# proves the CHECK has teeth. It copies the doc and ci.yml to a temp dir,
# deliberately breaks each class of staleness in the throwaway copies,
# points the check at them via its positional args, and asserts the check
# FAILS each time — plus asserts the pristine copies still PASS (so the
# teeth are meaningful, not trivially red). A check that has gone blind
# (e.g. its table parser broke and it now matches nothing) fails here.
# The real files are never touched.
#
# Runs under tests/run_guard_smokes.sh (the `tests/*_smoke.sh` glob);
# toolchain-free, like the check it guards.
#
# Exit: 0 if all teeth fired (broken copies failed, pristine passed),
# 1 otherwise.
set -u
cd "$(dirname "$0")/.."   # repo root

CHECK="tests/kernel_copy_matrix_check.sh"
DOC="docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md"
CI=".github/workflows/ci.yml"
MK="Makefile"

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

cp "$DOC" "$TMPD/matrix.md"
cp "$CI"  "$TMPD/ci.yml"

pass=0
fail=0

echo "kernel-copy matrix self-check smoke"
echo "==================================="
echo

# ── tooth 0: pristine copies must PASS (sanity — proves the check isn't
# ── trivially red, so the teeth below are meaningful).
echo "tooth 0: the pristine matrix passes the check"
if bash "$CHECK" "$DOC" "$CI" "$MK" >/dev/null 2>&1; then
    echo "  PASS  pristine matrix accepted"
    pass=$((pass + 1))
else
    echo "  FAIL  pristine matrix rejected — the check itself is broken"
    fail=$((fail + 1))
fi
echo

# ── tooth 1: a script the matrix names no longer exists. The stale-table
# ── case the check exists for: a guard renamed or deleted without the
# ── matrix being updated.
echo "tooth 1: a renamed script in the matrix fails the check"
sed 's/arm_kernel_copy_rediff_check\.sh/arm_kernel_copy_rediff_check_MISSING.sh/g' \
    "$TMPD/matrix.md" > "$TMPD/matrix_renamed.md"
if bash "$CHECK" "$TMPD/matrix_renamed.md" "$CI" "$MK" >/dev/null 2>&1; then
    echo "  FAIL  renamed script accepted — the existence check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  renamed script caught"
    pass=$((pass + 1))
fi
echo

# ── tooth 2: a wired script loses its CI reference. The run_arm64_tests.sh
# ── line is stripped from the throwaway ci.yml; the script still exists,
# ── so only the wiring assertion can catch it.
echo "tooth 2: a dropped CI reference fails the check"
sed '/run_arm64_tests\.sh/d' "$TMPD/ci.yml" > "$TMPD/ci_unwired.yml"
if bash "$CHECK" "$DOC" "$TMPD/ci_unwired.yml" "$MK" >/dev/null 2>&1; then
    echo "  FAIL  unwired script accepted — the wiring check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  unwired script caught"
    pass=$((pass + 1))
fi
echo

# ── tooth 3: a kernel .c the matrix names disappears from the build.
echo "tooth 3: a renamed kernel .c fails the check"
sed 's/kernel\/simi_x86\.c/kernel\/simi_nonexistent.c/g' \
    "$TMPD/matrix.md" > "$TMPD/matrix_badc.md"
if bash "$CHECK" "$TMPD/matrix_badc.md" "$CI" "$MK" >/dev/null 2>&1; then
    echo "  FAIL  unbuilt kernel .c accepted — the build-reference check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  unbuilt kernel .c caught"
    pass=$((pass + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
