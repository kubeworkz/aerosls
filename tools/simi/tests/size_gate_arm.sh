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
# a direct branch and keeps the cache — 208 bytes below its M0 baseline
# after M2.23's elimination drops its post-fold dead path)
# and jmpr_dyn (runtime index keeps the dynamic path — the index is a
# value read back from guest memory, since M2.3 added XOR to the fold
# set and the original r1 = 5 ^ r0 silently folded, stopping this test
# from exercising the dynamic path at all; the LOAD restores it. The
# dynamic JMPR emission is byte-identical to M0's; the 16 bytes below
# M0 come from the zero-displacement STORE/LOAD folds, which apply to
# every program regardless of g_alloc). M2.24 halves the dynamic
# path's table: entries are 32-bit byte offsets read with a single ldr
# W at base + (target << 2), and the per-JMPR base load is a 2-word
# movz+movk — jmpr_dyn 1132 -> 1088 (-44 = 9x4 table + 8 base).
# jmpr_table is
# M2.24, the compact JMPR table pinned at runtime: two DYNAMIC JMPRs
# (indices stored to and re-loaded from guest memory — never fold, so
# g_alloc = 0) dispatch SEQUENTIALLY through two distinct table
# entries in one run (table[7] -> block A, table[13] -> block B), each
# with a passing bounds check — the table's indexing, base load, and
# 32-bit entry width all exercised at runtime, with jmpr_oob pinning
# the same path's fault side. 108 bytes below its M0 baseline (the
# naive body + the 15x4 table + 2x8 base), and the M2.23-era 8-byte
# table measures 1312 (+76 = 15x4 + 16) — the teeth, isolating the
# width delta exactly. Under g_alloc=1 there is no table at all (every
# emitted JMPR folds — M2.23), so this is the naive-mode table only.
# jmpr_join is the
# fold-soundness pin: its index is set to different constants on two
# joining paths, so it must NOT fold (reverting the block-head constant
# reset makes it return 1 instead of 222, caught by the four-way
# parity). M2.25 turns its single dynamic dispatch into the inline
# compare-and-branch chain — the runtime index 8 is the FIRST candidate
# of {8, 9} — 108 bytes below its M0 baseline (the M2.24 table's 10x4 +
# 8 base and the bounds-check dispatch words are gone; only ld + 2x
# (cmp,b.eq) + UDF remain). jmpr_chain is
# M2.25's second-candidate pin: the index's provable set is {10, 12} —
# the union of the never-taken BC's branch-path value (10, delivered at
# the join) and the taken fall-through's (12) — and the runtime index 12
# takes the chain's SECOND b.eq, proving the chain dispatches past the
# first candidate. 140 bytes below its M0 baseline (the naive table and
# dispatch words are gone entirely — no table, no bounds-check indirect
# load). jmpr_oob's out-of-range constant 999 has no in-range candidate,
# so its chain collapses to a bare UDF — the same fault the table's
# bounds check produced. jmpr_chain2 is
# M2.26: the chain extended to an index BUILT BY ARITHMETIC — r1 = 2 +
# r2 with the join-dependent base r2 in {8, 10}, so the candidate set is
# the pair product {2} + {8, 10} = {10, 12} across the two tracked
# registers (ra + its feeder, per the closure), and the runtime index 12
# takes the chain's SECOND b.eq — jmpr_chain's second-candidate shape,
# now through a register ADD instead of a bare LOADI (M2.25's single-
# register walk could not follow the ADD's source; the closure is what
# M2.26 adds). 140 bytes below its M0 baseline, and disabling the
# register-form image grows it back to exactly 1184 (the table path,
# still correct) — the teeth. jmpr_chain3 is
# M2.27: the chain extended to an index built by MULTIPLICATION — r1 =
# r2 * 2 with the join-dependent base r2 in {5, 6}, so the candidate
# set is the MUL image {5, 6} * 2 = {10, 12}, and the runtime index 12
# takes the chain's SECOND b.eq. chain_img_alu now covers MUL (plain
# 64-bit, never faults, type-agnostic — the M2.2 fold's argument), imm
# and register forms alike. 140 bytes below its M0 baseline, and
# disabling the MUL image grows it back to exactly 1184 (the table
# path, still correct) — the teeth. jmpr_chain4 is
# M2.28: the chain extended to an index built by a BITWISE op — r1 =
# r2 ^ 8 with the join-dependent base r2 in {2, 4}, so the candidate
# set is the XOR image {2, 4} ^ 8 = {10, 12}, and the runtime index 12
# takes the chain's SECOND b.eq. chain_img_alu now covers AND/OR/XOR
# via the shared chain_alu_eval helper (plain 64-bit, never faults,
# type-agnostic — the M2.3 fold's argument), imm and register forms
# alike. 140 bytes below its M0 baseline, and disabling the bitwise
# image grows it back to exactly 1184 (the table path, still correct)
# — the teeth. jmpr_chain5 is
# M2.29: the ADAPTIVE chain cap. Its dispatch index is the pair product
# r1 = r2 + r3 over a 2-way join ({0,10}) and a 5-way join ({18..26
# evens}), so the candidate set has TEN in-range values {18..36 evens} —
# beyond the M2.25-M2.28 hard cap of 8, which made such a set keep the
# runtime table. M2.29 raises the collection/emission cap to 12 and
# makes the chain adaptive: it fires when 8*n + 4 < 40 + 4*num_instr
# (chain = 2 words per candidate + UDF; table = 10-word dispatch +
# num_instr 4-byte entries) — 84 < 192 here, so the 10-candidate chain
# wins. The runtime index 10 + 26 = 36 takes the chain's TENTH b.eq
# (block9, LOADI #1000), proving the full walk dispatches. 268 bytes
# below its M0 baseline, and forcing the old cap of 8 grows it back to
# exactly 1664 (the table path, still correct) — the teeth.
# jmpr_chain6 is
# M2.30: the DEFERRED pair product. Its dispatch index is r1 = r2 + r3
# over a 3-way join ({0,2,4}) and a 5-way join ({20,30,40,50,60}), so
# the candidate set has FIFTEEN in-range values {20,22,...,64} — beyond
# the walk's flat cap of 12. M2.25-M2.29 collapsed such a product to
# UNKNOWN at the 13th merge and kept the table; M2.30 DEFERS it: the
# walk records the op + the two source slots, and the dispatch
# re-computes the product into the BIG candidate set (cap 32) over the
# provably-unchanged sources (every write or union of a source flattens
# the deferred form eagerly), so the emission gate (8*n + 4 < 40 +
# 4*num_instr — here 124 < 304) can emit the 15-pair chain. The runtime
# index 4 + 60 = 64 takes the chain's FIFTEENTH b.eq (block14, LOADI
# #1500). 452 bytes below its M0 baseline, and disabling deferral grows
# it back to exactly 2188 (the table path, still correct) — the teeth.
# jmpr_chain7 is M2.31: the deferred pair product
# generalized beyond ADD. Its dispatch index is r1 = r2 ^ r3 (XOR,
# register form) over a 3-way join ({0,32,64}) and a 5-way join
# ({20,22,24,26,28}), so the candidate set has FIFTEEN distinct values
# {20,22,...,92} — again beyond the flat cap of 12. M2.30's machinery
# already records the op generically (def_op = op for the whole six-op
# branch, evaluated by chain_alu_eval at both materialization sites),
# but only ADD had a pin; M2.31 proves the bitwise path end-to-end: the
# XOR product is deferred, re-merged into the BIG set at the dispatch,
# and the gate (124 < 416) emits the 15-pair chain. The runtime index
# 64 ^ 28 = 92 takes the chain's FIFTEENTH b.eq (block14, LOADI #1500).
# 676 bytes below its M0 baseline, and restricting deferral to ADD-only
# grows it back to exactly 2636 (the table path, still correct) — the
# teeth.
# jmpr_chain8 is M2.32: the deferred pair product, NESTED. The
# index is r1 = (r2 + r3) + r4 over a 3-way join ({0,10,20}), a 5-way
# join ({22,24,26,28,30}) and a 2-way join ({0,32}) — the first
# product has FIFTEEN values {22,24,...,50} (deferred, M2.30), and the
# second is IN-PLACE (ADD r1, r1, r4) over that deferred source.
# M2.30/M2.31 flattened a deferred source eagerly — the capped flatten
# collapses (15 > 12) to UNKNOWN, killing the chain. M2.32 records the
# second product as an immutable def-record whose left operand is r1's
# OLD record (not cur[r1] itself — that is what makes the in-place form
# sound), re-merged at the dispatch through the record DAG into the BIG
# set: 15 + {0,32} = THIRTY candidates {22,24,...,50, 54,56,...,82},
# and the gate (244 < 376) emits the 30-pair chain. The runtime index
# (20 + 30) + 32 = 82 takes the chain's THIRTIETH b.eq (block29, LOADI
# #3000). 476 bytes below its M0 baseline, and disabling the nested
# record path grows it back to exactly 2720 (the table path, still
# correct) — the teeth. (The three joins precede both products: a
# deferred source cannot cross a block head — the head-union flattens
# what it mutates.)
# jmpr_chain9 is M2.33: the def-record pool gains an IMMEDIATE
# operand kind, so an imm-form product over a deferred source defers
# instead of collapsing to UNKNOWN. The index is r1 = (r2 + r3) + 32:
# the register product {0,10,20} + {22,24,26,28,30} has FIFTEEN
# values {22,24,...,50} (deferred, M2.30), and the second op is the
# IMM form (ADD r1, r1, #32, in-place) over that deferred source.
# M2.30-M2.32 flattened a deferred source before an imm image — the
# capped flatten collapses (15 > 12) to UNKNOWN, killing the chain.
# M2.33 records op(rec(R1), #32): the source as its immutable record,
# the 32 baked in as a CD_IMM constant, re-merged at the dispatch to
# the FIFTEEN candidates {54,56,...,82}; the gate (124 < 376) emits
# the 15-pair chain. The runtime index (20 + 30) + 32 = 82 takes the
# chain's FIFTEENTH b.eq (block14, LOADI #1500). 596 bytes below its
# M0 baseline, and disabling the CD_IMM path grows it back to exactly
# 2484 (the table path, still correct) — the teeth.
# jmpr_chain10 is M2.34: the deferred-preserving join. The
# index is r1 = (r2 + r3) + r4 (3-way, 5-way and 2-way joins) — but the
# r4 join sits BETWEEN the two products, not before them (chain8's
# shape, which M2.32 had to hoist all joins ahead of the products). The
# first product defers (15 values, R1), and the r4 join's head is then
# reached with R1 on BOTH paths — the BC delivers R1, the fall-through
# carries it. M2.30-M2.33 flattened every deferred form at a head,
# collapsing R1 to UNKNOWN and killing the chain. M2.34's head-union
# recognizes the SAME record on every path: the union is a no-op, so R1
# crosses the join preserved, and the in-place second product composes
# to THIRTY candidates {22,24,...,50, 54,56,...,82}; the gate
# (244 < 376) emits the 30-pair chain. The runtime index (20 + 30) + 32
# = 82 takes the chain's THIRTIETH b.eq (block29, LOADI #3000). 476
# bytes below its M0 baseline, and reverting the head-union to the old
# flatten-all grows it back to exactly 2720 (the table path, still
# correct) — the teeth.
# jmpr_chain11 is M2.35: the deferred-over-flat join — the mirror of
# chain10. The index is r1 = (r2 + r3) + r4 (3-way, 5-way and 2-way
# joins) plus a FLAT ARM: after every join, r1 is set to #34 (a value
# INSIDE the product's set) and a forward BC delivers {r1:34} into the
# join head J1 (the arm sits after ALL joins because a BC delivery
# carries every tracked slot — an early arm would deliver UNKNOWN for
# the not-yet-joined registers). The first product defers (15 values,
# R1) and falls through into J1 carrying R1; J1 thus mixes a DEFERRED
# carry with a FLAT arrival. M2.34 preserved only the SAME record on
# every path — record-vs-flat collapsed to UNKNOWN (15 > 12), killing
# the chain. M2.35 materializes the CARRY record (sound: it is live,
# its DAG slots provably unchanged) and checks containment: {34} is a
# subset of R1's true set, so the union IS R1 and the deferred form
# survives. The in-place second product composes to THIRTY candidates
# {24,26,...,52, 56,58,...,84}; the gate (244 < 384) emits the 30-pair
# chain. The runtime index (20 + 32) + 32 = 84 (the flat arm's #34 is
# overwritten by the product) takes the chain's THIRTIETH b.eq
# (block29, LOADI #3000). 492 bytes below its M0 baseline, and
# disabling the containment keep grows it back to exactly 2748 (the
# table path, still correct) — the teeth.
# jmpr_chain12 is M2.36: the deferred-arrival-over-flat join — the
# mirror of chain11. The index is r1 = (r2 + r3) + r4 (3-way, 5-way
# and 2-way joins). The R-arm (the product, FIFTEEN values R1) is
# followed by `BC r5, J1` — TAKEN at runtime (r5 = 1) — which
# delivers the DEFERRED R1 into J1 (a forward, same-iteration
# delivery); the fall-through then sets r1 = #34 (a value IN R1's
# set) and flows into J1 as the flat CARRY {34}. M2.35 handled only
# the deferred-CARRY side (sound because the carry record is live); an
# arrival record can be STALE (its DAG slots written since delivery),
# so M2.36 checks LIVENESS — no write to the record's transitive DAG
# since its creation pc (head-unions only widen, a sound superset —
# only writes kill it) — plus the M2.35 containment: {34} is a subset
# of R1's true set, so the union IS R1 and the deferred form
# survives. The in-place second product composes to THIRTY candidates
# {26,28,...,54, 58,60,...,86}; the gate (244 < 392) emits the 30-pair
# chain. The runtime index (22 + 32) + 32 = 86 (the BC is taken, so
# the flat arm is dead at runtime) takes the chain's THIRTIETH b.eq
# (block29, LOADI #3000). 508 bytes below its M0 baseline, and
# disabling the liveness keep grows it back to exactly 2780 (the
# table path, still correct) — the teeth.
# jmpr_chain13 is M2.37: the NON-contained deferred-over-flat join.
# chain11/chain12's flat joins were CONTAINED in the deferred record's
# true set (the union is exactly the record). Here the flat arm carries
# #70 — OUTSIDE R1's 15-value set {26..54} — so containment fails in
# the M2.36 orientation (deferred ARRIVAL R1 vs flat CARRY {70}) and
# the old behavior collapsed to UNKNOWN, killing the chain. M2.37
# materializes the union as a UNION record — op(rec(R1), flat{70}), a
# new record shape whose materialization MERGES the record's true set
# with the frozen flat set (capped at 32; the eager cap-check at the
# head is exact-or-conservative because a later head-union can only
# widen, never shrink) — so the record survives the join. The in-place
# second product composes R2 = op(rec(U), slot(r4)) over it to
# THIRTY-ONE candidates {26,28,...,54, 58,60,...,86, 102} (56 is a gap
# — R1 starts at 26 — and 70 duplicates); the gate (252 < 456) emits
# the 31-pair chain. The runtime index 70 + 32 = 102 (the BC is NOT
# taken — r5 == 0 — so the flat arm runs) takes the chain's
# THIRTY-FIRST b.eq (block31, LOADI #3200). 628 bytes below its M0
# baseline, and disabling both union fallbacks grows it back to
# exactly 3068 (the table path, still correct) — the teeth.
# jmpr_chain14 is M2.38: the RECORD-vs-RECORD join. chain13 closed the
# deferred-over-flat family; a join mixing a deferred record with a
# DIFFERENT deferred record still collapsed (the first loop's
# different-record unknowning — the union of two >12-value sets can't
# be either record). Here the dispatch index r1 is computed on two
# arms: the R-arm computes R1 = r2 + r3 (FIFTEEN values {40..68},
# DEFERS) and its `BC r5, J` delivers R1 into J; the fall-through
# S-arm (taken at runtime, r5 == 0) computes R2 = r2 + r6 over the
# SHARED r2 join (FIFTEEN DISJOINT values {70..98}, DEFERS — replacing
# R1 in cur, already delivered). All joins precede the delivery (a BC
# carries every tracked slot — an early arm would deliver UNKNOWN for
# the not-yet-joined r6), and both products share r2 so the
# tracked-register closure {r1,r2,r3,r6} stays within the 4-register
# cap. M2.38 generalizes the union record to reference BOTH records —
# op(rec(R2), rec(R1)) — when the merged true set fits the 32-cap
# (the carry record is live by construction; the arrival record must
# pass the M2.36 liveness check), and the first loop's unknowning is
# gone: every record-vs-record case is decided where the union can
# fire. The union materializes to THIRTY candidates {40..98}; the gate
# (244 < 440) emits the 30-pair chain. The runtime index 22 + 76 = 98
# takes the chain's THIRTIETH b.eq (block29, LOADI #3000). 604 bytes
# below its M0 baseline, and disabling the M2.38 union grows it back
# to exactly 2960 (the table path, still correct) — the teeth.
# jmpr_chain15 is M2.39: the NESTED union join — a proof milestone
# with NO new code. The union record's second side is already a CD_REC
# (M2.38), so its materialization recurses through chain_flatten_big_rec
# and chain_def_live recurses both operands: a union record meeting a
# THIRD distinct deferred record should compose for free. Three arms
# compute three DISTINCT deferred records over the four-register
# closure {r1,r2,r3,r6}: R1 = r2 + r3 (FIFTEEN values {42..70}),
# R2 = r2 + r6 (FIFTEEN values {42,46,50,...,78}), R3 = r6 + r3
# (THIRTEEN values {80..104}) — the r6/r3 sparse-vs-contiguous steps
# make R3's sums spread past 12. The R-arm's `BC r5, J1` delivers R1;
# the fall-through computes R2 and flows in as the carry, so J1 forms
# the union U = op(rec(R2), rec(R1)) = {42..70, 74, 78} (17 values).
# The next `BC r5, JN` delivers U into JN; the fall-through computes
# R3, so JN forms the NESTED union U2 = op(rec(R3), rec(U)) = THIRTY
# candidates {42..104}; the gate (244 < 464) emits the 30-pair chain.
# The runtime index 56 + 48 = 104 (both arm BCs fall through, r5 == 0)
# takes the chain's THIRTIETH b.eq (block29, LOADI #3000). 652 bytes
# below its M0 baseline, and disabling the M2.38 union grows it back
# to exactly 3060 (the table path, still correct) — the teeth prove
# the nested composition dispatches through the same union machinery.
# jmpr_chain22 is M2.46: TWO index generations over one tracked feeder
# — a PROOF milestone with no code change. The chain machinery requires
# exactly ONE dynamic JMPR, so the probe puts both generations in the
# SAME register with a single dispatch: build1 `r1 = r2 + r3` (TWENTY
# values {42..80}, stored image), then a TRACKED `LOADI r3, #100`
# (rewrites the feeder), then build2 IN-PLACE `r1 = r1 + r3` — which
# reads the OLD r1 image and the NEW r3 -> TWENTY values {142..180} —
# and the dispatch chains on the SECOND image. Each generation is
# computed from the feeder's value AT THAT BUILD (build1 uses r3's join
# set {40..48}, build2 uses {100}), while r1's stored image survives
# the LOADI untouched (M2.44's mechanism, now with a TRACKED writer).
# The pin discriminates: re-deriving r1 at the LOADI, a stale r3, or an
# in-place read-after-write all move the candidate set off the runtime
# index 80 + 100 = 180 and UDF-trap; the correct analysis takes the
# chain's TWENTIETH b.eq (block19, LOADI #2000). 1340 bytes below its
# M0 baseline. The teeth: an OPAQUE `SHL r3, r3, #1` at the SAME pc
# makes build2's image UNKNOWN — the chain dies (0 b.eq, table) and the
# runtime index 80 + 96 = 176 lands on block17 -> 1800 on all four
# engines — tracked vs opaque writer on the feeder, same position.
# jmpr_chain29 is M2.53: the unary-over-deferred boundary — a PROOF
# milestone with no code change. The static trace shows the unary
# branch's "deferred source -> UNKNOWN" fallback IS reachable in
# principle: the M2.30 root allocates a record whenever a
# register-form product overflows the merge cap with known FLAT
# sources (a >96-distinct image — the M2.42 "UNREACHABLE" comment
# means such records can never lead to a CHAIN, not that they are
# never allocated). The pin proves the FLAT side: r1 = -(r2 + r3)
# with r2, r3 in {-79..-61 step 2} — the ADD product is the 19 even
# values {-158..-122 step 2} (would DEFER under the old 12-cap, fits
# the current 96) — so NO record is created and the in-place NEG
# RE-DERIVES to {122..158}. Runtime -( -61 + -61 ) = 122 -> the
# chain's FIRST b.eq -> block0 -> 100 on all four engines.
# Dump-verified: 19 b.eq, compares exactly {122..158 step 2}, the
# first b.eq byte-exact on block0's movz x9, #100. Teeth: (a) a
# 20x20 MUL product (>96 distinct) + NEG collapses conservatively
# (0 b.eq, table, runtime -400 out of range -> fault rc=1 on ARM and
# interp); (b) the CAP-REVERT — with TX_AR_CHAIN_MAX back at 12 the
# same pin's product DEFERS (record created), the NEG reads the
# record and hits the fallback -> table, PASS 100 at 3952 bytes (0
# b.eq) — the unary-over-deferred fallback is correct when reachable.
# 1172 bytes below its M0 baseline.
# jmpr_chain28 is M2.52: the DOUBLE-UNARY image — NOT then NEG — a
# PROOF milestone with no code change. The second unary re-derives
# from the FIRST unary's STORED IMAGE: the index is r1 = ~r2 over a
# ten-way join (r2 in {39,41,...,57}) followed by in-place NEG r1, r1.
# The two maps compose exactly in two's complement, NEG(NOT(a)) = a+1,
# so the composed image is {40,42,...,58} (in range); runtime
# NEG(NOT(57)) = 58 takes the TENTH b.eq -> block9 -> 1000 on all
# four engines. Dump-verified: 10 b.eq, first subs #40, last #58, the
# tenth b.eq byte-exact on block9's movz x9, #1000. The test
# DISCRIMINATES: if the second unary re-read the JOIN set instead of
# the stored image, NEG over {39..57} would be {-57..-39} (out of
# range) and runtime 58 would UDF-trap — only the stored-image
# composition chains. Teeth: (a) the mirror NEG-then-NOT over {41..59}
# (NOT(NEG(a)) = a-1) lands the SAME candidate set {40..58} — 10 b.eq,
# PASS 1000, identical 1804 bytes, both orders compose; (b) disabling
# the unary tracking grows the row to exactly 2000 (table, 0 b.eq,
# still PASS) — the 196-byte delta is exactly the chain-vs-table
# difference, proving the chain came from the composed stored image.
# 444 bytes below its M0 baseline.
# jmpr_chain27 is M2.51: the unary cap discipline, promoted from
# M2.50's ad-hoc teeth into a committed pin — a PROOF milestone with no
# code change. chain24 pinned the EXACT side (ten-value NEG source ->
# ten candidates -> chain). The index is r1 = -r2 over a 65-way join
# (r2 in {-150..-86}) -> 65 values {86..150}. At the M2.51-era caps
# (64/64) that OVERFLOWED the image merge cap -> UNKNOWN -> the table
# (0 b.eq, dump-verified) — the conservative-collapse pin, whose
# discriminator is truncation (a chain over the first 64 candidates
# would miss runtime 150 and UDF-trap). M2.61's EQUAL-CAPS bump (MAX =
# BIG = 96) changed this row's meaning, exactly like chain33's: 65 <=
# 96 now FITS, so the image materializes and the chain fires with the
# full 65 candidates — runtime -(-150) = 150 = the SIXTY-FIFTH b.eq
# -> block0 at pc 150 -> 777 on all four engines. The M2.51 collapse
# was cap-specific; the collapse side of the discipline now lives at
# chain30 (100 values > 96). Dump-verified (96/96): 65 b.eq, 0 table
# words. 740 bytes below its M0 baseline (4500) by the 65-candidate
# chain and the accumulated emission folds.
# jmpr_chain26 is M2.50: the NOT-built index, IN RANGE — a PROOF
# milestone with no code change (the multi-value unary mapping was
# already there — chain24 pinned a ten-value NEG source; the
# "singleton-only" premise was inaccurate). What IS new: chain24's
# NOT side used POSITIVE constants (NOT of a small pc is huge — out of
# range, so the analysis computed the exact image, found no in-range
# candidate and emitted a bare UDF). Over NEGATIVE constants the NOT
# image lands IN RANGE: ~(-58) = 57, ~(-40) = 39. The index is
# r1 = ~r2 over a ten-way join of negative constants (r2 in
# {-58..-40} -> TEN values {39,41,...,57}); the gate (84 < 40 + 4*60 =
# 280) emits the 10-pair chain; runtime ~(-58) = 57 takes the TENTH
# b.eq -> block9 -> 1000 on all four engines. Dump-verified: 10 b.eq,
# first subs #39, last #57; the tenth b.eq (imm19 140) lands
# byte-exact on block9's movz x9, #1000. 436 bytes below its M0
# baseline. The teeth close the cap-discipline question for the
# UNARY image: a 65-DISTINCT join source through NEG overflows the
# merge cap to UNKNOWN (0 b.eq) and the full table runs, PASS 777 on
# all four engines — no truncated chain.
# jmpr_chain25 is M2.49: the register-form shift PRODUCT image — a
# PROOF milestone with no code change. M2.47 verified the shift image
# with a SINGLETON amount; this pin exercises the full product form:
# the amount register r5 carries a MULTI-VALUE set, so the image is
# { a << (b & 0x3F) : a in S(a), b in S(b) } — chain_img_alu's register
# path, exact until the merge cap. The pin's sets: r2 in {10,11,12} and
# r5 in {2,3,66} — where 66 & 0x3F = 2, so the amount 66 is MASKED to 2
# exactly as the hardware (lslv), the interpreter and ar_const_step do.
# The nine pairs collapse to SIX distinct candidates {40,44,48,80,88,96}
# (dump-verified: 6 b.eq, compares #40 #44 #48 #80 #88 #96): the product
# is EXACT and the masking is applied — an unmasked analysis would
# compute 12 << 66 out-of-range and emit a bare UDF, trapping the
# runtime index 48. The runtime index 12 << (66 & 0x3F) = 48 takes the
# b.eq for 48 (block2, LOADI #300). 780 bytes below its M0 baseline.
# The teeth verify the exact-or-conservative cap discipline both ways:
# (a) EXACT — a 65-PAIR product whose distinct set is under the 64 cap
# chains on the exact deduplicated candidates (22 in-range b.eq, and
# the runtime 13 << 5 = 416 correctly misses them all and faults);
# (b) CONSERVATIVE — a 240-pair product with >64 DISTINCT values
# collapses to UNKNOWN (0 b.eq, the full 10-word table), and the
# runtime 40 << 6 = 2560 faults through the bounds check — no
# truncated or wrong chain is ever emitted.
# jmpr_chain24 is M2.48: the NEG-built dispatch index — the unary
# NOT/NEG join the chain image. Before M2.48 the image family was the
# binary ADD/SUB/MUL/AND/OR/XOR plus the M2.47 shifts, so a NEG on the
# index path fell to the walk's opaque-writer branch and the chain
# died. M2.48 adds the UNARY image — { f(a) : a in S(a) } for f = NOT
# (~a) or NEG (-a), plain 64-bit, never faults, type-agnostic (the M2.2
# argument). The pin's index is r1 = -r2 over a TEN-way join of
# NEGATIVE constants (r2 in {-58,-56,...,-40} -> TEN values {40..58});
# the gate (84 < 40 + 4*60 = 280) emits the 10-pair chain. The runtime
# index -(-58) = 58 takes the chain's TENTH b.eq (block9, LOADI #1000).
# 444 bytes below its M0 baseline. The NOT side is out-of-range by
# construction (NOT of a small pc is huge), so it lands on the bare-UDF
# path — the analysis computes the image exactly, finds no in-range
# candidate and emits just the UDF (0 b.eq, 0 table, 1 UDF word, all
# four engines fault rc=1/2; verified ad hoc — the size discriminates
# it from the UNKNOWN -> table path). The teeth: reverting the M2.48
# tracking makes the NEG opaque again — the chain dies and the row
# grows back to exactly 2116 (the table path, 0 b.eq, still correct;
# the 196-byte delta is exactly (40 + 4*60) - (8*10 + 4)).
# jmpr_chain23 is M2.47: the SHL-built dispatch index. Before M2.47
# the shifts were NOT in the chain image family (chain_alu_eval handled
# only ADD/SUB/MUL/AND/OR/XOR), so a shift on the index path fell to
# the walk's opaque-writer branch and the chain died. M2.47 adds
# SHL/SHR/SAR to the image: a constant-amount shift of a known set is
# plain 64-bit, never faults and is type-agnostic (the M2.3 argument),
# so a shifted index RE-DERIVES. The pin's index is r1 = r2 << 1 over a
# TEN-way join (r2 in {20..29} -> TEN values {40,42,...,58}); the gate
# (84 < 40 + 4*60 = 280) emits the 10-pair chain. The runtime index
# 29 << 1 = 58 takes the chain's TENTH b.eq (block9, LOADI #1000).
# 444 bytes below its M0 baseline. The register-form amount (SHL r1,
# r2, r5 with LOADI r5, #1 — the amount set is a singleton {1}, masked
# mod 64) re-derives identically (10 b.eq, 1804 bytes, PASS 1000 on
# all four engines, verified ad hoc). The teeth: reverting the M2.47
# tracking (dropping the shifts from the walk's ALU branch) makes the
# SHL opaque again — cur[r1] goes UNKNOWN, the chain dies, and the row
# grows back to exactly 2000 (the table path, 0 b.eq, still correct;
# the 196-byte delta is exactly (40 + 4*60) - (8*10 + 4)).
# jmpr_chain21 is M2.45: a rewrite of the INDEX register r1 itself,
# between the index build and the dispatch — the mirror of M2.44 — a
# PROOF milestone with no code change. The walk stores a PER-REGISTER
# set, so a WRITE to r1 REPLACES cur[r1] entirely: a tracked writer
# installs the new image (M2.27/M2.28), an OPAQUE writer installs
# UNKNOWN. M2.45 (as built): the SHR was not in the image family, so
# the write installed UNKNOWN, the chain died and the table ran (2544
# bytes). M2.47 supersedes that narrative: the shifts joined the image,
# so the SHR now REPLACES the stored image with the SHIFTED one —
# {42..80} >> 1 = {21..40} — and the chain fires on the shifted
# candidates (20 b.eq, 2340 bytes, −204 = the chain-vs-table delta).
# The pin's index is r1 = r2 + r3 over joins (r2 in {2,12,22,32}, r3 in
# {40,42,44,46,48} -> TWENTY values {42..80}); the `SHR r1, r1, #1`
# sits BETWEEN the build and the dispatch. The runtime index 32 + 48 =
# 80 becomes 80 >> 1 = 40 — the TWENTIETH candidate — whose block
# (block_40 at pc 40) holds LOADI #10000, so the result is unchanged
# (10000 on all four engines). The teeth: reverting the M2.47 tracking
# grows the row back to exactly 2544 (SHR opaque -> UNKNOWN -> table,
# still correct).
# jmpr_chain20 is M2.44: a tracked FEEDER rewritten after the index
# computation — a PROOF milestone with no code change. The chain walk
# stores a PER-REGISTER set: the index build puts the image { r2 + r3 }
# into r1's slot, and a later WRITE to a feeder updates only that
# feeder's slot — r1's stored image is untouched, exactly like the
# runtime register (r1 holds the value computed at the build, not
# re-derived from the rewritten r2). The stress is a rewrite of the
# feeder r2: `SHL r2, r2, #1` — TRACKED since M2.47 (the walk
# re-derives {2,12,22,32} << 1 = {4,24,44,64} into r2's slot) — yet
# the index set survives, because the dispatch reads only r1's slot.
# The pin's index is r1 = r2 + r3 over joins (r2 in {2,12,22,32}, r3
# in {40,42,44,46,48} -> TWENTY values {42..80}); the rewrite sits
# BETWEEN the build and the dispatch; the gate (164 < 368) emits the
# 20-pair chain. The runtime index 32 + 48 = 80 (the SHL sets r2 = 64,
# but r1 was computed before it) takes the chain's TWENTIETH b.eq
# (block19, LOADI #2000). 540 bytes below its M0 baseline, and
# swapping the order (rewrite BEFORE the build) grows it back to
# exactly 2528 (the table path — the index is then computed from the
# rewritten feeder, still correct) — the teeth proving the analysis
# is order-sensitive.
# jmpr_chain19 is M2.43: the tracked-register closure above 4. M2.26
# sized TX_AR_CHAIN_REGS at 4 — the index register plus its feeders; a
# closure needing more feeders truncated silently (the missing feeder's
# set stayed UNKNOWN, poisoning the index set to UNKNOWN and falling to
# the table). M2.43 raises the cap to 8, so a 5-8-feeder index chain
# now analyzes. The pin's index is built in TWO stages over joins:
# r2 = r4 + r5 (r4 in {0,10}, r5 in {0,10,20} -> {0,10,20,30}), then
# r1 = r2 + r3 (r3 in {40,42,44,46,48} -> TWENTY values {40..78}). The
# tracked closure is {r1, r2, r3, r4, r5} — FIVE registers: under the
# 4-cap the closure stops at r4 and r5 is never tracked, so r1 falls to
# UNKNOWN and the table runs; under the 8-cap all five are tracked and
# the gate (164 < 360) emits the 20-pair chain. The runtime index
# 10 + 20 + 48 = 78 takes the chain's TWENTIETH b.eq (block19, LOADI
# #2000). 524 bytes below its M0 baseline, and reverting the cap to 4
# grows it back to exactly 2496 (the table path, still correct) — the
# teeth. The candidates are the block pcs (block0..block19 at 40..78),
# as the JMPR jumps to pc == index.
# jmpr_chain18 is M2.42: the flat walk cap MATCHES the BIG cap. M2.41
# raised TX_AR_CHAIN_MAX from 12 to 20; M2.42 raises it to 64, equal to
# TX_AR_CHAIN_BIG, so a 21-64-value product stays FLAT too — and since a
# set of > 64 values can never chain (the emission gate rejects ncand >
# 64), every reachable chain materializes from a flat set: the
# M2.30-M2.41 record machinery becomes unreachable (vestigial, retained
# as history and as a safety net if the caps ever diverge again). The
# pin's product is FORTY values {42..120}: r1 = r2 + r3 (r2 in
# {2,12,...,72} — EIGHT values, r3 in {40,42,44,46,48} — FIVE values);
# the arm `BC r5, J` delivers it; then `LOADI r2, #1` WRITES a source.
# Under the M2.41 20-cap the product deferred and the write flattened
# it to UNKNOWN (the table ran); under 64 it stays flat, the write is a
# no-op for r1's slot, and the gate (324 < 528) emits the 40-pair
# chain. The runtime index 72 + 48 = 120 takes the chain's FORTIETH
# b.eq (block39, LOADI #4000). 700 bytes below its M0 baseline, and
# reverting the cap to 20 grows it back to exactly 3460 (the table
# path, still correct) — the teeth.
# jmpr_chain17 is M2.41: the FLAT walk cap above 12. M2.25-M2.40 capped
# the walk's FLAT set (TX_AR_CHAIN_MAX) at 12: a register-form product
# whose set exceeded 12 DEFERRED (a record) and re-computed at the
# dispatch. M2.41 raises the cap to 20, so a 13-20-value product stays
# FLAT — and a flat set SURVIVES a write to one of its source
# registers: chain_prewrite only flattens DEFERRED forms (a write to r2
# leaves the flat set in r1's slot untouched), while a >12-value
# deferred record was flattened to UNKNOWN by the same write (the eager
# flatten caps at the walk bound). The raise needs two companions to
# stay airtight: (a) a flat+flat head-union whose merged set exceeds 20
# freezes as a PRE-MERGED union record (chain14/15's 30-value joins
# must not regress), and (b) an in-place product over a FLAT source
# whose image overflows freezes the aliased source as a CD_FLAT operand
# (chain13's second product must not regress). The pin's product is
# TWENTY values {42..80}: r1 = r2 + r3 (r2 in {2,12,22,32}, r3 in
# {40,42,44,46,48}); the arm `BC r5, J` delivers it; then `LOADI r2,
# #1` WRITES a source. Under the old 12-cap the product deferred and
# the write flattened it to UNKNOWN (the table ran); under 20 it stays
# flat, the write is a no-op for r1's slot, and the gate (164 < 368)
# emits the 20-pair chain. The runtime index 32 + 48 = 80 takes the
# chain's TWENTIETH b.eq (block19, LOADI #2000). 540 bytes below its
# M0 baseline, and reverting the cap to 12 grows it back to exactly
# 2516 (the table path, still correct) — the teeth.
# jmpr_chain16 is M2.40: BEYOND the 32-cap. M2.30-M2.39 capped the BIG
# candidate set — the walk's deferred-materialization set, the union
# record's merged set, and the emitted chain — at 32; a dispatch whose
# union of records exceeded 32 collapsed to UNKNOWN and fell to the
# table. M2.40 raises TX_AR_CHAIN_BIG to 64, so a 33-64-candidate
# dispatch chains when the honest cost gate (8*n + 4 < 40 + 4*num_instr)
# says it beats the table. The LINEAR chain is the right emission shape
# at any n: each candidate needs its own cmp + b.eq pair regardless, so
# two chained segments add a selector without reducing comparisons
# (8n + 8 > 8n + 4) and a compare-tree costs ~4 words per internal node
# — both strictly worse on bytes. The pin's union is FORTY values:
# R1 = r2 + r3 (TWENTY values {42..80}) and R2 = r2 + r6 (TWENTY-FIVE
# values {72..120}) mix at J into op(rec(R2), rec(R1)) = {42..120}
# evens — accepted at 64, rejected at 32. The gate (324 < 528) emits
# the 40-pair chain; runtime 32 + 88 = 120 takes the FORTIETH b.eq
# (block39, LOADI #4000). 700 bytes below its M0 baseline, and
# reverting the cap to 32 grows it back to exactly 3448 (the table
# path, still correct) — the teeth.
# jmpr_calc is M2.1: its index is COMPUTED at translate
# time through all four folded ADD/SUB shapes (reg+reg, reg+imm, reg+reg,
# reg+reg, reg+imm) — 224 bytes below its M0 baseline after M2.6's imm
# fold compounds 12 more on M2.1's 212, and disabling the ADD/SUB index
# fold grows it back to exactly 1288. M2.23's elimination drops the dead
# LOADI/RET dispatch blocks after the fold target — 276 below M0 now. jmpr_mix is the M2.1 mixed
# case: one runtime JMPR (forces g_alloc=0, whole program naive — the
# index is a LOAD result for the same M2.3 reason as jmpr_dyn) and one
# constant JMPR that still folds — 64 bytes below its M0 baseline, the
# folded-branch-under-naive-codegen interaction pinned. jmpr_calc_mul is
# M2.2: its index is COMPUTED through a MULTIPLY chain (MUL reg+reg,
# MUL reg+imm, SUB reg+imm) — 212 bytes below its M0 baseline after
# M2.6's imm fold compounds 4 more on M2.2's 208, and disabling the MUL
# fold grows it back to exactly 1272. M2.23's elimination drops its dead
# dispatch tail — 288 below M0 now. jmpr_calc_bit is
# M2.3: its index is COMPUTED through the BITWISE + SHIFT folds (OR/SAR/
# SHR/SHL/AND/XOR/ADD, with the shifts masking their amount mod 64 and
# SAR arithmetic on a sign-bit value) — 252 bytes below its M0 baseline
# after M2.6's imm fold compounds 4 more on M2.3's 248, and disabling
# the fold grows it back to exactly 1384. M2.23's elimination drops its
# dead dispatch tail — 332 below M0 now. src_resident is
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
# fusion itself is worth 16 of the 312. M2.23's elimination drops the
# whole dead path at the stream end — 396 below M0 now. jmpr_fall2 is M2.13:
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
# whole function naive), still correct. jmpr_callret is
# M2.19: a JMPR chain across a CALL-RETURN boundary, keyed on the
# callee's RETURN VALUE (r0). The callee is a straight-line leaf (ENTER,
# LOADI r0 = 3, RET — no branches, no nested calls), and the M2.19
# leaf-callee return-value analysis makes r0 = 3 known in the CALLER
# after the call. The fresh-frame model (the interpreter zeroes the
# callee's frame, copies r0-r7 as args, and on RET only r0 propagates
# back) already lets non-r0 chains survive a call — the probe showed a
# JMPR keyed on r2 folds with no new machinery; what the analysis adds
# is the RETURN register. The analysis runs the fixpoint's own constant
# step (factored into ar_const_step) over the callee with the fresh-frame
# map (r0-r7 unknown, r8+ = 0, zeroed by the prologue) and requires the
# callee to start with ENTER, so a leaf returning a value computed from
# its ARGS is conservatively not folded. JMPR r0 folds to pc 3 (a
# fall-through fold — the branch is dropped and the call site flows
# directly into the target) — 152 bytes below its M0 baseline, and
# disabling the analysis grows it back to exactly 2036 (the JMPR
# dynamic, g_alloc = 0, runtime dispatch table + bounds check), still
# correct. jmpr_callret_arg is
# M2.20: the call-site-aware extension of M2.19. The leaf returns
# arg0 + arg1 — a value computed from its ARGS, which the M2.19
# per-callee analysis (args unknown under the fresh-frame map)
# conservatively could not fold. M2.20 runs the same ar_const_step body
# walk AT EACH CALL SITE, seeding the scratch map's r0-r7 from the
# CALLER's constant map: the emitted call site marshals r0-r7 from the
# caller's frame (and the interpreter copies them), so a known constant
# argument is a known constant inside the leaf. TWO call sites to the
# SAME leaf pin the per-site dependence: main passes 3 + 5 = 8 (JMPR
# folds to pc 8), other passes 8 + 10 = 18 (JMPR folds to pc 18); both
# are REAL branches (unreachable filler at pc 7 / pc 17, so neither
# target is pc+1 and the coalescing pre-pass does not fire), and both
# folds are load-bearing — g_alloc requires EVERY JMPR in the stream
# to fold, so if either call site failed, the whole program would go
# naive. The precompute shrinks to a per-callee LEAF MARKER
# (ar_leaf_ret_pc — ENTER start, straight-line, first RET), and the
# args-unknown case is just the all-unknown seed, so jmpr_callret is
# byte-identical. 328 bytes below its M0 baseline under M2.20/2.21, and
# disabling the arg seeding grows it back to exactly 3344 (both JMPRs
# dynamic, g_alloc = 0, whole function naive), still correct. M2.22
# drops the SECOND caller's frame load: `other` (pc 10) is unreachable
# — main's RET at pc 9 exits and nothing targets pc 10 — so its ENTER
# prologue (~197 words) and its folded JMPR's flush are dead code,
# taking the row to 2224 (-1120 vs M0: 328 + 792). M2.23's whole-body
# elimination then drops `other` ENTIRELY (its arithmetic, its CALL
# site, its branches, its RET) — 1964, -1380 vs M0: the four dead-code
# rows now land on the same 1964-byte live core. jmpr_unreach is
# M2.21: the g_alloc gate is scoped to REACHABLE JMPRs. `dead` (pc 10)
# is an unreachable function — entered from nowhere (main's RET at pc 9
# is a terminal, nothing targets pc 10) — and its JMPR r7 reads an
# ENTER argument, unknown under the constant map, so it NEVER folds.
# Before M2.21 that one unreachable JMPR killed g_alloc for the whole
# program (the gate was every JMPR in the linear stream): main's own
# foldable JMPR (r0 = 8 after the M2.20 leaf call) was thrown away and
# everything ran naive. M2.21's static reachability pre-pass (a BFS
# from the entries over the non-JMPR edges: BR/BC targets + fall-
# through, CALL target + fall-through; RET and JMPR are terminals)
# shows pc 11 is never reachable — a JMPR that can never dispatch
# cannot make every pc a potential block head — so main's JMPR folds
# to pc 8 and the cache survives. dead's dynamic dispatch path is
# still emitted (correct, never executed; the cache discipline is
# per-path, so a fold into an unreachable-looking region would land
# on a rule-1 block-head flush anyway). 96 bytes below its M0 baseline
# (the extra 48 vs the teeth is M2.14 tail-reuse across the three
# RETs), and reverting the reachability gate grows it back to exactly
# 2996 (main's JMPR dynamic, g_alloc = 0, whole function naive), still
# correct. M2.22 adds the EMISSION side: dead's ENTER frame-load
# prologue (~197 words) is dead code — a function with zero live
# predecessors never runs, so its frame load is dropped (the dead
# region is compiled with an empty initial cache directory) — 884
# bytes below its M0 baseline now, and reverting only the
# reachability-aware emission (flush_owed -> always flush) grows it
# back to exactly 2948 (the M2.21 bytes, still correct) — the M2.22
# teeth, isolating the 788-byte prologue delta exactly. M2.23 then
# ELIMINATES dead's whole body (its dynamic JMPR path AND the 128-byte
# JMPR table, which only existed for it) — 1964, -1080 vs M0. jmpr_foldreach is
# M2.21 soundness pin (reviewer finding): a REACHABLE fold can dispatch
# into a statically-unreachable-looking region — main's JMPR r0 = 10
# folds to pc 10, which sits inside `dead` (the fold index is just a
# constant, any pc) — so the reachability pre-pass must be FOLD-AWARE:
# the fold edge makes pc 13 runtime-reachable, and dead's JMPR (index
# = a DIV result, 14/2 = 7, which never folds — the constant analysis
# deliberately excludes DIV) MUST gate. Without the fold edge, dead's
# JMPR hides from the gate, g_alloc stays 1, and the runtime dispatch
# could land mid-chain with a stale cache — the hazard the gate exists
# to prevent. With the closure the whole program goes naive (correct;
# the runtime still terminates: fold to pc 10, DIV -> 7, dispatch to
# pc 7, ADD -> 8). 48 bytes below its M0 baseline (the naive-path
# machinery: pc 6's fold as a direct branch + M2.14 tail-reuse), and
# removing the fold edge from the pre-pass drops it to exactly 2988
# (the buggy g_alloc=1 emission, dead's JMPR hidden), still passing —
# the teeth, isolating the fold-edge delta (3044 -> 2988 = -56) exactly.
# jmpr_deadmult is
# M2.22, the per-pc live-predecessor count in the EMISSION: `dead` (pc
# 13) is an unreachable MULTI-BLOCK function at the stream end (leaf's
# RET at pc 12 is a terminal), with an internal BR to a second block —
# so its whole body (ENTER frame-load prologue, the BR's spill of r4,
# the label's dynamic JMPR path) is correct-but-dead code. M2.22 drops
# the frame load (the ENTER emits ZERO words, dump-verified off[13] ==
# off[14]) and the dead flushes (the BR's first word is the b itself —
# no spill; r4 is never read reachable and the region is at the stream
# end, so the deferred spill never fires). 888 bytes below its M0
# baseline (197-word prologue + dead BR spill + M2.14 tail-reuse +
# fold machinery), and reverting the reachability-aware emission grows
# it back to exactly 2980 (the M2.21 bytes, still correct) — the
# teeth. M2.23 eliminates dead's whole body and the 144-byte JMPR
# table — 1964, -1112 vs M0. jmpr_deadfull is
# M2.23, whole-body DEAD-CODE ELIMINATION: a pc with zero live
# predecessors (g_npred[pc]==0 per the fold-aware BFS — entries are
# roots, reachable with count 0) never executes under g_alloc=1, so its
# ENTIRE body is not emitted — the ENTER frame load, arithmetic,
# branches, CALL sites, dynamic JMPR table paths, and RETs (M2.22
# dropped only the frame load and flushes; this supersedes that).
# `deadfull` (pc 10) is an unreachable function with a rich body: an
# arithmetic chain (r4 = (10+20)*20 = 600), a CALL to the leaf, a BR to
# a second block, and a JMPR whose index 600 is OUT OF RANGE — never
# folding, so its dynamic table path (and, since it was the only
# dynamic JMPR in the stream, the whole num_instr x 8-byte JMPR table)
# vanishes with the body: under g_alloc=1 every EMITTED JMPR folds, so
# no table is ever needed. 1352 bytes below its M0 baseline, and
# disabling the elimination grows it back to exactly 3204 (the M2.21
# emission — M2.23 minus the elimination is byte-identical to M2.21,
# the hoisted head flush and the reverted guards being byte-neutral —
# still correct) — the teeth, isolating the 1240-byte elimination
# delta (dead body + 176-byte table). The four dead-code rows now land
# on the same 1964-byte live core. jmpr_cross is
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
# check return), still correct. M2.23's elimination drops its dead RET
# (pc 6, never reached) — 224 below M0 now. The same teeth move jmpr_fall2 by its
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
# cas_simple and atomic_add are A3 (SIMI Phase 15 atomics): neither
# existed at M0 (they arrived with the A0 ISA), so their M0-equivalents
# are the NAIVE emissions measured with the A3 translator forced to
# g_alloc=0 — the same honest substitute float_ops's 2340 used. The
# atomics codegen's `if (!g_alloc)` branch (h_base=x10, h_other=x11,
# no cache flush/reserve) degrades to exactly the M0 sequences, so the
# baseline is the M0-era shape, measured not invented. The cached
# emission wins 44 bytes on cas_simple (1416->1372) and 24 on
# atomic_add (1220->1196) — the register frame loaded once, not once
# per instruction, and the CAS loop's old-value host reused for the
# stlxr status (zero extra words for the retry/result plumbing).
# float_ops is F1
# (SIMI Phase 10 float codegen): the row's 2340 M0-equivalent is the
# NAIVE emission measured with the F1 translator forced to g_alloc=0
# — the documented invariant that the naive path degrades the cache
# helpers to exactly the M0 sequences means this is byte-identical to
# what the M0 commit (1729f50) WOULD have emitted for float had float
# been in scope there (it was rejected outright instead, so there is
# no real M0 byte count to measure — the honest substitute is the
# naive shape, measured, not invented). The F1 cached emission is
# 2156 (-184: the register cache keeping the arithmetic chains
# resident, the same win every other row shows). Same skips as the
# runner: mem_ops address-0 convenience, jmpr_oob self-skip — its UDF
# fault path has no expected result, so it is checked by hand with
# `./simi-arm-verify tests/jmpr_oob.tmo main 111` (expect rc=1 and
# "execution error" — see the plan doc's M2 addendum, §10).
# stress_atomics is the
# Phase 15 concurrency stress fixture (tools/simi/stress_atomics.c runs it
# under pthreads on real x86 hardware). Its row's 2632 M0-equivalent is
# the NAIVE emission measured with the translator forced to g_alloc=0,
# the same honest substitute cas_simple/atomic_add/float_ops used — the
# fixture's consumer entry is unreachable from main and M2.23's
# dead-function elimination drops it, so this row measures the producer
# body only (the same code the harness actually executes). The cached
# emission is 2568 (-64: the atomics' register-frame loads and the
# enq/claim cells staying cache-resident across the loop, plus the
# claim-loop's CAS kept entirely in the 3-slot cache). The suite runners
# expect main's single-threaded result 0 (the harness-written M/R cells
# are zero there).
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
    [atomic_add]=1220
    [branch_cmp]=1096 [call_ret]=1932
    [cap_call_ret]=3192    [cap_forge]=1336    [cas_simple]=1416 [dead_reuse]=1188 [extra_ops]=1140
    [fetch_cross]=1216
    [float_ops]=2340
    [jmpr_basic]=1088 [jmpr_calc]=1288 [jmpr_calc_bit]=1384 [jmpr_calc_mul]=1272
    [jmpr_callret]=2036 [jmpr_callret_arg]=3344 [jmpr_chain]=1256 [jmpr_chain2]=1248 [jmpr_chain3]=1248 [jmpr_chain4]=1248 [jmpr_chain5]=1824 [jmpr_chain6]=2460 [jmpr_chain7]=3020 [jmpr_chain8]=3064 [jmpr_chain9]=2828 [jmpr_chain10]=3064 [jmpr_chain11]=3100 [jmpr_chain12]=3140 [jmpr_chain13]=3492 [jmpr_chain14]=3368 [jmpr_chain15]=3492 [jmpr_chain16]=3944 [jmpr_chain17]=2852 [jmpr_chain18]=3956 [jmpr_chain19]=2824 [jmpr_chain20]=2864 [jmpr_chain21]=2880 [jmpr_chain22]=4864 [jmpr_chain23]=2248 [jmpr_chain24]=2364 [jmpr_chain25]=2964 [jmpr_chain26]=2344 [jmpr_chain27]=4500 [jmpr_chain28]=2248    [jmpr_chain29]=4600    [jmpr_chain30]=2888    [jmpr_chain31]=6204 [jmpr_chain32]=12828    [jmpr_chain33]=7468    [jmpr_chain34]=4796    [jmpr_chain35]=1240    [jmpr_chain36]=1180    [jmpr_chain37]=9348 [jmpr_chain38]=11364 [jmpr_chain39]=7964 [jmpr_chain40]=7508    [jmpr_chain41]=7556 [jmpr_chain42]=5612    [jmpr_chain43]=1748 [jmpr_chain44]=1332 [jmpr_cross]=1212 [jmpr_deadfull]=3316 [jmpr_deadmult]=3076 [jmpr_dyn]=1148 [jmpr_fall]=1376 [jmpr_fall2]=1228 [jmpr_foldreach]=3092 [jmpr_join]=1144 [jmpr_mid]=1200 [jmpr_mix]=1312 [jmpr_table]=1344 [jmpr_unreach]=3044
    [epi_merge]=1140 [epi_merge2]=1160 [epi_merge3]=1180 [epi_merge4]=1148 [epi_merge5]=1168 [epi_merge6]=1156
    [loadi64]=968
    [loop_sum]=1040 [mem_neg]=1308 [mem_ops_native]=1048 [mem_pre]=2012
    [mem_reg]=1764 [obj_ops]=1164 [ptr_ops]=1120
    [rd_star]=1108 [arm64_boot_smoke]=928 [rv64_boot_smoke]=928 [src_resident]=1120
    [straight_line_bench]=1104 [stress_atomics]=2632 [tail_ret]=1048
)

