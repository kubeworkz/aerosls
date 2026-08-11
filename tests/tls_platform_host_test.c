/*
 * tls_platform_host_test.c — the mbedTLS bridge fails closed.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -DTLS_PLATFORM_HOST_TEST -DENTROPY_HOST_TEST \
 *       -DENTROPY_TEST_HOOKS -I . -I kernel -o /tmp/tls_platform_host_test \
 *       tests/tls_platform_host_test.c kernel/tls_platform.c kernel/entropy.c \
 *       kernel/sha256.c
 *   /tmp/tls_platform_host_test
 *
 * The bridge is twenty lines of glue, and the only thing that can go wrong in
 * it is the thing that matters most: reporting success when there is no
 * entropy. mbedTLS trusts these two functions completely. If
 * mbedtls_hardware_poll() returns 0 with a buffer nobody wrote, mbedTLS will
 * seed its accumulator from stack contents and every downstream check will
 * pass.
 *
 * So: unseeded must mean refused, and refused must mean the buffer is left
 * exactly as the caller left it.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "entropy.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size);
size_t   sls_tls_pool_used(void);
uint32_t sls_tls_pool_exhausted(void);
void*    sls_tls_calloc_PLACEHOLDER(size_t n, size_t size);

static int checks = 0, fails = 0;
static void ok(int c, const char* w) {
    checks++;
    if (c) printf("ok:   %s\n", w); else { printf("  FAIL  %s\n", w); fails++; }
}

int main(void) {
    unsigned char buf[64];
    size_t olen;

    printf("-- 1: unseeded means refused --\n");
    entropy_test_reset();

    memset(buf, 0xA5, sizeof buf);
    olen = 999;
    ok(mbedtls_hardware_poll(0, buf, sizeof buf, &olen) != 0,
       "mbedtls_hardware_poll() refuses with no entropy");
    ok(olen == 0, "  ...and reports zero bytes, not a partial read");
    {
        int untouched = 1;
        for (size_t i = 0; i < sizeof buf; i++) if (buf[i] != 0xA5) untouched = 0;
        ok(untouched, "  ...and leaves the buffer untouched");
    }

    memset(buf, 0x5A, sizeof buf);
    ok(sls_mbedtls_rng(0, buf, sizeof buf) != 0,
       "sls_mbedtls_rng() refuses with no entropy");
    {
        int untouched = 1;
        for (size_t i = 0; i < sizeof buf; i++) if (buf[i] != 0x5A) untouched = 0;
        ok(untouched, "  ...and leaves the buffer untouched");
    }

    printf("\n-- 2: seeded means it works --\n");
    entropy_test_reset();
    entropy_init();

    olen = 0;
    ok(mbedtls_hardware_poll(0, buf, 32, &olen) == 0, "poll succeeds once seeded");
    ok(olen == 32, "  ...and reports the full length asked for");
    ok(sls_mbedtls_rng(0, buf, 32) == 0, "rng callback succeeds once seeded");

    {
        unsigned char a[32], b[32];
        sls_mbedtls_rng(0, a, 32);
        sls_mbedtls_rng(0, b, 32);
        ok(memcmp(a, b, 32) != 0, "two rng calls return different bytes");
    }

    printf("\n-- 3: degenerate arguments --\n");
    ok(mbedtls_hardware_poll(0, 0, 32, &olen) != 0, "null output refused");
    ok(mbedtls_hardware_poll(0, buf, 0, &olen) != 0, "zero length refused");
    ok(sls_mbedtls_rng(0, 0, 32) != 0,               "null output refused (rng)");
    ok(sls_mbedtls_rng(0, buf, 0) != 0,              "zero size refused (rng)");

    printf("\n-- 4: the placeholder allocator, and its limit --\n");
    {
        void* p = sls_tls_calloc_PLACEHOLDER(4, 16);
        ok(p != 0, "a small allocation succeeds");
        int zeroed = 1;
        for (int i = 0; i < 64; i++) if (((unsigned char*)p)[i]) zeroed = 0;
        ok(zeroed, "  ...and is zeroed, as calloc must be");
        ok(sls_tls_pool_used() >= 64, "  ...and is accounted in the pool");

        ok(sls_tls_calloc_PLACEHOLDER((size_t)-1, 2) == 0,
           "an overflowing size is refused rather than wrapping");

        /* Exhaustion must be reported, not silently returned as a wild
         * pointer. This is the failure a bump allocator with no reuse WILL
         * reach in service -- which is why it is named PLACEHOLDER and why
         * MBEDTLS_MEMORY_BUFFER_ALLOC_C replaces it before anything ships. */
        while (sls_tls_calloc_PLACEHOLDER(1, 4096) != 0) { }
        ok(sls_tls_pool_exhausted() > 0, "pool exhaustion is counted, not hidden");

        /* Note what this does NOT assert. After 4096 is refused there may
         * still be room for 16 -- refusing the large request does not mean
         * the pool is empty, and an earlier version of this test claimed it
         * did and failed. Drain with the small size before asserting. */
        while (sls_tls_calloc_PLACEHOLDER(1, 16) != 0) { }
        ok(sls_tls_calloc_PLACEHOLDER(1, 16) == 0,
           "once genuinely full, every further allocation returns null");
    }

    printf("\n=== %d passed, %d failed ===\n", checks - fails, fails);
    return fails == 0 ? 0 : 1;
}
