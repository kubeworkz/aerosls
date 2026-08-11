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
    if [ "$name" = "lcg_slice" ]; then
        # §10.200: the per-slice LCG fixture is kernel-embedded (the
        # arm64 kernel's SIMI smoke carries its real gate — translate +
        # execute + serial assert of 0x0b6f2a40), NOT a parity-corpus
        # fixture (the arm64_boot_smoke model, plan doc §10.196). Its
        # host parity shape is covered by lcg_fairness.simi (same loop,
        # committed with step baselines in the corpus).
        echo "SKIP  $name (kernel-embedded LCG fixture — the arm64 kernel boot is its gate, plan doc §10.200)"
        skip=$((skip+1))
        continue
    fi
    if [ "$name" = "mem_ops" ]; then
        echo "SKIP  $name (address-0 pointer is a Phase 1 interpreter-only convenience; see mem_ops_native)"
        skip=$((skip+1))
        continue
    fi
    if [ "$name" = "arm64_boot_smoke" ]; then
        # M5.2 (plan doc §10.196): the kernel-embedded boot fixture is a
        # DELIBERATELY long-running loop (1e8 iterations, so a tick can
        # fire inside each EL0 excursion on the real kernel). Its emitted
        # size has no committed --bytes baseline (it is kernel-embedded,
        # not a parity-corpus fixture) — the same reason bench_corpus.c
        # SKIPs it, the ARM size gate raises the budget for this one row,
        # and the RV64 runner skips it. Its native behavior is covered by
        # the arm64 kernel boot smoke and by rv64_boot_smoke's row.
        echo "SKIP  $name (kernel-embedded boot fixture, 1e8-loop — outside the parity corpus, plan doc §10.196)"
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

    # M2.76: --bytes asserts the fixture's committed emitted-byte count
    # (bench_baselines_x86.h — the deterministic emission tripwire for the
    # native leg; the sandbox has no vPMU, so dynamic steps are unmeasurable
    # here — see bench_baselines_x86.h's top comment).
    if "$JIT" "$name.tmo" main "$expected" --bytes; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