# jmpr_chain30 is M2.54: the conservative side of the BINARY cap
# discipline, promoted from M2.49's ad-hoc teeth into a committed pin
# (the mirror of chain27's UNARY collapse). The index r1 = r2 + r3 over
# two SEQUENTIAL 10-way joins (r2 in {0,10,...,90} at Ja, r3 in {0..9}
# at Jb — the sets delivered independently) gives the analysis a
# 100-DISTINCT image {0..99}, which OVERFLOWS the image merge cap (96,
# TX_AR_CHAIN_MAX / TX_AR_CHAIN_BIG) -> UNKNOWN -> the chain dies ->
# the naive table dispatch runs. Runtime 90 + 9 = 99 lands on block0 at
# pc 99 -> LOADI #999 -> PASS 999 on all four engines, through the
# table's bounds check; runtime 99 is the LAST of the 100-value image,
# so a wrong analysis emitting a truncated 96-candidate chain would
# miss it and UDF-trap (rc=1). Dump-verified: 0 b.eq, the full table
# dispatch, 101 instructions. M2.54 ALSO fixed a latent bug the pin's
# control exposed: chain_flatten_big copied a flat set (stored at
# TX_AR_CHAIN_MAX width) into the TX_AR_CHAIN_BIG store with no cap
# check — a stack overflow whenever MAX > BIG (a raised-cap control
# crashed with stack smashing; now it collapses to UNKNOWN instead,
# exact-or-conservative). Under the shipped equal caps the fix is
# byte-invisible (n <= 96 = BIG always), so this row's M0 baseline
# (2888, measured at git 1729f50) differs from M1 (2476) only by the
# accumulated emission folds on the g_alloc=0 table path (-412).
# (Equal-caps note, M2.64: the >96 product DEFERS as record 0 —
# instrumented ndef=1 — and the dispatch materialization then
# collapses at the BIG store; same table, exact-or-conservative.)

