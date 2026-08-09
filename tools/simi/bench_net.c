/* bench_net.c — M2.71: translate-time micro-benchmark for the fold
 * fixpoint's safety net (simi_arm.c's M2.18/M2.70 2-cycle trip count).
 *
 * jmpr_chain36.simi is the only input in the corpus whose fold fixpoint
 * DIVERGES — the M2.60 relaxation 2-cycle (relax -> fold -> reset ->
 * un-fold, forever) — so it is the only input that ever fires the net.
 * M2.70 tightened the trip count 512 -> 16: the 2-cycle now burns 17
 * scans before the net fires (then the un-relaxed restart converges in
 * 1), instead of 513 + 1 — a 514/18 = 28.6x reduction in scan count.
 * The wall-clock win is smaller on this input (measured ~8-9x) because
 * the per-translate FIXED overhead (the 4096-entry array clears, the
 * reachability BFS, the emission — identical at both trip counts, in
 * the same process, same buffer, same warmup) dominates the 16-side's
 * ~18 scans; the scan-work win (28.6x) is what the fixpoint itself
 * spends, and it is what end-to-end translate approaches on larger
 * programs (the per-scan cost scales with num_instr; the fixed cost
 * does not).
 *
 * This harness pins BOTH as committed measurements: it translates
 * chain36 N times at the shipped trip count (16) and at the M2.69-era
 * count (512), timing each with CLOCK_MONOTONIC, and FAILS (exit 1) if
 * the scan ratio drops below 20x (deterministic truth 28.6x) or the
 * wall-clock ratio below 5x (measured ~8-9x, floor leaves margin for
 * noise and machine differences). Nothing here executes the translated
 * code — the runtime-function addresses are 0 (never called by a
 * translate-only run), so the harness is pure translate-time.
 */
#define _GNU_SOURCE   /* clock_gettime/CLOCK_MONOTONIC under -std=c11 (bench_harness.c does the same) */
#include "simi_arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <chain36.tmo> <iters>\n", argv[0]);
        return 2;
    }
    long long iters = atoll(argv[2]);
    if (iters < 1) { fprintf(stderr, "iters must be >= 1\n"); return 2; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty .tmo\n"); return 2; }
    uint8_t* obj = malloc((size_t)sz);
    if (!obj) return 2;
    if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    uint32_t cap = 1u << 20;
    uint8_t* code = malloc(cap);
    if (!code) return 2;

    const int trips[2] = { 16, 512 };   /* shipped (M2.70) vs M2.69-era */
    const char* names[2] = { "trip=16 (M2.70)", "trip=512 (M2.69-era)" };
    double secs[2];
    long long scans[2];

    for (int k = 0; k < 2; k++) {
        g_ar_net_trip = trips[k];
        /* warmup: one translate (first-call setup, cold caches) */
        uint32_t out_len = 0, entry_off = 0;
        int rc = simi_arm_translate(obj, (uint32_t)sz, code, cap, "main",
                                    0, 0, 0, 0, &out_len, &entry_off);
        if (rc != TX_AR_OK) {
            fprintf(stderr, "translate error at %s: %s\n", names[k], simi_arm_strerror(rc));
            return 2;
        }
        g_ar_net_scans = 0;
        double t0 = now_sec();
        for (long long i = 0; i < iters; i++)
            simi_arm_translate(obj, (uint32_t)sz, code, cap, "main",
                               0, 0, 0, 0, &out_len, &entry_off);
        double t1 = now_sec();
        secs[k] = (t1 - t0) / (double)iters;
        scans[k] = g_ar_net_scans / iters;
        printf("%-20s %5lld iters  %8.2f us/translate  %4lld scans/translate\n",
               names[k], iters, secs[k] * 1e6, scans[k]);
    }

    double ratio = secs[1] / secs[0];
    double scan_ratio = (double)scans[1] / (double)scans[0];
    printf("speedup (512/16): %.1fx wall-clock, %.1fx scans\n", ratio, scan_ratio);

    int fail = 0;
    if (scan_ratio < 20.0) {
        fprintf(stderr, "FAIL: scan ratio %.1fx < 20x (deterministic truth: 514/18 = 28.6x)\n", scan_ratio);
        fail = 1;
    }
    if (ratio < 5.0) {
        fprintf(stderr, "FAIL: wall-clock speedup %.1fx < 5x committed floor\n", ratio);
        fail = 1;
    }
    if (fail) return 1;
    printf("ALL CHECKS PASSED — the M2.70 net tightening is a committed %.1fx end-to-end\n", ratio);
    printf("translate-time win on chain36 (%.1fx in fixpoint scan work)\n", scan_ratio);
    return 0;
}
