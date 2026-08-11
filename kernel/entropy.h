#ifndef SLS_ENTROPY_H
#define SLS_ENTROPY_H

#include <stdint.h>
#include <stddef.h>

/*
 * entropy.h — the kernel's cryptographic random number generator.
 *
 * TLS Phase 0. See docs/AeroSLS-TLS-Design-v0.1.md §2.
 *
 * ─── Read this before calling anything here ────────────────────────────────
 * This is the one subsystem in the kernel whose failure is INVISIBLE. A broken
 * hash produces wrong digests and every test notices. A broken RNG produces
 * 32 bytes that look exactly like 32 good bytes, handshakes correctly,
 * interoperates with every browser, and is worthless. Debian shipped that for
 * two years and everything worked the whole time.
 *
 * Two consequences for callers:
 *
 *   1. entropy_get() CAN FAIL and its return value is not advisory. If it
 *      returns non-zero, the buffer is untouched and there is no random data.
 *      Do not fall back to anything. Do not use a counter, the TSC, or the
 *      contents of an uninitialised buffer. Fail the operation upward. A node
 *      that cannot generate keys safely must serve nothing rather than serve
 *      something insecure -- that is the whole design and it is why this
 *      returns int rather than void.
 *
 *   2. kernel/vec_index.c's vi_testonly_rand32() is NOT this and must never
 *      be reached for by anything cryptographic. It is a fixed-seed xorshift,
 *      deliberately deterministic so HNSW graph shape is reproducible for its
 *      host tests. tests/entropy_source_check.sh enforces the separation.
 *
 * ─── Construction ─────────────────────────────────────────────────────────
 *   sources  ->  pool (SHA-256 conditioned)  ->  HMAC-DRBG (SP 800-90A)
 *
 * HMAC-DRBG rather than a ChaCha20 construction for one reason: NIST publishes
 * CAVP vectors for the whole generator, so instantiate/reseed/generate can be
 * checked against someone else's answers rather than against our reading of a
 * design. For a subsystem that cannot be functionally tested, that outweighs
 * the speed difference. (This amends design doc §2.4, which proposed
 * ChaCha20.)
 *
 * Sources are MIXED, never selected. Every available source contributes to the
 * pool; a source that is broken, backdoored, or merely absent degrades the
 * pool but does not own it. No single source is load-bearing -- specifically
 * including RDRAND, which is an opaque hardware DRBG with a history of errata.
 */

/* Bytes of conditioned entropy required before the DRBG will instantiate.
 * 32 bytes = 256 bits, the security strength of the DRBG; there is no point
 * seeding above the strength of what consumes it, and no excuse below. */
#define ENTROPY_SEED_BYTES 32

/* A separate, higher bar for the cluster CA root key (design doc §4.2): it is
 * the highest-value key in the system and it is generated at the worst moment,
 * on a machine seconds after boot. */
#define ENTROPY_CA_SEED_BYTES 64

/* Return codes. Negative is failure; the caller's buffer is never written on
 * failure, so a caller that ignores the return gets whatever it passed in --
 * loud garbage rather than a plausible-looking key. */
#define ENTROPY_OK             0
#define ENTROPY_E_NOT_SEEDED (-1)   /* pool never reached the threshold */
#define ENTROPY_E_HEALTH     (-2)   /* a continuous health test failed */
#define ENTROPY_E_BADARG     (-3)

/* Bring up the pool from every source this machine offers, then instantiate
 * the DRBG. Safe to call more than once; the second call is a reseed.
 *
 * Returns ENTROPY_OK, or ENTROPY_E_NOT_SEEDED when the machine could not
 * supply enough entropy -- which is a real outcome on hardware with no RNG
 * instruction and a poor jitter source, and must be handled, not asserted
 * away. */
int entropy_init(void);

/* Fill out[0..len) with cryptographically strong bytes.
 * Returns ENTROPY_OK, or negative with `out` UNTOUCHED. */
int entropy_get(void* out, size_t len);

/* Non-zero once the DRBG is instantiated and no health test has failed since.
 * Intended for "should TLS start at all" decisions and for status reporting. */
int entropy_is_ready(void);

