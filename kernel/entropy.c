/*
 * entropy.c — pool, health tests, and an SP 800-90A HMAC-DRBG.
 *
 * See entropy.h for the contract and docs/AeroSLS-TLS-Design-v0.1.md §2 for
 * why this precedes every other line of TLS work. Implementation notes only
 * below.
 */

#include "entropy.h"
#include "sha256.h"
#include "dashboard.h"      /* read_tsc() -- rdtsc on x86, CNTVCT_EL0 on arm64 */

#ifndef ENTROPY_HOST_TEST
#include "kernel_io.h"      /* kernel_serial_print */
#else
#include <stdio.h>
#define kernel_serial_print(s) fputs((s), stderr)
#endif

/* ─── DRBG state (SP 800-90A §10.1.2) ──────────────────────────────────────
 * K and V are the whole secret. reseed_counter exists so that a DRBG which
 * has generated a great deal of output without fresh entropy is forced back
 * to the sources rather than trusted indefinitely. */
static uint8_t  drbg_k[SHA256_DIGEST_LEN];
static uint8_t  drbg_v[SHA256_DIGEST_LEN];
static uint32_t drbg_reseed_counter;
static uint8_t  drbg_instantiated;

/* Force a reseed after this many generate() calls. SP 800-90A permits 2^48
 * for HMAC-DRBG; this is vastly lower because reseeding is cheap here and the
 * standard's limit is a cryptographic bound, not an operational target. */
#define DRBG_RESEED_INTERVAL 1024

/* ─── Pool ─────────────────────────────────────────────────────────────────
 * A SHA-256 context absorbing everything any source offers. Conditioning by
 * hashing is the textbook construction and, unlike a hand-rolled mixer, it is
 * one whose properties someone else has argued about. */
static sha256_ctx pool;
static uint32_t   pool_bits;          /* estimated min-entropy, saturating */
static uint32_t   stat_health_failures;
static uint32_t   stat_rdseed_retries;
static uint8_t    have_rdseed, have_rdrand, have_jitter, cpu_probed;

#ifdef ENTROPY_TEST_HOOKS
static int force_rdseed_fail, force_rdrand_fail, force_jitter_fail;
static int force_rdseed_stuck, force_rdrand_stuck;
#endif

static void pool_absorb(const void* data, size_t len) {
    sha256_update(&pool, data, len);
}

/* ─── SP 800-90B §4.4 continuous health tests ──────────────────────────────
 * Both operate per-source on the raw samples, before conditioning, because
 * conditioning is designed to make a broken source look fine -- SHA-256 of a
 * stuck source is beautifully uniform garbage. Testing after the hash would
 * be testing SHA-256.
 *
 * Repetition Count: a source emitting the same value C times in a row has
 * failed, where C is derived from the claimed min-entropy H and a false-
 * positive target of 2^-20: C = 1 + ceil(20 / H). With a conservative H = 1
 * bit per sample this is 21.
 *
 * Adaptive Proportion: over a window of W samples, if the first sample recurs
 * more than a cutoff, the source has failed. Catches a source that is biased
 * rather than stuck, which the repetition test alone misses entirely. */
#define RCT_CUTOFF      21
#define APT_WINDOW      512
#define APT_CUTOFF      410     /* H=1, W=512, alpha=2^-20 (SP 800-90B Table 2) */

typedef struct {
    uint8_t  rct_last;
    uint8_t  rct_have_last;
    uint32_t rct_run;
    uint8_t  apt_first;
    uint32_t apt_seen;      /* samples in the current window */
    uint32_t apt_count;     /* times apt_first has recurred */
    uint8_t  failed;
} health_state;

static health_state hs_rdseed, hs_rdrand, hs_jitter;

/* Returns 0 if the sample is acceptable, non-zero if this source has just
 * failed. A failed source stays failed until entropy_test_reset(); a source
 * that intermittently misbehaves is not one to keep sampling. */
static int health_sample(health_state* h, uint8_t s) {
    if (h->failed) return 1;

    if (h->rct_have_last && s == h->rct_last) {
        h->rct_run++;
        if (h->rct_run >= RCT_CUTOFF) { h->failed = 1; return 1; }
    } else {
        h->rct_last = s;
        h->rct_have_last = 1;
        h->rct_run = 1;
    }

    if (h->apt_seen == 0) {
        h->apt_first = s;
        h->apt_count = 1;
        h->apt_seen  = 1;
    } else {
        h->apt_seen++;
        if (s == h->apt_first) {
            h->apt_count++;
            if (h->apt_count >= APT_CUTOFF) { h->failed = 1; return 1; }
        }
        if (h->apt_seen >= APT_WINDOW) { h->apt_seen = 0; h->apt_count = 0; }
    }
    return 0;
}

