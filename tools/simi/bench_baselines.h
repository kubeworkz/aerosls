/* bench_baselines.h — M2.75: the COMMITTED per-fixture execution
 * baselines, single source of truth for two consumers:
 *
 *   - bench_exec.c — the M2.74 execution-cost gate (per-row steps EXACT
 *     + ns/call at a 4x margin);
 *   - simi_arm_verify.c — the parity harness's `--steps` check (M2.75):
 *     after a successful run, the fixture's executed instruction count
 *     (A64Cpu.steps) must equal the committed count below. This catches
 *     decode regressions AT the parity harness — before any bench runs
 *     — because a change in how much the translated code executes
 *     (a decode that now faults early, a fold that changes the emitted
 *     control flow, an a64_exec regression) moves the count.
 *
 * The steps are deterministic and machine-independent: the same
 * translated bytes over zeroed guest state always execute the same
 * count, so they are the EXACT guard. ns_per_call is the wall-clock
 * median of three N=500 passes (measured 2026-08-09 on this sandbox)
 * and is only used by bench_exec, asserted at a 4x margin (the
 * execution wall-clock tail on this sandbox reached 2.28x per fixture —
 * see bench_exec.c's top comment). Keys are "<fixture>.simi" — the
 * runner derives the name from the .tmo path.
 *
 * A fixture with no row FAILS loudly in both consumers — adding or
 * removing a corpus fixture requires updating this table deliberately
 * (the size gate's M0_BASELINES discipline). See plan doc
 * §10.158/10.159 and §10.160/10.161.
 */
#ifndef BENCH_BASELINES_H
#define BENCH_BASELINES_H

#include <stddef.h>   /* (unused directly; kept for self-containment) */

struct FixtureBaseline {
    const char* name;         /* <fixture>.simi */
    long long steps;          /* EXACT execution work — the tight guard */
    double ns_per_call;       /* median of 3 N=500 passes (bench_exec only) */
};

