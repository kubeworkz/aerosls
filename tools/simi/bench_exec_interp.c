/* bench_exec_interp.c — M2.80: corpus-wide EXECUTION-cost benchmark for
 * the INTERPRETER leg, completing the four-way measurement story — the
 * last leg without one. bench-exec / bench-exec-rv64 pin the A64 and
 * RV64 translated-execution costs, bench-corpus pins translate cost, the
 * x86 leg has its emitted-byte gate; THIS pins what the REFERENCE
 * interpreter itself costs to run: every fixture is read once (the
 * interpreter doesn't translate — simi_obj_read loads the .tmo directly,
 * the "translate once" analog), then executed N times through
 * simi_interp_run() — the exact run loop simi-run uses, extracted from
 * its main() in M2.80 and driven in-process (simi_interp.c is linked
 * with -DSIMI_INTERP_NO_MAIN so the bench's own main links).
 *
 * The interpreter executes the RAW SIMI stream, so its per-fixture step
 * count is the ground-truth work the translated ISAs' counts
 * (bench_baselines.h A64 / bench_baselines_rv64.h) are compared against
 * by the cross-ISA reports — and this bench makes that count a
 * committed, asserted number on the same two-tier model as the other
 * benches (see plan doc §10.170/10.171):
 *   - steps: EXACT. The deterministic, machine-independent SIMI-
 *     instruction count per fixture (simi_interp_run's step counter —
 *     same counter as simi-run's). ANY change in the interpreter's
 *     dispatch, a decode/encoding regression that alters control flow,
 *     or an emission change that shifts how a fixture runs, moves the
 *     count and fails the row. This is the tight guard.
 *   - ns/call: asserted at BASELINE_MARGIN over the committed value
 *     (median of three N=500 passes). Same WSL wall-clock caveats as
 *     bench-exec — the margin is sized from the measured per-fixture
 *     noise (see the BASELINE_MARGIN define below).
 *
 * Execution is deterministic here — the same .tmo over zeroed memory
 * produces the same step count and the same result — and the bench
 * ASSERTS that: every iteration must execute the same number of steps
 * and produce the expected r0, or the row fails (this also catches the
 * fault-early hazard: a fixture that now faults after 3 instructions
 * looks FAST on the clock but fails both checks).
 *
 * The fixture set is run_tests.sh's: every fixture with an "Expected
 * result:" comment — INCLUDING mem_ops.simi (the address-0 pointer is a
 * Phase 1 interpreter-only convenience; the translated legs replace it
 * with mem_ops_native, so this table carries 97 rows to their 96).
 * Skipped, exactly as the runner skips them: jmpr_oob (its whole point
 * is that execution must NOT produce a result — it faults by design)
 * and cap_forge_debug (no expected result).
 *
 * A fixture with no baseline row FAILS loudly — adding or removing a
 * corpus fixture requires updating the table deliberately (the size
 * gate's M0_BASELINES discipline). Re-measure and update the table only
 * deliberately: SIMI_BENCH_MEASURE=1 bench-exec-interp <tests/ fixtures>
 * <iters> prints the table-format rows without asserting.
 *
 * Usage: bench-exec-interp <fixture.simi>... <iters>
 *   (the Makefile passes the whole tests/ corpus and the default iters;
 *   the .tmo path is derived from each .simi by swapping the extension)
 */
#define _GNU_SOURCE   /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include "simi_interp.h"
#include "bench_baselines_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BASELINE_MARGIN 4.0   /* ns/call wall-clock margin (see above; sized
                               * from measurement — same 2.3x-class tail as
                               * bench-exec's A64/RV64 sides on this sandbox) */

/* Last "Expected result:" value, run_tests.sh's rule. Returns 1 if
 * found, 0 if the fixture has none (and must be skipped). */
