#!/usr/bin/env bash
# M1 code-size gate (AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §6 as-built).
#
# M1 upgraded simi_arm.c from M0's naive load-operate-store codegen to a
# tiny three-register (x9/x10/x11) cache that keeps arithmetic chains
# resident. The plan's rule — "each gate is a measurement, not an opinion"
# — means the gate must measure emitted bytes against something real, so
# the M0_BASELINES below are the ACTUAL byte counts the committed M0
# translator (git 1729f50) emitted for each .tmo, measured by building
# that exact file and running this same harness. A future change that
# grows a test past its M0 baseline fails here — the allocator must never
# be a net regression on the corpus that defines parity.
#
# The gate deliberately measures the SAME parity set that run_arm_tests.sh
# runs, plus the M1/M2 tests that have no M0 twin in the shared corpus:
# rd_star (destination aliases rb — the spill is mandatory) and
# dead_reuse (destination resident but not an operand — the spill must be
# skipped; 10 forced occurrences across both clobber shapes = 40 bytes of
# the gate's savings, and reverting the rd_live hint grows the test by
# exactly that), plus the M2 JMPR pair: jmpr_mid (constant index folds to
# a direct branch and keeps the cache — 168 bytes below its M0 baseline)
# and jmpr_dyn (runtime index keeps the dynamic path, byte-identical to
# M0's naive codegen: 0 saved, the honest floor). jmpr_join is the
# fold-soundness pin: its index is set to different constants on two
# joining paths, so it must NOT fold (0 saved — and reverting the block-
# head constant reset makes it return 1 instead of 222, caught by the
# four-way parity). jmpr_calc is M2.1: its index is COMPUTED at translate
# time through all four folded ADD/SUB shapes (reg+reg, reg+imm, reg+reg,
# reg+reg, reg+imm) — 212 bytes below its M0 baseline, and disabling the
# ADD/SUB fold grows it back to exactly 1288. jmpr_mix is the M2.1 mixed
# case: one runtime JMPR (forces g_alloc=0, whole program naive) and one
# constant JMPR that still folds — 48 bytes below its M0 baseline, the
# folded-branch-under-naive-codegen interaction pinned. Same skips as the runner: float_ops rejected,
# mem_ops address-0 convenience, jmpr_oob self-skip — its UDF fault
# path has no expected result, so it is checked by hand with
# `./simi-arm-verify tests/jmpr_oob.tmo main 111` (expect rc=1 and
# "execution error" — see the plan doc's M2 addendum, §10).
# straight_line_bench is the
# interesting one: it is the register-pressure WORST case (six live
# values vs a 3-register pool, so everything spills) and shrinks only 4
# bytes — the big wins are the arithmetic-heavy programs (aggregate_abi
# -152, mem_ops_native -36). The gate is a ceiling, not a promise of a
# specific win: it asserts "never bigger than M0", and the printed table
# is the measurement the as-built note cites.
#
# Re-measuring a baseline (after editing a tests/*.simi corpus file):
#   git worktree add /tmp/m0 <M0 commit, e.g. 1729f50>
#   (cd /tmp/m0/tools/simi && make simi-asm simi-arm-verify)
#   git -C <repo> show <M0 commit>:tests/<name>.simi > /tmp/m0/tests/<name>.simi
#   (cd /tmp/m0/tools/simi && ./simi-arm-verify tests/<name>.tmo main <exp> \
#      | grep -oE '[0-9]+ bytes')
# then update the M0_BASELINES entry with the printed byte count.
set -u
cd "$(dirname "$0")"
ASM=../simi-asm
VERIFY=../simi-arm-verify

# name: M0 bytes (from git 1729f50, built + measured 2026-08-07)
declare -A M0_BASELINES=(
    [add]=976 [aggregate_abi]=4592 [branch_cmp]=1096 [call_ret]=1932
    [cap_call_ret]=3192 [cap_forge]=1336 [dead_reuse]=1188 [extra_ops]=1140
    [jmpr_basic]=1088 [jmpr_calc]=1288 [jmpr_dyn]=1136 [jmpr_join]=1144
    [jmpr_mid]=1200 [jmpr_mix]=1300 [loadi64]=968
    [loop_sum]=1040 [mem_ops_native]=1048 [obj_ops]=1164 [ptr_ops]=1120
    [rd_star]=1108 [rv64_boot_smoke]=928 [straight_line_bench]=1104
)

pass=0
fail=0
tot_m0=0
tot_m1=0
for name in $(printf '%s\n' "${!M0_BASELINES[@]}" | sort); do
    src="$name.simi"
    exp=$(grep -oE 'Expected result: -?[0-9]+' "$src" | grep -oE -- '-?[0-9]+$')
    if [ -z "$exp" ]; then
        echo "FAIL  $name (no 'Expected result:' comment — can't verify size honestly)"
        fail=$((fail+1))
        continue
    fi
    # Assemble on the spot: the .tmo files are gitignored build outputs that
    # `make clean` removes, so the gate must not assume they already exist
    # (a fresh checkout would otherwise fail every test with a missing file).
    if ! "$ASM" "$src" "$name.tmo" >/dev/null 2>&1; then
        echo "FAIL  $name (assembler error — size gate assumes a passing program)"
        fail=$((fail+1))
        continue
    fi
    out=$("$VERIFY" "$name.tmo" main "$exp" 2>/dev/null)
    if [ $? -ne 0 ]; then
        echo "FAIL  $name (verify failed — size gate assumes a passing program)"
        fail=$((fail+1))
        continue
    fi
    m1=$(echo "$out" | grep -oE '[0-9]+ bytes' | grep -oE '^[0-9]+')
    m0=${M0_BASELINES[$name]}
    tot_m0=$((tot_m0 + m0))
    tot_m1=$((tot_m1 + m1))
    if [ "$m1" -le "$m0" ]; then
        printf "PASS  %-22s M0=%5s M1=%5s (%-4s)\n" "$name" "$m0" "$m1" "$((m0 - m1))"
        pass=$((pass+1))
    else
        printf "FAIL  %-22s M0=%5s M1=%5s (grew %s bytes)\n" "$name" "$m0" "$m1" "$((m1 - m0))"
        fail=$((fail+1))
    fi
done

echo ""
echo "totals: M0=$tot_m0 M1=$tot_m1 saved=$((tot_m0 - tot_m1))"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
