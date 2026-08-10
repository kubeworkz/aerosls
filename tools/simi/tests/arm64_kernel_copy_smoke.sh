#!/usr/bin/env bash
# tools/simi/tests/arm64_kernel_copy_smoke.sh — teeth for the
# kernel/simi_arm.c freestanding-compile gate (M3 kernel half, plan doc
# §10.180/10.181).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The arm64-guards CI job compiles kernel/simi_arm.c under aarch64
# freestanding flags and asserts (a) zero compiler warnings and (b) no
# undefined symbols beyond the freestanding {memcpy, memset} pair — the
# two library functions every real kernel provides (the RV64/x86 kernel
# copies compile to zero undefined symbols on their toolchains; GCC 13 on
# AArch64 synthesizes memcpy/memset calls from the M2 chain-analysis
# struct copies, so the ARM copy's contract is precisely "nothing but the
# freestanding pair", not "no symbols at all"). This smoke proves the
# gate has teeth: it deliberately breaks BOTH halves of the check on a
# throwaway copy and asserts the gate FAILS each time, exactly like the
# tests/*_smoke.sh teeth in tests/run_guard_smokes.sh.
#
# Deliberately NOT globbed by tests/run_guard_smokes.sh (this file lives
# in tools/simi/tests/, not tests/): it needs the aarch64 cross toolchain
# and is run by the arm64-guards CI job, which installs it.
#
# Exit: 0 if both teeth fired (broken copies failed the gate, the clean
# copy passed), 1 otherwise, 2 if the toolchain is missing.
set -u
cd "$(dirname "$0")/../../.."   # repo root

command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
    echo "ABORT: aarch64-linux-gnu-gcc not found — run on a host with the arm64 cross toolchain (CI arm64-guards job)." >&2
    exit 2
}
command -v aarch64-linux-gnu-nm >/dev/null 2>&1 || {
    echo "ABORT: aarch64-linux-gnu-nm not found — run on a host with the arm64 cross toolchain." >&2
    exit 2
}

# The exact gate flags the arm64-guards job uses (mirror of the kernel's
# RV_CFLAGS freestanding discipline: -ffreestanding -O2 -Wall -Wextra).
# -Ikernel reproduces how the real gate's `#include "simi_arm.h"`
# resolves (the source lives in kernel/, so its own directory is searched
# first); the throwaway copies compile from a temp dir, so they need the
# include path spelled out — same header, same compile.
CC_FLAGS="-ffreestanding -O2 -Wall -Wextra -ffunction-sections -fdata-sections -Ikernel"

# gate_check <file.c> <obj> — run the gate against one file.
# Returns 0 (pass) iff: compile output is empty (zero warnings/errors)
# AND every undefined symbol is memcpy or memset (no libc beyond the
# freestanding pair). Anything else is a gate failure.
gate_check() {
    local src="$1" obj="$2"
    local out
    out=$(aarch64-linux-gnu-gcc $CC_FLAGS -c "$src" -o "$obj" 2>&1)
    if [ -n "$out" ]; then
        printf '%s\n' "  gate FAILED: compile output not empty:" "$out"
        return 1
    fi
    local bad
    bad=$(aarch64-linux-gnu-nm -u "$obj" | awk '$1 == "U" { print $2 }' \
            | grep -vx 'memcpy\|memset')
    if [ -n "$bad" ]; then
        printf '%s\n' "  gate FAILED: undefined symbols beyond {memcpy, memset}:" "$bad"
        return 1
    fi
    return 0
}

pass=0
fail=0
TMPDIR_D=$(mktemp -d)
trap 'rm -rf "$TMPDIR_D"' EXIT

echo "arm64 kernel-copy gate smoke"
echo "============================"
echo

# ── tooth 1: the clean copy must PASS the gate (sanity — proves the gate
# ── isn't trivially broken, so the teeth below are meaningful).
echo "tooth 0: clean kernel/simi_arm.c passes the gate"
if gate_check kernel/simi_arm.c "$TMPDIR_D/clean.o"; then
    echo "  PASS  clean copy accepted"
    pass=$((pass + 1))
else
    echo "  FAIL  clean copy rejected — the gate itself is broken"
    fail=$((fail + 1))
fi
echo

# ── tooth 1: inject a REAL libc call (printf). This is the classic
# ── freestanding violation: the file claims no libc dependency, so a
# ── printf reference must fail the gate's undefined-symbol check. The
# ── function is deliberately EXTERNAL (not static) so the compile is
# ── warning-free — only the nm check sees printf as an undefined symbol
# ── and fails, which is the point: the symbol check is what has teeth.
echo "tooth 1: a libc call (printf) fails the gate"
{
    sed '/^#include "simi_arm.h"$/a #include <stdio.h>' kernel/simi_arm.c
    printf '%s\n' 'int arm_teeth_printf(void) { return printf("x"); }'
} > "$TMPDIR_D/broken_printf.c"
if gate_check "$TMPDIR_D/broken_printf.c" "$TMPDIR_D/broken_printf.o"; then
    echo "  FAIL  printf slipped through — the undefined-symbol check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  printf caught by the undefined-symbol check"
    pass=$((pass + 1))
fi
echo

# ── tooth 2: inject an unused variable. The zero-warning half of the
# ── gate must catch it (-Wall -Wextra's -Wunused-variable).
echo "tooth 2: an unused variable fails the gate"
{
    sed '/^#include "simi_arm.h"$/a static int arm_teeth_unused = 42;' kernel/simi_arm.c
} > "$TMPDIR_D/broken_warn.c"
if gate_check "$TMPDIR_D/broken_warn.c" "$TMPDIR_D/broken_warn.o"; then
    echo "  FAIL  unused variable slipped through — the zero-warning check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  unused variable caught by the zero-warning check"
    pass=$((pass + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
