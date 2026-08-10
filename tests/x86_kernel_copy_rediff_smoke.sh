#!/usr/bin/env bash
# tests/x86_kernel_copy_rediff_smoke.sh — teeth for the x86 kernel-copy
# re-diff tripwire (tests/x86_kernel_copy_rediff_check.sh).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The check proves the kernel/simi_x86.{c,h} bodies are byte-identical to
# the tools copies; this proves the CHECK has teeth. It copies the real
# files to a temp dir, mutates one byte-stream in a throwaway copy, points
# the check at the copies via its positional args, and asserts the check
# FAILS each time — plus asserts the pristine copies still PASS (so the
# teeth are meaningful, not trivially red). A check that has gone blind
# (e.g. its marker extraction broke and it now diffs empty bodies) fails
# here. The real files are never touched. Mirror of
# tests/arm_kernel_copy_rediff_smoke.sh (§10.182).
#
# Runs under tests/run_guard_smokes.sh (the `tests/*_smoke.sh` glob);
# toolchain-free, like the check it guards.
#
# Exit: 0 if both teeth fired (mutated copies failed, pristine passed),
# 1 otherwise.
set -u
cd "$(dirname "$0")/.."   # repo root

CHECK="tests/x86_kernel_copy_rediff_check.sh"
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

cp tools/simi/simi_x86.c "$TMPD/host.c"
cp kernel/simi_x86.c     "$TMPD/kernel.c"
cp tools/simi/simi_x86.h "$TMPD/host.h"
cp kernel/simi_x86.h     "$TMPD/kernel.h"

pass=0
fail=0

echo "x86 kernel-copy re-diff smoke"
echo "============================="
echo

# ── tooth 0: pristine copies must PASS (sanity — proves the check isn't
# ── trivially red, so the teeth below are meaningful).
echo "tooth 0: pristine copies pass the check"
if bash "$CHECK" "$TMPD/host.c" "$TMPD/kernel.c" "$TMPD/host.h" "$TMPD/kernel.h" >/dev/null 2>&1; then
    echo "  PASS  pristine copies accepted"
    pass=$((pass + 1))
else
    echo "  FAIL  pristine copies rejected — the check itself is broken"
    fail=$((fail + 1))
fi
echo

# ── tooth 1: mutate the KERNEL copy body (a one-token edit in a line
# ── below the #include marker). The check must fail: a host-side fix
# ── that never re-derived the kernel copy is exactly the divergence this
# ── guards against.
echo "tooth 1: a mutated kernel copy fails the check"
sed 's/emit_instr/emit_instr_X/' "$TMPD/kernel.c" > "$TMPD/kernel_mut.c"
if bash "$CHECK" "$TMPD/host.c" "$TMPD/kernel_mut.c" "$TMPD/host.h" "$TMPD/kernel.h" >/dev/null 2>&1; then
    echo "  FAIL  divergent kernel copy accepted — the re-diff check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  divergent kernel copy caught"
    pass=$((pass + 1))
fi
echo

# ── tooth 2: mutate the HOST copy body. The check must fail the same
# ── way: the contract is two-way (the kernel copy is the staged encoder;
# ── a host edit without the re-derive breaks the identity either way).
echo "tooth 2: a mutated host copy fails the check"
sed 's/emit_instr/emit_instr_Y/' "$TMPD/host.c" > "$TMPD/host_mut.c"
if bash "$CHECK" "$TMPD/host_mut.c" "$TMPD/kernel.c" "$TMPD/host.h" "$TMPD/kernel.h" >/dev/null 2>&1; then
    echo "  FAIL  divergent host copy accepted — the re-diff check is blind"
    fail=$((fail + 1))
else
    echo "  PASS  divergent host copy caught"
    pass=$((pass + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
