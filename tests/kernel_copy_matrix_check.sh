#!/usr/bin/env bash
# tests/kernel_copy_matrix_check.sh — machine-checks the §10.184
# kernel-copy coverage matrix (plan doc AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# §10.184 is the single auditable table of what each kernel translator
# copy (ARM/x86/RV64) has per enforcement dimension (byte-identity
# tripwire, compile gate, undefined-symbol gate, teeth, real-execution
# leg, kernel build/link), naming the exact check/smoke that enforces
# each row. A doc table goes stale silently: a guard renamed or deleted,
# a CI reference dropped, a kernel file renamed — the prose keeps saying
# CHECK while nothing enforces it. This guard makes the matrix
# self-verifying: it parses the table and asserts, for every script
# named in a CHECK cell, that the script EXISTS and is WIRED, and for
# every .c file named anywhere in the table, that the Makefile or
# ci.yml references it.
#
# Wiring rules (the same rules the matrix text claims):
#   - tests/*_check.sh  → the run_checks.sh glob (guards=(tests/*_check.sh)),
#                         run by the main CI job on every push.
#   - tests/*_smoke.sh  → the run_guard_smokes.sh glob
#                         (smokes=(tests/*_smoke.sh)), run by kernel-guards.
#   - tools/simi/tests/ and bare script names → must be referenced by
#                         basename in .github/workflows/ci.yml, the root
#                         Makefile, or tools/simi/Makefile (the explicit
#                         toolchain jobs, X86_C_SRC/RV_C_SRC, and the
#                         test-native/test-riscv/test-arm targets).
#   - .c files → must appear in the root Makefile or ci.yml (X86_C_SRC /
#     RV_C_SRC membership, or the inline arm64-guards compile step).
#
# Toolchain-free: pure text parsing, so it rides the tests/*_check.sh
# glob in run_checks.sh and runs on every push.
#
# Exit: 0 pass, 1 fail, 2 abort (doc/table missing).
# Optional positional args (for the teeth smoke): <doc> <ci.yml> <Makefile>.
set -u
cd "$(dirname "$0")/.."   # repo root

DOC="${1:-docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md}"
CI="${2:-.github/workflows/ci.yml}"
MK="${3:-Makefile}"

[ -f "$DOC" ] || { echo "ABORT: doc '$DOC' not found" >&2; exit 2; }
[ -f "$CI" ]  || { echo "ABORT: ci '$CI' not found" >&2; exit 2; }
[ -f "$MK" ]  || { echo "ABORT: Makefile '$MK' not found" >&2; exit 2; }

# The matrix table: data rows only (skip the `| dimension |` header and
# the `|---|---|` separator, which has no letters). Rows are
# `| dim | arm | x86 | rv64 |`, so -F'|' gives $2=dim, $3-$5=cells.
rows=$(awk -F'|' '
    /^\| dimension \|/ { intable=1; next }
    intable && /^\|/ && $0 ~ /[A-Za-z]/ && NF >= 5 { print }
    intable && !/^\|/ { exit }
' "$DOC")
if [ -z "$rows" ]; then
    echo "ABORT: no §10.184 matrix table found in '$DOC' (the '| dimension |' header is missing or moved)" >&2
    exit 2
fi

# wired_in <file> <base> — true iff <file> references <base> on a
# NON-COMMENT line. Comment mentions are not wiring (tools/simi/Makefile
# mentions run_arm64_tests.sh in a comment only; only its real ci.yml
# reference wires it). Works for Makefiles and ci.yml alike, where a
# line starting (after whitespace) with '#' is a comment.
wired_in() {
    local file="$1" base="$2"
    grep -nF "$base" "$file" 2>/dev/null | grep -vE '^[0-9]+:[[:space:]]*#' | grep -q .
}

# check_script <name> <dim> — resolve the script and assert its wiring.
check_script() {
    local s="$1" dim="$2" loc="" base=""
    if [ -f "$s" ]; then
        loc="$s"
    elif [ -f "tests/$s" ]; then
        loc="tests/$s"
    elif [ -f "tools/simi/tests/$s" ]; then
        loc="tools/simi/tests/$s"
    else
        echo "FAIL  script '$s' (row '$dim') does not exist (looked in '$s', tests/, tools/simi/tests/)"
        return 1
    fi
    base=$(basename "$loc")
    case "$loc" in
        tests/*_check.sh)
            if grep -qF 'guards=(tests/*_check.sh)' tests/run_checks.sh; then
                echo "OK    $loc exists, wired via the run_checks.sh glob"
                return 0
            fi
            echo "FAIL  $loc: run_checks.sh no longer globs tests/*_check.sh" >&2
            return 1 ;;
        tests/*_smoke.sh)
            if grep -qF 'smokes=(tests/*_smoke.sh)' tests/run_guard_smokes.sh; then
                echo "OK    $loc exists, wired via the run_guard_smokes.sh glob"
                return 0
            fi
            echo "FAIL  $loc: run_guard_smokes.sh no longer globs tests/*_smoke.sh" >&2
            return 1 ;;
        *)
            # Wiring sources: ci.yml (explicit toolchain jobs) or either
            # Makefile (X86_C_SRC / RV_C_SRC / the tools/simi test targets).
            # Comment mentions do not count as wiring.
            if wired_in "$CI" "$base"; then src="ci.yml";
            elif wired_in "$MK" "$base"; then src="Makefile";
            elif wired_in "tools/simi/Makefile" "$base"; then src="tools/simi/Makefile";
            else
                echo "FAIL  $loc: '$base' is not referenced (non-comment) by $CI, $MK, or tools/simi/Makefile" >&2
                return 1
            fi
            echo "OK    $loc exists, wired ($src)"
            return 0 ;;
    esac
}

fails=0
count=0

while IFS= read -r row; do
    dim=$(printf '%s' "$row" | awk -F'|' '{gsub(/^ +| +$/, "", $2); print $2}')
    cells=$(printf '%s' "$row" | awk -F'|' '{print $3 " " $4 " " $5}')

    # Every .sh named in a CHECK cell must exist and be wired. Token-based
    # extraction (maximal non-space runs ending in .sh) so glob patterns
    # like tests/*_check.sh are dropped whole, never half-matched.
    for s in $(printf '%s' "$cells" | grep -oE '[^[:space:]|,;()]+\.sh' | grep -v '\*'); do
        count=$((count + 1))
        check_script "$s" "$dim" || fails=$((fails + 1))
    done

    # Every .c named anywhere must be referenced by the Makefile or ci.yml
    # (X86_C_SRC / RV_C_SRC membership, or the inline arm64-guards step).
    for c in $(printf '%s' "$cells" | grep -oE '[^[:space:]|,;()]+\.c'); do
        if wired_in "$CI" "$c" || wired_in "$MK" "$c"; then
            echo "OK    $c referenced by the build/CI (row '$dim')"
        else
            echo "FAIL  $c (row '$dim') is not referenced (non-comment) by $CI or $MK" >&2
            fails=$((fails + 1))
        fi
    done
done <<< "$rows"

echo
echo "kernel_copy_matrix_check: $count script claim(s) checked, $fails violations"
[ "$fails" -eq 0 ]
