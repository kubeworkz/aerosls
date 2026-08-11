/*
 * tls_platform.c — the glue mbedTLS needs from this kernel.
 *
 * TLS Phase 2. See docs/AeroSLS-TLS-Design-v0.1.md (and its amendment: the
 * library is mbedTLS 3.6 LTS, not BearSSL).
 *
 * mbedTLS is transport- and platform-agnostic by design: it asks the host for
 * randomness, memory and time, and does everything else itself. This file is
 * those three answers and nothing more. Every signature here was read out of
 * vendor/mbedtls's own headers, not recalled:
 *
 *   mbedtls_hardware_poll   library/entropy_poll.h:46
 *   mbedtls_f_rng_t         include/mbedtls/platform_util.h:209
 *   set_calloc_free         include/mbedtls/platform.h:160
 *
 * That is not pedantry. Twice this session a claim written from memory turned
 * out wrong -- a library's protocol support, and three date constants -- and
 * both were one lookup away. The headers are in the tree now.
 */

#include <stdint.h>
#include <stddef.h>
#include "entropy.h"
#include "rtc.h"

#ifndef TLS_PLATFORM_HOST_TEST
/* The kernel build compiles against the vendored tree; the host test does not,
 * so that the fail-closed RNG contract can be exercised without pulling 8 MB
 * of library into a unit test. */
#include "mbedtls/memory_buffer_alloc.h"

#endif

#ifndef TLS_PLATFORM_HOST_TEST
#include "kernel_io.h"
#else
#include <stdio.h>
#define kernel_serial_print(s) fputs((s), stderr)
#endif

/* ─── 1. Randomness ────────────────────────────────────────────────────────
 * Two entry points, because mbedTLS asks for randomness in two different
 * ways and both must route to the same place.
 *
 * mbedtls_hardware_poll() feeds its entropy accumulator (MBEDTLS_ENTROPY_
 * HARDWARE_ALT). sls_mbedtls_rng() is the mbedtls_f_rng_t handed to
 * mbedtls_ssl_conf_rng() for per-handshake randomness.
 *
 * ─── The important part: both fail closed ─────────────────────────────────
 * entropy_get() returns non-zero when the pool never reached its threshold,
 * and leaves the caller's buffer untouched. The tempting bug here is to
 * report success with olen = 0, or to return whatever the stack held --
 * mbedTLS would then either stall in its accumulator or, worse, proceed with
 * a buffer nobody wrote. Returning an error propagates up through
 * mbedtls_ssl_handshake() and the connection is refused, which is the correct
 * outcome for a node that cannot generate keys safely.
 *
 * A node with no entropy must serve nothing. It must not serve TLS badly. */

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen);

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;
    if (olen) *olen = 0;
    if (!output || len == 0) return -1;

    if (entropy_get(output, len) != ENTROPY_OK) {
        /* Deliberately NOT partial credit. Reporting a short read would let
         * mbedTLS keep polling and eventually satisfy its threshold from a
         * source that has already said it has nothing. */
        kernel_serial_print("[TLS] entropy unavailable -- refusing to supply "
                            "randomness to mbedTLS.\n");
        return -1;
    }
    if (olen) *olen = len;
    return 0;
}

/* Shape is mbedtls_f_rng_t exactly: int (*)(void *p_rng, unsigned char *out,
 * size_t out_size). p_rng is unused -- our generator is a kernel singleton,
 * not a per-context object -- but the parameter must stay for the type to
 * match, and a wrong-arity function pointer here is a silent miscompile
 * rather than a diagnostic. */
int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size);

int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size)
{
    (void)p_rng;
    if (!output || output_size == 0) return -1;
    return (entropy_get(output, output_size) == ENTROPY_OK) ? 0 : -1;
}