/* Mix additional data into the pool. Called by interrupt handlers, the network
 * stack, and anything else with unpredictable timing. `est_bits` is the
 * caller's honest estimate of the min-entropy contributed, which may be 0 --
 * data with no entropy is harmless to mix and is never counted toward the
 * seeding threshold. */
void entropy_add(const void* data, size_t len, uint32_t est_bits);

/* Reseed from the sources. Must be called after restoring a checkpoint:
 * restoring DRBG state would make two nodes restored from one checkpoint
 * produce identical output (design doc §6.3). */
int entropy_reseed(void);

/* ─── Observability ────────────────────────────────────────────────────────
 * Which sources this machine actually has, and whether each is contributing.
 * Exposed because "TLS is up" tells you nothing about whether the entropy
 * behind it is real, and an operator deserves to see the difference. */
typedef struct {
    uint8_t  ready;               /* DRBG instantiated and healthy */
    uint8_t  have_rdseed;         /* x86: CPUID.07H:EBX.bit18 */
    uint8_t  have_rdrand;         /* x86: CPUID.01H:ECX.bit30 */
    uint8_t  have_jitter;         /* counter-based; always present in principle */
    uint32_t pool_bits;           /* estimated min-entropy accumulated */
    uint32_t reseed_count;
    uint32_t health_failures;     /* cumulative, across all sources */
    uint32_t rdseed_retries;      /* RDSEED legitimately fails under load */
} entropy_status_t;

void entropy_get_status(entropy_status_t* out);

/* ─── Boot fingerprint: the cross-boot diversity gate ──────────────────────
 * A 32-byte digest computed once, immediately after the DRBG is instantiated.
 * Two nodes booted from an IDENTICAL image must produce different values; if
 * seeding is broken they will match, and that is the single check that would
 * have caught the Debian OpenSSL defect. Nothing else this subsystem does can
 * detect it -- see entropy_host_test.c's header on why in-process tests
 * cannot.
 *
 * Why a digest and not a sample. Publishing raw DRBG output over HTTP so a
 * test can compare it would hand an attacker part of the generator's stream,
 * which is a considerably worse bug than the one being tested for. Instead 32
 * bytes are generated, hashed with a domain-separating label, and DISCARDED --
 * never returned, never used as key material, never reachable again. The
 * digest is one-way, the bytes behind it are used for nothing else, and it is
 * therefore safe to serve publicly while still being a faithful witness to
 * whether the two nodes seeded differently.
 *
 * Returns ENTROPY_OK, or ENTROPY_E_NOT_SEEDED with `out` untouched. */
int entropy_boot_fingerprint(uint8_t out[32]);

/* ─── Test hooks ───────────────────────────────────────────────────────────
 * Compiled only under ENTROPY_TEST_HOOKS, which the kernel build never
 * defines. Source-failure injection is not a nicety here: "the node still
 * boots, drops the dead source, logs it, and either proceeds on what remains
 * or refuses TLS" is a behaviour with no other way to reach it, and the
 * alternative to testing it is finding out on the first machine whose RDSEED
 * is wedged. */
#ifdef ENTROPY_TEST_HOOKS
void entropy_test_force_source_failure(int rdseed_fails, int rdrand_fails,
                                       int jitter_fails);
/* A source that returns a CONSTANT while reporting success -- a stuck noise
 * source, the most common real hardware RNG failure and the one that failing
 * outright does not simulate. It is invisible to every caller above the health
 * tests, because a stuck source conditioned through SHA-256 is beautifully
 * uniform garbage. */
void entropy_test_force_stuck_source(int rdseed_stuck, int rdrand_stuck);
void entropy_test_reset(void);
int  entropy_test_drbg_instantiate(const uint8_t* seed, size_t seed_len,
                                   const uint8_t* nonce, size_t nonce_len,
                                   const uint8_t* pers, size_t pers_len);
int  entropy_test_drbg_reseed(const uint8_t* seed, size_t seed_len,
                              const uint8_t* addl, size_t addl_len);
int  entropy_test_drbg_generate(uint8_t* out, size_t len,
                                const uint8_t* addl, size_t addl_len);
#endif

#endif /* SLS_ENTROPY_H */
