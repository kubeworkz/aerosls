#ifndef SLS_SHA256_H
#define SLS_SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * sha256.h — SHA-256 and HMAC-SHA-256.
 *
 * Written for TLS Phase 0 (docs/AeroSLS-TLS-Design-v0.1.md), where the
 * entropy subsystem needs a conditioning function for the pool and a PRF for
 * the DRBG. It is deliberately the FIRST cryptographic primitive in this
 * kernel and it is deliberately one with published, official test vectors:
 * FIPS 180-4 for the hash, RFC 4231 for the HMAC, and NIST CAVP for the
 * HMAC-DRBG built on top of it in entropy.c.
 *
 * ─── Why a hash and not ChaCha20, reversing the design doc ──────────────────
 * §2.4 of the design doc proposed a ChaCha20 "fast key erasure" DRBG, as
 * Linux uses. That would work. This is better for one specific reason: the
 * ENTIRE generator can be validated against published vectors, not just its
 * primitive. NIST publishes CAVP vectors for HMAC-DRBG covering instantiate,
 * reseed and generate; there is no equivalent for the ChaCha20 construction,
 * so its correctness would rest on our reading of a design rather than on a
 * file of expected outputs.
 *
 * For a subsystem whose failures are invisible to functional testing, "can be
 * checked against someone else's answers" outweighs the speed difference. The
 * design doc is amended rather than quietly diverged from.
 *
 * ─── Constant time ─────────────────────────────────────────────────────────
 * SHA-256 has no data-dependent branches and no table lookups -- it is
 * naturally constant-time in software, which is why it is preferable here to
 * an AES-based construction (AES in software wants S-box tables, and table
 * lookups leak through the cache). No claim is made that the CALLERS of this
 * file are constant-time; that is their problem and entropy.c addresses it.
 *
 * ─── Not a general-purpose hash API ────────────────────────────────────────
 * There is no streaming interface beyond what the DRBG needs, no SHA-224, no
 * SHA-512. Add them when something needs them. A larger surface here is a
 * larger surface to get wrong.
 */

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t h[8];
    uint64_t total_len;              /* message bytes absorbed so far */
    uint8_t  buf[SHA256_BLOCK_LEN];
    uint32_t buf_len;                /* bytes currently in buf */
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, size_t len);
void sha256_final(sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience. */
void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* ─── HMAC-SHA-256 (RFC 2104) ──────────────────────────────────────────────
 * Keys longer than the block length are hashed first, per the RFC. Keys
 * shorter are zero-padded. Both cases are covered by the RFC 4231 vectors in
 * tests/sha256_host_test.c, because both are easy to get subtly wrong and a
 * wrong one still produces a plausible-looking 32 bytes. */
typedef struct {
    sha256_ctx inner;
    uint8_t    opad_key[SHA256_BLOCK_LEN];
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx* c, const void* key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx* c, const void* data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]);

void hmac_sha256(const void* key, size_t key_len,
                 const void* data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

#endif /* SLS_SHA256_H */
