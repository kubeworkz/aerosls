/*
 * sha256_host_test.c — kernel/sha256.c against published vectors.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/sha256_host_test tests/sha256_host_test.c kernel/sha256.c
 *   /tmp/sha256_host_test
 *
 * ─── Why vectors and not properties ────────────────────────────────────────
 * Every other host test in this tree checks behaviour the project decided on.
 * This one checks a transcription against numbers NIST and the IETF decided
 * on, and that difference is the point: a hash function with one wrong bit in
 * K[] still hashes. It is deterministic, it avalanches, it produces 32
 * plausible bytes for every input, and it agrees with nothing else in the
 * world. No property-style test written from the same misunderstanding that
 * produced the bug would catch it.
 *
 * So the vectors here are copied from the specifications, not generated from
 * this implementation:
 *
 *   FIPS 180-4 / NIST CSRC examples  — SHA-256 of "abc", the empty string,
 *                                      the 56-byte two-block message, and
 *                                      one million 'a'.
 *   RFC 4231 §4                      — HMAC-SHA-256 cases 1-7, which between
 *                                      them cover a short key, a key exactly
 *                                      at the block length boundary, and the
 *                                      two long-key cases (131 bytes) that
 *                                      exercise the hash-the-key path.
 *
 * The million-'a' case matters more than it looks: it is the only one here
 * that crosses the 2^32-bit boundary in the length encoding and that streams
 * enough blocks to catch a broken buffer-refill path.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sha256.h"

static int checks = 0, fails = 0;

static void hexdump(const uint8_t* p, size_t n, char* out) {
    static const char* h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static void expect_hex(const uint8_t* got, size_t n, const char* want,
                       const char* what) {
    char g[160];
    checks++;
    hexdump(got, n, g);
    if (strcmp(g, want) == 0) {
        printf("ok:   %s\n", what);
    } else {
        printf("  FAIL  %s\n         got  %s\n         want %s\n", what, g, want);
        fails++;
    }
}

/* Parses an even-length hex string into buf; returns byte count. Used for the
 * RFC 4231 keys and data, which are given in hex precisely because several of
 * them are not printable. */
static size_t unhex(const char* hex, uint8_t* buf) {
    size_t n = 0;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        uint8_t hi = (uint8_t)(p[0] <= '9' ? p[0] - '0' : (p[0] | 32) - 'a' + 10);
        uint8_t lo = (uint8_t)(p[1] <= '9' ? p[1] - '0' : (p[1] | 32) - 'a' + 10);
        buf[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

int main(void) {
    uint8_t d[SHA256_DIGEST_LEN];

    printf("-- 1: SHA-256, FIPS 180-4 examples --\n");

    sha256("abc", 3, d);
    expect_hex(d, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "\"abc\"");

    sha256("", 0, d);
    expect_hex(d, 32,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty string");

    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    expect_hex(d, 32,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "56-byte two-block message");

    /* One million 'a', fed in awkward 1000-byte chunks so the buffer-refill
     * path is exercised rather than the aligned fast path. */
    {
        sha256_ctx c;
        uint8_t chunk[1000];
        for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = 'a';
        sha256_init(&c);
        for (int i = 0; i < 1000; i++) sha256_update(&c, chunk, sizeof chunk);
        sha256_final(&c, d);
        expect_hex(d, 32,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "one million 'a' (streamed in 1000-byte chunks)");
    }

    /* Unaligned, byte-at-a-time streaming of "abc" must equal the one-shot.
     * This is the update() path with buf_len walking 1, 2, 3 -- a different
     * code path from every vector above, all of which enter with buf_len 0. */
    {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, "a", 1);
        sha256_update(&c, "b", 1);
        sha256_update(&c, "c", 1);
        sha256_final(&c, d);
        expect_hex(d, 32,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "\"abc\" one byte per update() call");
    }

    /* A message of exactly 55 and exactly 56 bytes: 55 is the largest that
     * fits with its padding in one block, 56 is the smallest that forces a
     * second. The boundary either side of it is where padding code breaks.
     *
     * These two expected values are NOT from a specification -- no published
     * suite covers this input -- so they were computed with Python's hashlib,
     * an independent implementation. Stated because it is a weaker guarantee
     * than the vectors above: it proves agreement with CPython's OpenSSL, not
     * with FIPS 180-4. Never fill an expected value in from this code's own
     * output; that turns a test into a snapshot of the bug. */
    {
        uint8_t m[64];
        for (size_t i = 0; i < sizeof m; i++) m[i] = (uint8_t)i;
        sha256(m, 55, d);
        expect_hex(d, 32,
            "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59",
            "55 bytes (padding fits in one block)");
        sha256(m, 56, d);
        expect_hex(d, 32,
            "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562",
            "56 bytes (padding forces a second block)");
    }

    printf("\n-- 2: HMAC-SHA-256, RFC 4231 --\n");
    {
        uint8_t key[200], data[200], out[32];
        size_t kl, dl;

        /* Case 1: 20-byte key of 0x0b, data "Hi There" */
        kl = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", key);
        dl = unhex("4869205468657265", data);
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
            "case 1: 20-byte key");

        /* Case 2: key "Jefe", data "what do ya want for nothing?" --
         * a key SHORTER than the digest, exercising zero-padding. */
        kl = unhex("4a656665", key);
        dl = unhex("7768617420646f2079612077616e7420666f72206e6f7468696e673f", data);
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "case 2: 4-byte key (zero-padding path)");

        /* Case 3: 20-byte 0xaa key, 50 bytes of 0xdd */
        kl = unhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", key);
        for (dl = 0; dl < 50; dl++) data[dl] = 0xdd;
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
            "case 3: 50 bytes of 0xdd");

        /* Case 4: 25-byte incrementing key, 50 bytes of 0xcd */
        kl = unhex("0102030405060708090a0b0c0d0e0f10111213141516171819", key);
        for (dl = 0; dl < 50; dl++) data[dl] = 0xcd;
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
            "case 4: 25-byte key");

        /* Case 6: 131-byte key -- LONGER than the 64-byte block, so the key
         * is hashed first. This is the branch most often written wrong (by
         * truncating rather than hashing), and a wrong version still returns
         * 32 confident bytes. */
        for (kl = 0; kl < 131; kl++) key[kl] = 0xaa;
        dl = unhex("54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a"
                   "65204b6579202d2048617368204b6579204669727374", data);
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
            "case 6: 131-byte key (hash-the-key path)");

        /* Case 7: same long key, long message. */
        for (kl = 0; kl < 131; kl++) key[kl] = 0xaa;
        {
            const char* msg =
                "This is a test using a larger than block-size key and a larger "
                "than block-size data. The key needs to be hashed before being "
                "used by the HMAC algorithm.";
            dl = strlen(msg);
            memcpy(data, msg, dl);
        }
        hmac_sha256(key, kl, data, dl, out);
        expect_hex(out, 32,
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
            "case 7: long key and long message");
    }

    printf("\n=== %d passed, %d failed ===\n", checks - fails, fails);
    return fails == 0 ? 0 : 1;
}
