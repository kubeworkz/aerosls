/* cross_size.c — M2.79: cross-ISA EMITTED-SIZE report (A64 vs x86), the
 * code-density dimension the M2.78 step report misses.
 *
 * The two sides are honestly asymmetric, exactly matching where their
 * truth lives:
 *   - A64: the size gate (size_gate_arm.sh) measures the CURRENT
 *     emitted bytes (its "M1") LIVE every run — there is no committed
 *     current-A64 table, because every codegen change moves it. So this
 *     tool does the same: it translates each fixture with the real
 *     simi_arm.c and reads out_len (translate-only — no execution, the
 *     bench-corpus pattern). This is the same measurement the size gate
 *     gates on (M1 <= M0).
 *   - x86: the CURRENT emitted bytes are COMMITTED in
 *     bench_baselines_x86.h (the simi-jit-test --bytes tripwire's
 *     table, M2.76), so the tool reads them from there.
 *
 * Output: the per-fixture A64-vs-x86 emitted-byte ratio table sorted by
 * ratio (the code-density extremes visible), the totals and the
 * aggregate ratio (A64 M1 total / x86 total). Gates, mirroring
 * cross_isa:
 *   1. No orphans — every fixture must translate on the A64 side AND
 *      have a committed x86 row (a fixture in one side missing from the
 *      other is a deliberate-update error).
 *   2. The aggregate ratio within +/-AGG_RATIO_BAND of the measured
 *      value (2026-08-09). A codegen change that rebalances the two
 *      ISAs' emitted sizes moves it; the band tolerates deliberate
 *      divergence while failing gross drift.
 *
 * Skips, mirroring the parity runners exactly: mem_ops.simi (address-0
 * pointer is a Phase 1 interpreter-only convenience — see
 * mem_ops_native) and fixtures with no "Expected result:" comment
 * (jmpr_oob — its whole point is that execution must NOT produce one —
 * and cap_forge_debug), which have no committed x86 row.
 *
 * Usage: cross-size <fixture.simi>...   (the Makefile passes the whole
 * tests/ corpus, assembled on the spot when stale/missing). In `all`
 * — translate-only for the corpus is ~1 s. See plan doc §10.168/10.169.
 */
#define _GNU_SOURCE
#include "simi_arm.h"
#include "bench_baselines_x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGG_RATIO_MEASURED 0.9155  /* A64 M1 211028 / x86 230505, 2026-08-10 (re-measured with lcg_fairness's row: 1304 A64 / 1086 x86) */
#define AGG_RATIO_BAND     0.25   /* +/-25% tolerance on the aggregate */

#define CODE_CAP 262144u   /* same as the other ARM tools */

struct Row {
    const char* name;
    long long a64;
    long long x86;
    double ratio;
};

static int cmp_ratio_desc(const void* a, const void* b) {
    const struct Row* ra = (const struct Row*)a;
    const struct Row* rb = (const struct Row*)b;
    if (ra->ratio > rb->ratio) return -1;
    if (ra->ratio < rb->ratio) return  1;
    return strcmp(ra->name, rb->name);
}

static const struct X86Baseline* find_x86(const char* name) {
    for (size_t i = 0; i < sizeof(X86_BASELINES) / sizeof(X86_BASELINES[0]); i++)
        if (strcmp(X86_BASELINES[i].name, name) == 0) return &X86_BASELINES[i];
    return NULL;
}