/* Runs the health tests over a buffer, byte by byte. Returns 0 if the source
 * survived; on failure the caller must NOT absorb the buffer -- data from a
 * source that has just been shown to be broken is not entropy. */
static int health_check_buf(health_state* h, const uint8_t* p, size_t n,
                            const char* source_name) {
    for (size_t i = 0; i < n; i++) {
        if (health_sample(h, p[i])) {
            stat_health_failures++;
            kernel_serial_print("[ENTROPY] health test FAILED for source ");
            kernel_serial_print(source_name);
            kernel_serial_print(" -- dropping it from the pool.\n");
            return 1;
        }
    }
    return 0;
}

/* ─── x86-64 hardware sources ──────────────────────────────────────────────
 * The kernel already runs CPUID for its vendor string (kernel/kernel.c); this
 * is the same instruction asking for feature bits.
 *
 * RDSEED is the true entropy source and is allowed to fail: it drains, and
 * under contention it returns CF=0 with no data. That is documented, expected
 * behaviour and NOT an error -- but a bounded retry matters, because the
 * alternative is an unbounded spin in a kernel with no preemption. */
#if defined(__x86_64__)

static void cpu_probe(void) {
    if (cpu_probed) return;
    cpu_probed = 1;

    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    have_rdrand = (c >> 30) & 1u;              /* CPUID.01H:ECX.bit30 */

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
    if (a >= 7u) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
        have_rdseed = (b >> 18) & 1u;          /* CPUID.07H:EBX.bit18 */
    }
    have_jitter = 1;
}

static int rd_seed64(uint64_t* out) {
#ifdef ENTROPY_TEST_HOOKS
    if (force_rdseed_fail)  return 0;
    if (force_rdseed_stuck) { *out = 0x5555555555555555ull; return 1; }
#endif
    unsigned char ok = 0;
    uint64_t v = 0;
    /* Ten attempts. Intel's own guidance is to retry a small bounded number of
     * times and then treat the source as unavailable for now, rather than to
     * loop: a wedged RDSEED must not hang the boot. */
    for (int i = 0; i < 10 && !ok; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (!ok) stat_rdseed_retries++;
    }
    if (ok) *out = v;
    return ok ? 1 : 0;
}

static int rd_rand64(uint64_t* out) {
#ifdef ENTROPY_TEST_HOOKS
    if (force_rdrand_fail)  return 0;
    if (force_rdrand_stuck) { *out = 0x5555555555555555ull; return 1; }
#endif
    unsigned char ok = 0;
    uint64_t v = 0;
    for (int i = 0; i < 10 && !ok; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    }
    if (ok) *out = v;
    return ok ? 1 : 0;
}

#else   /* not x86-64 */

static void cpu_probe(void) {
    if (cpu_probed) return;
    cpu_probed = 1;
    /* No architectural RNG instruction on any ARM core in scope: RNDR is
     * ARMv8.5 (FEAT_RNG) and Cortex-A53, Cortex-A72 (Pi 4) and Neoverse N1
     * (Oracle Ampere) are all older. Jitter is not a fallback here, it is the
     * primary source. Design doc §2.3. */
    have_rdseed = 0;
    have_rdrand = 0;
    have_jitter = 1;
}
static int rd_seed64(uint64_t* out) { (void)out; return 0; }
static int rd_rand64(uint64_t* out) { (void)out; return 0; }

#endif

/* ─── Jitter ───────────────────────────────────────────────────────────────
 * Timing variation of a deliberately unpredictable workload, measured with
 * the architectural counter. The signal is the LOW bits of the delta: the
 * high bits are the workload's cost and are entirely predictable, so only the
 * bottom of each delta is folded in.
 *
 * This is the mechanism Linux's jitterentropy uses and it is the only source
 * guaranteed to exist on the ARM64 target. Its quality is hardware-dependent
 * and MUST be measured on the real boards rather than assumed -- design doc
 * §8.2 records that as an open assumption, not a settled one.
 *
 * The `volatile` accumulator is load-bearing. Without it the compiler is
 * entitled to delete the loop entirely, and the measurement would collapse to
 * the cost of two counter reads -- stable, predictable, and indistinguishable
 * from a working jitter source in every output this file produces. */
