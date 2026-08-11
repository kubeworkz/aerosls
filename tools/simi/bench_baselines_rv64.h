/* bench_baselines_rv64.h — M2.76/M2.77: the COMMITTED per-fixture
 * execution baselines for the RV64 leg, the single source of truth for
 * two consumers:
 *
 *   - simi_riscv_verify.c — the parity harness's `--steps` check (M2.76):
 *     after a successful run, the executed instruction count
 *     (RvCpu.steps) must equal the committed count. This catches decode/
 *     emission regressions at the parity harness, before any bench runs.
 *   - bench_exec_rv64.c — the RV64 execution-cost gate (M2.77): per-row
 *     steps EXACT + ns/call at a 4x margin, the symmetric counterpart of
 *     bench_exec (the A64 side).
 *
 * The steps are deterministic and machine-independent: rv64_exec_run()
 * increments RvCpu.steps once per decoded instruction, so the same
 * translated bytes over zeroed guest state always execute the same
 * count — the EXACT guard. ns_per_call is the wall-clock median of
 * three N=500 passes (measured 2026-08-09 on this sandbox) and is only
 * used by bench_exec_rv64, asserted at a 4x margin (three passes swung
 * up to 2.37x per fixture on this sandbox — the same WSL load tail
 * bench_exec documented; see bench_exec_rv64.c's top comment). Keys are
 * "<fixture>.simi" — the runner derives the name from the .tmo path.
 * The counts are for the "main" entry's full run.
 *
 * A fixture with no row FAILS loudly in both consumers — adding or
 * removing a corpus fixture requires updating this table deliberately
 * (the size gate's M0_BASELINES discipline). Re-measure with
 * RV64_STEPS_MEASURE=1 ./tests/run_riscv_tests.sh (steps) or
 * RV64_BENCH_MEASURE=1 ./bench-exec-rv64 <tests/ fixtures> 500 (ns).
 * See plan doc §10.162/10.163 and §10.164/10.165.
 */
#ifndef BENCH_BASELINES_RV64_H
#define BENCH_BASELINES_RV64_H

struct Rv64Baseline {
    const char* name;         /* <fixture>.simi */
    long long steps;          /* EXACT execution work — the tight guard */
    double ns_per_call;       /* median of 3 N=500 passes (bench_exec_rv64 only) */
};

