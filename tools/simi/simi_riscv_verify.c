/*
 * simi_riscv_verify.c — Phase 5 verification harness.
 *
 * Deliberately NOT named "jit test" like its x86 counterpart
 * (simi_jit_test.c) — this does not execute real RISC-V machine code on
 * real (or emulated) hardware. It translates a real .tmo with the exact
 * simi_riscv.c that (eventually) ports into the kernel, then feeds the
 * resulting bytes into rv64_exec.c, a small purpose-built decoder+
 * executor, and checks the resulting register value. See rv64_exec.h and
 * AeroSLS-SIMI-ISA-v0.1.md §12 for why this is the honest fallback given
 * no riscv64 toolchain or QEMU is available in this environment, and how
 * it compares in strength to Phase 3's real-CPU-execution proof.
 *
 * Usage: simi-riscv-verify program.tmo entry_name expected_value [--steps]
 *
 * With `--steps` (M2.76): after a successful run, the executed
 * instruction count (RvCpu.steps) must equal the fixture's COMMITTED
 * count in bench_baselines_rv64.h — the deterministic,
 * machine-independent execution work per fixture — keyed by the .tmo's
 * fixture name (basename with .tmo -> .simi). This catches decode/
 * emission regressions at the parity harness itself, before any bench
 * runs (the same tripwire M2.75 added to simi-arm-verify). The
 * committed counts are for the "main" entry's full run — the entry the
 * runners execute. A fixture with no committed row FAILS loudly
 * (adding/removing a corpus fixture requires updating the table
 * deliberately). The check is skipped when execution does not complete
 * (a fault is already reported as FAIL). With RV64_STEPS_MEASURE=1 the
 * measured count is printed per fixture without asserting — the
 * documented re-measure tool for a deliberate table update.
 */
#include "simi_riscv.h"
#include "rv64_exec.h"
#include "bench_baselines_rv64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_CAP     262144u   /* 256 KiB — generous vs. x86's 64 KiB cap; auipc+ld
                                 * literal sequences run a few more instructions per
                                 * op than x86's single-instruction movabs. */
#define STACK_SIZE    65536u
#define SCRATCH_SIZE   4096u
#define GUEST_MEM_SIZE (CODE_CAP + STACK_SIZE + SCRATCH_SIZE)

/* v0.3 (Phase 6): mock object catalog for RESOLVE/OBJSIZE/OBJTYPE — same
 * names/sizes/types as simi_interp.c's g_mock_catalog and
 * simi_jit_test.c's, so tests/obj_ops.tmo produces the identical expected
 * result (305) across all three implementations. These run as REAL host
 * C functions, invoked by rv64_exec.c's host-callback sentinel mechanism
 * (see rv64_exec.h) when translated RV64 code executes a `jalr` to one of
 * the sentinel addresses handed to simi_riscv_translate() below. */
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

/* rt_resolve's argument is a raw pointer into the guest's own name-pool
 * bytes (r6/namepool_ptr — see simi_riscv.c), which for this harness IS a
 * real host pointer: obj_data is a normal host malloc'd buffer, and
 * simi_riscv_translate() bakes namepool_ptr as a pointer straight into it
 * (not into guest_mem), so mock_rt_resolve can dereference it directly. */
#define HOSTFN_RESOLVE 0
#define HOSTFN_OBJSIZE 1
#define HOSTFN_OBJTYPE 2

