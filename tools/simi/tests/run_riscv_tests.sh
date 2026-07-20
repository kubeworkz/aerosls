#!/usr/bin/env bash
# Phase 5 verification: assembles every tests/*.simi (Phase 1 host toolchain,
# unchanged), translates the resulting .tmo to RV64 machine code
# (simi_riscv.c), and executes it via simi-riscv-verify — a small
# purpose-built RV64 decoder+executor (rv64_exec.c), not real hardware or
# QEMU (neither available in this environment; see simi_riscv_verify.c's
# top comment and AeroSLS-SIMI-ISA-v0.1.md §12). Checks the result against
# each program's "Expected result:" comment, exactly like run_native_tests.sh
# does for the x86 target — same .tmo files, same expected values, proving
# the SAME unmodified SIMI object retargets correctly to a second ISA.
#
# mem_ops.simi is skipped for the same reason run_native_tests.sh skips it:
# address-0 is a Phase 1 interpreter-only convenience, not a legitimate
# pointer under either native target's r7 scratch-pointer convention.
#
# float_ops.simi is skipped here (Gap Remediation SIMI Phase 10): RV64
# float codegen is explicitly scoped OUT of Phase 10 v1 (deferred to ride
# alongside Phase 9's RISC-V kernel wiring, itself not yet done either) --
# simi_riscv.c is expected to reject every float-typed instruction in this
# program with TX_RV_ERR_FLOAT_UNSUPPORTED, not produce a wrong numeric
# answer, which is exactly what a plain FAIL-on-mismatch check here can't
# distinguish from a real regression. See tests/float_ops.simi's own top
# comment and AeroSLS-SIMI-ISA-v0.1.md §16 Phase 10 for the full story.
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
VERIFY=../simi-riscv-verify

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
        echo "SKIP  $name (RV64 float codegen scoped out of Phase 10 v1; see simi_riscv.h's TX_RV_ERR_FLOAT_UNSUPPORTED)"
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
