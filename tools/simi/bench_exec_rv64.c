/* bench_exec_rv64.c — M2.77: corpus-wide EXECUTION-cost benchmark for
 * the RV64 leg, the symmetric counterpart of bench_exec.c (which drives
 * a64_exec). bench_corpus pins translate cost; bench-exec pins the A64
 * execution cost; THIS pins what the translated RV64 code COSTS TO RUN:
 * every fixture is translated once, then executed N times through
 * rv64_exec.c — the same purpose-built RV64 decoder/executor the
 * four-way parity uses (simi_riscv_verify.c) — with the same mock host
 * functions and the same per-fixture Expected result parsed from the
 * .simi comment (run_riscv_tests.sh's rule: the LAST "Expected result:"
 * match).
 *
 * COMMITTED PER-ROW BASELINES, the bench_exec two-tier model — the
 * table lives in bench_baselines_rv64.h, the single source of truth
 * shared with the simi-riscv-verify --steps check:
 *   - steps: EXACT. The deterministic, machine-independent execution
 *     work per fixture (RvCpu.steps, rv64_exec_run's per-instruction
 *     counter, added M2.76). ANY change in how much the translated code
 *     executes — a decode that now faults early, an emission change
 *     that alters the control flow, an rv64_exec regression — moves the
 *     count and fails the row. This is the tight guard.
 *   - ns/call: asserted at BASELINE_MARGIN over the committed value
 *     (median of three N=500 passes). Same WSL wall-clock caveats as
 *     bench_exec — the margin is sized from the measured per-fixture
 *     noise (see the BASELINE_MARGIN define and plan doc
 *     §10.164/10.165).
 *
 * Execution is deterministic here — the same translated bytes over
 * zeroed guest state produce the same step count and the same result —
 * and the bench ASSERTS that: every iteration must execute the same
 * number of steps and produce the expected x5, or the row fails (this
 * also catches the fault-early hazard: a fixture that now faults after
 * 3 instructions looks FAST on the clock but fails both checks).
 *
 * Skips, mirroring run_riscv_tests.sh exactly: mem_ops.simi (address-0
 * pointer is a Phase 1 interpreter-only convenience — see
 * mem_ops_native) and fixtures with no "Expected result:" comment
 * (jmpr_oob — its whole point is that execution must NOT produce one —
 * and cap_forge_debug).
 *
 * A fixture with no baseline row FAILS loudly — adding or removing a
 * corpus fixture requires updating the table deliberately (the size
 * gate's M0_BASELINES discipline). Re-measure and update the table only
 * deliberately: RV64_BENCH_MEASURE=1 bench-exec-rv64 <tests/ fixtures>
 * <iters> prints the table-format rows without asserting. See plan doc
 * §10.164/10.165.
 *
 * Usage: bench-exec-rv64 <fixture.simi>... <iters>
 *   (the Makefile passes the whole tests/ corpus and the default iters;
 *   the .tmo path is derived from each .simi by swapping the extension)
 */
#define _GNU_SOURCE   /* clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#include "simi_riscv.h"
#include "rv64_exec.h"
#include "bench_baselines_rv64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BASELINE_MARGIN 4.0   /* ns/call wall-clock margin (see above; sized
                               * from measurement — same 2.3x-class tail as
                               * bench_exec's A64 side on this sandbox) */

#define CODE_CAP     262144u   /* same caps as simi_riscv_verify.c */
#define STACK_SIZE    65536u
#define SCRATCH_SIZE   4096u
#define GUEST_MEM_SIZE (CODE_CAP + STACK_SIZE + SCRATCH_SIZE)
#define MAX_STEPS     10000000ull   /* same budget as simi_riscv_verify.c */

/* v0.3 (Phase 6) mock object catalog — the SAME names/sizes/types as
 * simi_riscv_verify.c's, so obj_ops.simi produces the identical expected
 * result (305) here. */
struct MockCatalogEntry {
    const char *name;
    uint64_t base_vaddr;
    uint32_t byte_size;
    uint32_t obj_type;
};
static const struct MockCatalogEntry g_mock_catalog[] = {
    { "simi_add_test2", 0x2000, 303, 1 },
    { "simi_verify3",   0x3000, 100, 2 },
};
#define MOCK_CATALOG_N (sizeof(g_mock_catalog) / sizeof(g_mock_catalog[0]))

static uint64_t mock_rt_resolve(uint64_t name_ptr) {
    const char *name = (const char *)(uintptr_t)name_ptr;
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (strcmp(g_mock_catalog[i].name, name) == 0) return g_mock_catalog[i].base_vaddr;
    return 0;
}
static uint64_t mock_rt_objsize(uint64_t base_vaddr) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (g_mock_catalog[i].base_vaddr == base_vaddr) return g_mock_catalog[i].byte_size;
    return 0;
}
static uint64_t mock_rt_objtype(uint64_t base_vaddr) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (g_mock_catalog[i].base_vaddr == base_vaddr) return g_mock_catalog[i].obj_type;
    return 0xFFFFFFFFu;
}
#define HOSTFN_RESOLVE 0
#define HOSTFN_OBJSIZE 1
#define HOSTFN_OBJTYPE 2

