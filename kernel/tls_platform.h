#ifndef SLS_TLS_PLATFORM_H
#define SLS_TLS_PLATFORM_H

/* tls_platform.h — the parts of kernel/tls_platform.c other kernel files call.
 *
 * Created when tls_cert.c needed sls_mbedtls_rng(). Until then tls_platform.c
 * declared it immediately above its own definition, which satisfies
 * -Wmissing-prototypes but gives a second caller nothing to include -- and a
 * hand-copied extern that drifts from the definition is a silent ABI bug the
 * compiler cannot see. */

#include <stddef.h>

/* mbedtls_f_rng_t exactly: 0 on success, non-zero on failure.
 *
 * Fail-closed. It returns non-zero when entropy_get() reports the pool never
 * reached its threshold, and does NOT write to `output` in that case -- so
 * mbedTLS gets an error rather than a buffer the caller might mistake for
 * randomness. Every mbedTLS entry point that needs randomness takes this:
 * key generation, and ECDSA's per-signature nonce during certificate
 * signing. */
int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size);

/* The fixed pool's size, and its peak usage since boot. The second is what
 * TLS_SERVER_MAX_SESSIONS must be sized against -- see that constant's
 * comment, which asked for exactly this measurement and could not have it. */
size_t sls_tls_pool_bytes(void);
void   sls_tls_pool_high_water(size_t *max_used, size_t *max_blocks);

/* The pool's declared size, in bytes -- the single source of truth. It
 * used to be defined only in tls_platform.c, which let the host test
 * freeze its own copy (256 KiB) that survived the measured raise to
 * 512 KiB -- the "two places holding one number" bug the init-time
 * printf comment describes. Any other number that must track this one
 * (TLS_SERVER_MAX_SESSIONS against the peak, for instance) reads it
 * here. */
#define SLS_TLS_POOL_BYTES (512u * 1024u)

#endif /* SLS_TLS_PLATFORM_H */