static const struct FixtureBaseline BASELINES[] = {
    { "add.simi", 241, 4858 },
    { "aggregate_abi.simi", 1108, 16414 },
    { "alu_imm.simi", 265, 5896 },
    { "alu_imm_ext.simi", 274, 6944 },
    { "atomic_add.simi", 541, 9630 },
    { "branch_cmp.simi", 259, 6410 },
    { "call_ret.simi", 484, 9065 },
    { "cap_call_ret.simi", 796, 12422 },
    { "cap_forge.simi", 315, 6338 },
    { "cas_simple.simi", 340, 5897 },
    { "dead_reuse.simi", 274, 5703 },
    { "epi_merge.simi", 251, 5812 },
    { "epi_merge2.simi", 255, 5769 },
    { "epi_merge3.simi", 258, 5923 },
    { "epi_merge4.simi", 251, 5086 },
    { "epi_merge5.simi", 255, 6187 },
    { "epi_merge6.simi", 252, 5728 },
    { "extra_ops.simi", 277, 5207 },
    { "fetch_cross.simi", 278, 5660 },
    { "float_ops.simi", 539, 8600 },
    { "jmpr_basic.simi", 236, 5492 },
    { "jmpr_calc.simi", 253, 5561 },
    { "jmpr_calc_bit.simi", 263, 5844 },
    { "jmpr_calc_mul.simi", 246, 4925 },
    { "jmpr_callret.simi", 478, 7006 },
    { "jmpr_callret_arg.simi", 498, 8542 },
    { "jmpr_chain.simi", 248, 5476 },
    { "jmpr_chain10.simi", 350, 4945 },
    { "jmpr_chain11.simi", 355, 6882 },
    { "jmpr_chain12.simi", 355, 6781 },
    { "jmpr_chain13.simi", 360, 5848 },
    { "jmpr_chain14.simi", 370, 7012 },
    { "jmpr_chain15.simi", 377, 7323 },
    { "jmpr_chain16.simi", 420, 8549 },
    { "jmpr_chain17.simi", 330, 5272 },
    { "jmpr_chain18.simi", 390, 6052 },
    { "jmpr_chain19.simi", 330, 3819 },
    { "jmpr_chain2.simi", 256, 5844 },
    { "jmpr_chain20.simi", 327, 6739 },
    { "jmpr_chain21.simi", 327, 7448 },
    { "jmpr_chain22.simi", 330, 6800 },
    { "jmpr_chain23.simi", 309, 5593 },
    { "jmpr_chain24.simi", 338, 4998 },
    { "jmpr_chain25.simi", 273, 5708 },
    { "jmpr_chain26.simi", 338, 6734 },
    { "jmpr_chain27.simi", 888, 11836 },
    { "jmpr_chain28.simi", 312, 6650 },
    { "jmpr_chain29.simi", 403, 5586 },
    { "jmpr_chain3.simi", 253, 5686 },
    { "jmpr_chain30.simi", 346, 7032 },
    { "jmpr_chain31.simi", 473, 6005 },
    { "jmpr_chain32.simi", 378, 7037 },
    { "jmpr_chain33.simi", 773, 12266 },
    { "jmpr_chain34.simi", 1034, 15039 },
    { "jmpr_chain35.simi", 249, 5961 },
    { "jmpr_chain36.simi", 278, 5194 },
    { "jmpr_chain37.simi", 547, 5542 },
    { "jmpr_chain38.simi", 385, 3564 },
    { "jmpr_chain39.simi", 328, 6650 },
    { "jmpr_chain4.simi", 253, 5474 },
    { "jmpr_chain40.simi", 824, 9932 },
    { "jmpr_chain41.simi", 829, 9472 },
    { "jmpr_chain42.simi", 362, 6946 },
    { "jmpr_chain43.simi", 328, 3964 },
    { "jmpr_chain44.simi", 263, 5861 },
    { "jmpr_chain5.simi", 292, 4608 },
    { "jmpr_chain6.simi", 307, 6435 },
    { "jmpr_chain7.simi", 307, 6203 },
    { "jmpr_chain8.simi", 350, 6829 },
    { "jmpr_chain9.simi", 312, 5581 },
    { "jmpr_cross.simi", 247, 5796 },
    { "jmpr_deadfull.simi", 498, 8302 },
    { "jmpr_deadmult.simi", 498, 8710 },
    { "jmpr_dyn.simi", 252, 5661 },
    { "jmpr_fall.simi", 245, 4483 },
    { "jmpr_fall2.simi", 245, 5339 },
    { "jmpr_foldreach.simi", 518, 8785 },
    { "jmpr_join.simi", 243, 4603 },
    { "jmpr_mid.simi", 248, 5579 },
    { "jmpr_mix.simi", 261, 6050 },
    { "jmpr_table.simi", 272, 4223 },
    { "jmpr_unreach.simi", 498, 8481 },
    { "lcg_fairness.simi", 6700281, 80012240 },
    { "loadi64.simi", 239, 5524 },
    { "loop_sum.simi", 439, 7377 },
    { "mem_neg.simi", 294, 6010 },
    { "mem_ops_native.simi", 251, 5778 },
    { "mem_pre.simi", 368, 5296 },
    { "mem_reg.simi", 360, 5273 },
    { "obj_ops.simi", 287, 6098 },
    { "ptr_ops.simi", 275, 5902 },
    { "rd_star.simi", 264, 4656 },
    { "rv64_boot_smoke.simi", 232, 5562 },
    { "rv64_float_smoke_a.simi", 242, 5731 },
    { "rv64_float_smoke_b.simi", 240, 7533 },
    { "src_resident.simi", 264, 4990 },
    { "straight_line_bench.simi", 275, 5942 },
    { "stress_atomics.simi", 272, 5477 },
    { "tail_ret.simi", 243, 5658 },
};
/* 98 rows — the "main" entry's full run (the count for the entry the
 * runners execute; other entries in the same .tmo would differ). */

#endif /* BENCH_BASELINES_H */
