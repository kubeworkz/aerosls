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
# reg+reg, reg+imm) — 224 bytes below its M0 baseline after M2.6's imm
# fold compounds 12 more on M2.1's 212, and disabling the ADD/SUB index
# fold grows it back to exactly 1288. jmpr_mix is the M2.1 mixed
# case: one runtime JMPR (forces g_alloc=0, whole program naive) and one
# constant JMPR that still folds — 48 bytes below its M0 baseline, the
# folded-branch-under-naive-codegen interaction pinned. jmpr_calc_mul is
# M2.2: its index is COMPUTED through a MULTIPLY chain (MUL reg+reg,
# MUL reg+imm, SUB reg+imm) — 212 bytes below its M0 baseline after
# M2.6's imm fold compounds 4 more on M2.2's 208, and disabling the MUL
# fold grows it back to exactly 1272. jmpr_calc_bit is
# M2.3: its index is COMPUTED through the BITWISE + SHIFT folds (OR/SAR/
# SHR/SHL/AND/XOR/ADD, with the shifts masking their amount mod 64 and
# SAR arithmetic on a sign-bit value) — 252 bytes below its M0 baseline
# after M2.6's imm fold compounds 4 more on M2.3's 248, and disabling
# the fold grows it back to exactly 1384. src_resident is
# M2.4: a chain that re-reads r1/r2 as sources five times; cache_reserve
# now prefers a non-source victim, so the sources stay resident — 40
# bytes below its M0 baseline, and reverting the source preference grows
# it back to 1108 (28 over the hint). fetch_cross is M2.5: a chain that
# alternates operand order so ra@slot1 and rb@slot0 are CROSSED, the
# shape where the default fetch targets (ra->x9, rb->x10) spill both
# operands and reload one — cache_fetch_hosts swaps the targets so each
# fetch is a no-op into its own slot — 104 bytes below its M0 baseline,
# and reverting the swap grows it back to 1160 (the M2.4 state). alu_imm
# is M2.6: an ADD/SUB #imm chain whose small non-negative immediates fold
# into A64's 12-bit add-imm/sub-imm forms (one word each instead of the
# movz+add materialization) — 72 bytes below its M0 baseline after M2.7
# also folds its 4096 row (add #1, lsl #12) and its #-5 row (negative
# SUB inverts to add #5), and disabling the fold grows it back to 1100
# (the full materialize state). alu_imm_ext is M2.7: the NEGATIVE and
# SHIFTED families — ADD #-7 emits sub #7, SUB #-12 emits add #12, and
# magnitudes that are multiples of 4096 (4096, 8192, the 4095<<12 max,
# and #-4096) emit add/sub #u, lsl #12 — 100 bytes below its M0
# baseline; its 16777216 (past 0xFFFFFF) and #-200000 (not a multiple of
# 4096) rows keep the materialized fallback exercised. mem_neg is M2.8:
# LOAD/STORE displacement address math — a negative displacement
# previously fell to the movz(+movk)+add_shift materialization even at
# |disp| <= 4095 (A64's scaled load/store immediate is unsigned), now
# |disp| folds into a single add/sub-imm (plain, or shifted for
# multiples of 4096): rows at [r6-8] (M2.10 unscaled stur_w/ldur_w #-8),
# [r5-4096] (sub #1, lsl #12), [r5-4101] (honest materialize — |disp| >
# 4095, not a multiple of 4096) and [r6+5] (positive but UNALIGNED for
# i32, so the scaled path can't take it — M2.10's unscaled form folds
# #5 in one word), with the i32 addresses kept >= 4 bytes apart and
# confined to the r7+[560, 4092] band portable across all four engines'
# r7 scratch conventions — 124 bytes below its M0 baseline, and
# disabling the unscaled fold grows it back to 1200. mem_pre is M2.10:
# ANY displacement that fits A64's signed 9-bit imm9 ([-256, 255])
# folds into a single unscaled ldr/str xt, [xb, #imm] word — one word,
# no writeback, no alignment requirement, superseding the M2.9
# pre-indexed form (which required both). Rows at [r5-16] and [r5-256]
# (the exact imm9 boundary), [r5-264] and [r6-258] (honest non-folds —
# magnitude past 255, still aligned so they take the M2.8 sub path),
# i32 at [r5-8]/[r5-4] plus the two rows ONLY the unscaled form can
# fold: [r5+5] and [r5-45], both UNALIGNED for i32 (the scaled fast
# path rejects +5 for alignment and M2.9's pre-indexed form rejected
# -45 too), i16 at [r6-6], u8 at [r6-1], the sign-extending i8 ldursb
# at [r6-9], and i64 at [r6-24], all round-tripping store->load and
# re-reading their bases (no writeback means the base's cache entry
# stays valid — the M2.9 clobber_scratch pin is gone) — 540 bytes
# below its M0 baseline, and disabling the unscaled fold grows it back
# to 1552.
# mem_ops_native is
# unchanged (its 0/8 displacements were already in the scaled fast
# path). Same skips as the runner: float_ops rejected,
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
    [add]=976 [aggregate_abi]=4592 [alu_imm]=1132 [alu_imm_ext]=1196
    [branch_cmp]=1096 [call_ret]=1932
    [cap_call_ret]=3192    [cap_forge]=1336 [dead_reuse]=1188 [extra_ops]=1140
    [fetch_cross]=1216
    [jmpr_basic]=1088 [jmpr_calc]=1288 [jmpr_calc_bit]=1384 [jmpr_calc_mul]=1272
    [jmpr_dyn]=1136 [jmpr_join]=1144 [jmpr_mid]=1200 [jmpr_mix]=1300
    [loadi64]=968
    [loop_sum]=1040 [mem_neg]=1308 [mem_ops_native]=1048 [mem_pre]=2012 [obj_ops]=1164 [ptr_ops]=1120
    [rd_star]=1108 [rv64_boot_smoke]=928 [src_resident]=1120
    [straight_line_bench]=1104
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
