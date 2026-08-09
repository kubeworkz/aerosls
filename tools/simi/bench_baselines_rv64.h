/* bench_baselines_rv64.h — M2.76: the COMMITTED per-fixture execution
 * step counts for the RV64 leg, the single source of truth for the
 * simi-riscv-verify `--steps` check (the parity harness's RV64 leg).
 *
 * The counts are deterministic and machine-independent: rv64_exec_run()
 * increments RvCpu.steps once per decoded instruction (mirroring
 * a64_exec's A64Cpu.steps — the same design as the M2.75 ARM tripwire),
 * so the same translated bytes over zeroed guest state always execute
 * the same count. ANY change in how much the translated RV64 code
 * executes — a decode that now faults early, an emission change that
 * alters the control flow, an rv64_exec regression — moves the count
 * and fails the fixture in run_riscv_tests.sh, at the parity harness,
 * before any bench runs.
 *
 * Keys are "<fixture>.simi" — the runner derives the name from the .tmo
 * path. The counts are for the "main" entry's full run. A fixture with
 * no row FAILS loudly (adding/removing a corpus fixture requires
 * updating this table deliberately). Re-measure with
 * RV64_STEPS_MEASURE=1 ./tests/run_riscv_tests.sh (the harness prints
 * the measured count per fixture without asserting). See plan doc
 * §10.162/10.163.
 */
#ifndef BENCH_BASELINES_RV64_H
#define BENCH_BASELINES_RV64_H

struct Rv64Baseline {
    const char* name;         /* <fixture>.simi */
    long long steps;          /* EXACT execution work — the tight guard */
};

static const struct Rv64Baseline RV64_BASELINES[] = {
    { "add.simi", 225 },
    { "aggregate_abi.simi", 1110 },
    { "alu_imm.simi", 268 },
    { "alu_imm_ext.simi", 274 },
    { "atomic_add.simi", 517 },
    { "branch_cmp.simi", 244 },
    { "call_ret.simi", 454 },
    { "cap_call_ret.simi", 749 },
    { "cap_forge.simi", 298 },
    { "cas_simple.simi", 343 },
    { "dead_reuse.simi", 282 },
    { "epi_merge.simi", 245 },
    { "epi_merge2.simi", 249 },
    { "epi_merge3.simi", 253 },
    { "epi_merge4.simi", 247 },
    { "epi_merge5.simi", 251 },
    { "epi_merge6.simi", 249 },
    { "extra_ops.simi", 266 },
    { "fetch_cross.simi", 286 },
    { "float_ops.simi", 557 },
    { "jmpr_basic.simi", 226 },
    { "jmpr_calc.simi", 261 },
    { "jmpr_calc_bit.simi", 273 },
    { "jmpr_calc_mul.simi", 247 },
    { "jmpr_callret.simi", 461 },
    { "jmpr_callret_arg.simi", 478 },
    { "jmpr_chain.simi", 236 },
    { "jmpr_chain10.simi", 290 },
    { "jmpr_chain11.simi", 296 },
    { "jmpr_chain12.simi", 296 },
    { "jmpr_chain13.simi", 300 },
    { "jmpr_chain14.simi", 314 },
    { "jmpr_chain15.simi", 321 },
    { "jmpr_chain16.simi", 350 },
    { "jmpr_chain17.simi", 291 },
    { "jmpr_chain18.simi", 315 },
    { "jmpr_chain19.simi", 290 },
    { "jmpr_chain2.simi", 245 },
    { "jmpr_chain20.simi", 287 },
    { "jmpr_chain21.simi", 287 },
    { "jmpr_chain22.simi", 290 },
    { "jmpr_chain23.simi", 290 },
    { "jmpr_chain24.simi", 288 },
    { "jmpr_chain25.simi", 263 },
    { "jmpr_chain26.simi", 288 },
    { "jmpr_chain27.simi", 618 },
    { "jmpr_chain28.simi", 292 },
    { "jmpr_chain29.simi", 351 },
    { "jmpr_chain3.simi", 242 },
    { "jmpr_chain30.simi", 347 },
    { "jmpr_chain31.simi", 356 },
    { "jmpr_chain32.simi", 322 },
    { "jmpr_chain33.simi", 654 },
    { "jmpr_chain34.simi", 1083 },
    { "jmpr_chain35.simi", 247 },
    { "jmpr_chain36.simi", 277 },
    { "jmpr_chain37.simi", 368 },
    { "jmpr_chain38.simi", 322 },
    { "jmpr_chain39.simi", 321 },
    { "jmpr_chain4.simi", 242 },
    { "jmpr_chain40.simi", 826 },
    { "jmpr_chain41.simi", 831 },
    { "jmpr_chain42.simi", 366 },
    { "jmpr_chain43.simi", 328 },
    { "jmpr_chain44.simi", 274 },
    { "jmpr_chain5.simi", 269 },
    { "jmpr_chain6.simi", 275 },
    { "jmpr_chain7.simi", 275 },
    { "jmpr_chain8.simi", 290 },
    { "jmpr_chain9.simi", 281 },
    { "jmpr_cross.simi", 252 },
    { "jmpr_deadfull.simi", 478 },
    { "jmpr_deadmult.simi", 478 },
    { "jmpr_dyn.simi", 237 },
    { "jmpr_fall.simi", 246 },
    { "jmpr_fall2.simi", 260 },
    { "jmpr_foldreach.simi", 501 },
    { "jmpr_join.simi", 232 },
    { "jmpr_mid.simi", 246 },
    { "jmpr_mix.simi", 256 },
    { "jmpr_table.simi", 262 },
    { "jmpr_unreach.simi", 478 },
    { "loadi64.simi", 221 },
    { "loop_sum.simi", 411 },
    { "mem_neg.simi", 297 },
    { "mem_ops_native.simi", 243 },
    { "mem_pre.simi", 421 },
    { "mem_reg.simi", 392 },
    { "obj_ops.simi", 264 },
    { "ptr_ops.simi", 259 },
    { "rd_star.simi", 262 },
    { "rv64_boot_smoke.simi", 212 },
    { "src_resident.simi", 261 },
    { "straight_line_bench.simi", 261 },
    { "stress_atomics.simi", 259 },
    { "tail_ret.simi", 224 },
};
/* 96 rows — steps EXACT, measured 2026-08-09 via RV64_STEPS_MEASURE. */

#endif /* BENCH_BASELINES_RV64_H */
