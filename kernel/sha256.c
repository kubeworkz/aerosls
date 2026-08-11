/*
 * sha256.c — SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104).
 *
 * See sha256.h for why this exists and why it is a hash rather than a stream
 * cipher. Implementation notes only below.
 *
 * No libc: this kernel's convention is explicit loops rather than memcpy /
 * memset (see kernel/secure_api.c), which suits cryptographic code anyway --
 * a compiler is entitled to optimise memset away on a buffer it can prove is
 * dead, which is exactly how key material survives in stack memory.
 *
 * Every constant here is a transcription, and a transcription is exactly the
 * kind of thing that is wrong in one nibble and produces confident garbage.
 * That is why nothing in this file is trusted until tests/sha256_host_test.c
 * reproduces the published vectors -- a single wrong bit in K[] changes every
 * digest, so the vectors are a complete check on the table, not a sample.
 */

#include "sha256.h"

/* First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

/* One 64-byte block. Straight from FIPS 180-4 §6.2.2 with the message
 * schedule kept in full (64 words) rather than the 16-word rolling window --
 * the rolling version is a correct optimisation and an easy place to
 * introduce an off-by-one, and 256 bytes of stack is affordable here. */
static void sha256_block(uint32_t h[8], const uint8_t p[SHA256_BLOCK_LEN]) {
    uint32_t w[64];
    unsigned i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4 + 0] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] <<  8) |
               ((uint32_t)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i -  2], 17) ^ rotr(w[i - 2], 19) ^ (w[i -  2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = hh + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;

        hh = g; g = f; f = e; e = d + t1;
        d  = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;

    /* w[] held message material. Zeroed through a volatile pointer so the
     * compiler cannot decide the writes are dead -- the ordinary loop IS dead
     * by any local analysis, which is precisely the problem. */
    volatile uint32_t* wz = w;
    for (i = 0; i < 64; i++) wz[i] = 0;
}

void sha256_init(sha256_ctx* c) {
    /* First 32 bits of the fractional parts of the square roots of the first
     * eight primes (FIPS 180-4 §5.3.3). */
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total_len = 0;
    c->buf_len   = 0;
    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) c->buf[i] = 0;
}

void sha256_update(sha256_ctx* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->total_len += (uint64_t)len;

    /* Top up a partial block first. */
    if (c->buf_len > 0) {
        while (len > 0 && c->buf_len < SHA256_BLOCK_LEN) {
            c->buf[c->buf_len++] = *p++;
            len--;
        }
        if (c->buf_len == SHA256_BLOCK_LEN) {
            sha256_block(c->h, c->buf);
            c->buf_len = 0;
        }
    }

    while (len >= SHA256_BLOCK_LEN) {
        sha256_block(c->h, p);
        p   += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }

    while (len > 0) {
        c->buf[c->buf_len++] = *p++;
        len--;
    }
}

void sha256_final(sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]) {
    /* Length in BITS, big-endian, appended after 0x80 and zero padding, such
     * that the total is a multiple of 64. Captured before padding is appended
     * -- total_len must not count the padding itself. */
    uint64_t bit_len = c->total_len * 8u;

    c->buf[c->buf_len++] = 0x80u;
    if (c->buf_len > SHA256_BLOCK_LEN - 8) {
        while (c->buf_len < SHA256_BLOCK_LEN) c->buf[c->buf_len++] = 0;
        sha256_block(c->h, c->buf);
        c->buf_len = 0;
    }
    while (c->buf_len < SHA256_BLOCK_LEN - 8) c->buf[c->buf_len++] = 0;

    for (int i = 7; i >= 0; i--) {
        c->buf[c->buf_len++] = (uint8_t)(bit_len >> (i * 8));
    }
    sha256_block(c->h, c->buf);

    for (unsigned i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >>  8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }

    /* The context held message state; a caller that hashes a key must not
     * leave it on the stack. Volatile for the same reason as in the block
     * function. */
    volatile uint8_t*  bz = c->buf;
    volatile uint32_t* hz = c->h;
    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) bz[i] = 0;
    for (unsigned i = 0; i < 8; i++)                hz[i] = 0;
    c->buf_len = 0; c->total_len = 0;
}

void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ─── HMAC-SHA-256 ─────────────────────────────────────────────────────────
 * H((K ^ opad) || H((K ^ ipad) || m)), RFC 2104. */

void hmac_sha256_init(hmac_sha256_ctx* c, const void* key, size_t key_len) {
    uint8_t k[SHA256_BLOCK_LEN];
    unsigned i;

    for (i = 0; i < SHA256_BLOCK_LEN; i++) k[i] = 0;

    if (key_len > SHA256_BLOCK_LEN) {
        /* RFC 2104: keys longer than the block are hashed, and the DIGEST
         * becomes the key -- zero-padded to the block, not truncated to it. */
        uint8_t kh[SHA256_DIGEST_LEN];
        sha256(key, key_len, kh);
        for (i = 0; i < SHA256_DIGEST_LEN; i++) k[i] = kh[i];
        volatile uint8_t* z = kh;
        for (i = 0; i < SHA256_DIGEST_LEN; i++) z[i] = 0;
    } else {
        const uint8_t* kp = (const uint8_t*)key;
        for (i = 0; i < key_len; i++) k[i] = kp[i];
    }

    uint8_t ipad_key[SHA256_BLOCK_LEN];
    for (i = 0; i < SHA256_BLOCK_LEN; i++) {
        ipad_key[i]     = (uint8_t)(k[i] ^ 0x36u);
        c->opad_key[i]  = (uint8_t)(k[i] ^ 0x5cu);
    }

    sha256_init(&c->inner);
    sha256_update(&c->inner, ipad_key, SHA256_BLOCK_LEN);

    volatile uint8_t* kz = k;
    volatile uint8_t* iz = ipad_key;
    for (i = 0; i < SHA256_BLOCK_LEN; i++) { kz[i] = 0; iz[i] = 0; }
}

void hmac_sha256_update(hmac_sha256_ctx* c, const void* data, size_t len) {
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t inner_digest[SHA256_DIGEST_LEN];
    sha256_final(&c->inner, inner_digest);

    sha256_ctx outer;
    sha256_init(&outer);
    sha256_update(&outer, c->opad_key, SHA256_BLOCK_LEN);
    sha256_update(&outer, inner_digest, SHA256_DIGEST_LEN);
    sha256_final(&outer, out);

    volatile uint8_t* oz = c->opad_key;
    volatile uint8_t* dz = inner_digest;
    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++)  oz[i] = 0;
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) dz[i] = 0;
}

void hmac_sha256(const void* key, size_t key_len,
                 const void* data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, key_len);
    hmac_sha256_update(&c, data, data_len);
    hmac_sha256_final(&c, out);
}
