/* bench_corpus.c — M2.72/M2.73: corpus-wide translate-cost benchmark,
 * the regression guard for the fold-fixpoint series. M2.72 pinned the
 * aggregate (total scans + a per-instruction ceiling); M2.73 tightens
 * it to COMMITTED PER-ROW BASELINES, the size gate's per-fixture model:
 * every fixture's fixpoint work (scans — EXACT, deterministic) and
 * translate cost (us/instr — wall-clock, asserted at a margin) are
 * committed in the BASELINES table below, and the bench FAILS on any
 * fixture that moves either.
 *
 * The two dimensions are deliberately different in tightness:
 *   - scans: EXACT. The scan count is the deterministic,
 *     machine-independent truth (1 for the plain fixtures, 2 for the
 *     M2-era chain/join fixtures whose fold set changes once, 3 for
 *     chain35/43's retroactive split, 4 for chain44's relaxation-flip,
 *     18 for chain36's net-firing 2-cycle). ANY convergence change in
 *     ANY fixture — more passes, a fold that now survives or dies —
 *     fails its row. This is the tight per-fixture guard.
 *   - us/instr: asserted at BASELINE_MARGIN (3.0) over the committed
 *     value. Per-fixture wall-clock on this sandbox is noisy — two
 *     N=500 runs swung up to 42% on individual fixtures (WSL timer
 *     granularity, load drift; the aggregate stayed within 2.3%) — so
 *     a tighter margin would flake. 3.0 absorbs the noise and machine
 *     speed differences while still failing a fixture whose translate
 *     cost grows 3x or more — the gross single-fixture regression the
 *     old aggregate ceiling could not see. The printed table shows
 *     every fixture's measured value next to its baseline, so drifts
 *     below the margin remain visible for a human.
 *
 * A fixture with no baseline row FAILS loudly — adding or removing a
 * corpus fixture requires updating the table deliberately (exactly the
 * size gate's M0_BASELINES discipline). Re-measure and update the
 * table only deliberately; see plan doc §10.156/10.157.
 *
 * Usage: bench_corpus <fixture.tmo>... <iters>   (the Makefile passes
 * tests/*.tmo and the default iters). Nothing executes the translated
 * code — the runtime-function addresses are 0 — so this is pure
 * translate-time.
 */
#define _GNU_SOURCE   /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include "simi_arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BASELINE_MARGIN 3.0   /* us/instr wall-clock margin (see above) */

struct FixtureBaseline {
    const char* name;         /* <fixture>.tmo */
    int scans;                /* EXACT fixpoint work — the tight guard */
    double us_per_instr;      /* measured 2026-08-09 on this sandbox (N=500) */
};

