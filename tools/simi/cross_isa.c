/* cross_isa.c — M2.78/M2.82: cross-ISA execution-work report
 * (interpreter ground truth + A64 + RV64).
 *
 * The per-fixture executed-instruction counts are already COMMITTED and
 * asserted EXACTLY by the parity tripwires — simi-arm-verify --steps
 * (bench_baselines.h), simi-riscv-verify --steps
 * (bench_baselines_rv64.h), and the simi-run --steps check M2.81
 * (bench_baselines_interp.h) — so this tool is the derived VIEW over
 * those pinned numbers, with no execution and no timing of its own
 * (pure computation over the three headers: deterministic, instant):
 *
 *   - the per-fixture A64-vs-RV64 ratio table, sorted by ratio so the
 *     codegen-density extremes are visible;
 *   - M2.82: the per-fixture ISA/interp EXPANSION table — the
 *     interpreter executes the raw SIMI stream (the GROUND-TRUTH work
 *     count), so a64/interp and rv64/interp are how many translated
 *     instructions one SIMI instruction costs on each ISA, per fixture,
 *     sorted by expansion so the extremes are visible;
 *   - the totals and aggregate ratios (measured 2026-08-09: A64 35032 /
 *     RV64 32820 = 1.0674 — the A64 codegen executes ~6.7% more
 *     instructions across the corpus than the RV64 codegen; and the
 *     pinned expansion factors: A64/interp ~13.55, RV64/interp ~12.70 —
 *     one SIMI instruction costs ~13.5 A64 / ~12.7 RV64 instructions on
 *     average, the ~13x expansion of the M2.80 headline);
 *   - consistency gates that catch TABLE DRIFT a single-ISA check
 *     cannot see:
 *       1. No orphan rows — every fixture must have a committed count
 *          in the tables it belongs to. A row in one table missing from
 *          another is a deliberate-update error (adding a fixture to
 *          one ISA's baselines and not the others').
 *       2. The aggregate ratios must stay within their bands of the
 *          measured values. A codegen change that rebalances the ISAs'
 *          instruction mixes (a fold that saves A64 steps but not RV64
 *          steps, or an emission change that alters the translated
 *          stream's size relative to the SIMI ground truth) moves them;
 *          the bands tolerate deliberate divergence while failing gross
 *          drift.
 *
 * Fixture-set asymmetry, deliberate: the interpreter runs mem_ops.simi
 * (the address-0 pointer is a Phase 1 interpreter-only convenience; the
 * translated legs use mem_ops_native instead), so the interp table
 * carries 97 rows to the translated legs' 96 — the shared comparison
 * set is the 96, and mem_ops.simi is reported as an interp-only row
 * (counted in the interp total, not in any ratio).
 *
 * Usage: cross-isa   (reads the three committed headers; prints the
 * tables and exits nonzero on any gate failure). In `all` — instant,
 * and it keeps the three tables honest on every build. See plan doc
 * §10.166/10.167 (M2.78) and §10.174/10.175 (M2.82).
 */
#include "bench_baselines.h"
#include "bench_baselines_rv64.h"
#include "bench_baselines_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGG_RATIO_MEASURED 0.7713   /* A64 6735795 / RV64 8733529, 2026-08-10 — re-measured when lcg_fairness joined: its loop-dominated ratios (0.770) swamp the tiny-fixture corpus, so the aggregate now pins the loop codegen mix */
#define AGG_RATIO_BAND     0.25     /* +/-25% tolerance on the aggregate */
#define EXP_A64_MEASURED   3.7367   /* a64/interp over the shared 99, 2026-08-10 (same lcg_fairness re-measure) */
#define EXP_RV64_MEASURED  4.8449   /* rv64/interp over the shared 99, 2026-08-10 (same lcg_fairness re-measure) */
#define EXP_BAND           0.25     /* +/-25% tolerance on the expansion factors */

struct Row {
    const char* name;
    long long interp;
    long long a64;
    long long rv64;
    double ratio_av;   /* a64/rv64 */
    double exp_a64;    /* a64/interp */
    double exp_rv64;   /* rv64/interp */
};