static uint8_t jitter_byte(void) {
    static volatile uint64_t acc = 0x243f6a8885a308d3ull;   /* pi, any nonzero */
    uint64_t t0 = read_tsc();
    uint64_t local = acc;
    /* Variable trip count derived from the counter itself, so the workload is
     * not the same length every call. */
    unsigned n = (unsigned)(t0 & 0x3fu) + 16u;
    for (unsigned i = 0; i < n; i++) {
        local ^= local << 13;
        local ^= local >> 7;
        local ^= local << 17;
        local += t0 + i;
    }
    acc = local;
    uint64_t t1 = read_tsc();
    return (uint8_t)((t1 - t0) ^ (local & 0xffu));
}

static int jitter_fill(uint8_t* out, size_t n) {
#ifdef ENTROPY_TEST_HOOKS
    if (force_jitter_fail) return 0;
#endif
    for (size_t i = 0; i < n; i++) out[i] = jitter_byte();
    return 1;
}

/* ─── HMAC-DRBG (SP 800-90A §10.1.2) ───────────────────────────────────────
 * Update, Instantiate, Reseed and Generate exactly as specified. The
 * separation matters for verification: NIST's CAVP vectors drive these four
 * operations individually, so a transcription error in any one of them is
 * caught by someone else's expected output rather than by our own reasoning. */

static void drbg_update(const uint8_t* p1, size_t n1,
                        const uint8_t* p2, size_t n2,
                        const uint8_t* p3, size_t n3) {
    uint8_t sep;
    hmac_sha256_ctx h;

    /* K = HMAC(K, V || 0x00 || provided_data) */
    sep = 0x00;
    hmac_sha256_init(&h, drbg_k, sizeof drbg_k);
    hmac_sha256_update(&h, drbg_v, sizeof drbg_v);
    hmac_sha256_update(&h, &sep, 1);
    if (n1) hmac_sha256_update(&h, p1, n1);
    if (n2) hmac_sha256_update(&h, p2, n2);
    if (n3) hmac_sha256_update(&h, p3, n3);
    hmac_sha256_final(&h, drbg_k);

    /* V = HMAC(K, V) */
    hmac_sha256(drbg_k, sizeof drbg_k, drbg_v, sizeof drbg_v, drbg_v);

    if (n1 == 0 && n2 == 0 && n3 == 0) return;   /* provided_data was null */

    /* K = HMAC(K, V || 0x01 || provided_data); V = HMAC(K, V) */
    sep = 0x01;
    hmac_sha256_init(&h, drbg_k, sizeof drbg_k);
    hmac_sha256_update(&h, drbg_v, sizeof drbg_v);
    hmac_sha256_update(&h, &sep, 1);
    if (n1) hmac_sha256_update(&h, p1, n1);
    if (n2) hmac_sha256_update(&h, p2, n2);
    if (n3) hmac_sha256_update(&h, p3, n3);
    hmac_sha256_final(&h, drbg_k);

    hmac_sha256(drbg_k, sizeof drbg_k, drbg_v, sizeof drbg_v, drbg_v);
}

static void drbg_instantiate(const uint8_t* seed, size_t seed_len,
                             const uint8_t* nonce, size_t nonce_len,
                             const uint8_t* pers, size_t pers_len) {
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) { drbg_k[i] = 0x00; drbg_v[i] = 0x01; }
    drbg_update(seed, seed_len, nonce, nonce_len, pers, pers_len);
    drbg_reseed_counter = 1;
    drbg_instantiated   = 1;
}

static void drbg_reseed_with(const uint8_t* seed, size_t seed_len,
                             const uint8_t* addl, size_t addl_len) {
    drbg_update(seed, seed_len, addl, addl_len, 0, 0);
    drbg_reseed_counter = 1;
}

static int drbg_generate(uint8_t* out, size_t len,
                         const uint8_t* addl, size_t addl_len) {
    if (!drbg_instantiated) return ENTROPY_E_NOT_SEEDED;
    if (drbg_reseed_counter > DRBG_RESEED_INTERVAL) return ENTROPY_E_NOT_SEEDED;

    if (addl_len) drbg_update(addl, addl_len, 0, 0, 0, 0);

    size_t done = 0;
    while (done < len) {
        hmac_sha256(drbg_k, sizeof drbg_k, drbg_v, sizeof drbg_v, drbg_v);
        size_t take = len - done;
        if (take > SHA256_DIGEST_LEN) take = SHA256_DIGEST_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = drbg_v[i];
        done += take;
    }

    drbg_update(addl, addl_len, 0, 0, 0, 0);
    drbg_reseed_counter++;
    return ENTROPY_OK;
}

