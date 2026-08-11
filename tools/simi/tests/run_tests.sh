#!/usr/bin/env bash
# Assembles + runs every tests/*.simi, checks output against the
# "Expected result: N" comment in each source file, round-trips through
# the disassembler (just checks it doesn't crash and re-assembles-equivalent
# instruction count).
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
RUN=../simi-run
DIS=../simi-dis

pass=0
fail=0

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
        continue
    fi
    if [ "$name" = "mem_touch" ]; then
        # §10.202: the mem-touch fixture is kernel-embedded (the arm64
        # kernel's EL0 excursion carries its real gate — translate with
        # scratch = USER_SCRATCH_VA + execute + serial assert of
        # 0x0d15ea5e). It is EL0-only: its baked scratch is a user VA
        # the kernel's TTBR1 tables do not map, so it has no host-parity
        # shape at all — the ARM runner still executes it (a few hundred
        # A64 steps) and the size gate pins its emission.
        echo "SKIP  $name (kernel-embedded EL0 mem fixture — the arm64 kernel EL0 excursion is its gate, plan doc §10.202)"
        continue
    fi
    if [ "$name" = "arm64_boot_smoke" ]; then
        # M5.2 (plan doc §10.196): the kernel-embedded boot fixture is a
        # DELIBERATELY long-running loop (1e8 iterations, so a tick can
        # fire inside each EL0 excursion on the real kernel). Its step
        # count dwarfs every parity-corpus row, so the interpreter's
        # MAX_STEPS budget trips on it (a step-limit exit) — the same
        # reason bench_corpus.c SKIPs it, the ARM size gate raises the
        # budget for this one row, and the RV64 runner skips it. It is
        # kernel-embedded, not a parity-corpus fixture; its interpreter
        # behavior is covered by the same program's kernel boot (the
        # arm64 kernel smoke) and its parity shape by rv64_boot_smoke.
        echo "SKIP  $name (kernel-embedded boot fixture, 1e8-loop — outside the parity corpus, plan doc §10.196)"
        continue
    fi
    expected=$(grep -oE 'Expected result: -?[0-9]+' "$src" | grep -oE -- '-?[0-9]+$')
    if [ -z "$expected" ]; then
        echo "SKIP  $name (no 'Expected result:' comment)"
        continue
    fi

    if ! "$ASM" "$src" "$name.tmo" 2>"$name.asm.log"; then
        echo "FAIL  $name (assembler error)"
        cat "$name.asm.log"
        fail=$((fail+1))
        continue
    fi

    # M2.81: --steps asserts the fixture's committed SIMI-instruction
    # count (bench_baselines_interp.h) after the run — the interpreter-leg
    # tripwire mirroring the ARM/RV64 --steps checks (M2.75/M2.76). A
    # decode/emission/interpreter regression that moves the count fails
    # the row here, before any bench runs.
    actual=$("$RUN" --steps "$name.tmo" main 2>"$name.run.log")
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL  $name (interpreter exited $rc)"
        cat "$name.run.log"
        fail=$((fail+1))
        continue
    fi

    if [ "$actual" = "$expected" ]; then
        echo "PASS  $name = $actual"
        pass=$((pass+1))
    else
        echo "FAIL  $name (expected $expected, got $actual)"
        fail=$((fail+1))
    fi

    "$DIS" "$name.tmo" > "$name.dis.txt" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "FAIL  $name (disassembler crashed)"
        fail=$((fail+1))
    fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