# jmpr_chain31 is M2.55: the mirror cap divergence pinned from the shipped
# side. The M2.54 control proved MAX > BIG is overflow-free (flatten
# collapses); the mirror is BIG > MAX — the union store's frozen
# g_chain_def_flat is BIG-shaped, so a deferred record whose true set
# exceeds the walk's flat bound (TX_AR_CHAIN_MAX) materializes soundly
# through the BIG path. Under the shipped EQUAL caps (96/96, M2.61)
# that divergence is unreachable — a record's store holds at most 96,
# exactly the flat bound. At the M2.55-era caps (64/64) this fixture WAS
# the TURN-OVER BOUNDARY (n == MAX == BIG, exactly 64 values); after
# M2.61's equal bump the same 64-value union now sits BELOW the cap and
# materializes FLAT (the record machinery never engages — instrumented
# ndef=0 at M2.64), and the boundary itself moved to chain37 (96 values)
# / chain30 (100 values, collapses). The index is the union of two
# 32-value products (R1 = r2 + r3 -> {44..106} step 2, R2 = r2 + r6 ->
# {152..214}), 64 DISTINCT values total — below the cap, so the flat
# union path runs, no records (see the fixture header). Runtime 92 +
# 122 = 214 takes the chain's SIXTY-FOURTH b.eq, so a truncated
# materialization (a store one short, a guard keeping 63) misses it and
# UDF-traps. Dump-verified: 64 b.eq, 65 br, 1 udf, 0 table words. M2.55
# ALSO fixed the freeze half of the M2.54 bug: chain_def_alloc's freeze
# copy (a flat ChainSet at MAX width into the record's BIG-shaped store)
# had no cap check — ASan-proven global-buffer-overflow at the last
# record with BIG < MAX; the guard refuses (-> UNKNOWN, exact-or-
# conservative). The control (BIG=128 > MAX=64, ASan) ran the full ARM
# suite 83/0/3: >MAX records materialize soundly. Byte-invisible under
# equal caps (M2.61 raised both to 96 together); this row's M0 baseline
# (6204, measured at git 1729f50) differs from M1 (4944) by the
# accumulated emission folds (-1260).