/* ─── 2. Memory ────────────────────────────────────────────────────────────
 * mbedTLS wants calloc/free. This was the strongest argument for BearSSL,
 * which needs neither, and the design doc amendment says plainly that moving
 * to mbedTLS reduces rather than eliminates the problem.
 *
 * What reduces it is a FIXED POOL rather than the kernel's general heap: TLS
 * allocation cannot fragment or exhaust the arena the database lives in, so
 * the worst case is a refused handshake instead of a node dying somewhere
 * unrelated.
 *
 * The allocator itself is now upstream's MBEDTLS_MEMORY_BUFFER_ALLOC_C, not
 * ours. The previous version here was a bump allocator named PLACEHOLDER
 * because it never freed anything -- correct on overflow, correct on
 * exhaustion, and guaranteed to run out after N handshakes. Upstream's keeps a
 * real free list over the same buffer, so a connection's memory comes back
 * when it closes. Writing our own allocator to sit under a TLS stack was never
 * the right call when the library ships one built for exactly this.
 *
 * Size is a budget, not a tuning knob. TLS 1.3 records are up to 16 KB and
 * neither MAX_FRAGMENT_LENGTH nor VARIABLE_BUFFER_LENGTH is supported under
 * 1.3, so per-connection cost is fixed at full record size (design doc §3.2
 * and the amendment). Raising this number to make a failure go away is
 * choosing to run out later, in production, instead of now, in a test. */
#define SLS_TLS_POOL_BYTES (256u * 1024u)
#ifndef TLS_PLATFORM_HOST_TEST
static uint8_t tls_pool[SLS_TLS_POOL_BYTES] __attribute__((aligned(16)));
#endif
static uint8_t tls_pool_ready;

int  sls_tls_memory_init(void);
void sls_tls_memory_free(void);

int sls_tls_memory_init(void)
{
    if (tls_pool_ready) return 0;
#ifndef TLS_PLATFORM_HOST_TEST
    mbedtls_memory_buffer_alloc_init(tls_pool, sizeof tls_pool);
#endif
    tls_pool_ready = 1;
    kernel_serial_print("[TLS] memory pool initialised (256 KiB, fixed).\n");
    return 0;
}

void sls_tls_memory_free(void)
{
    if (!tls_pool_ready) return;
#ifndef TLS_PLATFORM_HOST_TEST
    mbedtls_memory_buffer_alloc_free();
#endif
    tls_pool_ready = 0;
}

size_t sls_tls_pool_bytes(void);
size_t sls_tls_pool_bytes(void) { return SLS_TLS_POOL_BYTES; }
int    sls_tls_memory_ready(void);
int    sls_tls_memory_ready(void) { return tls_pool_ready != 0; }

/* ─── 3. Time ──────────────────────────────────────────────────────────────
 * X.509 asks the platform for the current time to check notBefore/notAfter
 * (vendor/mbedtls/library/x509.c:1072 calls mbedtls_time(NULL)).
 * MBEDTLS_PLATFORM_TIME_MACRO binds that name to this function at COMPILE
 * time -- there is no pointer to install and no window in which one is unset.
 * Proof it is bound rather than merely declared: x509.o carries an undefined
 * reference to sls_mbedtls_time.
 *
 * ─── What happens with no trusted clock, and why 0 is not a fallback ──────
 * kernel/rtc.c refuses rather than guessing -- no RTC, a dead battery reading
 * 2000-01-01, a time before this kernel was built. When it refuses, this
 * returns 0, and 0 is the epoch: every certificate our private CA issues has
 * a notBefore far later than 1970, so X.509 rejects it as NOT YET VALID.
 *
 * That is fail-closed, but it is fail-closed by CONSEQUENCE, and relying on a
 * consequence is how a security property quietly stops holding -- a
 * certificate with an early enough notBefore would sail through. So it is the
 * second line, not the first. The first is sls_tls_time_init(), which REFUSES
 * on a node with no trusted clock and must be called before a listener is ever
 * opened. That is the check to keep honest. */

long long sls_mbedtls_time(long long *t);

long long sls_mbedtls_time(long long *t)
{
    uint64_t now = 0;
    long long v = 0;

    if (rtc_get_unix(&now) == RTC_OK) v = (long long)now;
    /* else v stays 0 -- see the note above; this is not a guess at the time,
     * it is a value chosen because it cannot pass a validity check. */

    if (t) *t = v;
    return v;
}

int sls_tls_time_init(void);
int sls_tls_time_trusted(void);

/* No pointer to install: MBEDTLS_PLATFORM_TIME_MACRO binds mbedtls_time to
 * sls_mbedtls_time at compile time. This exists purely to make the refusal
 * explicit and early, at a place a caller must pass through. */
int sls_tls_time_init(void)
{
    if (!rtc_is_trusted()) {
        kernel_serial_print("[TLS] no trusted wall clock -- certificate "
                            "validity cannot be checked. TLS must not start.\n");
        return -1;
    }
    return 0;
}

int sls_tls_time_trusted(void) { return rtc_is_trusted(); }