static const struct Rv64Baseline RV64_BASELINES[] = {
    { "add.simi", 225, 5321 },
    { "aggregate_abi.simi", 1110, 16823 },
    { "alu_imm.simi", 268, 5909 },
    { "alu_imm_ext.simi", 274, 5798 },
    { "atomic_add.simi", 517, 10244 },
    { "branch_cmp.simi", 244, 5472 },
    { "call_ret.simi", 454, 8753 },
    { "cap_call_ret.simi", 749, 13089 },
    { "cap_forge.simi", 298, 5249 },
    { "cas_simple.simi", 343, 7352 },
    { "dead_reuse.simi", 282, 4807 },
    { "epi_merge.simi", 245, 4318 },
    { "epi_merge2.simi", 249, 5520 },
    { "epi_merge3.simi", 253, 5371 },
    { "epi_merge4.simi", 247, 4526 },
    { "epi_merge5.simi", 251, 5692 },
    { "epi_merge6.simi", 249, 5855 },
    { "extra_ops.simi", 266, 5512 },
    { "fetch_cross.simi", 286, 5902 },
    { "float_ops.simi", 557, 5717 },
    { "jmpr_basic.simi", 226, 5241 },
    { "jmpr_calc.simi", 261, 5798 },
    { "jmpr_calc_bit.simi", 273, 4914 },
    { "jmpr_calc_mul.simi", 247, 4415 },
    { "jmpr_callret.simi", 461, 8387 },
    { "jmpr_callret_arg.simi", 478, 6047 },
    { "jmpr_chain.simi", 236, 5554 },
    { "jmpr_chain10.simi", 290, 6274 },
    { "jmpr_chain11.simi", 296, 5043 },
    { "jmpr_chain12.simi", 296, 6349 },
    { "jmpr_chain13.simi", 300, 6182 },
    { "jmpr_chain14.simi", 314, 4135 },
    { "jmpr_chain15.simi", 321, 6219 },
    { "jmpr_chain16.simi", 350, 6737 },
    { "jmpr_chain17.simi", 291, 6669 },
    { "jmpr_chain18.simi", 315, 6355 },
    { "jmpr_chain19.simi", 290, 6267 },
    { "jmpr_chain2.simi", 245, 5187 },
    { "jmpr_chain20.simi", 287, 4866 },
    { "jmpr_chain21.simi", 287, 4955 },
    { "jmpr_chain22.simi", 290, 6072 },
    { "jmpr_chain23.simi", 290, 5078 },
    { "jmpr_chain24.simi", 288, 4645 },
    { "jmpr_chain25.simi", 263, 5002 },
    { "jmpr_chain26.simi", 288, 5345 },
    { "jmpr_chain27.simi", 618, 9477 },
    { "jmpr_chain28.simi", 292, 4651 },
    { "jmpr_chain29.simi", 351, 6109 },
    { "jmpr_chain3.simi", 242, 5583 },
    { "jmpr_chain30.simi", 347, 6746 },
    { "jmpr_chain31.simi", 356, 4791 },
    { "jmpr_chain32.simi", 322, 6568 },
    { "jmpr_chain33.simi", 654, 7151 },
    { "jmpr_chain34.simi", 1083, 16133 },
    { "jmpr_chain35.simi", 247, 5107 },
    { "jmpr_chain36.simi", 277, 6176 },
    { "jmpr_chain37.simi", 368, 4201 },
    { "jmpr_chain38.simi", 322, 6854 },
    { "jmpr_chain39.simi", 321, 4964 },
    { "jmpr_chain4.simi", 242, 4052 },
    { "jmpr_chain40.simi", 826, 9871 },
    { "jmpr_chain41.simi", 831, 9801 },
    { "jmpr_chain42.simi", 366, 5167 },
    { "jmpr_chain43.simi", 328, 6289 },
    { "jmpr_chain44.simi", 274, 5794 },
    { "jmpr_chain5.simi", 269, 5555 },
    { "jmpr_chain6.simi", 275, 4747 },
    { "jmpr_chain7.simi", 275, 5888 },
    { "jmpr_chain8.simi", 290, 6018 },
    { "jmpr_chain9.simi", 281, 4323 },
    { "jmpr_cross.simi", 252, 5307 },
    { "jmpr_deadfull.simi", 478, 8618 },
    { "jmpr_deadmult.simi", 478, 8581 },
    { "jmpr_dyn.simi", 237, 4321 },
    { "jmpr_fall.simi", 246, 4342 },
    { "jmpr_fall2.simi", 260, 5422 },
    { "jmpr_foldreach.simi", 501, 8691 },
    { "jmpr_join.simi", 232, 5318 },
    { "jmpr_mid.simi", 246, 4969 },
    { "jmpr_mix.simi", 256, 4608 },
    { "jmpr_table.simi", 262, 5367 },
    { "jmpr_unreach.simi", 478, 8424 },
    { "lcg_fairness.simi", 8700265, 91445308 },
    { "loadi64.simi", 221, 5043 },
    { "loop_sum.simi", 411, 7277 },
    { "mem_neg.simi", 297, 6351 },
    { "mem_ops_native.simi", 243, 5557 },
    { "mem_pre.simi", 421, 7585 },
    { "mem_reg.simi", 392, 6577 },
    { "obj_ops.simi", 264, 4801 },
    { "ptr_ops.simi", 259, 4579 },
    { "rd_star.simi", 262, 5610 },
    { "rv64_boot_smoke.simi", 212, 5294 },
    { "rv64_float_smoke_a.simi", 224, 5730 },
    { "rv64_float_smoke_b.simi", 220, 3648 },
    { "src_resident.simi", 261, 5454 },
    { "straight_line_bench.simi", 261, 4360 },
    { "stress_atomics.simi", 259, 6067 },
    { "tail_ret.simi", 224, 5145 },
};
/* 98 rows — steps EXACT (cross-checked against the parity table's
 * counts), ns/call = median of three N=500 passes (measured 2026-08-09
 * on this sandbox). */

#endif /* BENCH_BASELINES_RV64_H */