static int parse_expected(const char* path, long long* out) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    char buf[4096];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        const char* p = buf;
        while ((p = strstr(p, "Expected result:")) != NULL) {
            p += strlen("Expected result:");
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '-' || (*p >= '0' && *p <= '9')) {
                *out = atoll(p);
                found = 1;
            }
        }
    }
    fclose(f);
    return found;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture.simi>... <iters>\n", argv[0]);
        return 2;
    }
    long long iters = atoll(argv[argc - 1]);
    if (iters < 1) { fprintf(stderr, "iters must be >= 1\n"); return 2; }
    int nfixtures = argc - 2;
    int measure = getenv("SIMI_BENCH_MEASURE") != NULL;

    long long tot_steps = 0;
    double tot_ns = 0.0;
    int fails = 0, nrows = 0, nskip = 0;

    for (int a = 0; a < nfixtures; a++) {
        const char* path = argv[a + 1];
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;

        if (strcmp(name, "arm64_boot_smoke.simi") == 0) {
            printf("SKIP  %-24s kernel-embedded boot fixture (M4b/M5.2): by design LONG-RUNNING (the 5e7-iteration contention-probe loop) — a 500x bench-exec-interp run would take ~an hour; outside the parity corpus (plan doc §10.196)\n", name);
            nskip++;
            continue;
        }
        if (strcmp(name, "lcg_slice.simi") == 0) {
            /* §10.200: kernel-embedded LCG fixture (the arm64 kernel
             * boot is its gate) — outside the parity corpus, the
             * arm64_boot_smoke model; no row a bench would maintain. */
            printf("SKIP  %-24s kernel-embedded LCG fixture (arm64 kernel boot is its gate, plan doc §10.200)\n", name);
            nskip++;
            continue;
        }
        if (strcmp(name, "mem_touch.simi") == 0) {
            /* §10.202: kernel-embedded EL0 mem fixture (the arm64
             * kernel's EL0 excursion is its gate) — outside the parity
             * corpus, the arm64_boot_smoke model; no row a bench would
             * maintain. */
            printf("SKIP  %-24s kernel-embedded EL0 mem fixture (arm64 kernel EL0 excursion is its gate, plan doc §10.202)\n", name);
            nskip++;
            continue;
        }
        long long expected = 0;
        if (!parse_expected(path, &expected)) {
            printf("SKIP  %-24s no 'Expected result:' comment (jmpr_oob faults by design; cap_forge_debug)\n", name);
            nskip++;
            continue;
        }

        /* derive the .tmo path: swap the .simi extension */
        size_t plen = strlen(path);
        char* tmopath = malloc(plen + 1);
        if (!tmopath) return 2;
        memcpy(tmopath, path, plen + 1);
        if (plen >= 5 && strcmp(tmopath + plen - 5, ".simi") == 0)
            memcpy(tmopath + plen - 5, ".tmo", 5);

        SimiObject obj = {0};
        if (simi_obj_read(tmopath, &obj) != 0) {
            fprintf(stderr, "obj read failed: %s\n", tmopath);
            return 2;
        }
        free(tmopath);

        uint32_t entry_pc = UINT32_MAX;
        for (uint32_t i = 0; i < obj.num_entries; i++) {
            if (strcmp(obj.entries[i].name, "main") == 0) { entry_pc = obj.entries[i].offset; break; }
        }
        if (entry_pc == UINT32_MAX) {
            fprintf(stderr, "no entry 'main' in %s\n", name);
            return 2;
        }

        long long first_steps = -1;
        int row_fail = 0;
        long long done = 0;
        double t0 = now_sec();
        for (long long i = 0; i < iters && !row_fail; i++) {
            /* simi_interp_run zeroes the frame stack AND memory on entry,
             * so runs are independent and deterministic by construction. */
            long steps = 0;
            long long result = 0;
            int rc = simi_interp_run(obj, entry_pc, &steps, &result);
            if (rc != 0) {
                fprintf(stderr, "FAIL  %-24s interpreter error (rc=%d)\n", name, rc);
                row_fail = 1;
                break;
            }
            if (i == 0) first_steps = (long long)steps;
            else if ((long long)steps != first_steps) {
                fprintf(stderr, "FAIL  %-24s nondeterministic steps: %lld != first run %lld\n",
                        name, (long long)steps, first_steps);
                row_fail = 1;
                break;
            }
            if (result != expected) {
                fprintf(stderr, "FAIL  %-24s expected %lld, got %lld (steps=%lld)\n",
                        name, expected, result, (long long)steps);
                row_fail = 1;
                break;
            }
            done++;
        }
        double t1 = now_sec();

        if (row_fail) {
            fails++;
            simi_obj_free(&obj);
            continue;
        }
        double ns_call = (t1 - t0) * 1e9 / (double)done;

        if (measure) {
            printf("    { \"%s\", %lld, %.0f },\n", name, first_steps, ns_call);
            nrows++;
            simi_obj_free(&obj);
            continue;
        }

        const struct InterpBaseline* bl = NULL;
        for (size_t i = 0; i < sizeof(INTERP_BASELINES) / sizeof(INTERP_BASELINES[0]); i++)
            if (strcmp(INTERP_BASELINES[i].name, name) == 0) { bl = &INTERP_BASELINES[i]; break; }
        if (!bl) {
            fprintf(stderr, "FAIL  %-24s no committed baseline (update bench_baselines_interp.h — see plan doc §10.170)\n", name);
            fails++;
            simi_obj_free(&obj);
            continue;
        }
        if (first_steps != bl->steps) {
            fprintf(stderr, "FAIL  %-24s steps=%lld != committed %lld (interpreter/emission work changed)\n",
                    name, first_steps, bl->steps);
            fails++;
        } else if (ns_call > bl->ns_per_call * BASELINE_MARGIN) {
            fprintf(stderr, "FAIL  %-24s %.0f ns/call > committed %.0f x %.0f (execution cost regression)\n",
                    name, ns_call, bl->ns_per_call, BASELINE_MARGIN);
            fails++;
        } else {
            printf("PASS  %-24s steps=%5lld %8.0f ns/call (committed %lld / %.0f)\n",
                   name, first_steps, ns_call, bl->steps, bl->ns_per_call);
            nrows++;
        }

        tot_steps += first_steps;
        tot_ns += ns_call * (double)done;
        simi_obj_free(&obj);
    }

    if (measure) {
        printf("\n%5d fixtures measured (%d skipped, %d failed) — paste into bench_baselines_interp.h\n",
               nrows, nskip, fails);
        return fails ? 1 : 0;
    }

    double ns_agg = tot_ns / (double)(tot_steps > 0 ? tot_steps : 1);
    printf("\n%5d fixtures: %lld SIMI steps, %.1f ns/step aggregate (%d skipped, %d failed)\n",
           nfixtures, tot_steps, ns_agg, nskip, fails);

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %d fixture rows within their committed baselines\n", nrows);
    return 0;
}
