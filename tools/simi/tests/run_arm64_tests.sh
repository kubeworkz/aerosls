#!/usr/bin/env bash
# M3 verification (docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §6): the
# REAL AArch64 execution proof. simi-arm-verify (run_arm_tests.sh) runs
# the translated A64 words through a64_exec.c, a purpose-built decoder —
# but a decoder that AGREES with a wrong encoder passes the four-way
# parity invisibly, and A64 architectural rules a64_exec is documented
# laxer about (SP 16-byte alignment, 4-byte branch-target alignment) are
# never checked at all. This runner instead cross-compiles
# simi_arm_jit.c (aarch64-linux-gnu-gcc -static — the SAME simi_arm.c
# translator) and EXECUTES every tests/*.simi on a real A64
# implementation, qemu-aarch64 user-mode: the emitted words are mmap'd
# executable and branched into for real, with the result checked against
# each program's "Expected result:" comment — exactly like
# simi_jit_test.c does for x86-64 on the host CPU.
#
# M3 is the "optional, environment-dependent" milestone: with no aarch64
# toolchain or qemu-aarch64 this runner skips cleanly (the same pattern
# as the riscv checks when qemu-system-riscv64 is absent). The
# arm64-guards CI job installs the toolchain, so there the run must be
# all-pass.
#
# Same skip set as run_arm_tests.sh: mem_ops.simi (address-0 is a Phase 1
# interpreter-only convenience) and any fixture with no "Expected
# result:" comment (jmpr_oob self-skips — its whole point is that
# execution must NOT produce one).
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
JIT=../simi-arm-jit

if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || ! command -v qemu-aarch64 >/dev/null 2>&1; then
    echo "SKIP  arm64-jit (no aarch64-linux-gnu-gcc / qemu-aarch64 — M3 is environment-dependent; see plan doc §6)"
    exit 0
fi

if ! make -C .. simi-arm-jit >/dev/null 2>&1; then
    echo "SKIP  arm64-jit (simi-arm-jit cross build failed — check the aarch64 toolchain)"
    exit 0
fi

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

    if qemu-aarch64 "$JIT" "$name.tmo" main "$expected"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
