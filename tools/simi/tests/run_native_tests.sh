#!/usr/bin/env bash
# Phase 3 verification: assembles every tests/*.simi (Phase 1 host toolchain,
# unchanged), translates the resulting .tmo to real x86-64 machine code
# (simi_x86.c — the same file destined for kernel/simi_translate.c), and
# *executes it on the host CPU* via simi-jit-test, checking the result
# against each program's "Expected result:" comment.
#
# mem_ops.simi is deliberately skipped here: it uses address 0 as a base
# pointer, which is a valid offset into the Phase 1 interpreter's simulated
# memory array but not a legitimate address in a real process — see
# simi_x86.h. mem_ops_native.simi is the Phase-3-appropriate equivalent
# (uses the r7 scratch-pointer convention) and is included below.
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
JIT=../simi-jit-test

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

    if "$JIT" "$name.tmo" main "$expected"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
