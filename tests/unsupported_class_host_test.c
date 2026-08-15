/*
 * unsupported_class_host_test.c — the §4.5 permanent-unsupported classifier,
 * against the REAL sls_i386_stub_class from qemu/sls/sls-i386-stub-class.c.
 *
 * ─── What this pins ──────────────────────────────────────────────────────
 * Every helper_* this build leaves as a halting stub belongs to a written
 * class in §4.5 of the AMD64 plan (AVX/AVX-512, SSE vector, MMX, x87,
 * 3DNow!, MPX, SHA, the system/legacy tail), and the halting message names
 * that class instead of the old "the next thing to write" -- the M7
 * requirement that no stub read as live but is not. The classifier is pure
 * C and compiles in the kernel AND here, so the two can never drift.
 *
 * The two rows that matter most are the ones that must NOT say "permanent":
 * the signed/byte divide forms (divb_AL .. idivq_EAX) are the M3 integer
 * debt -- the agreed target's compiler DOES emit them, so the classifier
 * returns NULL and the halting message says "owed", not §4.5.
 *
 * The classifier's rules were validated against the full 777-name survivor
 * census (sls/gen_unsupported_list.py): every name maps, only the six
 * div/idiv forms are NULL. This test pins representative names per class
 * plus the NULL family; the census-to-rules agreement is a documented
 * regeneration step, not a CI check.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I ../qemu/sls \
 *       -o /tmp/unsupported_class_host_test \
 *       tests/unsupported_class_host_test.c ../qemu/sls/sls-i386-stub-class.c
 *   /tmp/unsupported_class_host_test
 */
#include <stdio.h>
#include <string.h>
#include "sls-i386-stub-class.h"

static int checks = 0;

static void expect(const char *name, const char *want)
{
    const char *got = sls_i386_stub_class(name);
    if ((got == NULL && want != NULL) ||
        (got != NULL && (want == NULL || strcmp(got, want) != 0))) {
        printf("FAIL %-16s -> %s (want %s)\n",
               name, got ? got : "(null)", want ? want : "(null)");
        checks = -1;
        return;
    }
    printf("ok: %s -> %s\n", name, got ? got : "(null)");
    checks++;
}

int main(void)
{
    /* x87: every f* helper except the fma4/fxsave/fxrstor exclusions. */
    expect("fadd_ST0_FT0", "x87 FPU");
    expect("fyl2x", "x87 FPU");
    expect("fninit", "x87 FPU");
    expect("fwait", "x87 FPU");
    /* The exclusions must NOT land in x87. */
    expect("fma4sd", "AVX-FMA");
    expect("fma4ps_ymm", "AVX-FMA");
    expect("fxsave", "system/legacy (state save)");
    expect("fxrstor", "system/legacy (state save)");
    /* 3DNow!. */
    expect("pfacc", "3DNow!");
    expect("pi2fw", "3DNow!");
    expect("pswapd", "3DNow!");
    /* MMX: the _mmx glue family and the MMX converts. */
    expect("psrlw_mmx", "MMX");
    expect("addpd_mmx", "MMX");
    expect("cvtpi2pd", "MMX");
    expect("emms", "MMX");
    /* SSE vector (xmm): the _xmm family, scalar-xmm, SSE3 scalar. */
    expect("addps_xmm", "SSE vector (xmm)");
    expect("cmpeqps_xmm", "SSE vector (xmm)");
    /* iteration 38: the scalar compare ss/sd forms are the same SSE
     * compare family, not the system/legacy tail. */
    expect("cmpeqss", "SSE vector (xmm)");
    expect("cmpltsd", "SSE vector (xmm)");
    expect("cmpordsd", "SSE vector (xmm)");
    expect("crc32", "SSE vector (xmm)");
    expect("addsubsd", "SSE vector (xmm)");
    expect("haddpd_xmm", "SSE vector (xmm)");
    expect("cvtsd2si", "SSE vector (xmm)");
    expect("rcpss", "SSE vector (xmm)");
    expect("maxsd", "SSE vector (xmm)");
    expect("minsd", "SSE vector (xmm)");
    /* AVX/AVX-512 (ymm). */
    expect("addpd_ymm", "AVX/AVX-512 (ymm)");
    expect("vpermd_ymm", "AVX/AVX-512 (ymm)");
    /* MPX and SHA. */
    expect("bndck", "MPX bounds");
    expect("bndstx64", "MPX bounds");
    expect("sha1msg1", "SHA");
    expect("sha256rnds2", "SHA");
    /* The M3 integer debt: NOT permanent, so NULL -- the halting message
     * must say "owed", never §4.5. */
    expect("idivq_EAX", NULL);
    expect("idivl_EAX", NULL);
    expect("divb_AL", NULL);
    expect("idivw_AX", NULL);
    /* The system/legacy tail. */
    expect("sysenter", "system/legacy");
    expect("inb", "system/legacy");
    expect("monitor", "system/legacy");
    expect("lar", "system/legacy");
    expect("rdpmc", "system/legacy");

    if (checks < 0) {
        printf("FAIL: %d checks passed, at least one failed\n", -checks);
        return 1;
    }
    printf("ok: all %d classifier checks passed\n", checks);
    return 0;
}
