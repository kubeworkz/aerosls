#!/usr/bin/env bash
# Phase 5 verification: assembles every tests/*.timi (Phase 1 host toolchain,
# unchanged), translates the resulting .tmo to RV64 machine code
# (timi_riscv.c), and executes it via timi-riscv-verify — a small
# purpose-built RV64 decoder+executor (rv64_exec.c), not real hardware or
# QEMU (neither available in this environment; see timi_riscv_verify.c's
# top comment and AeroSLS-TIMI-ISA-v0.1.md §12). Checks the result against
# each program's "Expected result:" comment, exactly like run_native_tests.sh
# does for the x86 target — same .tmo files, same expected values, proving
# the SAME unmodified TIMI object retargets correctly to a second ISA.
#
# mem_ops.timi is skipped for the same reason run_native_tests.sh skips it:
# address-0 is a Phase 1 interpreter-only convenience, not a legitimate
# pointer under either native target's r7 scratch-pointer convention.
set -u
cd "$(dirname "$0")"
ASM=../timi-asm
VERIFY=../timi-riscv-verify

pass=0
fail=0
skip=0

for src in *.timi; do
    name="${src%.timi}"
    if [ "$name" = "mem_ops" ]; then
        echo "SKIP  $name (address-0 pointer is a Phase 1 interpreter-only convenience; see mem_ops_native)"
        skip=$((skip+1))
        continue
    fi

    expected=$(grep -oE 'Expected result: -?[0-9]+' "$src" | grep -oE -- '-?[0-9]+$')
    if [ -z "$expected" ]; then
        echo "SKIP  $name (no 'Expected result:' comment)"
        skip=$((skip+1))
        continue
    fi

    if ! "$ASM" "$src" "$name.tmo" 2>"$name.asm.log"; then
        echo "FAIL  $name (assembler error)"
        cat "$name.asm.log"
        fail=$((fail+1))
        continue
    fi

    if "$VERIFY" "$name.tmo" main "$expected"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