static int cmp_ratio_desc(const void* a, const void* b) {
    const struct Row* ra = (const struct Row*)a;
    const struct Row* rb = (const struct Row*)b;
    if (ra->ratio_av > rb->ratio_av) return -1;
    if (ra->ratio_av < rb->ratio_av) return  1;
    return strcmp(ra->name, rb->name);
}
static int cmp_exp_desc(const void* a, const void* b) {
    const struct Row* ra = (const struct Row*)a;
    const struct Row* rb = (const struct Row*)b;
    if (ra->exp_a64 > rb->exp_a64) return -1;
    if (ra->exp_a64 < rb->exp_a64) return  1;
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
static const struct InterpBaseline* find_interp(const char* name) {
    for (size_t i = 0; i < sizeof(INTERP_BASELINES) / sizeof(INTERP_BASELINES[0]); i++)
        if (strcmp(INTERP_BASELINES[i].name, name) == 0) return &INTERP_BASELINES[i];
    return NULL;
}

int main(void) {
    size_t na = sizeof(BASELINES) / sizeof(BASELINES[0]);
    size_t nr = sizeof(RV64_BASELINES) / sizeof(RV64_BASELINES[0]);
    size_t ni = sizeof(INTERP_BASELINES) / sizeof(INTERP_BASELINES[0]);
    struct Row rows[256];
    size_t nrows = 0;
    int fails = 0;

    long long tot_a64 = 0, tot_rv64 = 0, tot_interp = 0;
    double min_ratio = 1e9, max_ratio = 0.0;
    const char *min_name = NULL, *max_name = NULL;
    double min_exp = 1e9, max_exp = 0.0;
    const char *min_exp_name = NULL, *max_exp_name = NULL;

    for (size_t i = 0; i < na; i++) {
        const struct Rv64Baseline* r = find_rv64(BASELINES[i].name);
        if (!r) {
            fprintf(stderr, "FAIL  orphan A64 row: %s has no RV64 baseline (update bench_baselines_rv64.h — see plan doc §10.166)\n",
                    BASELINES[i].name);
            fails++;
            continue;
        }
        const struct InterpBaseline* ip = find_interp(BASELINES[i].name);
        if (!ip) {
            fprintf(stderr, "FAIL  orphan A64 row: %s has no interpreter baseline (update bench_baselines_interp.h — see plan doc §10.174)\n",
                    BASELINES[i].name);
            fails++;
            continue;
        }
        struct Row* row = &rows[nrows++];
        row->name = BASELINES[i].name;
        row->interp = ip->steps;
        row->a64 = BASELINES[i].steps;
        row->rv64 = r->steps;
        row->ratio_av = (double)row->a64 / (double)row->rv64;
        row->exp_a64 = (double)row->a64 / (double)row->interp;
        row->exp_rv64 = (double)row->rv64 / (double)row->interp;
        tot_a64 += row->a64;
        tot_rv64 += row->rv64;
        tot_interp += row->interp;
        if (row->ratio_av < min_ratio) { min_ratio = row->ratio_av; min_name = row->name; }
        if (row->ratio_av > max_ratio) { max_ratio = row->ratio_av; max_name = row->name; }
        if (row->exp_a64 < min_exp) { min_exp = row->exp_a64; min_exp_name = row->name; }
        if (row->exp_a64 > max_exp) { max_exp = row->exp_a64; max_exp_name = row->name; }
    }
    for (size_t i = 0; i < nr; i++) {
        if (!find_a64(RV64_BASELINES[i].name)) {
            fprintf(stderr, "FAIL  orphan RV64 row: %s has no A64 baseline (update bench_baselines.h — see plan doc §10.166)\n",
                    RV64_BASELINES[i].name);
            fails++;
        }
        if (!find_interp(RV64_BASELINES[i].name)) {
            fprintf(stderr, "FAIL  orphan RV64 row: %s has no interpreter baseline (update bench_baselines_interp.h — see plan doc §10.174)\n",
                    RV64_BASELINES[i].name);
            fails++;
        }
    }
    /* interp-only rows: every interp row must be in the shared set except
     * mem_ops.simi (the deliberate interpreter-only fixture). Its steps are
     * NOT added to tot_interp — the expansion factors divide the shared-96
     * totals only (a fair per-ISA comparison over the same fixture set). */
    for (size_t i = 0; i < ni; i++) {
        if (strcmp(INTERP_BASELINES[i].name, "mem_ops.simi") == 0)
            continue;
        if (!find_a64(INTERP_BASELINES[i].name)) {
            fprintf(stderr, "FAIL  orphan interp row: %s has no A64 baseline (update bench_baselines.h — see plan doc §10.174)\n",
                    INTERP_BASELINES[i].name);
            fails++;
        }
    }
    const struct InterpBaseline* mem_ops = find_interp("mem_ops.simi");
    if (!mem_ops) {
        fprintf(stderr, "FAIL  interpreter table missing mem_ops.simi (the deliberate interpreter-only fixture — see plan doc §10.174)\n");
        fails++;
    }

    qsort(rows, nrows, sizeof(rows[0]), cmp_ratio_desc);
    printf("=== cross-ISA execution-work (A64 vs RV64, committed steps) ===\n");
    printf("fixture                     a64    rv64   a64/rv64\n");
    for (size_t i = 0; i < nrows; i++)
        printf("%-24s %5lld %6lld %8.3f\n", rows[i].name, rows[i].a64, rows[i].rv64, rows[i].ratio_av);

    qsort(rows, nrows, sizeof(rows[0]), cmp_exp_desc);
    printf("\n=== interpreter expansion (committed SIMI ground-truth steps) ===\n");
    printf("fixture                     interp  a64    rv64  a64/interp rv64/interp\n");
    for (size_t i = 0; i < nrows; i++)
        printf("%-24s %6lld %5lld %6lld %10.3f %11.3f\n",
               rows[i].name, rows[i].interp, rows[i].a64, rows[i].rv64,
               rows[i].exp_a64, rows[i].exp_rv64);
    if (mem_ops)
        printf("%-24s %6lld  (interp-only: the translated legs use mem_ops_native instead)\n",
               mem_ops->name, mem_ops->steps);

    double agg = (double)tot_a64 / (double)tot_rv64;
    double exp_a64 = (double)tot_a64 / (double)tot_interp;
    double exp_rv64 = (double)tot_rv64 / (double)tot_interp;
    printf("\ntotals: a64=%lld rv64=%lld interp=%lld%c%lld (%s)  aggregate ratio=%.4f\n",
           tot_a64, tot_rv64, tot_interp,
           mem_ops ? '+' : ' ', mem_ops ? mem_ops->steps : 0,
           mem_ops ? "mem_ops" : "?", agg);
    printf("expansion: a64/interp=%.4f rv64/interp=%.4f\n", exp_a64, exp_rv64);
    printf("a64/rv64 per-fixture range: %.3f (%s) .. %.3f (%s) over %zu fixtures\n",
           min_ratio, min_name ? min_name : "?", max_ratio, max_name ? max_name : "?", nrows);
    printf("a64/interp per-fixture expansion range: %.3f (%s) .. %.3f (%s) over %zu fixtures\n",
           min_exp, min_exp_name ? min_exp_name : "?", max_exp, max_exp_name ? max_exp_name : "?", nrows);

    if (nrows != na || nrows != nr) {
        fprintf(stderr, "FAIL  fixture-set mismatch: a64=%zu rv64=%zu paired=%zu\n", na, nr, nrows);
        fails++;
    }
    if (ni != na + 1) {
        fprintf(stderr, "FAIL  fixture-set mismatch: interp=%zu a64=%zu (interp must be a64 + mem_ops.simi)\n", ni, na);
        fails++;
    }
    double lo = AGG_RATIO_MEASURED * (1.0 - AGG_RATIO_BAND);
    double hi = AGG_RATIO_MEASURED * (1.0 + AGG_RATIO_BAND);
    if (agg < lo || agg > hi) {
        fprintf(stderr, "FAIL  aggregate ratio %.4f outside committed band [%.4f, %.4f] around %.4f (codegen mix drift)\n",
                agg, lo, hi, AGG_RATIO_MEASURED);
        fails++;
    }
    double elo = EXP_A64_MEASURED * (1.0 - EXP_BAND);
    double ehi = EXP_A64_MEASURED * (1.0 + EXP_BAND);
    if (EXP_A64_MEASURED > 0.0 && (exp_a64 < elo || exp_a64 > ehi)) {
        fprintf(stderr, "FAIL  a64/interp expansion %.4f outside committed band [%.4f, %.4f] around %.4f (ISA expansion drift)\n",
                exp_a64, elo, ehi, EXP_A64_MEASURED);
        fails++;
    }
    double rlo = EXP_RV64_MEASURED * (1.0 - EXP_BAND);
    double rhi = EXP_RV64_MEASURED * (1.0 + EXP_BAND);
    if (EXP_RV64_MEASURED > 0.0 && (exp_rv64 < rlo || exp_rv64 > rhi)) {
        fprintf(stderr, "FAIL  rv64/interp expansion %.4f outside committed band [%.4f, %.4f] around %.4f (ISA expansion drift)\n",
                exp_rv64, rlo, rhi, EXP_RV64_MEASURED);
        fails++;
    }

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %zu paired fixtures, aggregate ratio %.4f, expansion a64/interp %.4f rv64/interp %.4f within their committed bands\n",
           nrows, agg, exp_a64, exp_rv64);
    return 0;
}
