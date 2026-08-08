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
# mem_ops.simi is skipped for the same reason run_riscv_tests.sh skips
# it: address-0 is a Phase 1 interpreter-only convenience (see
# mem_ops_native). float_ops.simi RAN starting at F3: simi_arm.c's F1
# float codegen (GP-bounce ADD/SUB/MUL/DIV/NEG/CMP, D5 swapped-operand
# fcmp+cset) emits real IEEE-754 words and the A64 executor decodes and
# executes them (F0), so the ARM engine now joins interp/x86 in the
# float four-way parity — the same .tmo, the same Expected result: 15.
# (RV64 still skips it: float is scoped out of Phase 10 v1 there, an
# explicit rejection, per the ISA doc §16 and run_riscv_tests.sh.)
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
    # Phase 15 (shared-memory atomics) A0: CAS/ATOMIC_ADD landed in the
    # ISA + interpreter; the A64 translator gets them at A3 (ldaxr/stlxr
    # loops). Until then its BAD_OPCODE rejection looks like a FAIL.
    if [ "$name" = "cas_simple" ] || [ "$name" = "atomic_add" ]; then
        echo "SKIP  $name (Phase 15 atomics: interpreter-only until A3; see plan doc Part II §7)"
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