int main(int argc, char** argv) {
    int check_steps = 0;
    if (argc == 5 && strcmp(argv[4], "--steps") == 0) {
        check_steps = 1;
    } else if (argc != 4) {
        fprintf(stderr, "usage: %s program.tmo entry_name expected_value [--steps]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const char* entry_name = argv[2];
    long long expected = atoll(argv[3]);
    int measure = getenv("RV64_STEPS_MEASURE") != NULL;

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* obj = malloc((size_t)sz);
    if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    uint8_t* guest_mem = calloc(1, GUEST_MEM_SIZE);
    if (!guest_mem) { perror("calloc guest_mem"); return 1; }

    uint64_t scratch_ptr = (uint64_t)CODE_CAP + STACK_SIZE;

    rv64_exec_set_hostfn(HOSTFN_RESOLVE, mock_rt_resolve);
    rv64_exec_set_hostfn(HOSTFN_OBJSIZE, mock_rt_objsize);
    rv64_exec_set_hostfn(HOSTFN_OBJTYPE, mock_rt_objtype);

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_riscv_translate(obj, (uint32_t)sz, guest_mem, CODE_CAP,
                                   entry_name, scratch_ptr,
                                   rv64_exec_hostfn_addr(HOSTFN_RESOLVE),
                                   rv64_exec_hostfn_addr(HOSTFN_OBJSIZE),
                                   rv64_exec_hostfn_addr(HOSTFN_OBJTYPE),
                                   &out_len, &entry_off);
    if (rc != TX_RV_OK) {
        fprintf(stderr, "FAIL  %-28s translate error: %s\n", path, simi_riscv_strerror(rc));
        return 1;
    }

    struct RvCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = guest_mem;
    cpu.mem_size = GUEST_MEM_SIZE;
    cpu.pc = entry_off;
    cpu.x[1 /* ra */] = RV_EXEC_SENTINEL_RA;
    cpu.x[2 /* sp */] = (uint64_t)CODE_CAP + STACK_SIZE - 16;

    int erc = rv64_exec_run(&cpu, 10000000ull);
    if (erc != RV_EXEC_OK) {
        fprintf(stderr, "FAIL  %-28s execution error: %s (pc=0x%llx)\n",
                path, rv64_exec_strerror(erc), (unsigned long long)cpu.pc);
        return 1;
    }

    long long result = (long long)cpu.x[5 /* t0 — result register, see simi_riscv.c's RET/trampoline design */];
    if (result != expected) {
        printf("FAIL  %-28s expected %lld, got %lld\n", path, expected, result);
        return 1;
    }

    /* derive the fixture name: <dir>/<fixture>.tmo -> <fixture>.simi */
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t blen = strlen(base);
    char fname[256];
    if (blen < 4 || strcmp(base + blen - 4, ".tmo") != 0 ||
        blen - 4 + 5 >= sizeof(fname)) {
        fprintf(stderr, "FAIL  %-28s --steps: cannot derive fixture name from '%s'\n", path, path);
        return 1;
    }
    memcpy(fname, base, blen - 4);
    memcpy(fname + blen - 4, ".simi", 5);
    fname[blen - 4 + 5] = '\0';

    if (measure) {
        printf("MEASURE %-24s %llu\n", fname, (unsigned long long)cpu.steps);
        printf("PASS  %-28s = %lld  (%u bytes native code, steps measured)\n", path, result, out_len);
        return 0;
    }

    if (check_steps) {
        const struct Rv64Baseline* bl = NULL;
        for (size_t i = 0; i < sizeof(RV64_BASELINES) / sizeof(RV64_BASELINES[0]); i++)
            if (strcmp(RV64_BASELINES[i].name, fname) == 0) { bl = &RV64_BASELINES[i]; break; }
        if (!bl) {
            fprintf(stderr, "FAIL  %-28s --steps: no committed baseline for %s (update bench_baselines_rv64.h — see plan doc §10.162)\n",
                    path, fname);
            return 1;
        }
        if ((long long)cpu.steps != bl->steps) {
            fprintf(stderr, "FAIL  %-28s --steps: executed %llu instructions != committed %lld (decode/emission regression)\n",
                    path, (unsigned long long)cpu.steps, bl->steps);
            return 1;
        }
    }

    printf("PASS  %-28s = %lld  (%u bytes native code%s)\n",
           path, result, out_len, check_steps ? ", steps ok" : "");
    return 0;
}
