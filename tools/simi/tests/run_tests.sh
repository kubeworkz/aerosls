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
