/*
 * timi_riscv_verify.c — Phase 5 verification harness.
 *
 * Deliberately NOT named "jit test" like its x86 counterpart
 * (timi_jit_test.c) — this does not execute real RISC-V machine code on
 * real (or emulated) hardware. It translates a real .tmo with the exact
 * timi_riscv.c that (eventually) ports into the kernel, then feeds the
 * resulting bytes into rv64_exec.c, a small purpose-built decoder+
 * executor, and checks the resulting register value. See rv64_exec.h and
 * AeroSLS-TIMI-ISA-v0.1.md §12 for why this is the honest fallback given
 * no riscv64 toolchain or QEMU is available in this environment, and how
 * it compares in strength to Phase 3's real-CPU-execution proof.
 *
 * Usage: timi-riscv-verify program.tmo entry_name expected_value
 */
#include "timi_riscv.h"
#include "rv64_exec.h"
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
 * names/sizes/types as timi_interp.c's g_mock_catalog and
 * timi_jit_test.c's, so tests/obj_ops.tmo produces the identical expected
 * result (305) across all three implementations. These run as REAL host
 * C functions, invoked by rv64_exec.c's host-callback sentinel mechanism
 * (see rv64_exec.h) when translated RV64 code executes a `jalr` to one of
 * the sentinel addresses handed to timi_riscv_translate() below. */
struct MockCatalogEntry {
    const char *name;
    uint64_t base_vaddr;
    uint32_t byte_size;
    uint32_t obj_type;
};
static const struct MockCatalogEntry g_mock_catalog[] = {
    { "timi_add_test2", 0x2000, 303, 1 },
    { "timi_verify3",   0x3000, 100, 2 },
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
 * bytes (r6/namepool_ptr — see timi_riscv.c), which for this harness IS a
 * real host pointer: obj_data is a normal host malloc'd buffer, and
 * timi_riscv_translate() bakes namepool_ptr as a pointer straight into it
 * (not into guest_mem), so mock_rt_resolve can dereference it directly. */
#define HOSTFN_RESOLVE 0
#define HOSTFN_OBJSIZE 1
#define HOSTFN_OBJTYPE 2

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s program.tmo entry_name expected_value\n", argv[0]);
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

    rv64_exec_set_hostfn(HOSTFN_RESOLVE, mock_rt_resolve);
    rv64_exec_set_hostfn(HOSTFN_OBJSIZE, mock_rt_objsize);
    rv64_exec_set_hostfn(HOSTFN_OBJTYPE, mock_rt_objtype);

    uint32_t out_len = 0, entry_off = 0;
    int rc = timi_riscv_translate(obj, (uint32_t)sz, guest_mem, CODE_CAP,
                                   entry_name, scratch_ptr,
                                   rv64_exec_hostfn_addr(HOSTFN_RESOLVE),
                                   rv64_exec_hostfn_addr(HOSTFN_OBJSIZE),
                                   rv64_exec_hostfn_addr(HOSTFN_OBJTYPE),
                                   &out_len, &entry_off);
    if (rc != TX_RV_OK) {
        fprintf(stderr, "FAIL  %-28s translate error: %s\n", path, timi_riscv_strerror(rc));
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

    long long result = (long long)cpu.x[5 /* t0 — result register, see timi_riscv.c's RET/trampoline design */];
    if (result == expected) {
        printf("PASS  %-28s = %lld  (%u bytes native code)\n", path, result, out_len);
        return 0;
    } else {
        printf("FAIL  %-28s expected %lld, got %lld\n", path, expected, result);
        return 1;
    }
}