/* ─── Gathering ────────────────────────────────────────────────────────────
 * Every source that works contributes. Note the ordering of health check and
 * absorb: a buffer whose source just failed its health test is discarded, not
 * hashed in. Mixing it would be harmless cryptographically and dishonest
 * accounting -- pool_bits would rise for entropy that was never there. */
static uint32_t gather(void) {
    uint32_t bits = 0;
    uint8_t  buf[32];

    cpu_probe();

    if (have_rdseed) {
        int got = 0;
        for (unsigned i = 0; i < sizeof buf; i += 8) {
            uint64_t v;
            if (!rd_seed64(&v)) break;
            for (unsigned j = 0; j < 8; j++) buf[i + j] = (uint8_t)(v >> (j * 8));
            got += 8;
        }
        if (got == (int)sizeof buf && !health_check_buf(&hs_rdseed, buf, sizeof buf, "rdseed")) {
            pool_absorb(buf, sizeof buf);
            bits += 8u * (uint32_t)sizeof buf;      /* full credit: true source */
        } else if (hs_rdseed.failed) {
            have_rdseed = 0;
        }
    }

    if (have_rdrand) {
        int got = 0;
        for (unsigned i = 0; i < sizeof buf; i += 8) {
            uint64_t v;
            if (!rd_rand64(&v)) break;
            for (unsigned j = 0; j < 8; j++) buf[i + j] = (uint8_t)(v >> (j * 8));
            got += 8;
        }
        if (got == (int)sizeof buf && !health_check_buf(&hs_rdrand, buf, sizeof buf, "rdrand")) {
            pool_absorb(buf, sizeof buf);
            /* Quarter credit. RDRAND is a DRBG, not a noise source -- its
             * output is conditioned already, so crediting it byte-for-byte
             * with RDSEED would overstate what it contributes.
             *
             * Precisely what this does and does not do, because an earlier
             * version of this comment claimed more: a machine with ONLY
             * RDRAND still seeds. It needs four times the samples, which is a
             * hedge against the source being weaker than advertised, not a
             * prohibition on relying on it. Refusing to boot such a machine
             * would be a different and probably worse policy; if that is
             * wanted it has to be written, not inferred from this number. */
            bits += 2u * (uint32_t)sizeof buf;
        } else if (hs_rdrand.failed) {
            have_rdrand = 0;
        }
    }

    if (have_jitter) {
        uint8_t jbuf[64];
        if (jitter_fill(jbuf, sizeof jbuf) &&
            !health_check_buf(&hs_jitter, jbuf, sizeof jbuf, "jitter")) {
            pool_absorb(jbuf, sizeof jbuf);
            /* One bit of min-entropy per BYTE, deliberately pessimistic. The
             * real figure is hardware-dependent and unmeasured on the target
             * boards; claiming more than can be defended is how a pool ends up
             * believing it is seeded when it is not. */
            bits += (uint32_t)sizeof jbuf;
        } else if (hs_jitter.failed) {
            have_jitter = 0;
        }
    }

    return bits;
}

void entropy_add(const void* data, size_t len, uint32_t est_bits) {
    if (!data || len == 0) return;
    pool_absorb(data, len);
    if (est_bits) {
        pool_bits += est_bits;
        if (pool_bits > 100000u) pool_bits = 100000u;      /* saturate */
    }
}

static int seed_drbg_from_pool(int reseeding) {
    /* Gather until the threshold or until every source is exhausted. The loop
     * bound is what stops a machine with no working source from hanging at
     * boot: it gives up and reports failure, which is the correct answer. */
    for (int round = 0; round < 64 && pool_bits < 8u * ENTROPY_SEED_BYTES; round++) {
        uint32_t got = gather();
        pool_bits += got;
        if (got == 0 && !have_rdseed && !have_rdrand && !have_jitter) break;
    }

    if (pool_bits < 8u * ENTROPY_SEED_BYTES) {
        kernel_serial_print("[ENTROPY] REFUSING to seed: pool below threshold. "
                            "TLS will not start on this node.\n");
        return ENTROPY_E_NOT_SEEDED;
    }

    uint8_t seed[SHA256_DIGEST_LEN];
    sha256_ctx snapshot = pool;          /* finalising would destroy the pool */
    sha256_final(&snapshot, seed);

    /* Re-absorb the extracted seed so the pool does not repeat it, and add a
     * counter reading so two extractions in the same boot differ even with no
     * new source data. */
    uint64_t t = read_tsc();
    pool_absorb(seed, sizeof seed);
    pool_absorb(&t, sizeof t);

    if (reseeding) {
        drbg_reseed_with(seed, sizeof seed, (const uint8_t*)&t, sizeof t);
    } else {
        /* Nonce: the counter. Personalisation: a build-fixed string, so two
         * different builds with identical entropy still diverge. */
        static const char pers[] = "AeroSLS-entropy-v1";
        drbg_instantiate(seed, sizeof seed,
                         (const uint8_t*)&t, sizeof t,
                         (const uint8_t*)pers, sizeof pers - 1);
    }

    volatile uint8_t* sz = seed;
    for (unsigned i = 0; i < sizeof seed; i++) sz[i] = 0;
    return ENTROPY_OK;
}