# jmpr_chain32 is M2.56: the tracked-register closure at its 8-cap
# (TX_AR_CHAIN_REGS, raised 4 -> 8 in M2.43). M2.43's chain19 pinned the
# 5-register closure; this pin regression-guarded that turn-over
# boundary — a closure needing EXACTLY 8 registers, the largest the cap
# admitted then. M2.62 later raised TX_AR_CHAIN_REGS to 12, so the same
# 8-feeder closure now sits BELOW the cap: the fixture still fires its
# 30-pair chain, and the boundary moved to chain38 (12) / chain39 (13). The index is built in FOUR stages: r4 = r8 + r8 (r8 in
# {0,10}), r2 = r4 + r5 (r5 in {0,10,...,50}), r3 = r6 + r7 (r6 in
# {80,90,100}, r7 in {0,200,400}), r1 = r2 + r3 -> THIRTY distinct
# values {80..170} U {280..370} U {480..570} step 10. The closure
# {r1..r8} = EIGHT registers (<= TX_AR_CHAIN_REGS 12): every feeder is
# tracked and the 30-pair chain fires (8*30+4 = 244 < 40 + 4*572 =
# 2328). Runtime 10+10+50+100+400 = 570 is the LAST of the 30
# candidates and takes the chain's THIRTIETH b.eq, landing on block29's
# LOADI #3000 — a truncated image (29 candidates, or one feeder
# dropped) misses 570 and UDF-traps: the discriminator. Dump-verified:
# 30 b.eq, 31 br, 1 udf, 0 table words. The turn-over control (ad hoc,
# NOT committed): a NINTH feeder (r4 = r8 + r9) makes the closure need
# 9 > 8, so r9 is never tracked and its set stays UNKNOWN -> r1 falls
# to the table — the control PASSes its expected value via the table's
# bounds check with 0 b.eq (conservative and correct). This row's M0
# baseline (12828, measured at git 1729f50) differs from M1 (8448) by
# the accumulated emission folds (-4380).