/* Last "Expected result:" value, the runners' rule. Returns 1 if found,
 * 0 if the fixture has none (and must be skipped). */
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

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture.simi>...\n", argv[0]);
        return 2;
    }
    int nfixtures = argc - 1;
    struct Row rows[256];
    size_t nrows = 0;
    int fails = 0, nskip = 0;

    long long tot_a64 = 0, tot_x86 = 0;
    double min_ratio = 1e9, max_ratio = 0.0;
    const char *min_name = NULL, *max_name = NULL;
    uint8_t* code = malloc(CODE_CAP);
    if (!code) return 2;

    for (int a = 0; a < nfixtures; a++) {
        const char* path = argv[a + 1];
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;

        if (strcmp(name, "mem_ops.simi") == 0) {
            printf("SKIP  %-24s address-0 pointer is a Phase 1 interpreter-only convenience (see mem_ops_native)\n", name);
            nskip++;
            continue;
        }
        if (strcmp(name, "arm64_boot_smoke.simi") == 0) {
            printf("SKIP  %-24s kernel-embedded boot fixture (M4b/M5.2), outside the parity corpus — no x86 --bytes row (plan doc §10.196)\n", name);
            nskip++;
            continue;
        }
        if (strcmp(name, "lcg_slice.simi") == 0) {
            /* §10.200: kernel-embedded LCG fixture (the arm64 kernel
             * boot is its gate) — outside the parity corpus, no x86
             * --bytes row; its A64 emission is pinned by the size
             * gate's M0 row instead. */
            printf("SKIP  %-24s kernel-embedded LCG fixture (arm64 kernel boot is its gate, plan doc §10.200) — no x86 --bytes row\n", name);
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
        size_t nlen = strlen(name);
        char fname[256];
        if (nlen < 5 || nlen - 5 + 5 >= sizeof(fname)) {
            fprintf(stderr, "FAIL  cannot derive fixture name from '%s'\n", path);
            fails++;
            free(tmopath);
            continue;
        }
        memcpy(fname, name, nlen - 5);   /* strip the .simi suffix */
        memcpy(fname + nlen - 5, ".simi", 5);
        fname[nlen - 5 + 5] = '\0';

        FILE* f = fopen(tmopath, "rb");
        if (!f) { perror(tmopath); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fprintf(stderr, "empty .tmo: %s\n", tmopath); return 2; }
        uint8_t* obj = malloc((size_t)sz);
        if (!obj) return 2;
        if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        free(tmopath);

        g_ar_net_trip = 16;   /* the shipped default */
        uint32_t out_len = 0, entry_off = 0;
        int rc = simi_arm_translate(obj, (uint32_t)sz, code, CODE_CAP,
                                     "main", 0, 0, 0, 0, &out_len, &entry_off);
        free(obj);
        if (rc != TX_AR_OK) {
            fprintf(stderr, "translate error %s: %s\n", fname, simi_arm_strerror(rc));
            fails++;
            continue;
        }

        const struct X86Baseline* x = find_x86(fname);
        if (!x) {
            fprintf(stderr, "FAIL  %-24s no committed x86 baseline (update bench_baselines_x86.h — see plan doc §10.168)\n", fname);
            fails++;
            continue;
        }
        struct Row* row = &rows[nrows++];
        row->name = strdup(fname);
        row->a64 = out_len;
        row->x86 = x->bytes;
        row->ratio = (double)row->a64 / (double)row->x86;
        tot_a64 += row->a64;
        tot_x86 += row->x86;
        if (row->ratio < min_ratio) { min_ratio = row->ratio; min_name = row->name; }
        if (row->ratio > max_ratio) { max_ratio = row->ratio; max_name = row->name; }
    }
    free(code);

    /* orphan check on the x86 side: every committed row must have been
     * measured on the A64 side (a fixture dropped from the A64 corpus
     * would silently disappear from the report). */
    for (size_t i = 0; i < sizeof(X86_BASELINES) / sizeof(X86_BASELINES[0]); i++) {
        int seen = 0;
        for (size_t r = 0; r < nrows; r++)
            if (strcmp(rows[r].name, X86_BASELINES[i].name) == 0) { seen = 1; break; }
        if (!seen) {
            fprintf(stderr, "FAIL  orphan x86 row: %s was not measured on the A64 side (fixture missing from the corpus?)\n",
                    X86_BASELINES[i].name);
            fails++;
        }
    }

    qsort(rows, nrows, sizeof(rows[0]), cmp_ratio_desc);

    printf("=== cross-ISA emitted size (A64 M1 live vs x86 committed bytes) ===\n");
    printf("fixture                     a64     x86   a64/x86\n");
    for (size_t i = 0; i < nrows; i++)
        printf("%-24s %5lld %6lld %8.3f\n", rows[i].name, rows[i].a64, rows[i].x86, rows[i].ratio);

    double agg = (double)tot_a64 / (double)tot_x86;
    printf("\ntotals: a64=%lld x86=%lld  aggregate ratio=%.4f\n", tot_a64, tot_x86, agg);
    printf("per-fixture ratio range: %.3f (%s) .. %.3f (%s) over %zu fixtures\n",
           min_ratio, min_name ? min_name : "?", max_ratio, max_name ? max_name : "?", nrows);

    if (nrows != (size_t)nfixtures - (size_t)nskip) {
        fprintf(stderr, "FAIL  measured %zu of %d fixtures (%d skipped)\n", nrows, nfixtures, nskip);
        fails++;
    }
    double lo = AGG_RATIO_MEASURED * (1.0 - AGG_RATIO_BAND);
    double hi = AGG_RATIO_MEASURED * (1.0 + AGG_RATIO_BAND);
    if (agg < lo || agg > hi) {
        fprintf(stderr, "FAIL  aggregate ratio %.4f outside committed band [%.4f, %.4f] around %.4f (code-size mix drift)\n",
                agg, lo, hi, AGG_RATIO_MEASURED);
        fails++;
    }

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %zu paired fixtures (%d skipped), aggregate ratio %.4f within the committed band\n",
           nrows, nskip, agg);
    return 0;
}
