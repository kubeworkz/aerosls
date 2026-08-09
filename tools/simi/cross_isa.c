/* cross_isa.c — M2.78: cross-ISA execution-work report (A64 vs RV64).
 *
 * The per-fixture executed-instruction counts are already COMMITTED and
 * asserted EXACTLY by the parity tripwires — simi-arm-verify --steps
 * (bench_baselines.h) and simi-riscv-verify --steps
 * (bench_baselines_rv64.h) — so this tool is the derived VIEW over
 * those pinned numbers, with no execution and no timing of its own
 * (pure computation over the two headers: deterministic, instant):
 *
 *   - the per-fixture A64-vs-RV64 ratio table, sorted by ratio so the
 *     codegen-density extremes are visible;
 *   - the totals and the aggregate ratio (measured 2026-08-09: A64
 *     35032 / RV64 32820 = 1.0674 — the A64 codegen executes ~6.7%
 *     more instructions across the corpus than the RV64 codegen);
 *   - two consistency gates that catch TABLE DRIFT a single-ISA check
 *     cannot see:
 *       1. No orphan rows — every fixture must have a committed count
 *          in BOTH tables. A row in one table missing from the other is
 *          a deliberate-update error (adding a fixture to one ISA's
 *          baselines and not the other's).
 *       2. The aggregate ratio must stay within AGG_RATIO_BAND of the
 *          measured value. A codegen change that rebalances the two
 *          ISAs' instruction mixes (a fold that saves A64 steps but
 *          not RV64 steps, or vice versa) moves it; the band tolerates
 *          deliberate divergence while failing gross drift.
 *
 * Usage: cross-isa   (reads the two committed headers; prints the table
 * and exits nonzero on any gate failure). In `all` — instant, and it
 * keeps the two tables honest on every build. See plan doc
 * §10.166/10.167.
 */
#include "bench_baselines.h"
#include "bench_baselines_rv64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGG_RATIO_MEASURED 1.0674   /* A64 35032 / RV64 32820, 2026-08-09 */
#define AGG_RATIO_BAND     0.25     /* +/-25% tolerance on the aggregate */

struct Row {
    const char* name;
    long long a64;
    long long rv64;
    double ratio;
};

static int cmp_ratio_desc(const void* a, const void* b) {
    const struct Row* ra = (const struct Row*)a;
    const struct Row* rb = (const struct Row*)b;
    if (ra->ratio > rb->ratio) return -1;
    if (ra->ratio < rb->ratio) return  1;
    return strcmp(ra->name, rb->name);
}

static const struct Rv64Baseline* find_rv64(const char* name) {
    for (size_t i = 0; i < sizeof(RV64_BASELINES) / sizeof(RV64_BASELINES[0]); i++)
        if (strcmp(RV64_BASELINES[i].name, name) == 0) return &RV64_BASELINES[i];
    return NULL;
}
static const struct FixtureBaseline* find_a64(const char* name) {
    for (size_t i = 0; i < sizeof(BASELINES) / sizeof(BASELINES[0]); i++)
        if (strcmp(BASELINES[i].name, name) == 0) return &BASELINES[i];
    return NULL;
}

int main(void) {
    size_t na = sizeof(BASELINES) / sizeof(BASELINES[0]);
    size_t nr = sizeof(RV64_BASELINES) / sizeof(RV64_BASELINES[0]);
    struct Row rows[256];
    size_t nrows = 0;
    int fails = 0;

    long long tot_a64 = 0, tot_rv64 = 0;
    double min_ratio = 1e9, max_ratio = 0.0;
    const char *min_name = NULL, *max_name = NULL;

    for (size_t i = 0; i < na; i++) {
        const struct Rv64Baseline* r = find_rv64(BASELINES[i].name);
        if (!r) {
            fprintf(stderr, "FAIL  orphan A64 row: %s has no RV64 baseline (update bench_baselines_rv64.h — see plan doc §10.166)\n",
                    BASELINES[i].name);
            fails++;
            continue;
        }
        struct Row* row = &rows[nrows++];
        row->name = BASELINES[i].name;
        row->a64 = BASELINES[i].steps;
        row->rv64 = r->steps;
        row->ratio = (double)row->a64 / (double)row->rv64;
        tot_a64 += row->a64;
        tot_rv64 += row->rv64;
        if (row->ratio < min_ratio) { min_ratio = row->ratio; min_name = row->name; }
        if (row->ratio > max_ratio) { max_ratio = row->ratio; max_name = row->name; }
    }
    for (size_t i = 0; i < nr; i++) {
        if (!find_a64(RV64_BASELINES[i].name)) {
            fprintf(stderr, "FAIL  orphan RV64 row: %s has no A64 baseline (update bench_baselines.h — see plan doc §10.166)\n",
                    RV64_BASELINES[i].name);
            fails++;
        }
    }

    qsort(rows, nrows, sizeof(rows[0]), cmp_ratio_desc);

    printf("=== cross-ISA execution-work (A64 vs RV64, committed steps) ===\n");
    printf("fixture                     a64    rv64   a64/rv64\n");
    for (size_t i = 0; i < nrows; i++)
        printf("%-24s %5lld %6lld %8.3f\n", rows[i].name, rows[i].a64, rows[i].rv64, rows[i].ratio);

    double agg = (double)tot_a64 / (double)tot_rv64;
    printf("\ntotals: a64=%lld rv64=%lld  aggregate ratio=%.4f\n", tot_a64, tot_rv64, agg);
    printf("per-fixture ratio range: %.3f (%s) .. %.3f (%s) over %zu fixtures\n",
           min_ratio, min_name ? min_name : "?", max_ratio, max_name ? max_name : "?", nrows);

    if (nrows != na || nrows != nr) {
        fprintf(stderr, "FAIL  fixture-set mismatch: a64=%zu rv64=%zu paired=%zu\n", na, nr, nrows);
        fails++;
    }
    double lo = AGG_RATIO_MEASURED * (1.0 - AGG_RATIO_BAND);
    double hi = AGG_RATIO_MEASURED * (1.0 + AGG_RATIO_BAND);
    if (agg < lo || agg > hi) {
        fprintf(stderr, "FAIL  aggregate ratio %.4f outside committed band [%.4f, %.4f] around %.4f (codegen mix drift)\n",
                agg, lo, hi, AGG_RATIO_MEASURED);
        fails++;
    }

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %zu paired fixtures, aggregate ratio %.4f within the committed band\n",
           nrows, agg);
    return 0;
}
