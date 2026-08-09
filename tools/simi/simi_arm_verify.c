/*
 * simi_arm_verify.c — M0 verification harness.
 *
 * Deliberately NOT named "jit test" like its x86 counterpart
 * (simi_jit_test.c) — this does not execute real AArch64 machine code on
 * real (or emulated) hardware. It translates a real .tmo with the exact
 * simi_arm.c that (eventually) ports into the kernel, then feeds the
 * resulting bytes into a64_exec.c, a small purpose-built A64 decoder+
 * executor, and checks the resulting register value. See a64_exec.h and
 * docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §2 for why this is the
 * honest fallback given no AArch64 toolchain or QEMU is available in this
 * environment, and how it compares in strength to Phase 3's real-CPU-
 * execution proof and Phase 5's RV64 decoder proof.
 *
 * Usage: simi-arm-verify program.tmo entry_name expected_value [--steps]
 *
 * With `--steps` (M2.75): after a successful run, the executed
 * instruction count (A64Cpu.steps) must equal the fixture's COMMITTED
 * count in bench_baselines.h — the same table bench-exec gates on —
 * keyed by the .tmo's fixture name (basename with .tmo -> .simi). This
 * catches decode regressions at the parity harness itself, before any
 * bench runs: a change in how much the translated code executes (a
 * decode that now faults early, a fold that alters the emitted control
 * flow, an a64_exec regression) moves the count and fails the fixture.
 * The committed counts are for the "main" entry's full run — the entry
 * the runners execute. A fixture with no committed row FAILS loudly
 * (adding/removing a corpus fixture requires updating the table
 * deliberately). The check is skipped when execution does not complete
 * (a fault is already reported as FAIL).
 *
 * The result register is x9 (t0): simi_arm.c's OP_RET leaves r0's value
 * in t0 (x9) and r0's capability tag in t1 (x10) — the epilogue
 * (add sp; ldr x30; ldr x29; add sp; br x30) touches neither, and the
 * trampoline's own `br x30` to the sentinel doesn't either, so when
 * a64_exec_run() returns AR_EXEC_OK the value is still in x9. This is
 * the exact convention rv64_exec.c's harness uses for t0 (x5).
 */
#include "simi_arm.h"
#include "a64_exec.h"
#include "bench_baselines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_CAP     262144u   /* 256 KiB — same generous cap as RV64; A64's
                                * movz/movk constants are 2-4 words so this is
                                * plenty even for the whole test corpus. */
#define STACK_SIZE    65536u
#define SCRATCH_SIZE   4096u
#define GUEST_MEM_SIZE (CODE_CAP + STACK_SIZE + SCRATCH_SIZE)

/* v0.3 (Phase 6): mock object catalog for RESOLVE/OBJSIZE/OBJTYPE — the
 * SAME names/sizes/types as simi_interp.c's g_mock_catalog and
 * simi_riscv_verify.c's, so tests/obj_ops.simi produces the identical
 * expected result (305) across all implementations. These run as REAL
 * host C functions, invoked by a64_exec.c's host-callback sentinel
 * mechanism (see a64_exec.h) when translated A64 code executes a `blr`
 * to one of the sentinel addresses handed to simi_arm_translate() below. */
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
 * bytes (r6/namepool_ptr — see simi_arm.c), which for this harness IS a
 * real host pointer: obj_data is a normal host malloc'd buffer, and
 * simi_arm_translate() bakes namepool_ptr as a pointer straight into it
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

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* obj = malloc((size_t)sz);
    if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    uint8_t* guest_mem = calloc(1, GUEST_MEM_SIZE);
    if (!guest_mem) { perror("calloc guest_mem"); return 1; }

    uint64_t scratch_ptr = (uint64_t)CODE_CAP + STACK_SIZE;

    a64_exec_set_hostfn(HOSTFN_RESOLVE, mock_rt_resolve);
    a64_exec_set_hostfn(HOSTFN_OBJSIZE, mock_rt_objsize);
    a64_exec_set_hostfn(HOSTFN_OBJTYPE, mock_rt_objtype);

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_arm_translate(obj, (uint32_t)sz, guest_mem, CODE_CAP,
                                 entry_name, scratch_ptr,
                                 a64_exec_hostfn_addr(HOSTFN_RESOLVE),
                                 a64_exec_hostfn_addr(HOSTFN_OBJSIZE),
                                 a64_exec_hostfn_addr(HOSTFN_OBJTYPE),
                                 &out_len, &entry_off);
    if (rc != TX_AR_OK) {
        fprintf(stderr, "FAIL  %-28s translate error: %s\n", path, simi_arm_strerror(rc));
        return 1;
    }

    struct A64Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = guest_mem;
    cpu.mem_size = GUEST_MEM_SIZE;
    cpu.pc = entry_off;
    cpu.x[30 /* x30 = LR */] = AR_EXEC_SENTINEL_LR;
    cpu.x[31 /* sp */] = (uint64_t)CODE_CAP + STACK_SIZE - 16;

    int erc = a64_exec_run(&cpu, 10000000ull);
    if (erc != AR_EXEC_OK) {
        fprintf(stderr, "FAIL  %-28s execution error: %s (pc=0x%llx)\n",
                path, a64_exec_strerror(erc), (unsigned long long)cpu.pc);
        return 1;
    }

    long long result = (long long)cpu.x[9 /* t0 — result register, see simi_arm.c's OP_RET/trampoline design */];
    if (result != expected) {
        printf("FAIL  %-28s expected %lld, got %lld\n", path, expected, result);
        return 1;
    }

    if (check_steps) {
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

        const struct FixtureBaseline* bl = NULL;
        for (size_t i = 0; i < sizeof(BASELINES) / sizeof(BASELINES[0]); i++)
            if (strcmp(BASELINES[i].name, fname) == 0) { bl = &BASELINES[i]; break; }
        if (!bl) {
            fprintf(stderr, "FAIL  %-28s --steps: no committed baseline for %s (update bench_baselines.h — see plan doc §10.160)\n",
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
