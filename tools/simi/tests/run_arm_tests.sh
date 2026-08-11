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
    # Phase 15 (shared-memory atomics): the A0 skip block for
    # cas_simple/atomic_add was removed at A3 — the A64 translator emits
    # ldaxr/stlxr loops now, so both fixtures RUN here (cas_simple = 5,
    # atomic_add = 2, through simi-arm-verify).
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

    # M2.75: --steps asserts the fixture's committed executed-instruction
    # count (bench_baselines.h — the same table bench-exec gates on) right
    # here in the parity harness, catching decode/emission regressions
    # before any bench runs. Deterministic and machine-independent.
    # arm64_boot_smoke is the exception (M5.2, §10.196): the kernel's
    # embedded program is a deliberately LONG-RUNNING 1e8-iteration loop
    # (~1e9 executed steps — the contention probe's EL0 window), so its
    # step count is not a parity signal, it has no committed baseline row
    # (bench-exec skips it — a row no bench maintains would be a lie),
    # and the run needs the raised budget. The kernel boot itself is the
    # real gate for this program; here we assert the result (42) only.
    if [ "$name" = arm64_boot_smoke ]; then
        "$VERIFY" "$name.tmo" main "$expected" --max-steps 2000000000
    elif [ "$name" = lcg_slice ]; then
        # §10.200: kernel-embedded LCG fixture (the arm64 kernel boot is
        # its real gate — translate + execute + serial assert of
        # 0x0b6f2a40). It still RUNS here (its ~4.4M A64 steps fit the
        # tight 10M budget — no raised budget needed) but without the
        # --steps baseline check: it has no committed row, the
        # arm64_boot_smoke model ("a row no bench maintains would be a
        # lie").
        "$VERIFY" "$name.tmo" main "$expected"
    elif [ "$name" = mem_touch ]; then
        # §10.202: kernel-embedded EL0 mem fixture (the arm64 kernel's
        # EL0 excursion is its real gate — translate with scratch =
        # USER_SCRATCH_VA + execute + serial assert of 0x0d15ea5e). It
        # still RUNS here (a few hundred A64 steps) but without the
        # --steps baseline check: no committed row, the arm64_boot_smoke
        # model.
        "$VERIFY" "$name.tmo" main "$expected"
    else
        "$VERIFY" "$name.tmo" main "$expected" --steps
    fi
    if [ $? -eq 0 ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
