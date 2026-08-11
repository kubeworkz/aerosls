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
 * which needs neither, and the amendment to the design doc says plainly that
 * moving to mbedTLS reduces rather than eliminates the problem.
 *
 * What reduces it: a FIXED POOL, not the kernel's general heap. TLS
 * allocation cannot then fragment or exhaust the arena the database lives in,
 * and the worst case is that a handshake fails rather than that the node
 * runs out of memory somewhere unrelated. The pool size is a budget to be
 * chosen against §3.2's per-connection figures, not a number to tune upward
 * when something fails.
 *
 * This is a bump allocator with no reuse, which is WRONG for a long-lived
 * server and is here as a placeholder with a loud name. mbedTLS's own
 * MBEDTLS_MEMORY_BUFFER_ALLOC_C provides a real fixed-pool allocator with
 * free-list handling; wiring that is the correct answer and is the next
 * commit. Shipping this as-is would exhaust the pool after N handshakes.  */

#define SLS_TLS_POOL_BYTES (256u * 1024u)
static uint8_t  tls_pool[SLS_TLS_POOL_BYTES] __attribute__((aligned(16)));
static size_t   tls_pool_used;
static uint32_t tls_pool_exhausted;

void* sls_tls_calloc_PLACEHOLDER(size_t n, size_t size);
void  sls_tls_free_PLACEHOLDER(void* p);

void* sls_tls_calloc_PLACEHOLDER(size_t n, size_t size)
{
    if (n && size > (size_t)-1 / n) return 0;       /* overflow */
    size_t want = (n * size + 15u) & ~(size_t)15u;
    if (want > SLS_TLS_POOL_BYTES - tls_pool_used) {
        tls_pool_exhausted++;
        return 0;
    }
    uint8_t* p = &tls_pool[tls_pool_used];
    tls_pool_used += want;
    for (size_t i = 0; i < want; i++) p[i] = 0;      /* calloc must zero */
    return p;
}

void sls_tls_free_PLACEHOLDER(void* p) { (void)p; }

size_t sls_tls_pool_used(void);
size_t sls_tls_pool_used(void) { return tls_pool_used; }
uint32_t sls_tls_pool_exhausted(void);
uint32_t sls_tls_pool_exhausted(void) { return tls_pool_exhausted; }
