/* bench_baselines_x86.h — M2.76: the COMMITTED per-fixture EMITTED-BYTE
 * counts for the x86 leg, the single source of truth for the
 * simi-jit-test `--bytes` check (the parity harness's native leg).
 *
 * WHY BYTES, NOT STEPS: the x86 leg executes real machine code on the
 * host CPU (mmap PROT_EXEC + a function-pointer call) — there is no
 * executor to count retired instructions, and this sandbox has no
 * virtualized PMU to count them in hardware (probed: perf_event_open
 * PERF_COUNT_HW_INSTRUCTIONS returns ENOENT on the WSL2 kernel; the
 * software counters measure time, not instructions). The deterministic,
 * machine-independent per-fixture count that IS available is the size
 * of the emitted translation (simi_x86_translate's out_len — the bytes
 * the translator produced for the fixture). That is the x86 analog of
 * the ARM size gate's per-fixture byte baselines, asserted right in the
 * native parity runner: ANY emission change (a fold that alters how
 * many instructions/bytes are emitted, a codegen restructure) moves the
 * count and fails the fixture. Dynamic correctness on the native leg
 * remains the execution itself + the result check + the crash handler.
 *
 * Keys are "<fixture>.simi" — the runner derives the name from the .tmo
 * path. The counts are for the whole translation driven with entry
 * "main" (trampoline + all entries; the runner always uses "main"). A
 * fixture with no row FAILS loudly (adding/removing a corpus fixture
 * requires updating this table deliberately). Re-measure by capturing
 * the "%u bytes native code" field from ./tests/run_native_tests.sh.
 * See plan doc §10.162/10.163.
 */
#ifndef BENCH_BASELINES_X86_H
#define BENCH_BASELINES_X86_H

struct X86Baseline {
    const char* name;         /* <fixture>.simi */
    long long bytes;          /* EXACT emitted size — the tight guard */
};

static const struct X86Baseline X86_BASELINES[] = {
    { "add.simi", 442 },
    { "aggregate_abi.simi", 2853 },
    { "alu_imm.simi", 656 },
    { "alu_imm_ext.simi", 682 },
    { "atomic_add.simi", 822 },
    { "branch_cmp.simi", 601 },
    { "call_ret.simi", 960 },
    { "cap_call_ret.simi", 1954 },
    { "cap_forge.simi", 1006 },
    { "cas_simple.simi", 995 },
    { "dead_reuse.simi", 662 },
    { "epi_merge.simi", 610 },
    { "epi_merge2.simi", 639 },
    { "epi_merge3.simi", 668 },
    { "epi_merge4.simi", 623 },
    { "epi_merge5.simi", 652 },
    { "epi_merge6.simi", 632 },
    { "extra_ops.simi", 638 },
    { "fetch_cross.simi", 722 },
    { "float_ops.simi", 2426 },
    { "jmpr_basic.simi", 553 },
    { "jmpr_calc.simi", 821 },
    { "jmpr_calc_bit.simi", 935 },
    { "jmpr_calc_mul.simi", 815 },
    { "jmpr_callret.simi", 1055 },
    { "jmpr_callret_arg.simi", 2007 },
    { "jmpr_chain.simi", 747 },
    { "jmpr_chain10.simi", 2663 },
    { "jmpr_chain11.simi", 2716 },
    { "jmpr_chain12.simi", 2766 },
    { "jmpr_chain13.simi", 3226 },
    { "jmpr_chain14.simi", 3107 },
    { "jmpr_chain15.simi", 3278 },
    { "jmpr_chain16.simi", 3691 },
    { "jmpr_chain17.simi", 2606 },
    { "jmpr_chain18.simi", 3690 },
    { "jmpr_chain19.simi", 2559 },
    { "jmpr_chain2.simi", 747 },
    { "jmpr_chain20.simi", 2628 },
    { "jmpr_chain21.simi", 2634 },
    { "jmpr_chain22.simi", 5525 },
    { "jmpr_chain23.simi", 1998 },
    { "jmpr_chain24.simi", 1988 },
    { "jmpr_chain25.simi", 3142 },
    { "jmpr_chain26.simi", 1959 },
    { "jmpr_chain27.simi", 4179 },
    { "jmpr_chain28.simi", 1991 },
    { "jmpr_chain29.simi", 4791 },
    { "jmpr_chain3.simi", 751 },
    { "jmpr_chain30.simi", 3113 },
    { "jmpr_chain31.simi", 6352 },
    { "jmpr_chain32.simi", 16820 },
    { "jmpr_chain33.simi", 7816 },
    { "jmpr_chain34.simi", 5149 },
    { "jmpr_chain35.simi", 738 },
    { "jmpr_chain36.simi", 668 },
    { "jmpr_chain37.simi", 10098 },
    { "jmpr_chain38.simi", 14612 },
    { "jmpr_chain39.simi", 10478 },
    { "jmpr_chain4.simi", 750 },
    { "jmpr_chain40.simi", 9276 },
    { "jmpr_chain41.simi", 9340 },
    { "jmpr_chain42.simi", 7046 },
    { "jmpr_chain43.simi", 1378 },
    { "jmpr_chain44.simi", 836 },
    { "jmpr_chain5.simi", 1385 },
    { "jmpr_chain6.simi", 2178 },
    { "jmpr_chain7.simi", 2990 },
    { "jmpr_chain8.simi", 2663 },
    { "jmpr_chain9.simi", 2713 },
    { "jmpr_cross.simi", 693 },
    { "jmpr_deadfull.simi", 1962 },
    { "jmpr_deadmult.simi", 1605 },
    { "jmpr_dyn.simi", 618 },
    { "jmpr_fall.simi", 868 },
    { "jmpr_fall2.simi", 702 },
    { "jmpr_foldreach.simi", 1641 },
    { "jmpr_join.simi", 635 },
    { "jmpr_mid.simi", 710 },
    { "jmpr_mix.simi", 791 },
    { "jmpr_table.simi", 817 },
    { "jmpr_unreach.simi", 1567 },
    { "lcg_fairness.simi", 1086 },
    { "loadi64.simi", 429 },
    { "loop_sum.simi", 583 },
    { "mem_neg.simi", 719 },
    { "mem_ops_native.simi", 511 },
    { "mem_pre.simi", 1265 },
    { "mem_reg.simi", 1098 },
    { "obj_ops.simi", 781 },
    { "ptr_ops.simi", 625 },
    { "rd_star.simi", 602 },
    { "rv64_boot_smoke.simi", 387 },
    { "rv64_float_smoke_a.simi", 445 },
    { "rv64_float_smoke_b.simi", 425 },
    { "src_resident.simi", 607 },
    { "straight_line_bench.simi", 618 },
    { "stress_atomics.simi", 1996 },
    { "tail_ret.simi", 509 },
};
/* 98 rows — emitted bytes EXACT, measured 2026-08-09. */

#endif /* BENCH_BASELINES_X86_H */
