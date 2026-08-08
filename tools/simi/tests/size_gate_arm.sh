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
# and jmpr_dyn (runtime index keeps the dynamic path — the index is a
# value read back from guest memory, since M2.3 added XOR to the fold
# set and the original r1 = 5 ^ r0 silently folded, stopping this test
# from exercising the dynamic path at all; the LOAD restores it. The
# dynamic JMPR emission is byte-identical to M0's; the 16 bytes below
# M0 come from the zero-displacement STORE/LOAD folds, which apply to
# every program regardless of g_alloc). jmpr_join is the
# fold-soundness pin: its index is set to different constants on two
# joining paths, so it must NOT fold (0 saved — and reverting the block-
# head constant reset makes it return 1 instead of 222, caught by the
# four-way parity). jmpr_calc is M2.1: its index is COMPUTED at translate
# time through all four folded ADD/SUB shapes (reg+reg, reg+imm, reg+reg,
# reg+reg, reg+imm) — 224 bytes below its M0 baseline after M2.6's imm
# fold compounds 12 more on M2.1's 212, and disabling the ADD/SUB index
# fold grows it back to exactly 1288. jmpr_mix is the M2.1 mixed
# case: one runtime JMPR (forces g_alloc=0, whole program naive — the
# index is a LOAD result for the same M2.3 reason as jmpr_dyn) and one
# constant JMPR that still folds — 64 bytes below its M0 baseline, the
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
# r7 scratch conventions — 132 bytes below its M0 baseline (M2.11's
# register-offset fold turns the [r5-4101] store and load into
# li64 + one register-offset word each, two more words saved), and
# disabling the unscaled fold grows it back to 1200. mem_reg is M2.11:
# the register-offset load/store — a displacement past the imm12 range
# (|disp| > 4095, not a multiple of 4096) now lives in a register and
# the access is ONE ldr/str xt, [xb, xm] word instead of the M2.8
# li64 + add_shift + zero-offset access, and consecutive LOAD/STORE
# sharing the SAME materialize-class displacement form a RUN: the
# displacement is materialized once into x12 (X_DR, outside the
# x9/x10/x11 cache) and every access in the run is just the one
# register-offset word. Rows: imm9 [r4-100] and imm12 [r5-4096]
# controls (unchanged — runs are only formed for materialize-class
# displacements), a standalone [r5-5250] pair separated by an ADD into
# a free register (each pays its own per-instruction fold), i64 runs of
# 5 at [r5-5000] and 3 at [r6-7000], and an i32 run of 2 at [r6-7100]
# (the narrow forms MUST use SXTW — LSL #0 would zero-extend Wm and
# turn -7100 into a huge positive offset; that row pins it) — 324
# bytes below its M0 baseline, and disabling the run-reuse (run
# threshold 2 -> 9999) grows it back to 1580. jmpr_fall is M2.12:
# BLOCK COALESCING — the JMPR's index folds to pc+1, the VERY NEXT
# instruction, so the fold's `b +0` is a dead branch (control falls
# through to the target anyway) and the translator drops it entirely.
# Since the target has no other incoming edge (no BR/BC/CALL, no other
# folded JMPR, no entry), the two blocks FUSE: the block-head flush at
# the boundary is skipped and the x9/x10/x11 cache survives the join,
# so the register chain r0/r1/r2 built before the JMPR stays resident
# into the target block (the register frame is loaded once, not once
# per block). A dead-path section pins the NON-FUSED half: a fall-
# through fold whose target (pc 11) has a SECOND incoming edge — a
# backward fold at pc 13 also targets it — so the blocks do NOT fuse;
# the branch is still dropped but the block-head flush stays. — 312
# bytes below its M0 baseline, and disabling the coalescing (fold-
# target check pc+1 -> pc+9999) grows it back to 1080, so the
# fusion itself is worth 16 of the 312. jmpr_fall2 is M2.13:
# CASCADING fall-through folds — three JMPRs in a row each folding to
# their OWN next pc, with all three index constants loaded BEFORE the
# chain. Under M2.12's merge this chain collapses: the first fold's
# rule-1 mark on its target resets the constant map at the second
# JMPR's index, un-folding it (a JMPR that fails to fold is DYNAMIC,
# which kills g_alloc for the whole function — the rest fall back to
# the runtime table + bounds check). M2.13's fix: the fold fixpoint's
# rule-1 merge skips fall-through targets (target == pc+1 — its branch
# is the dead one being dropped), so the constant map flows through
# all three joins, every JMPR folds, and the coalescing pre-pass fuses
# all three boundaries: no branch, no flush, x9/x10/x11 resident
# across the whole chain. — 248 bytes below its M0 baseline (1228);
# the old mark-all merge measures 1180 (the cascade alone is worth
# 200 of the 248), and disabling the coalescing grows it back to 996
# (fusion worth 16). tail_ret is M2.14: TAIL REUSE — every RET in a
# function emits an identical 7-word return tail (ldr x9 slot0; ldrb
# x10 tag0; pop frame; restore x30/x29; br x30), cache-independent (the
# RET's flush emptied x9/x10/x11 first) and pc-independent, so N RETs
# share ONE copy: the first RET emits it in place, the rest emit just
# `cache_flush; b tail`. A BC dispatch (no JMPR) pins that tail reuse is
# independent of the JMPR folding: 3 RETs -> 1 copy -> 48 bytes below
# its M0 baseline (2 sharing RETs x 24 bytes; disabling the sharing
# grows it back to exactly 1048 = M0, still correct). epi_merge is
# M2.15: EPILOGUE/PROLOGUE MERGING across a single-edge BACKWARD fold
# (the last §10.29 lever). A folded JMPR to an EARLIER pc (pc 9 -> pc 4
# here) emits cache_flush + b; the target block's head reloads its
# operands from the frame (rule 1 gives it an empty cache) — and the
# source block's tail (pc 8) RELOADS THE SAME r2, r1 in the same order a
# few words earlier. The fold's flush stores only the directory's
# resident RESULTS, so the tail's fetched values stay in x9/x10 across
# the branch: the head's matching reloads re-read the same slot values
# into the same hosts — dead code, and the merge drops them (the frame
# is loaded once, at the tail, not once per block). Soundness requires
# the head to be reached ONLY via the fold (no other edge's x9/x10 state
# is known): pc 4 has no BR/BC/CALL, no other JMPR, no entry, and pc 3
# BR jumps INTO the fold source (pc 6) rather than falling through — no
# fall-through edge into the head. The test is LIVE-executed: the live
# path runs pc 0-2, BRs to pc 6, flows tail -> fold -> head, and the
# head's pattern writes r0 = r2 + r1 = 7, so the expected value flows
# THROUGH the dropped reads — the ARM engine executes them and any wrong
# drop makes its result differ from the three interpreters
# (self-validating, not a dead-path shape verified only by dump). The
# layout is rigid (pattern@T, RET@T+1, two LOADIs@T+2/T+3,
# matching tail@T+4, fold@T+5) and the pre-T results (r2, r1) are
# pairwise distinct, so the cache cursor at the tail is the result count
# mod 3 (= 2), the tail's result lands in x11, and BOTH head fetches
# drop — 136 bytes below its M0 baseline, and disabling the merge (the
# fetch-drop hook) grows it back to exactly 1012 (the 8 bytes are the
# two dropped reload words), still correct. epi_merge2 is the
# ASYMMETRIC half: three distinct live results (r2, r1, r6) make the
# result count mod 3 = 0, so the tail's result lands in x9 and CLOBBERS
# the r2 transient — only the r1 fetch survives the fold's flush, the
# head drops ONLY its second fetch (flags = 2), and its first reload of
# r2 stays (reading the unchanged slot — the live flush stored r2 = 3).
# A bug that drops both fetches reads x9 = the tail's result (7) as r2
# and computes 11 instead of 7 — the four-way parity fails on its own,
# pinning the flags formula's asymmetric branch — 140 bytes below its
# M0 baseline, and disabling the merge grows it back to exactly 1024
# (the one kept fetch, 4 bytes), still correct. epi_merge3 is the
# M2.15 follow-up that closes the cursor-class ledger: FOUR distinct
# live results (r2, r1, r6, r7) make the result count mod 3 = 1, so
# the tail's result lands in x10 and CLOBBERS the g2 (r1) transient —
# the mirror of epi_merge2. Only the g1 (r2) fetch survives the fold's
# flush (x9), so the head drops ONLY its FIRST fetch (flags = 1) and
# its second reload of r1 stays (reading the unchanged slot — the live
# region's flush stored r1 = 4 at pc 5's BR). A bug that computes
# flags = 2 or 3 for this shape reads x10 = the tail's result (7) as
# r1 and computes 10 instead of 7 — the four-way parity fails on its
# own, pinning the flags formula's third branch — 148 bytes below its
# M0 baseline, and disabling the merge grows it back to exactly 1036
# (the one kept fetch, 4 bytes), still correct. epi_merge4 is
# M2.17: the epi-merge dead region with a COMPUTED fold index — the
# §10.37 emission-order constraint relaxed. The M2.15 pass required
# both dead-region intermediates to be LOADI/LOADI64; M2.17 widens
# them to any plain result-op (ar_is_interm_op) because the merge
# invariants depend only on the CLAIM COUNT (two fresh claims net +3
# ≡ 0 mod 3, so the tail's result host stays cnt%3) and the
# destinations being distinct and not the pattern's sources — NOT on
# what computes the values. Here LOADI r4 (seed) then ADD r5, r4, #3
# computes the fold index r5 = 4 (the M2.1 constant analysis folds
# the chain, which sits entirely inside the dead region after the
# BR-target reset at T+2), the JMPR folds BACKWARD to T, and the
# tail's fetches evict the intermediate results (standard clobber)
# before establishing the r2/r1 transients — flags = 3, both head
# fetches drop, 144 bytes below its M0 baseline. The teeth: reverting
# T+3 to LOADI-only rejects the shape (the merge does not fire, the
# head reloads both sources) — measured at exactly 1012, +8, still
# correct. A post-hoc fixup could not have done this: an emitted ldr
# word cannot be un-emitted, so the generalization stays a pre-pass
# decision (plan doc §10.42). epi_merge5 is
# M2.17-follow-up: the COMPUTED fold index in the flags=2 cursor
# class — the epi_merge2 mirror (three live results r2/r1/r6 → count
# mod 3 = 0 → the tail's result lands in x9, clobbering the r2
# transient, so the head keeps its r2 reload and drops ONLY the r1
# fetch), but with the fold index COMPUTED by an arithmetic chain
# (seed LOADI r4, #2 then ADD r5, r4, #3 → r5 = 5 = T) instead of a
# plain LOADI — the M2.17 widening at work in a second cursor class,
# closing the §10.43 flags=1/2-with-computed pin. It also records a
# STRUCTURAL CEILING: both intermediates cannot be computed, because
# T+2 is necessarily the fold-source BR target — a block head where
# the fold fixpoint resets every constant — so a computed T+2 has no
# known source, the JMPR goes dynamic, and g_alloc dies (probe_2c
# proved it: the whole function emits the naive path). The seed at
# T+2 must be a LOADI; at most one dead-region intermediate can be
# computed (plan doc §10.44). 148 bytes below its M0 baseline, and
# disabling the merge grows it back to exactly 1024 (the one kept
# fetch, 4 bytes) — the exact mirror of epi_merge2's teeth, still
# correct. epi_merge6 is
# M2.18: the TWO-COMPUTED epi-merge dead region — the §10.44 structural
# ceiling, broken. Both dead-region intermediates are computed: a
# DEPENDENT chain ADD r4, r2, #2 then SUB r5, r4, #1 computes the fold
# index r5 = (3 + 2) - 1 = 4 = T instead of a LOADI seed. The M2.17
# widening already accepted the ops; what blocked the shape was the
# constant fixpoint — T+2 is the fold-source BR target (a block head),
# so the reset there killed the seed r2 = 3, the chain broke, the JMPR
# went dynamic, and g_alloc died (probe_2c measured the whole function
# falling back to the naive path). M2.18's targeted fixpoint
# relaxation: a head pc whose ONLY incoming path is a single forward
# BR/BC (X-1 is a terminal, so the fall-through is dead; not an entry;
# not a fold target) is NOT a join, so the scan restores the constant
# map as it was at that branch instead of resetting. The fold-target
# exclusion is per-scan (a fold edge is another incoming path) and a
# hard safety cap falls back to the un-relaxed fixpoint if a
# relaxation-enabled backward fold ever 2-cycles the fixpoint. The
# epi-merge invariants are untouched (claim count still three → tail
# result host x11, both head fetches drop): the head is a bare add, the
# dead region computes the index, the fold's flush leaves the r2/r1
# transients alive — 148 bytes below its M0 baseline, and disabling
# the relaxation grows it back to exactly 1156 (the JMPR dynamic, the
# whole function naive), still correct. jmpr_cross is
# M2.16: the §10.30-era FIXPOINT-vs-COALESCING interaction, pinned. A
# folded JMPR to the very next pc (fold A, pc 3 -> pc 4) is a dead
# branch the coalescing pass DROPS and FUSES (pc 4 has no other
# incoming edge) — but pc 4 sits INSIDE another JMPR's index chain: r2
# (fold B's index) is loaded at pc 1, BEFORE pc 4, and read at pc 5.
# Under M2.12's mark-all merge, the rule-1 mark on pc 4 reset the
# constant map at the fused boundary, un-folding B — and a JMPR that
# fails to fold is DYNAMIC, which kills g_alloc for the whole function.
# M2.13's rule-1 skip (fall-through fold targets are never marked by
# the merge — the coalescing pass is the authority on their mark) lets
# the chain flow through the fused boundary: r2 survives from pc 1
# across pc 4 to B, B folds to pc 7, and g_alloc stays 1 — 220 bytes
# below its M0 baseline, and reverting the skip to mark-all grows it
# back to exactly 1164 (B dynamic, g_alloc=0, runtime table + bounds
# check return), still correct. The same teeth move jmpr_fall2 by its
# exact M2.13 measurement (+200) — the two shapes the skip protects.
# The pass-order design note is plan doc §10.38: a post-coalescing
# re-run of the fold analysis is byte-neutral (the coalescing pass
# changes no fold-relevant mark — verified by instrumentation across
# the whole corpus), so the fixpoint-vs-coalescing frontier is closed
# by construction, not by ordering. mem_pre is M2.10:
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
    [jmpr_cross]=1212 [jmpr_dyn]=1148 [jmpr_fall]=1376 [jmpr_fall2]=1228 [jmpr_join]=1144 [jmpr_mid]=1200 [jmpr_mix]=1312
    [epi_merge]=1140 [epi_merge2]=1160 [epi_merge3]=1180 [epi_merge4]=1148 [epi_merge5]=1168 [epi_merge6]=1156
    [loadi64]=968
    [loop_sum]=1040 [mem_neg]=1308 [mem_ops_native]=1048 [mem_pre]=2012
    [mem_reg]=1764 [obj_ops]=1164 [ptr_ops]=1120
    [rd_star]=1108 [rv64_boot_smoke]=928 [src_resident]=1120
    [straight_line_bench]=1104 [tail_ret]=1048
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
