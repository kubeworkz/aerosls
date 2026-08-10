#!/usr/bin/env bash
# tools/simi/tests/simi_riscv_kernel_smoke.sh — teeth for the RV64
# kernel-copy guard (tools/simi/tests/simi_riscv_kernel_check.sh).
#
# The check proves kernel/simi_riscv.c compiles clean under the kernel's
# RV_CFLAGS and links with zero undefined symbols; this proves the CHECK
# has teeth. It copies the file to a temp dir, injects a real libc call
# (printf) and an unused variable into throwaway copies, points the check
# at them via its positional args, and asserts the check FAILS each time
# — plus asserts the pristine copy still PASSES (so the teeth are
# meaningful, not trivially red). The real file is never touched. A check
# that has gone blind fails here.
#
# Deliberately NOT globbed by tests/run_guard_smokes.sh (this file lives
# in tools/simi/tests/, not tests/): it needs the RISC-V cross toolchain
# and is run by the riscv-guards CI job, which installs it.
#
# Exit: 0 if both teeth fired, 1 otherwise, 2 if the toolchain is missing.
set -u
cd "$(dirname "$0")/../../.."   # repo root

CHECK="tools/simi/tests/simi_riscv_kernel_check.sh"
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

command -v riscv64-unknown-elf-gcc >/dev/null 2>&1 || {
    echo "ABORT: riscv64-unknown-elf-gcc not found — run on a host with the RISC-V cross toolchain (CI riscv-guards job)." >&2
    exit 2
}

cp kernel/simi_riscv.c "$TMPD/rv.c"

pass=0
fail=0

echo "riscv64 kernel-copy guard smoke"
echo "==============================="
echo

echo "tooth 0: the pristine copy passes the check"
if bash "$CHECK" "$TMPD/rv.c" "$TMPD/clean.o" >/dev/null 2>&1; then
    echo "  PASS  pristine copy accepted"
    pass=$((pass + 1))
else
    echo "  FAIL  pristine copy rejected — the check itself is broken"
    fail=$((fail + 1))
fi
echo

echo "tooth 1: a libc call (printf) fails the check"
{
    sed '/^#include "simi_riscv.h"$/a #include <stdio.h>' "$TMPD/rv.c"
    printf '%s\n' 'int rv_teeth_printf(void) { return printf("x"); }'
} > "$TMPD/printf.c"
if bash "$CHECK" "$TMPD/printf.c" "$TMPD/printf.o" >/dev/null 2>&1; then
    echo "  FAIL  printf slipped through — the undefined-symbol check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  printf caught"
    pass=$((pass + 1))
fi
echo

echo "tooth 2: an unused variable fails the check"
sed '/^#include "simi_riscv.h"$/a static int rv_teeth_unused = 42;' "$TMPD/rv.c" > "$TMPD/warn.c"
if bash "$CHECK" "$TMPD/warn.c" "$TMPD/warn.o" >/dev/null 2>&1; then
    echo "  FAIL  unused variable slipped through — the zero-warning check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  unused variable caught"
    pass=$((pass + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