/* Last "Expected result:" value, run_riscv_tests.sh's rule. Returns 1 if
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
    int measure = getenv("RV64_BENCH_MEASURE") != NULL;

    rv64_exec_set_hostfn(HOSTFN_RESOLVE, mock_rt_resolve);
    rv64_exec_set_hostfn(HOSTFN_OBJSIZE, mock_rt_objsize);
    rv64_exec_set_hostfn(HOSTFN_OBJTYPE, mock_rt_objtype);

    long long tot_steps = 0;
    double tot_ns = 0.0;
    int fails = 0, nrows = 0, nskip = 0;

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
            printf("SKIP  %-24s kernel-embedded boot fixture (M4b/M5.2): by design LONG-RUNNING (the 5e7-iteration contention-probe loop) — a 500x bench-exec-rv64 run would take ~an hour; outside the parity corpus (plan doc §10.196)\n", name);
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

        FILE* f = fopen(tmopath, "rb");
        if (!f) { perror(tmopath); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fprintf(stderr, "empty .tmo: %s\n", tmopath); return 2; }
        uint8_t* obj = malloc((size_t)sz);
        if (!obj) return 2;
        if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        free(tmopath);

        uint8_t* guest_mem = calloc(1, GUEST_MEM_SIZE);
        if (!guest_mem) return 2;
        uint64_t scratch_ptr = (uint64_t)CODE_CAP + STACK_SIZE;

        uint32_t out_len = 0, entry_off = 0;
        int rc = simi_riscv_translate(obj, (uint32_t)sz, guest_mem, CODE_CAP,
                                       "main", scratch_ptr,
                                       rv64_exec_hostfn_addr(HOSTFN_RESOLVE),
                                       rv64_exec_hostfn_addr(HOSTFN_OBJSIZE),
                                       rv64_exec_hostfn_addr(HOSTFN_OBJTYPE),
                                       &out_len, &entry_off);
        if (rc != TX_RV_OK) {
            fprintf(stderr, "translate error %s: %s\n", name, simi_riscv_strerror(rc));
            return 2;
        }

        struct RvCpu cpu;
        long long first_steps = -1;
        int row_fail = 0;
        long long done = 0;
        double t0 = now_sec();
        for (long long i = 0; i < iters && !row_fail; i++) {
            /* fresh data region + fresh CPU: runs must be independent and
             * deterministic (the code region is read-only and untouched). */
            memset(guest_mem + CODE_CAP, 0, GUEST_MEM_SIZE - CODE_CAP);
            memset(&cpu, 0, sizeof(cpu));
            cpu.mem = guest_mem;
            cpu.mem_size = GUEST_MEM_SIZE;
            cpu.pc = entry_off;
            cpu.x[1 /* ra */] = RV_EXEC_SENTINEL_RA;
            cpu.x[2 /* sp */] = (uint64_t)CODE_CAP + STACK_SIZE - 16;

            int erc = rv64_exec_run(&cpu, MAX_STEPS);
            if (erc != RV_EXEC_OK) {
                fprintf(stderr, "FAIL  %-24s execution error: %s (pc=0x%llx)\n",
                        name, rv64_exec_strerror(erc), (unsigned long long)cpu.pc);
                row_fail = 1;
                break;
            }
            if (i == 0) first_steps = (long long)cpu.steps;
            else if ((long long)cpu.steps != first_steps) {
                fprintf(stderr, "FAIL  %-24s nondeterministic steps: %lld != first run %lld\n",
                        name, (long long)cpu.steps, first_steps);
                row_fail = 1;
                break;
            }
            long long result = (long long)cpu.x[5];  /* t0 — result register (see simi_riscv_verify.c) */
            if (result != expected) {
                fprintf(stderr, "FAIL  %-24s expected %lld, got %lld (steps=%lld)\n",
                        name, expected, result, (long long)cpu.steps);
                row_fail = 1;
                break;
            }
            done++;
        }
        double t1 = now_sec();

        if (row_fail) {
            fails++;
            free(obj); free(guest_mem);
            continue;
        }
        double ns_call = (t1 - t0) * 1e9 / (double)done;

        if (measure) {
            printf("    { \"%s\", %lld, %.0f },\n", name, first_steps, ns_call);
            nrows++;
            free(obj); free(guest_mem);
            continue;
        }

        const struct Rv64Baseline* bl = NULL;
        for (size_t i = 0; i < sizeof(RV64_BASELINES) / sizeof(RV64_BASELINES[0]); i++)
            if (strcmp(RV64_BASELINES[i].name, name) == 0) { bl = &RV64_BASELINES[i]; break; }
        if (!bl) {
            fprintf(stderr, "FAIL  %-24s no committed baseline (update bench_baselines_rv64.h — see plan doc §10.164)\n", name);
            fails++;
            free(obj); free(guest_mem);
            continue;
        }
        if (first_steps != bl->steps) {
            fprintf(stderr, "FAIL  %-24s steps=%lld != committed %lld (execution work changed)\n",
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
        free(obj); free(guest_mem);
    }

    if (measure) {
        printf("\n%5d fixtures measured (%d skipped, %d failed) — paste into bench_baselines_rv64.h\n",
               nrows, nskip, fails);
        return fails ? 1 : 0;
    }

    double ns_agg = tot_ns / (double)(tot_steps > 0 ? tot_steps : 1);
    printf("\n%5d fixtures: %lld steps, %.1f ns/step aggregate (%d skipped, %d failed)\n",
           nfixtures, tot_steps, ns_agg, nskip, fails);

    if (fails) return 1;
    printf("ALL CHECKS PASSED — %d fixture rows within their committed baselines\n", nrows);
    return 0;
}
