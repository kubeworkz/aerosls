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

#endif /* SLS_TLS_PLATFORM_H */
