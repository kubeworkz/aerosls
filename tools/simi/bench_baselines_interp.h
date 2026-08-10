/* bench_baselines_interp.h — M2.80: the COMMITTED per-fixture execution
 * baselines for the INTERPRETER leg — the reference interpreter's own
 * execution cost, the last leg of the four-way measurement story.
 * Single source of truth for bench_exec_interp.c (the execution-cost
 * gate, M2.80): per-row steps EXACT + ns/call at a 4x margin, the same
 * two-tier model as the A64/RV64 benches.
 *
 * The steps are the SIMI-instruction count the reference interpreter
 * executes per fixture (simi_interp_run's step counter — the same loop
 * simi-run uses), deterministic and machine-independent: the same .tmo
 * over zeroed memory always executes the same count. This is the
 * GROUND-TRUTH work count — the interpreter executes the raw SIMI
 * stream, no translation — the translated ISAs' counts
 * (bench_baselines.h A64 / bench_baselines_rv64.h) are compared against
 * by the cross-ISA reports. ns_per_call is the wall-clock median of
 * three N=500 passes (measured 2026-08-09 on this sandbox), asserted at
 * a 4x margin (the same WSL load tail the A64/RV64 benches documented).
 *
 * The fixture set is run_tests.sh's: every fixture with an "Expected
 * result:" comment — INCLUDING mem_ops.simi (the address-0 pointer is a
 * Phase 1 interpreter-only convenience the translated legs replace with
 * mem_ops_native), so this table carries 97 rows to the translated
 * legs' 96. jmpr_oob (faults by design) and cap_forge_debug (no
 * expected result) are skipped, exactly as the runner skips them. Keys
 * are "<fixture>.simi"; counts are for the "main" entry's full run.
 *
 * A fixture with no row FAILS loudly — adding or removing a corpus
 * fixture requires updating this table deliberately (the size gate's
 * M0_BASELINES discipline). Re-measure with
 * SIMI_BENCH_MEASURE=1 ./bench-exec-interp <tests/ fixtures> 500.
 * See plan doc §10.170/10.171.
 */
#ifndef BENCH_BASELINES_INTERP_H
#define BENCH_BASELINES_INTERP_H

struct InterpBaseline {
    const char* name;         /* <fixture>.simi */
    long long steps;          /* EXACT SIMI instructions executed */
    double ns_per_call;       /* median of 3 N=500 passes (bench_exec_interp only) */
};

