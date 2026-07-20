/* bench_harness.c — throwaway Phase 11 verification tool, NOT part of the
 * shipped toolchain. Translates a .tmo once (same simi_x86.c the kernel
 * gets), then calls the resulting function N times in a tight loop, timing
 * it with clock_gettime. Used to get a concrete wall-clock number for the
 * register allocator's win, comparing an allocator-enabled build against a
 * temporarily pool-disabled one (TX_POOL_SIZE hacked to 0) on the same
 * straight-line-heavy program. Not linked into any Makefile target.
 */
#define _GNU_SOURCE
#include "simi_x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

uint64_t simi_rt_resolve(const char *name) { (void)name; return 0; }
uint64_t simi_rt_objsize(uint64_t v) { (void)v; return 0; }
uint64_t simi_rt_objtype(uint64_t v) { (void)v; return 0xFFFFFFFFu; }

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s program.tmo iters\n", argv[0]); return 1; }
    long long iters = atoll(argv[2]);

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* obj = malloc((size_t)sz);
    fread(obj, 1, (size_t)sz, f);
    fclose(f);

    uint8_t* scratch = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uint32_t cap = 65536;
    uint8_t* code = mmap(NULL, cap, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_x86_translate(obj, (uint32_t)sz, code, cap, "main",
                                 (uint64_t)(uintptr_t)scratch,
                                 (uint64_t)(uintptr_t)simi_rt_resolve,
                                 (uint64_t)(uintptr_t)simi_rt_objsize,
                                 (uint64_t)(uintptr_t)simi_rt_objtype,
                                 &out_len, &entry_off);
    if (rc != TX_OK) { fprintf(stderr, "translate error: %s\n", simi_x86_strerror(rc)); return 1; }
    mprotect(code, cap, PROT_READ|PROT_EXEC);

    typedef long long (*entry_fn)(void);
    entry_fn fn = (entry_fn)(void*)(code + entry_off);

    volatile long long sink = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long long i = 0; i < iters; i++) sink += fn();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%u bytes native code, %lld iters, %.4f sec, %.2f ns/call, sink=%lld\n",
           out_len, iters, secs, secs * 1e9 / (double)iters, sink);
    return 0;
}
