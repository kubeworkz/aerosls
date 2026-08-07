#!/usr/bin/env bash
# M0 verification (AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md): assembles every
# tests/*.simi (Phase 1 host toolchain, unchanged), translates the
# resulting .tmo to AArch64 (A64) machine code (simi_arm.c), and executes
# it via simi-arm-verify — a small purpose-built A64 decoder+executor
# (a64_exec.c), not real hardware or QEMU (neither available in this
# environment; see simi_arm_verify.c's top comment and the plan doc §2).
# Checks the result against each program's "Expected result:" comment,
# exactly like run_riscv_tests.sh does for the RV64 target — the SAME
# .tmo files and SAME expected values, proving the same unmodified SIMI
# object retargets correctly to a third ISA.
#
# mem_ops.simi and float_ops.simi are skipped for the same reasons
# run_riscv_tests.sh skips them: address-0 is a Phase 1 interpreter-only
# convenience, and float codegen is scoped out of M0 (simi_arm.c is
# expected to reject every float-typed instruction with
# TX_AR_ERR_FLOAT_UNSUPPORTED, not produce a wrong numeric answer — see
# tests/float_ops.simi's own top comment and the ISA doc §16 Phase 10).
# jmpr_oob.simi self-skips via the "no Expected result:" rule below (its
# whole point is that execution must NOT produce one — the translator
# lands on UDF #0, which a64_exec.c reports as a fault).
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
VERIFY=../simi-arm-verify

pass=0
fail=0
skip=0

for src in *.simi; do
    name="${src%.simi}"
    if [ "$name" = "mem_ops" ]; then
        echo "SKIP  $name (address-0 pointer is a Phase 1 interpreter-only convenience; see mem_ops_native)"
        skip=$((skip+1))
        continue
    fi
    if [ "$name" = "float_ops" ]; then
        echo "SKIP  $name (A64 float codegen scoped out of M0; see simi_arm.h's TX_AR_ERR_FLOAT_UNSUPPORTED)"
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