static const struct InterpBaseline INTERP_BASELINES[] = {
    { "add.simi", 6, 3218 },
    { "aggregate_abi.simi", 52, 3883 },
    { "alu_imm.simi", 13, 3166 },
    { "alu_imm_ext.simi", 14, 3022 },
    { "atomic_add.simi", 77, 3287 },
    { "branch_cmp.simi", 12, 2715 },
    { "call_ret.simi", 8, 3350 },
    { "cap_call_ret.simi", 19, 3962 },
    { "cap_forge.simi", 17, 3279 },
    { "cas_simple.simi", 27, 3204 },
    { "dead_reuse.simi", 18, 2848 },
    { "epi_merge.simi", 10, 3106 },
    { "epi_merge2.simi", 11, 3258 },
    { "epi_merge3.simi", 12, 3113 },
    { "epi_merge4.simi", 10, 3302 },
    { "epi_merge5.simi", 11, 2888 },
    { "epi_merge6.simi", 10, 3276 },
    { "extra_ops.simi", 14, 3188 },
    { "fetch_cross.simi", 18, 3167 },
    { "float_ops.simi", 61, 3291 },
    { "jmpr_basic.simi", 5, 3247 },
    { "jmpr_calc.simi", 12, 3140 },
    { "jmpr_calc_bit.simi", 14, 3096 },
    { "jmpr_calc_mul.simi", 9, 2899 },
    { "jmpr_callret.simi", 8, 3526 },
    { "jmpr_callret_arg.simi", 12, 3788 },
    { "jmpr_chain.simi", 8, 3173 },
    { "jmpr_chain10.simi", 24, 3307 },
    { "jmpr_chain11.simi", 26, 2598 },
    { "jmpr_chain12.simi", 26, 2634 },
    { "jmpr_chain13.simi", 27, 2592 },
    { "jmpr_chain14.simi", 32, 2436 },
    { "jmpr_chain15.simi", 34, 2565 },
    { "jmpr_chain16.simi", 44, 2888 },
    { "jmpr_chain17.simi", 25, 2718 },
    { "jmpr_chain18.simi", 33, 2652 },
    { "jmpr_chain19.simi", 24, 3176 },
    { "jmpr_chain2.simi", 10, 3130 },
    { "jmpr_chain20.simi", 23, 2615 },
    { "jmpr_chain21.simi", 23, 2597 },
    { "jmpr_chain22.simi", 24, 2606 },
    { "jmpr_chain23.simi", 25, 2476 },
    { "jmpr_chain24.simi", 25, 2589 },
    { "jmpr_chain25.simi", 16, 3202 },
    { "jmpr_chain26.simi", 25, 2757 },
    { "jmpr_chain27.simi", 135, 3086 },
    { "jmpr_chain28.simi", 26, 2514 },
    { "jmpr_chain29.simi", 45, 2631 },
    { "jmpr_chain3.simi", 9, 3153 },
    { "jmpr_chain30.simi", 44, 2680 },
    { "jmpr_chain31.simi", 46, 2614 },
    { "jmpr_chain32.simi", 33, 2453 },
    { "jmpr_chain33.simi", 104, 3624 },
    { "jmpr_chain34.simi", 203, 4037 },
    { "jmpr_chain35.simi", 8, 3113 },
    { "jmpr_chain36.simi", 16, 3260 },
    { "jmpr_chain37.simi", 50, 2745 },
    { "jmpr_chain38.simi", 31, 2685 },
    { "jmpr_chain39.simi", 30, 2840 },
    { "jmpr_chain4.simi", 9, 3072 },
    { "jmpr_chain40.simi", 140, 3763 },
    { "jmpr_chain41.simi", 141, 3830 },
    { "jmpr_chain42.simi", 49, 2836 },
    { "jmpr_chain43.simi", 20, 2677 },
    { "jmpr_chain44.simi", 12, 3095 },
    { "jmpr_chain5.simi", 18, 3058 },
    { "jmpr_chain6.simi", 20, 2797 },
    { "jmpr_chain7.simi", 20, 3169 },
    { "jmpr_chain8.simi", 24, 1515 },
    { "jmpr_chain9.simi", 21, 3077 },
    { "jmpr_cross.simi", 9, 3289 },
    { "jmpr_deadfull.simi", 12, 3398 },
    { "jmpr_deadmult.simi", 12, 3250 },
    { "jmpr_dyn.simi", 7, 2666 },
    { "jmpr_fall.simi", 9, 3113 },
    { "jmpr_fall2.simi", 10, 2716 },
    { "jmpr_foldreach.simi", 16, 3414 },
    { "jmpr_join.simi", 7, 2860 },
    { "jmpr_mid.simi", 9, 3005 },
    { "jmpr_mix.simi", 10, 3133 },
    { "jmpr_table.simi", 11, 2633 },
    { "jmpr_unreach.simi", 12, 3484 },
    { "loadi64.simi", 5, 2513 },
    { "loop_sum.simi", 58, 3142 },
    { "mem_neg.simi", 18, 2675 },
    { "mem_ops.simi", 10, 2983 },
    { "mem_ops_native.simi", 9, 3130 },
    { "mem_pre.simi", 42, 3096 },
    { "mem_reg.simi", 34, 2765 },
    { "obj_ops.simi", 10, 2978 },
    { "ptr_ops.simi", 13, 3011 },
    { "rd_star.simi", 14, 2874 },
    { "rv64_boot_smoke.simi", 3, 2530 },
    { "rv64_float_smoke_a.simi", 5, 3107 },
    { "rv64_float_smoke_b.simi", 4, 3411 },
    { "src_resident.simi", 13, 2714 },
    { "straight_line_bench.simi", 14, 3128 },
    { "stress_atomics.simi", 13, 2898 },
    { "tail_ret.simi", 7, 2915 },
};
/* 99 rows — steps EXACT (the reference interpreter's own SIMI count,
 * cross-checked identical across three measure passes), ns/call = median
 * of three N=500 passes (measured 2026-08-09 on this sandbox). The
 * counts are tiny vs the translated legs' (corpus 2595 SIMI steps vs
 * A64 35032 / RV64 32820): the interpreter executes the raw SIMI stream
 * — the ~13x expansion is the ISA-level work the translators emit. */

#endif /* BENCH_BASELINES_INTERP_H */