static const struct FixtureBaseline BASELINES[] = {
    { "add.tmo", 1, 1.131 },
    { "aggregate_abi.tmo", 1, 0.207 },
    { "alu_imm.tmo", 1, 0.651 },
    { "alu_imm_ext.tmo", 1, 0.639 },
    { "atomic_add.tmo", 1, 0.426 },
    { "branch_cmp.tmo", 1, 0.439 },
    { "call_ret.tmo", 1, 1.109 },
    { "cap_call_ret.tmo", 1, 0.653 },
    { "cap_forge.tmo", 1, 0.501 },
    { "cap_forge_debug.tmo", 1, 1.050 },
    { "cas_simple.tmo", 1, 0.357 },
    { "dead_reuse.tmo", 1, 0.476 },
    { "epi_merge.tmo", 2, 0.861 },
    { "epi_merge2.tmo", 2, 0.783 },
    { "epi_merge3.tmo", 2, 0.738 },
    { "epi_merge4.tmo", 2, 0.873 },
    { "epi_merge5.tmo", 2, 0.557 },
    { "epi_merge6.tmo", 2, 0.864 },
    { "extra_ops.tmo", 1, 0.490 },
    { "fetch_cross.tmo", 1, 0.477 },
    { "float_ops.tmo", 1, 0.224 },
    { "jmpr_basic.tmo", 2, 1.188 },
    { "jmpr_calc.tmo", 2, 0.586 },
    { "jmpr_calc_bit.tmo", 2, 0.492 },
    { "jmpr_calc_mul.tmo", 2, 0.554 },
    { "jmpr_callret.tmo", 2, 1.147 },
    { "jmpr_callret_arg.tmo", 2, 0.439 },
    { "jmpr_chain.tmo", 1, 1.644 },
    { "jmpr_chain10.tmo", 1, 1.052 },
    { "jmpr_chain11.tmo", 1, 1.034 },
    { "jmpr_chain12.tmo", 1, 1.167 },
    { "jmpr_chain13.tmo", 1, 0.957 },
    { "jmpr_chain14.tmo", 1, 0.887 },
    { "jmpr_chain15.tmo", 1, 0.937 },
    { "jmpr_chain16.tmo", 1, 0.807 },
    { "jmpr_chain17.tmo", 1, 0.565 },
    { "jmpr_chain18.tmo", 1, 0.583 },
    { "jmpr_chain19.tmo", 1, 1.281 },
    { "jmpr_chain2.tmo", 1, 1.842 },
    { "jmpr_chain20.tmo", 1, 0.731 },
    { "jmpr_chain21.tmo", 1, 0.829 },
    { "jmpr_chain22.tmo", 1, 0.464 },
    { "jmpr_chain23.tmo", 1, 0.596 },
    { "jmpr_chain24.tmo", 1, 0.580 },
    { "jmpr_chain25.tmo", 1, 0.668 },
    { "jmpr_chain26.tmo", 1, 0.601 },
    { "jmpr_chain27.tmo", 1, 0.564 },
    { "jmpr_chain28.tmo", 1, 0.756 },
    { "jmpr_chain29.tmo", 1, 0.625 },
    { "jmpr_chain3.tmo", 1, 2.764 },
    { "jmpr_chain30.tmo", 1, 0.840 },
    { "jmpr_chain31.tmo", 1, 0.557 },
    { "jmpr_chain32.tmo", 1, 0.720 },
    { "jmpr_chain33.tmo", 1, 4.575 },
    { "jmpr_chain34.tmo", 1, 1.602 },
    { "jmpr_chain35.tmo", 3, 1.783 },
    { "jmpr_chain36.tmo", 18, 3.176 },
    { "jmpr_chain37.tmo", 1, 0.581 },
    { "jmpr_chain38.tmo", 1, 1.106 },
    { "jmpr_chain39.tmo", 1, 1.097 },
    { "jmpr_chain4.tmo", 1, 2.741 },
    { "jmpr_chain40.tmo", 1, 0.673 },
    { "jmpr_chain41.tmo", 1, 0.623 },
    { "jmpr_chain42.tmo", 1, 0.626 },
    { "jmpr_chain43.tmo", 3, 0.307 },
    { "jmpr_chain44.tmo", 4, 1.813 },
    { "jmpr_chain5.tmo", 1, 1.476 },
    { "jmpr_chain6.tmo", 1, 0.867 },
    { "jmpr_chain7.tmo", 1, 0.609 },
    { "jmpr_chain8.tmo", 1, 1.025 },
    { "jmpr_chain9.tmo", 1, 0.660 },
    { "jmpr_cross.tmo", 2, 0.767 },
    { "jmpr_deadfull.tmo", 2, 0.450 },
    { "jmpr_deadmult.tmo", 2, 0.381 },
    { "jmpr_dyn.tmo", 1, 3.574 },
    { "jmpr_fall.tmo", 2, 0.545 },
    { "jmpr_fall2.tmo", 2, 0.722 },
    { "jmpr_foldreach.tmo", 2, 7.143 },
    { "jmpr_join.tmo", 1, 2.456 },
    { "jmpr_mid.tmo", 2, 0.733 },
    { "jmpr_mix.tmo", 2, 2.178 },
    { "jmpr_oob.tmo", 1, 4.603 },
    { "jmpr_table.tmo", 1, 0.502 },
    { "jmpr_unreach.tmo", 2, 0.565 },
    { "loadi64.tmo", 1, 1.412 },
    { "loop_sum.tmo", 1, 0.561 },
    { "mem_neg.tmo", 1, 0.449 },
    { "mem_ops.tmo", 1, 0.758 },
    { "mem_ops_native.tmo", 1, 0.668 },
    { "mem_pre.tmo", 1, 0.260 },
    { "mem_reg.tmo", 1, 0.314 },
    { "obj_ops.tmo", 1, 0.732 },
    { "ptr_ops.tmo", 1, 0.441 },
    { "rd_star.tmo", 1, 0.457 },
    { "rv64_boot_smoke.tmo", 1, 2.277 },
    { "src_resident.tmo", 1, 0.582 },
    { "straight_line_bench.tmo", 1, 0.427 },
    { "stress_atomics.tmo", 1, 0.261 },
    { "tail_ret.tmo", 1, 0.575 },
};
/* 99 rows — total scans 144 */