# jmpr_chain33 is M2.57: the deferred-record pool boundary, now a FLAT
# fixture under the equal caps (see the fixture header for the full
# story). At the M2.57-era caps (DEFS = 64) the index was a 64-record
# DAG: the root product r1 = r2 + r3
# (r2 in {0,20,...,140}, r3 in {0..18 step 2} -> ALL even numbers 0..158,
# 80 DISTINCT values > 64) OVERFLOWS the image merge cap and DEFERS
# (M2.30: record 0, CD_SLOT/CD_SLOT); then 63 IN-PLACE ADDs r1 = r1 + r4
# with r4 = {2} (a singleton — the closure stays {r1,r2,r3,r4} = 4 <= 8)
# each defer over the deferred source (M2.32/M2.41) — 64 records total,
# EXACTLY the pool, all allocating (def = 63 at the dispatch). M2.61's
# EQUAL-CAPS bump (MAX = BIG = 96) changed this row's meaning: the
# root's 80-value true set now FITS the BIG store, so the DAG
# materializes and the chain fires with the 64 in-range candidates
# (even 126..252, < num_instr) — 64 b.eq, runtime 252 = the sixty-
# fourth, PASS 7777 on all four engines. The M2.57-era collapse (0
# b.eq, the table — the M2.42 "vestigial" note) was CAP-SPECIFIC, not
# structural: M2.57's Control A (BIG=128) already proved the same DAG
# chains when the caps admit it, and the 96/96 shipped state is exactly
# that behavior, now regression-guarded. The pool boundary moved to the
# M2.64/M2.65 pins: chain40 (96 records, at-cap) and chain41 (97
# records, exhaustion); chain30 (100 values > 96) still collapses.
# Dump-verified (96/96): 64 b.eq, 0 table words. This row's
# M0 baseline (7468, measured at git 1729f50) differs from M1 (5904)
# by the 64-candidate chain replacing the table and the accumulated
# emission folds (-1564).

