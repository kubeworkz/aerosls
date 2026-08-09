/*
 * simi_jit_test.c — Phase 3 verification harness.
 *
 * This is the strongest verification available without a bootable AeroSLS
 * image: it translates a real .tmo (produced by the Phase 1 simi-asm) with
 * the exact same simi_x86.c that will be ported into the kernel, mmaps the
 * result PROT_EXEC, and *actually executes it* on the host CPU via a
 * function-pointer call — not a simulation, not a static read of the
 * bytes. If the encoder has a bug, this either crashes (SIGSEGV/SIGILL,
 * caught and reported below) or returns the wrong value.
 *
 * Usage: simi-jit-test program.tmo entry_name expected_value [--bytes]
 *
 * With `--bytes` (M2.76): the emitted translation's size (out_len) must
 * equal the fixture's COMMITTED count in bench_baselines_x86.h — the
 * deterministic, machine-independent emission tripwire for the native
 * leg. The x86 leg runs real machine code on the host CPU, so there is
 * no executor to count retired instructions and this sandbox has no
 * virtualized PMU (perf_event_open PERF_COUNT_HW_INSTRUCTIONS returns
 * ENOENT — probed); the emitted-byte count is the x86 analog of the
 * ARM size gate's per-fixture baselines, asserted right in the native
 * parity runner so ANY emission change fails the fixture. Dynamic
 * correctness remains the execution itself + the result check + the
 * crash handler. Keyed by the .tmo's fixture name (basename with .tmo
 * -> .simi); a fixture with no committed row FAILS loudly.
 */
#define _GNU_SOURCE
#include "simi_x86.h"
#include "bench_baselines_x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

static jmp_buf g_jmp;
static void crash_handler(int sig) {
    (void)sig;
    longjmp(g_jmp, 1);
}

/* v0.3 (Phase 6): mock object catalog for RESOLVE/OBJSIZE/OBJTYPE, called
 * as REAL functions by translated x86-64 code via a baked-in absolute
 * address (see simi_x86.h/.c). Deliberately the same names/sizes/types as
 * the Phase 1 reference interpreter's g_mock_catalog in simi_interp.c, so
 * tests/obj_ops.tmo produces the identical expected result (305) whether
 * it runs interpreted or natively — cross-implementation agreement is the
 * whole point of this harness (see the top comment). */
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

uint64_t simi_rt_resolve(const char *name) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (strcmp(g_mock_catalog[i].name, name) == 0) return g_mock_catalog[i].base_vaddr;
    return 0;
}
uint64_t simi_rt_objsize(uint64_t base_vaddr) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (g_mock_catalog[i].base_vaddr == base_vaddr) return g_mock_catalog[i].byte_size;
    return 0;
}
uint64_t simi_rt_objtype(uint64_t base_vaddr) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (g_mock_catalog[i].base_vaddr == base_vaddr) return g_mock_catalog[i].obj_type;
    return 0xFFFFFFFFu;
}

int main(int argc, char** argv) {
    /* volatile: set before setjmp(), read after the longjmp-protected
     * execution — the crash path returns before the read, but the
     * compiler can't prove that. */
    volatile int check_bytes = 0;
    if (argc == 5 && strcmp(argv[4], "--bytes") == 0) {
        check_bytes = 1;
    } else if (argc != 4) {
        fprintf(stderr, "usage: %s program.tmo entry_name expected_value [--bytes]\n", argv[0]);
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

    /* Scratch buffer for r7 (see simi_x86.h — the trampoline convention
     * standing in for real SIMI object/pointer allocation). A page-aligned
     * heap buffer is a legitimate, writable, real address, exactly the
     * kind of thing the kernel-side trampoline will substitute its own
     * process-owned scratch region for. */
    uint8_t* scratch = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) { perror("mmap scratch"); return 1; }

    uint32_t cap = 65536;
    uint8_t* code = mmap(NULL, cap, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap code"); return 1; }

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_x86_translate(obj, (uint32_t)sz, code, cap,
                                 entry_name, (uint64_t)(uintptr_t)scratch,
                                 (uint64_t)(uintptr_t)simi_rt_resolve,
                                 (uint64_t)(uintptr_t)simi_rt_objsize,
                                 (uint64_t)(uintptr_t)simi_rt_objtype,
                                 &out_len, &entry_off);
    if (rc != TX_OK) {
        fprintf(stderr, "FAIL  %-28s translate error: %s\n", path, simi_x86_strerror(rc));
        return 1;
    }

    if (mprotect(code, cap, PROT_READ|PROT_EXEC) != 0) { perror("mprotect"); return 1; }

    typedef long long (*entry_fn)(void);
    entry_fn fn = (entry_fn)(void*)(code + entry_off);

    struct sigaction sa = {0}, old_segv, old_ill, old_fpe;
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGFPE,  &sa, &old_fpe);   /* catches real hardware #DE too */

    long long result;
    if (setjmp(g_jmp) == 0) {
        result = fn();
    } else {
        fprintf(stderr, "FAIL  %-28s CRASHED during execution (bad codegen)\n", path);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGILL,  &old_ill,  NULL);
        sigaction(SIGFPE,  &old_fpe,  NULL);
        return 1;
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGFPE,  &old_fpe,  NULL);

    if (result != expected) {
        printf("FAIL  %-28s expected %lld, got %lld\n", path, expected, result);
        return 1;
    }

    if (check_bytes) {
        /* derive the fixture name: <dir>/<fixture>.tmo -> <fixture>.simi */
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;
        size_t blen = strlen(base);
        char fname[256];
        if (blen < 4 || strcmp(base + blen - 4, ".tmo") != 0 ||
            blen - 4 + 5 >= sizeof(fname)) {
            fprintf(stderr, "FAIL  %-28s --bytes: cannot derive fixture name from '%s'\n", path, path);
            return 1;
        }
        memcpy(fname, base, blen - 4);
        memcpy(fname + blen - 4, ".simi", 5);
        fname[blen - 4 + 5] = '\0';

        const struct X86Baseline* bl = NULL;
        for (size_t i = 0; i < sizeof(X86_BASELINES) / sizeof(X86_BASELINES[0]); i++)
            if (strcmp(X86_BASELINES[i].name, fname) == 0) { bl = &X86_BASELINES[i]; break; }
        if (!bl) {
            fprintf(stderr, "FAIL  %-28s --bytes: no committed baseline for %s (update bench_baselines_x86.h — see plan doc §10.162)\n",
                    path, fname);
            return 1;
        }
        if ((long long)out_len != bl->bytes) {
            fprintf(stderr, "FAIL  %-28s --bytes: emitted %u bytes != committed %lld (emission regression)\n",
                    path, out_len, bl->bytes);
            return 1;
        }
    }

    printf("PASS  %-28s = %lld  (%u bytes native code%s)\n",
           path, result, out_len, check_bytes ? ", bytes ok" : "");
    return 0;
}