static const struct FixtureBaseline* find_baseline(const char* name) {
    for (size_t i = 0; i < sizeof(BASELINES) / sizeof(BASELINES[0]); i++)
        if (strcmp(BASELINES[i].name, name) == 0) return &BASELINES[i];
    return NULL;
}

static uint32_t obj_num_instr(const uint8_t* obj) {
    /* struct SimiObjHdrAR: magic u32, then num_instr u32 (see simi_arm.h). */
    uint32_t n;
    memcpy(&n, obj + 4, 4);
    return n;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture.tmo>... <iters>\n", argv[0]);
        return 2;
    }
    long long iters = atoll(argv[argc - 1]);
    if (iters < 1) { fprintf(stderr, "iters must be >= 1\n"); return 2; }
    int nfixtures = argc - 2;

    long long tot_instr = 0, tot_scans = 0;
    double tot_us = 0.0;
    int fails = 0, nrows = 0;

    for (int a = 0; a < nfixtures; a++) {
        const char* path = argv[a + 1];
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;

        const struct FixtureBaseline* bl = find_baseline(name);
        if (!bl) {
            fprintf(stderr, "FAIL  %-24s no committed baseline (update the BASELINES table — see plan doc §10.156)\n", name);
            fails++;
            continue;
        }

        FILE* f = fopen(path, "rb");
        if (!f) { perror(path); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fprintf(stderr, "empty .tmo: %s\n", path); return 2; }
        uint8_t* obj = malloc((size_t)sz);
        if (!obj) return 2;
        if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);

        uint32_t cap = 1u << 20;
        uint8_t* code = malloc(cap);
        if (!code) return 2;

        g_ar_net_trip = 16;   /* the shipped default — bench-net may have left 512 */
        uint32_t out_len = 0, entry_off = 0;
        int rc = simi_arm_translate(obj, (uint32_t)sz, code, cap, "main",
                                    0, 0, 0, 0, &out_len, &entry_off);
        if (rc != TX_AR_OK) {
            fprintf(stderr, "translate error %s: %s\n", name, simi_arm_strerror(rc));
            return 2;
        }
        g_ar_net_scans = 0;
        double t0 = now_sec();
        for (long long i = 0; i < iters; i++)
            simi_arm_translate(obj, (uint32_t)sz, code, cap, "main",
                               0, 0, 0, 0, &out_len, &entry_off);
        double t1 = now_sec();
        double us = (t1 - t0) * 1e6 / (double)iters;
        long long scans = g_ar_net_scans / iters;
        uint32_t instr = obj_num_instr(obj);
        double us_in = us / (double)instr;

        /* per-row assertions: scans EXACT, us/instr within the margin */
        int row_fail = 0;
        if (scans != bl->scans) {
            fprintf(stderr, "FAIL  %-24s scans=%lld != committed %d (fixpoint convergence changed)\n",
                    name, scans, bl->scans);
            row_fail = 1;
        }
        if (us_in > bl->us_per_instr * BASELINE_MARGIN) {
            fprintf(stderr, "FAIL  %-24s %.3f us/instr > committed %.3f x %.0f (translate cost regression)\n",
                    name, us_in, bl->us_per_instr, BASELINE_MARGIN);
            row_fail = 1;
        }
        if (!row_fail)
            printf("PASS  %-24s instr=%5u scans=%2lld %7.3f us/instr (committed %d / %.3f)\n",
                   name, instr, scans, us_in, bl->scans, bl->us_per_instr);
        else
            fails++;

        tot_instr += (long long)instr;
        tot_scans += scans;
        tot_us += us * (double)iters;
        nrows++;
        free(obj); free(code);
    }

    double us_per_instr = tot_us / (double)(tot_instr * iters);
    printf("\n%5d fixtures: %lld instr, %lld scans, %.3f us/instr aggregate (%d failed)\n",
           nfixtures, tot_instr, tot_scans, us_per_instr, fails);

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %d fixture rows within their committed baselines\n", nrows);
    return 0;
}
