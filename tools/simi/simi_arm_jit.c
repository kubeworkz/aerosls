/*
 * simi_arm_jit.c — M3: the REAL AArch64 execution proof.
 *
 * simi_arm_verify.c feeds the translated bytes to a64_exec.c, a small
 * purpose-built A64 decoder+executor standing in for real hardware. M3
 * (docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §6) is the milestone that
 * closes what no decoder can see: an encoder AND decoder that AGREE but
 * are both wrong pass the four-way parity invisibly, and A64
 * architectural rules (SP 16-byte alignment, 4-byte branch-target
 * alignment) that a64_exec is documented laxer about are never checked
 * at all. This harness is the x86 leg's simi_jit_test.c for AArch64: it
 * translates a real .tmo with the exact simi_arm.c, mmaps the result
 * PROT_EXEC, and ACTUALLY EXECUTES it on a real A64 implementation —
 * qemu-aarch64 user-mode (a Linux aarch64 ELF run on the host) — via a
 * direct branch-and-link, not a simulation. If the encoder has a bug,
 * this either crashes (SIGSEGV/SIGILL/SIGBUS, caught and reported
 * below) or returns the wrong value.
 *
 * Why this needed no translator changes: simi_arm.c bakes the runtime
 * call targets (RESOLVE/OBJSIZE/OBJTYPE) as absolute 64-bit constants
 * and calls them with a real `blr xN` — exactly like x86's
 * movabs+call-reg. The a64_exec harness passes sentinel addresses the
 * executor intercepts; THIS harness passes the REAL addresses of the
 * mock C functions below, so the emitted blr lands in genuine AArch64
 * C code under the real AAPCS64 convention (x0-x7 args, x0 result,
 * callee-saved x19-x29 preserved by the callee). The translator's cache
 * discipline — flush before every runtime call, reload after — is what
 * makes that safe, and the fact that it IS safe is exactly what M3
 * proves by executing.
 *
 * The trampoline simi_arm.c emits is itself a normal callable A64
 * subroutine (saves LR/FP, marshals r6/r7 into the outgoing area, `bl`
 * to the entry, restores LR/FP, `br x30`), so the whole blob is called
 * like a plain function. call_blob() below does `blr x0` into it and
 * reads the result out of x9 (t0) — OP_RET's result register, the same
 * convention simi_arm_verify.c uses — then returns it in x0 as a C
 * value. The emitted code touches only x0-x12, x29, x30 and sp (the
 * cache lives in x9/x10/x11, the run-reuse displacement in x12, the
 * frame in x29), so the stub needs no callee-saved juggling, and the
 * blob carves its frames off the caller's stack (16-byte aligned by the
 * ABI; the corpus call depth is shallow — a64_exec already runs every
 * fixture inside a 64 KiB stack).
 *
 * Usage: simi-arm-jit program.tmo entry_name expected_value
 *
 * Exit 0 = executed on real A64 and returned the expected value.
 * Cross-compiled by the Makefile's simi-arm-jit target
 * (aarch64-linux-gnu-gcc -static) and driven by
 * tools/simi/tests/run_arm64_tests.sh under qemu-aarch64.
 */
#define _GNU_SOURCE
#include "simi_arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

#define CODE_CAP     262144u   /* same cap as simi_arm_verify.c */
#define SCRATCH_SIZE  4096u

static sigjmp_buf g_jmp;
static int g_crash_sig = 0;
static uintptr_t g_crash_addr = 0;
static void crash_handler(int sig, siginfo_t* info, void* ctx) {
    (void)ctx;
    g_crash_sig = sig;
    g_crash_addr = info ? (uintptr_t)info->si_addr : 0;
    siglongjmp(g_jmp, 1);
}

/* v0.3 (Phase 6): mock object catalog for RESOLVE/OBJSIZE/OBJTYPE — the
 * SAME names/sizes/types as simi_arm_verify.c (and simi_interp.c), so
 * tests/obj_ops.simi produces the identical expected result (305) under
 * real execution too. These run as REAL AArch64 C functions, reached by
 * the translated code's own `blr` to their baked-in absolute addresses. */
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

/* The call stub. NOTE: aarch64 GCC ignores `__attribute__((naked))`
 * (it is only honored on x86/ARM32 — the first version of this file
 * found that out the hard way: the compiler emitted a prologue around
 * the asm, so the blob returned into the middle of the function and
 * the machine faulted at address 0). So the stub is a plain function
 * whose body is exactly one asm block: `blr x0` branches-and-links
 * into the translated blob (trampoline + entry; x30 becomes the blob's
 * return address), and when the blob's final `br x30` returns, `mov
 * x9, <result>` captures the OP_RET result (still in t0/x9 — the
 * trampoline's post-return path touches nothing but sp/x29/x30). The
 * compiler's own prologue/epilogue then handle the real return to
 * main. The emitted code clobbers x0-x12/x29/x30/sp across the call,
 * which the asm clobber list declares; the compiler's saved LR/FP sit
 * ABOVE the blob's frames (the blob only carves below its entry sp),
 * so the epilogue reads them back intact. */
static uint64_t call_blob(uint64_t code) {
    uint64_t result;
    __asm__ volatile(
        "blr %[code]\n\t"
        "mov %[res], x9\n\t"
        : [res] "=r"(result)
        : [code] "r"(code)
        : "x9", "x10", "x11", "x12", "x29", "x30", "memory");
    return result;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s program.tmo entry_name expected_value\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const char* entry_name = argv[2];
    long long expected = atoll(argv[3]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* obj = malloc((size_t)sz);
    if (!obj || fread(obj, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    /* The trampoline preloads r7 with this real pointer (TX_AR_SCRATCH),
     * the same role the a64_exec harness's guest scratch region plays. */
    uint8_t* scratch = calloc(1, SCRATCH_SIZE);
    if (!scratch) { perror("calloc scratch"); return 1; }

    /* Code buffer: writable to translate into, executable to run. */
    uint8_t* code = mmap(NULL, CODE_CAP, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }

    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_arm_translate(obj, (uint32_t)sz, code, CODE_CAP,
                                 entry_name, (uint64_t)(uintptr_t)scratch,
                                 (uint64_t)(uintptr_t)mock_rt_resolve,
                                 (uint64_t)(uintptr_t)mock_rt_objsize,
                                 (uint64_t)(uintptr_t)mock_rt_objtype,
                                 &out_len, &entry_off);
    if (rc != TX_AR_OK) {
        fprintf(stderr, "FAIL  %-28s translate error: %s\n", path, simi_arm_strerror(rc));
        return 1;
    }

    /* Flip the mapping to executable (W^X: never simultaneously writable
     * and executable). The A64 icache is coherent with the dcache on the
     * cores qemu-aarch64 models, but the clear_cache intrinsic is the
     * correct portable hygiene regardless. */
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)code & ~(uintptr_t)(page - 1);
    if (mprotect((void*)base, CODE_CAP + ((uintptr_t)code - base),
                 PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect"); return 1;
    }
    __builtin___clear_cache((char*)code, (char*)code + out_len);

    long long result = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        result = (long long)call_blob((uint64_t)(uintptr_t)(code + entry_off));
    } else {
        fprintf(stderr, "FAIL  %-28s execution fault on real A64 (sig=%d addr=0x%llx)\n",
                path, g_crash_sig, (unsigned long long)g_crash_addr);
        return 1;
    }

    if (result != expected) {
        printf("FAIL  %-28s expected %lld, got %lld\n", path, expected, result);
        return 1;
    }

    printf("PASS  %-28s = %lld  (%u bytes, real A64 execution)\n", path, result, out_len);
    return 0;
}
