/* bench_corpus.c — M2.72: corpus-wide translate-cost micro-benchmark,
 * the regression guard for the fold-fixpoint series BEYOND chain36
 * (bench_net.c pins the safety-net win on the only net-firing input;
 * this pins the translate cost of the WHOLE corpus).
 *
 * It translates every tests/*.tmo fixture N times (at the shipped trip
 * count g_ar_net_trip = 16 — set explicitly, bench-net may have left
 * 512), timing with CLOCK_MONOTONIC, and records per fixture: the
 * instruction count (from the object header), the fixpoint scan count
 * (g_ar_net_scans — the deterministic work the fixpoint did), and the
 * per-translate / per-instruction wall-clock time. Two committed
 * assertions gate the run:
 *
 *   1. The TOTAL scan count must equal COMMITTED_TOTAL_SCANS — the
 *      deterministic, machine-independent truth (each fixture's fixpoint
 *      converges in a fixed number of scans: 1 for the plain fixtures,
 *      2 for the retroactive-split ones, 3 for chain35/43, 4 for the
 *      chain44 relaxation-flip, 18 for chain36's net-firing 2-cycle).
 *      Any fixpoint change that alters convergence — more passes, a
 *      fold that now survives or dies — moves this number and FAILS.
 *
 *   2. The aggregate per-instruction translate time must stay below
 *      COMMITTED_MAX_US_PER_INSTR — a GENEROUS ceiling (measured
 *      aggregate is well under it; the margin tolerates machine speed
 *      differences) that still catches broad translate-cost regressions:
 *      a new per-pass walk, an O(n^2) pass, a heavier scan step.
 *
 * The per-instruction metric is fixed-cost-dominated for the small
 * fixtures (the per-translate array clears, pass A, the reachability
 * BFS, and the emission are identical per translate), which is exactly
 * what makes the aggregate a stable, comparable number. Nothing here
 * executes the translated code (the runtime-function addresses are 0).
 *
 * Usage: bench_corpus <fixture.tmo>... <iters>   (the Makefile passes
 * tests/*.tmo and the default iters).
 */
#define _GNU_SOURCE   /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include "simi_arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Committed regression constants. COMMITTED_TOTAL_SCANS is deterministic
 * and must be updated deliberately when the corpus changes (adding or
 * removing a fixture, or a fixpoint change that legitimately moves a
 * fixture's convergence — see plan doc §10.154). COMMITTED_MAX_US_PER_INSTR
 * is the generous wall-clock ceiling — 4x the measured aggregate
 * (0.991 us/instr, recorded in §10.155) so slower machines tolerate it,
 * while a gross translate-cost regression (an O(n^2) pass, a new
 * per-pass 4096-walk, a 5x slower scan step) still fails. The precise
 * fixpoint guard is the deterministic scan total above; the ceiling
 * catches the cost side. Re-measure and update both only deliberately.
 */
#define COMMITTED_TOTAL_SCANS      144
#define COMMITTED_MAX_US_PER_INSTR 4.0

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint32_t obj_num_instr(const uint8_t* obj) {
    /* struct SimiObjHdrAR: magic u32, then num_instr u32 (see simi_arm.h). */
    uint32_t n;
    memcpy(&n, obj + 4, 4);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture.tmo>... <iters>\n", argv[0]);
        return 2;
    }
    long long iters = atoll(argv[argc - 1]);
    if (iters < 1) { fprintf(stderr, "iters must be >= 1\n"); return 2; }
    int nfixtures = argc - 2;

    long long tot_instr = 0, tot_scans = 0, tot_trans = 0;
    double tot_us = 0.0;

    for (int a = 0; a < nfixtures; a++) {
        const char* path = argv[a + 1];
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;

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
        printf("%-24s instr=%5u scans=%2lld %8.2f us/translate %7.3f us/instr\n",
               name, instr, scans, us, us / (double)instr);

        tot_instr += (long long)instr;
        tot_scans += scans;
        tot_trans += iters;
        tot_us += us * (double)iters;
        free(obj); free(code);
    }

    /* per-instruction aggregate = total translate time / (instr x iters) */
    double us_per_instr = tot_us / (double)(tot_instr * iters);
    printf("\n%5d fixtures x %lld iters: %lld instr, %lld scans, %.3f us/instr aggregate\n",
           nfixtures, iters, tot_instr, tot_scans, us_per_instr);

    int fail = 0;
    if (tot_scans != COMMITTED_TOTAL_SCANS) {
        fprintf(stderr, "FAIL: total scans %lld != committed %d (a fixpoint convergence change — see plan doc §10.154)\n",
                tot_scans, COMMITTED_TOTAL_SCANS);
        fail = 1;
    }
    if (us_per_instr > COMMITTED_MAX_US_PER_INSTR) {
        fprintf(stderr, "FAIL: aggregate %.3f us/instr > committed ceiling %.1f (broad translate-cost regression)\n",
                us_per_instr, COMMITTED_MAX_US_PER_INSTR);
        fail = 1;
    }
    if (fail) return 1;
    printf("ALL CHECKS PASSED — corpus translate cost committed (%.3f us/instr, %lld scans)\n",
           us_per_instr, tot_scans);
    return 0;
}
