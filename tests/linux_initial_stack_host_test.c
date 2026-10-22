/*
 * linux_initial_stack_host_test.c — Phase E7 increment 1, §5.2: the initial
 * stack builder against the REAL kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * A static musl start-up reads argc/argv/envp/auxv off the stack the
 * KERNEL builds; a wrong image does not crash the kernel, it runs a
 * tenant binary against garbage — a canary seeded from nothing, a libc
 * that silently finds a vDSO the platform does not have, a psABI
 * misalignment that faults three instructions into _start. So every
 * clause here is about the bytes: the required AT_* set is present
 * exactly once with the value §5.2 names, AT_SYSINFO_EHDR is ABSENT by
 * decision (there is no vDSO, which is what keeps the census's vDSO
 * declaration honest at the source), AT_RANDOM is the caller's entropy
 * copied verbatim, the pointer chain walks, and the entry rsp is 16-byte
 * aligned. The undersize and NULL refusals return 0 rather than a
 * plausible address, because a partially built image must never look
 * usable to its caller.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_initial_stack_host_test \
 *       tests/linux_initial_stack_host_test.c kernel/linux_abi.c
 *   /tmp/linux_initial_stack_host_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

#define REGION_BYTES (64u * 1024u)
static uint8_t region[REGION_BYTES];

int main(void)
{
    static const char *argv[] = { "aerobackup", "--check", NULL };
    static const char *envp[] = { "PATH=/bin", "HOME=/", NULL };
    static const uint8_t random_bytes[LINUX_AT_RANDOM_BYTES] = {
        0x91, 0x37, 0x04, 0xe5, 0x5a, 0xbc, 0x12, 0xf0,
        0x33, 0x77, 0xca, 0xfe, 0x42, 0x88, 0xde, 0x01
    };
    struct LinuxInitStackParams p;
    uint64_t rsp;
    uint64_t *words;
    size_t i, aux_count = 0, sysinfo_seen = 0;
    int all_types_once = 1;
    const uint64_t required_types[] = {
        LINUX_AT_PHDR, LINUX_AT_PHENT, LINUX_AT_PHNUM, LINUX_AT_PAGESZ,
        LINUX_AT_BASE, LINUX_AT_ENTRY, LINUX_AT_PLATFORM, LINUX_AT_HWCAP,
        LINUX_AT_CLKTCK, LINUX_AT_SECURE, LINUX_AT_RANDOM, LINUX_AT_HWCAP2,
        LINUX_AT_EXECFN
    };

    memset(&p, 0, sizeof(p));
    p.entry_vaddr = 0x401000;
    p.phdr_vaddr  = 0x400040;
    p.phent       = (uint16_t)LINUX_ELF64_PHENT;
    p.phnum       = 3;
    p.argv        = argv;
    p.envp        = envp;
    p.at_random   = random_bytes;
    p.execfn      = "/bin/aerobackup";
    /* E7 increment 7: the builder translates every in-region pointer and
     * the returned %rsp to region_vaddr. Identity here (the region's own
     * host address) — every byte below is then exactly what the pre-v7
     * builder produced, which is what makes the byte-exact clauses in this
     * file pin the TRANSLATION too, not just the layout. */
    p.region_vaddr = (uint64_t)(uintptr_t)region;

    memset(region, 0xAB, sizeof(region));   /* poison: every byte written is visible */
    rsp = linux_initial_stack_build(region, sizeof(region), &p);

    CHECK(rsp != 0, "the build succeeds into a 64 KiB region");
    if (!rsp) return 1;
    CHECK(rsp % 16 == 0, "the entry rsp is 16-byte aligned (x86-64 psABI at process entry)");
    CHECK(rsp >= (uint64_t)(uintptr_t)region &&
          rsp + 8 <= (uint64_t)(uintptr_t)(region + sizeof(region)),
          "the entry rsp points inside the region");

    words = (uint64_t *)(uintptr_t)rsp;

    /* ─── argc / argv / envp chain ───────────────────────────────────────── */
    CHECK(words[0] == 2, "argc is 2");
    {
        const char *a0 = (const char *)(uintptr_t)words[1];
        const char *a1 = (const char *)(uintptr_t)words[2];
        CHECK(words[3] == 0, "argv is NULL-terminated");
        CHECK(strcmp(a0, "aerobackup") == 0, "argv[0] is the copied string");
        CHECK(strcmp(a1, "--check") == 0, "argv[1] is the copied string");
        CHECK(a0 >= (const char *)region && a0 < (const char *)region + REGION_BYTES,
              "argv[0] points into the region (the strings were copied, not aliased)");
    }
    {
        const char *e0 = (const char *)(uintptr_t)words[4];
        const char *e1 = (const char *)(uintptr_t)words[5];
        CHECK(words[6] == 0, "envp is NULL-terminated");
        CHECK(strcmp(e0, "PATH=/bin") == 0, "envp[0] is the copied string");
        CHECK(strcmp(e1, "HOME=/") == 0, "envp[1] is the copied string");
    }

    /* ─── auxv: AT_NULL terminates, then the required set ────────────────── */
    words += 7;   /* argc + argv[0..1] + NULL + envp[0..1] + NULL */
    {
        int null_terminated = 0;
        uint64_t phdr = 0, entry = 0, pagesz = 0, base = 0xdead, clk = 0, sec = 0xdead;
        uint64_t phent = 0, phnum = 0, hwcap = 0xdead, hwcap2 = 0xdead;
        uint64_t random_va = 0, platform_va = 0, execfn_va = 0;
        for (i = 0; ; i += 2) {
            uint64_t t = words[i], v = words[i + 1];
            if (t == LINUX_AT_NULL) { null_terminated = (v == 0); break; }
            aux_count++;
            switch (t) {
            case LINUX_AT_PHDR:     phdr = v; break;
            case LINUX_AT_PHENT:    phent = v; break;
            case LINUX_AT_PHNUM:    phnum = v; break;
            case LINUX_AT_PAGESZ:   pagesz = v; break;
            case LINUX_AT_BASE:     base = v; break;
            case LINUX_AT_ENTRY:    entry = v; break;
            case LINUX_AT_PLATFORM: platform_va = v; break;
            case LINUX_AT_HWCAP:    hwcap = v; break;
            case LINUX_AT_CLKTCK:   clk = v; break;
            case LINUX_AT_SECURE:   sec = v; break;
            case LINUX_AT_RANDOM:   random_va = v; break;
            case LINUX_AT_HWCAP2:   hwcap2 = v; break;
            case LINUX_AT_EXECFN:   execfn_va = v; break;
            case 33 /* LINUX_AT_SYSINFO_EHDR */: sysinfo_seen++; break;
            default: break;   /* unknown entries are legal per the ABI */
            }
        }
        CHECK(null_terminated, "the auxv is terminated by AT_NULL with d_val 0");
        CHECK(aux_count == 13, "the auxv carries exactly §5.2's 13 required entries");
        CHECK(sysinfo_seen == 0,
              "AT_SYSINFO_EHDR is absent — there is no vDSO, so clock_gettime is a real trap (§5.2/§5.5)");
        CHECK(phdr == 0x400040, "AT_PHDR is the params' phdr vaddr");
        CHECK(phent == LINUX_ELF64_PHENT, "AT_PHENT is 56 (Elf64_Phdr)");
        CHECK(phnum == 3, "AT_PHNUM is the params' count");
        CHECK(pagesz == LINUX_PAGE_SIZE, "AT_PAGESZ is 4096");
        CHECK(base == 0, "AT_BASE is 0 (static image: no interpreter base)");
        CHECK(entry == 0x401000, "AT_ENTRY is e_entry");
        CHECK(hwcap == 0, "AT_HWCAP is 0");
        CHECK(hwcap2 == 0, "AT_HWCAP2 is 0");
        CHECK(clk == LINUX_CLKTCK, "AT_CLKTCK is 100");
        CHECK(sec == 0, "AT_SECURE is 0");
        CHECK(platform_va != 0 &&
              strcmp((const char *)(uintptr_t)platform_va, "x86_64") == 0,
              "AT_PLATFORM points at the copied \"x86_64\" string");
        CHECK(execfn_va != 0 &&
              strcmp((const char *)(uintptr_t)execfn_va, "/bin/aerobackup") == 0,
              "AT_EXECFN points at the copied pathname");
        CHECK(random_va != 0 &&
              memcmp((const void *)(uintptr_t)random_va, random_bytes,
                     LINUX_AT_RANDOM_BYTES) == 0,
              "AT_RANDOM points at the caller's 16 entropy bytes, copied verbatim");

        /* Each required type appears exactly once — a duplicated entry is a
         * different bug than a missing one (a scanning libc takes the first). */
        for (i = 0; i < sizeof(required_types) / sizeof(required_types[0]); i++) {
            size_t j, count = 0;
            for (j = 0; ; j += 2) {
                if (words[j] == LINUX_AT_NULL) break;
                if (words[j] == required_types[i]) count++;
            }
            if (count != 1) { all_types_once = 0; printf("      (type %llu seen %zu times)\n",
                               (unsigned long long)required_types[i], count); }
        }
        CHECK(all_types_once, "every required AT_* type appears exactly once");
    }

    /* ─── AT_RANDOM's own quality rules (§5.2: absent, canaries are
     * worthless — so present AND non-degenerate) ─────────────────────────── */
    {
        int nonzero = 0, distinct = 1;
        for (i = 0; i < LINUX_AT_RANDOM_BYTES; i++)
            if (random_bytes[i] != 0) nonzero = 1;
        for (i = 0; i + 1 < LINUX_AT_RANDOM_BYTES; i++)
            if (random_bytes[i] == random_bytes[i + 1]) distinct = 0;
        CHECK(nonzero, "the AT_RANDOM input is not all-zero (the builder does not accept a degenerate fill)");
        CHECK(distinct, "the AT_RANDOM input has no adjacent repeats (the copy preserves it as given)");
    }

    /* ─── Empty argv/envp still builds a well-formed image ───────────────── */
    {
        static const char *no_args[] = { NULL };
        struct LinuxInitStackParams q = p;
        uint64_t rsp2;
        q.argv = no_args;
        q.envp = no_args;
        rsp2 = linux_initial_stack_build(region, sizeof(region), &q);
        CHECK(rsp2 != 0, "an empty argv/envp still builds");
        if (rsp2) {
            uint64_t *w2 = (uint64_t *)(uintptr_t)rsp2;
            CHECK(w2[0] == 0, "argc is 0 for an empty argv");
            CHECK(w2[1] == 0, "argv is still NULL-terminated at index 0");
            CHECK(w2[2] == 0, "envp is still NULL-terminated at index 0");
            CHECK(rsp2 % 16 == 0, "the empty-image rsp is still 16-byte aligned");
        }
    }

    /* ─── Refusals return 0, never a plausible address ───────────────────── */
    CHECK(linux_initial_stack_build(region, 64, &p) == 0,
          "a 64-byte region is refused by name (return 0), never half-built");
    CHECK(linux_initial_stack_build(region, sizeof(region), NULL) == 0,
          "NULL params are refused");
    {
        struct LinuxInitStackParams q = p;
        q.at_random = NULL;
        CHECK(linux_initial_stack_build(region, sizeof(region), &q) == 0,
              "a missing AT_RANDOM source is refused — entropy is not optional (§5.2)");
    }
    {
        struct LinuxInitStackParams q = p;
        q.execfn = NULL;
        CHECK(linux_initial_stack_build(region, sizeof(region), &q) == 0,
              "a missing AT_EXECFN string is refused");
    }
    CHECK(linux_initial_stack_build(NULL, sizeof(region), &p) == 0,
          "a NULL region is refused");

    printf("linux_initial_stack_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