# jmpr_chain34 is M2.58: the walk's fixpoint convergence at the
# 64-iteration cap, and the UNSOUNDNESS the probe found. A BACKWARD-
# branch lattice: the loop at LOOP increments r2 by r3 = {1} and
# branches BACK to LOOP, delivering r2's set to its own head. The walk
# is path-insensitive, so the head set grows by ONE value per walk
# iteration ({100}, {100,101}, ...) — an accumulator loop's fixpoint is
# UNBOUNDED, so the walk never converges and the 64-iteration cap
# (iter < 64) stops it at iteration 63. The pre-fix snapshot was the
# post-increment {101..164} and the chain fired over it — but the
# runtime executes the loop 65 times (r5 counts 65 -> 0), giving index
# 165, a legitimate value OUTSIDE the truncated set -> the chain fell
# through to UDF: the ARM engine faulted (rc=1) while interp/x86/rv64
# (no chain analysis) passed 8000. The fix is exact-or-conservative:
# an UNCONVERGED walk (changed still true after the cap) collapses the
# snapshot to UNKNOWN -> the table, which dispatches 165 through its
# bounds check to block32's LOADI #8000 -> PASS 8000 on all four
# engines. Byte-invisible: every existing program's lattice converges
# within the cap (all 85 shared rows byte-identical). Dump-verified:
# 0 b.eq, the full table dispatch. This row's M0 baseline (4796,
# measured at git 1729f50) differs from M1 (4120) by the accumulated
# emission folds (-676).
# jmpr_chain35 is M2.59: the fold fixpoint's RETROACTIVE SPLIT — the
# convergence case the rule-1 comment documents but the corpus never
# tested. A's fold (pc 3, index 5) targets pc 5, INSIDE B's chain
# (source pc 2, target pc 5, B at pc 6): pass 1 folds both, the
# post-pass mark on pc 5 resets the map at T in pass 2, killing B's
# index constant -> B UN-FOLDS, pass 3 confirms (instrumented:
# settled in 3 scans, fold3=5 fold6=-1). The converged state — A
# folded, B the single dynamic JMPR — feeds the walk: g_alloc = 0,
# snapshot {9}, 1-candidate chain (1 b.eq), the M2.58 guard silent
# (forward-only, converges in one iteration). The teeth: disabling
# the rule-1 mark keeps B folded -> g_alloc = 1 -> cache mode -> 984
# bytes, 0 b.eq (control-measured). This row's M0 baseline (1240,
# measured at git 1729f50) differs from M1 (1068) by the split's
# direct-branch fold for A, the 1-candidate chain replacing B's
# table, and the naive-mode frame discipline (-172).
# jmpr_chain36 is M2.60: the fold fixpoint's RELAXATION 2-CYCLE — a
# fold enabled by the M2.18 relaxation that targets the relaxed head
# itself (JMPR J at pc 8 dispatches r2 = 5 back to the relax-eligible
# loop head V at pc 5, whose unique forward edge is S's BR at pc 3 and
# whose predecessor pc 4 is a terminal). The index constant r2 = 5 is
# established BEFORE V, so the reset at the pre-marked head kills it
# unless the relaxation restores it from the S snapshot — exactly
# "a fold enabled by the relaxation": pass 1 relaxes V and folds J,
# pass 2 excludes V (now a fold target) and un-folds J, pass 3 relaxes
# again — a 2-cycle that drives the fixpoint past its 512-pass safety
# net (instrumented: NET FIRED at total pass 513), which restarts with
# the relaxation disabled and terminates at total pass 514 with J
# un-folded (fold8=-1). The post-net state equals the un-relaxed
# analysis (J dynamic, the walk's 1-candidate chain {5} looping back
# to V), so the pin guards TERMINATION, not emission: Control A (head
# not relax-eligible) takes 1 pass, no net; Control B (net disabled,
# timeout-bounded) HANGS on the 2-cycle (exit 124) — the net is the
# termination mechanism. Dump-verified: 1 b (S's BR), 1 b.eq (J's
# chain, backward to V), 1 udf, 0 table words. This row's M0 baseline
# (1180, measured at git 1729f50) differs from M1 (1056) by the
# 1-candidate chain replacing J's runtime table and the accumulated
# emission folds (-124).
# jmpr_chain37 is M2.61: the EQUAL-CAPS regression — MAX and BIG bumped
# together 64 -> 96, pinning the NEW turn-over boundary at exactly 96.
# The index is the union of two flat products (chain31's R-arm shape
# scaled): R1 = r2 + r3 (r2 in {52,68,84,100,116,132}, r3 in
# {0,2,...,14}) = FORTY-EIGHT values {52..146} step 2, and R2 = r2 + r6
# (r6 in {200,202,...,214}) = FORTY-EIGHT values {252..346} step 2;
# the union {52..146} U {252..346} = 96 DISTINCT values — exactly the
# new MAX = BIG. Runtime 132 + 214 = 346 (all join BCs fall through:
# r0 == 0, r5 == 0) takes the chain's NINETY-SIXTH b.eq -> block95's
# LOADI #987. A truncated materialization (a union one short, or a
# store one short) misses 346 and UDF-traps. The bump is monotone in
# capability: sets <= 64 behave byte-identically (all 88 shared rows
# unchanged except jmpr_chain27 and jmpr_chain33, whose 65- and
# 80-value images now fit 96 and chain — chain33 is the M2.57
# Control-A behavior, documented above), and chain30's 100-value
# index still collapses conservatively (100 > 96). Dump-verified:
# 96 b.eq, 0 table words. This row's M0 baseline
# (9348, measured at git 1729f50) differs from M1 (7288) by the
# 96-candidate chain replacing the runtime table and the accumulated
# emission folds (-2060).
# jmpr_chain38 is M2.62: the tracked-register closure AT its NEW 12-cap
# (TX_AR_CHAIN_REGS 8 -> 12, the EQUAL-CAPS regression's register
# dimension). The index is built in six stages over joins: r4 = r8 + r8
# (the self-add tracks r8 ONCE), r5 = r9 + r10, r2 = r4 + r5, r6 = r11 +
# r12, r3 = r6 + r7, r1 = r2 + r3 — the closure {r1..r12} = exactly
# TWELVE registers, the cap: every feeder tracked, and the 33-candidate
# chain fires (the gate: 8*33+4 = 268 < 40 + 4*495 = 2020). Runtime
# 10 + (10+5) + (50+5) + 400 = 490 (all join BCs fall through, r0 == 0)
# = the LAST of the 33 candidates -> block32 at pc 490 -> LOADI #3200,
# on all four engines; a truncated image (32 candidates, or one feeder
# dropped) misses 490 and UDF-traps. The turn-over control (REGS=8 ad
# hoc): the closure stops at 8, the untracked feeders poison r1 to
# UNKNOWN -> 0 b.eq, the table, PASS 3200 through the bounds check
# (9376 bytes, control-measured). Dump-verified (12-cap): 33 b.eq, 0
# table words. This row's M0 baseline (11364, measured at git 1729f50)
# differs from M1 (7624) by the 33-candidate chain replacing the
# runtime table and the accumulated emission folds (-3740).
# jmpr_chain39 is M2.63: the tracked-register closure ONE PAST its
# 12-cap — the OVER-cap side of the M2.62 turn-over. The index is
# chain38's six stages plus an intermediate (r13 = r2 + r6, r1 = r13 +
# r3) and a 2-way r7 join (narrowed so the COMPLETED image fits the
# 96-cap): the closure {r1,r13,r3,r2,r6,r7,r4,r5,r11,r12,r8,r9} =
# exactly TWELVE registers — the scan fills in stream order and the
# ntr < cap guard blocks the THIRTEENTH (r10), so r5 = r9 + r10 has an
# UNKNOWN source and r1 collapses to UNKNOWN: no chain, the runtime
# table (dump: 0 b.eq, cbz bounds-check + br + UDF dispatch, the
# 350-entry table), PASS 3200 through the bounds check. The turn-over
# control (REGS=13 ad hoc): r10 tracked, the image completes at 28
# values <= TX_AR_CHAIN_MAX=96 and the 28-candidate chain fires (dump:
# 28 b.eq, 0 table words, 5344 vs 6556 bytes) — the same fixture, the
# chain shape, proving the boundary discriminates. This row's M0
# baseline (7964, measured at git 1729f50) differs from M1 (6556) by
# the M2.24 table compaction (32-bit entries, 2-word base) and the
# accumulated emission folds (-1408) — the table path itself is
# byte-identical to M0's shape, no chain.
# jmpr_chain40 is M2.64: the deferred-record pool AT its NEW 96-cap
# (TX_AR_CHAIN_DEFS 64 -> 96 — the EQUAL-CAPS regression's third
# dimension, after M2.61's MAX/BIG and M2.62's REGS). The index is a
# 96-record DAG: the root product r1 = r2 + r3 (r2 in {0,10,...,90},
# r3 in {0..9} -> ALL 100 values {0..99} DISTINCT > MAX 96) overflows
# the image merge cap and DEFERS (record 0, the M2.30 path), then 95
# in-place ADDs r1 = r1 + r4 (r4 = {2}) defer (record k = op(rec(k-1),
# slot r4)) — 96 records = TX_AR_CHAIN_DEFS EXACTLY (instrumented:
# ndef = 96, r1's def = 95 at the dispatch). Under the EQUAL caps the
# materialization is conservative: the root's 100-value true set
# overflows the BIG store (chain_merge_big -> UNKNOWN, exact-or-
# conservative), so the dispatch is the TABLE — 0 b.eq, and runtime
# 289 (fall-through arms 90 + 9 plus 95*2, the HUNDREDTH value of
# {190..289}) passes the bounds check to block0 -> LOADI #7777; any
# truncated materialization (96-99 candidates) would miss 289 and
# UDF-trap (rc=1), so the four-way PASS proves the collapse is exact.
# The boundary control (DEFS=95 ad hoc): the 96th allocation returns
# -1 (pool exhausted), r1 falls to FLAT UNKNOWN at the walk — the
# SAME 6324-byte table, a DIFFERENT mechanism (walk-state UNKNOWN vs
# materialization collapse), proven by the walk state (ndef=95 vs 96).
# The M2.57 64-record boundary is a fossil: chain33's root (80 values)
# stays FLAT at MAX=96 and never touches the pool (instrumented ndef
# = 0) — the pool is a pure ceiling, all 91 shared rows byte-
# identical. This row's M0 baseline (7508, measured at git 1729f50)
# differs from M1 (6324) by the M2.24 table compaction and the
# accumulated emission folds (-1184) — the table path is byte-
# identical to M0's shape, no chain.
# jmpr_chain41 is M2.65: the deferred-record pool ONE PAST its 96-cap —
# the OVER-cap side of the M2.64 turn-over (chain40 + ONE in-place
# add: the root's 100-value product defers as record 0, then 96 adds
# r1 = r1 + r4 defer — 97 records needed). At TX_AR_CHAIN_DEFS = 96
# the 97th allocation returns -1 (pool exhausted): r1 falls to FLAT
# UNKNOWN at the walk (instrumented: ndef = 96, r1's def = -1 — vs
# chain40's def = 95, the fits-and-materializes side) — the dispatch
# is the TABLE, 0 b.eq, and runtime 291 (fall-through arms 90 + 9
# plus 96*2, the 100th value of {192..291}) passes the bounds check
# to block0 -> LOADI #8888 on all four engines; a non-conservative
# exhaustion fallback (a truncated partial image) would miss 291 and
# UDF-trap (rc=1), so the four-way PASS proves the -1 fallback is
# exact-or-conservative. The boundary control (DEFS=97 ad hoc): the
# 97-record DAG FITS the pool (ndef = 97, r1's def = 96) and the
# 100-value materialization then collapses at the BIG store — the
# SAME 6364-byte table, a DIFFERENT mechanism (walk-state exhaustion
# vs materialization collapse), proven by the walk state. Together
# chain40/chain41 bracket the DEFS boundary: at-cap fits, one-past
# exhausts conservatively. Pure pin: simi_arm.c unchanged, all 92
# shared rows byte-identical. This row's M0 baseline (7556, measured
# at git 1729f50) differs from M1 (6364) by the M2.24 table
# compaction and the accumulated emission folds (-1192) — the table
# path is byte-identical to M0's shape, no chain.
# jmpr_chain42 is M2.66: the deferred-UNION path probed at the EQUAL
# caps — the structural question of whether any UNION record can
# survive at MAX = BIG = 96. The shape is chain13's skeleton scaled
# so the deferred side is a > 96-value product: R1 = r2 + r3 (r2 in
# {0,10,...,90}, r3 in {0..9} -> all 100 values {0..99} distinct >
# MAX 96) defers as record 0; a BC (r5 = 0, not taken) delivers R1
# at J1 while the flat arm writes r1 = #200 — OUTSIDE the set — so
# the M2.36 containment test fails and the M2.37 union path runs:
# chain_def_alloc_union flattens its record side, rec0's 100-value
# true set OVERFLOWS the BIG store (chain_merge_big -> UNKNOWN), the
# allocator returns -1, r1 falls to FLAT UNKNOWN at the walk
# (instrumented: ndef = 1, r1's def = -1) — the dispatch is the
# TABLE, 0 b.eq, and runtime 232 (99 at the root, the flat arm's
# 200, then +32) passes the bounds check to block0 -> LOADI #4200 on
# all four engines. The control (BIG=128 ad hoc, the M2.55
# direction): rec0's materialization fits the store, the union
# record IS created (ndef = 3, r1's def = 2), and the dispatch
# materializes {32..131} U {232} = 101 candidates -> the 101-pair
# chain FIRES (dump: 101 b.eq, 0 table words, 4480 vs 4656 bytes),
# runtime 232 = the LAST b.eq. The probe proves the cap check is the
# binding constraint: the union path is REACHED, and the allocator's
# BIG-store check is what makes it unreachable at equal caps. 232 is
# the 101st candidate — any truncated materialization misses it and
# UDF-traps, so the four-way PASS proves the collapse is exact.
# Pure pin: simi_arm.c unchanged, all 93 shared rows byte-identical.
# This row's M0 baseline (5612, measured at git 1729f50) differs from
# M1 (4656) by the M2.24 table compaction and the accumulated
# emission folds (-956) — the table path is byte-identical to M0's
# shape, no chain.
# jmpr_chain43 is M2.69: the fold fixpoint's retroactive-split CASCADE —
# the largest constructible shape for the "many scans" question. SIX
# JMPRs: J_k folds (pass 1) to T_k, where T_k sits INSIDE J_{k+1}'s
# chain, so the pass-1 rule-1 mark at T_k resets r_{k+1} in pass 2. The
# structural argument: g_pc_target only ACCRETES inside the fixpoint, so
# the fold set is monotone-decreasing and every pass-1 mark is already
# live in pass 2 — all five dependent JMPRs un-fold in the SAME pass,
# convergence is exactly 3 scans (fold-all, un-fold-all, confirm) for
# any RULE-1-driven cascade size, and the pass count never scales with
# the number of JMPRs. The M2.18 relaxation's only non-monotone force
# is the head-reentry 2-cycle (chain36's pin) — and M2.70 (chain44)
# corrects the "ANY": when a fold INTO a relax-eligible head un-folds,
# the head's JMPR re-folds once (a legitimate 4th scan, passes=3). The
# 2-cycle is the only DIVERGENCE, so the safety net fires only there;
# a slow-converging fold analysis still does not exist.
# Instrumented (temporary, reverted): chain43 FIXPOINT passes=2 relax=1
# net=0 (3 scans, net silent); chain36 at the same level FIXPOINT
# passes=0 relax=0 net=1 — the net fired at 513, disabled relaxation,
# cleared the fold set, and the un-relaxed restart converged immediately
# (the restart-and-terminate behavior, still PASSing = 10).
# Emission (dump-verified): J1's fold SURVIVES (its chain (1, 7)
# contains no mark) — 1 direct `b`, 0 b.eq; J2..J6 went dynamic -> the
# table (5 br x11 dispatches) — multiple dynamic JMPRs mean no inline
# chain, so
# g_alloc = 0 and the naive path emits the compacted table. Runtime
# 9 -> 12 -> 15 -> 18 -> 21 -> 24 -> r0 = 1 on all four engines.
# This row's M0 baseline (1748, measured at git 1729f50 — the naive
# translator, all 6 JMPRs via table) differs from M1 (1556) by the
# M2.24 table compaction and the accumulated naive-path emission folds
# (-192) — the cascade's un-folds are the point, not the size: the row
# pins the 3-scan convergence (teeth: reverting the rule-1 mark keeps
# J2..J6 folded -> g_alloc = 1 -> cache mode -> a different emission).
# jmpr_chain44 is M2.70: the fold fixpoint's RELAXATION-FLIP re-fold —
# the legitimate convergence case that CORRECTS the M2.69 "exactly 3
# scans for ANY cascade" bound. The flip: J2 (pc 8) folds forward INTO
# J3's relax-eligible head H3 (pc 10) in pass 1, while J1 (pc 4) folds
# to T1 = 6 inside J2's chain. Pass 2: T1's rule-1 mark kills r2
# PERMANENTLY (J2 un-folds — its fold to 10 leaves the fold set); H3's
# per-pass exclusion (g_fold_tgt_prev, recomputed from the PREVIOUS
# pass's fold set) kills r3 at H3 -> J3 un-folds. Pass 3: J2's fold to
# 10 is GONE from pass 2's fold set, so H3 is no longer excluded -> the
# relaxation restores r3 = 12 at H3 -> J3 RE-FOLDS (an un-fold ENABLED a
# re-fold). Pass 4: same -> converge. Instrumented: FIXPOINT passes=3
# relax=1 net=0 — 4 scans, the net NEVER fires (it stays silent on all
# legitimate input; the corpus max is passes=2). J3's re-fold is STABLE
# (its target 12 is fall-through, not the head 10 — no 2-cycle); the
# flip is a one-time transient, so the legitimate max is passes=3.
# M2.70 tightens the safety net 512 -> 16 (5x margin over passes=3,
# 8x over the corpus max) — the 2-cycle (chain36) still fires, now at
# pass 17 instead of 513 (measured 28.6x fewer fixpoint scans, ~8-9x
# end-to-end translate on chain36 — bench_net, M2.71), with the same
# un-relaxed restart and the same emission.
# Emission: J1 and J3 fold (final state), J2 is the single dynamic JMPR
# -> the M2.25 1-candidate inline chain (cmp + b.eq for 10); 0 table
# words. Runtime 6 -> 10 -> 12 -> r0 = 1 on all four engines (the BR at
# pc 5 and the RET at pc 9 are dead). This row's M0 baseline (1332,
# measured at git 1729f50 — the naive translator, all 3 JMPRs via
# table) differs from M1 (1088) by the M2.24 table compaction, the two
# folds, and the inline chain (-244). Teeth: making H3 non-eligible
# (H3-1 = ADD instead of RET) keeps J3 folded from pass 1 -> a
# different emission, caught by the row.

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