int entropy_init(void) {
    if (!drbg_instantiated) {
        sha256_init(&pool);
        pool_bits = 0;
    }
    int rc = seed_drbg_from_pool(drbg_instantiated ? 1 : 0);
    if (rc == ENTROPY_OK) {
        kernel_serial_print("[ENTROPY] seeded.\n");
    }
    return rc;
}

int entropy_reseed(void) {
    if (!drbg_instantiated) return entropy_init();
    drbg_reseed_counter = 1;
    return seed_drbg_from_pool(1);
}

int entropy_get(void* out, size_t len) {
    if (!out || len == 0) return ENTROPY_E_BADARG;
    if (!drbg_instantiated)  return ENTROPY_E_NOT_SEEDED;

    /* Reseed on interval rather than refusing. Refusing here would turn a
     * routine limit into an outage; refusing at init is the fail-closed case
     * that matters, because there we genuinely have nothing. */
    if (drbg_reseed_counter > DRBG_RESEED_INTERVAL) {
        if (seed_drbg_from_pool(1) != ENTROPY_OK) return ENTROPY_E_NOT_SEEDED;
    }

    /* Fresh jitter as additional input on every call: cheap, and it means two
     * calls cannot return the same bytes even if the DRBG state were somehow
     * duplicated (a restored checkpoint, a cloned VM image). */
    uint8_t addl[8];
    for (unsigned i = 0; i < sizeof addl; i++) addl[i] = jitter_byte();

    return drbg_generate((uint8_t*)out, len, addl, sizeof addl);
}

int entropy_is_ready(void) {
    return drbg_instantiated && pool_bits >= 8u * ENTROPY_SEED_BYTES;
}

void entropy_get_status(entropy_status_t* out) {
    if (!out) return;
    cpu_probe();
    out->ready            = (uint8_t)entropy_is_ready();
    out->have_rdseed      = have_rdseed;
    out->have_rdrand      = have_rdrand;
    out->have_jitter      = have_jitter;
    out->pool_bits        = pool_bits;
    out->reseed_count     = drbg_reseed_counter;
    out->health_failures  = stat_health_failures;
    out->rdseed_retries   = stat_rdseed_retries;
}

#ifdef ENTROPY_TEST_HOOKS
void entropy_test_force_source_failure(int rs, int rr, int j) {
    force_rdseed_fail = rs; force_rdrand_fail = rr; force_jitter_fail = j;
}
void entropy_test_force_stuck_source(int rs, int rr) {
    force_rdseed_stuck = rs; force_rdrand_stuck = rr;
}
void entropy_test_reset(void) {
    drbg_instantiated = 0; drbg_reseed_counter = 0; pool_bits = 0;
    stat_health_failures = 0; stat_rdseed_retries = 0;
    cpu_probed = 0; have_rdseed = have_rdrand = have_jitter = 0;
    force_rdseed_fail = force_rdrand_fail = force_jitter_fail = 0;
    force_rdseed_stuck = force_rdrand_stuck = 0;
    hs_rdseed = (health_state){0}; hs_rdrand = (health_state){0}; hs_jitter = (health_state){0};
    sha256_init(&pool);
}
int entropy_test_drbg_instantiate(const uint8_t* seed, size_t sl,
                                  const uint8_t* nonce, size_t nl,
                                  const uint8_t* pers, size_t pl) {
    drbg_instantiate(seed, sl, nonce, nl, pers, pl);
    return ENTROPY_OK;
}
int entropy_test_drbg_reseed(const uint8_t* seed, size_t sl,
                             const uint8_t* addl, size_t al) {
    drbg_reseed_with(seed, sl, addl, al);
    return ENTROPY_OK;
}
int entropy_test_drbg_generate(uint8_t* out, size_t len,
                               const uint8_t* addl, size_t al) {
    return drbg_generate(out, len, addl, al);
}
#endif
