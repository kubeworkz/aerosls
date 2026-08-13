/*
 * tls_platform_host_test.c — the mbedTLS bridge fails closed.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -DTLS_PLATFORM_HOST_TEST -DENTROPY_HOST_TEST \
 *       -DENTROPY_TEST_HOOKS -DRTC_HOST_TEST -DRTC_TEST_HOOKS \
 *       -I . -I kernel -o /tmp/tls_platform_host_test \
 *       tests/tls_platform_host_test.c kernel/tls_platform.c kernel/entropy.c \
 *       kernel/sha256.c kernel/rtc.c
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
#include "tls_platform.h"
#include "entropy.h"
#include "rtc.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size);
size_t sls_tls_pool_bytes(void);
int    sls_tls_memory_init(void);
void   sls_tls_memory_free(void);
int    sls_tls_memory_ready(void);
long long sls_mbedtls_time(long long *t);
int    sls_tls_time_init(void);
int    sls_tls_time_trusted(void);

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

    printf("\n-- 4: the memory pool --\n");

    /* The bump allocator this section used to test is gone. It was named
     * PLACEHOLDER, never freed anything, and has been replaced by upstream's
     * MBEDTLS_MEMORY_BUFFER_ALLOC_C -- a real free list over the same fixed
     * buffer. Its behaviour is mbedTLS's to test, not ours; what remains ours
     * is that the pool is a bounded, declared budget and that init is
     * idempotent.
     *
     * The host build deliberately does NOT link the vendored library (see
     * kernel/tls_platform.c), so these are the only allocator assertions this
     * file can honestly make. Anything stronger belongs in a test that boots
     * the kernel. */
    ok(sls_tls_pool_bytes() == SLS_TLS_POOL_BYTES,
       "the pool is the declared fixed budget, not an open heap");
    ok(sls_tls_memory_ready() == 0, "not ready before init");
    ok(sls_tls_memory_init() == 0,  "init succeeds");
    ok(sls_tls_memory_ready() == 1, "  ...and reports ready");
    ok(sls_tls_memory_init() == 0,  "init is idempotent");
    sls_tls_memory_free();
    ok(sls_tls_memory_ready() == 0, "free returns it to not-ready");

    printf("\n-- 5: the clock X.509 will consult --\n");

    /* MBEDTLS_PLATFORM_TIME_MACRO binds mbedtls_time to sls_mbedtls_time at
     * compile time, so this IS the function library/x509.c:1072 calls when it
     * checks notBefore/notAfter. Verified structurally too: x509.o carries an
     * undefined reference to sls_mbedtls_time. */
    rtc_test_reset();
    rtc_test_set_build_epoch(1767225600ull);      /* 2026-01-01Z */

    ok(sls_tls_time_trusted() == 0, "no trusted clock before rtc is set");
    ok(sls_tls_time_init() != 0,
       "TLS time init REFUSES without a trusted clock");

    {
        long long out = 0x5A5A5A5A;
        ok(sls_mbedtls_time(&out) == 0,
           "with no trusted clock the time function returns 0");
        ok(out == 0, "  ...and writes 0 through the out-parameter too");
    }

    /* 0 is the epoch, so every certificate our CA issues is "not yet valid"
     * and X.509 rejects it. That is fail-closed by CONSEQUENCE, which is why
     * sls_tls_time_init() refusing is the real gate and this is the backstop.
     * Asserting both keeps the distinction from eroding. */

    ok(rtc_set_unix(1786451445ull) == RTC_OK, "operator supplies a sane time");
    ok(sls_tls_time_trusted() == 1, "  ...clock is now trusted");
    ok(sls_tls_time_init() == 0,    "  ...and TLS time init accepts it");
    {
        long long now = 0;
        sls_mbedtls_time(&now);
        ok(now >= 1786451445LL, "  ...and the time function returns it");
    }

    printf("\n=== %d passed, %d failed ===\n", checks - fails, fails);
    return fails == 0 ? 0 : 1;
}
