/*
 * tls_cert_oracle.c — generate a real certificate with kernel/tls_cert.c and
 * write it out for OpenSSL to judge. Driven by tests/tls_cert_oracle_smoke.sh.
 *
 * ─── Why an oracle and not a unit test ─────────────────────────────────────
 * The Phase 3 gate is Chrome, Firefox and curl completing a handshake. That is
 * the right gate and a bad first oracle: a browser says only that the
 * connection is not private, and leaves you to work out which of a dozen
 * fields it disliked. OpenSSL reading the DER names the field.
 *
 * mbedtls_x509write_crt_set_validity() checks the LENGTH of a string and
 * nothing else. set_serial_raw() is a bounds check and a memcpy. Neither
 * inspects what it is given, so nothing inside this project can tell us the
 * encoding is right. Something outside it has to.
 *
 * ─── What is real here and what is not ─────────────────────────────────────
 * REAL: kernel/tls_cert.c, kernel/rtc.c, kernel/tls_platform.c (so
 * sls_mbedtls_rng is the shipping one), kernel/stubs.c (so sls_tls_snprintf is
 * the shipping one -- mbedTLS routes OID and DN formatting through it, and
 * this project has already had one near-miss where a snprintf that ignores its
 * format and writes "<nofmt>" would have put that literal in a subject name),
 * and vendor/mbedtls built with the KERNEL's own config.
 *
 * STUBBED: entropy_get(), so the fail-closed path can be driven on demand.
 * That is the pattern that hid the entropy_init() bug behind 31 passing host
 * tests, so state the limit plainly: this proves the ENCODING, not the
 * wiring. Whether the booted kernel reaches a seeded pool is what
 * entropy_boot_diversity_check.sh is for, and nothing here substitutes.
 *
 * NOT stubbed, though it easily could have been: the clock. rtc_set_unix() is
 * the real operator-supplied path, the one a Raspberry Pi with no RTC uses.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "tls_cert.h"
#include "entropy.h"
#include "rtc.h"

/* 2026-08-12T00:00:00Z. The smoke asserts notBefore comes out as 2026-08-11,
 * which is the only way to see TLS_CERT_BACKDATE_SECONDS actually applied. */
#define ORACLE_NOW 1786492800ULL

static int entropy_refuses = 0;
static unsigned char counter = 0;

/* --seed varies the stream so two runs produce two different certificates.
 * The default is 0 and deterministic, because tls_cert_oracle_smoke.sh's
 * expected values depend on it; the seed exists so
 * tls_gate_evidence.sh --compare can be shown detecting a DIFFERENCE, which
 * an unvarying harness cannot demonstrate. A test whose positive path has
 * never been observed is half a test. */

/* Deterministic, so the smoke's expected values are stable. On refusal the
 * buffer is left UNTOUCHED, which is entropy_get()'s real contract and the
 * whole reason the serial path copies through a local. */
int entropy_get(void *out, size_t len)
{
    if (entropy_refuses) { return ENTROPY_E_NOT_SEEDED; }
    unsigned char *p = out;
    for (size_t i = 0; i < len; i++) {
        p[i] = (unsigned char)(counter++ * 7 + i * 31 + 11);
    }
    return ENTROPY_OK;
}

int main(int argc, char **argv)
{
    unsigned char crt[4096], key[2048], ca[4096];
    size_t crt_len = 0, key_len = 0, ca_len = 0;
    /* The same set net/http.c installs, so this smoke judges the certificate
     * a node actually presents rather than a simpler one made for the test. */
    static const struct tls_cert_san sans[] = {
        { "localhost",  { 0, 0, 0, 0 } },
        { 0,            { 127, 0, 0, 1 } },
        { 0,            { 10, 0, 2, 15 } },
    };
    const char *crt_path = NULL, *key_path = NULL, *ca_path = NULL;
    int rc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-entropy") == 0)      { entropy_refuses = 1; }
        else if (strcmp(argv[i], "--crt") == 0 && i + 1 < argc) { crt_path = argv[++i]; }
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) { key_path = argv[++i]; }
        else if (strcmp(argv[i], "--ca") == 0 && i + 1 < argc) { ca_path = argv[++i]; }
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            unsigned v = 0; const char *q = argv[++i];
            while (*q >= '0' && *q <= '9') { v = v * 10u + (unsigned)(*q++ - '0'); }
            counter = (unsigned char)v;
        }
        else { fprintf(stderr, "usage: %s [--no-entropy] [--seed N] --crt F --key F\n", argv[0]); return 2; }
    }

    /* kernel.c:257 does this at boot. Without it mbedtls_calloc has no pool and
     * generation fails as an opaque TLS_CERT_E_MBEDTLS -- which is how this
     * harness first failed, and is a real dependency of tls_cert_self_signed()
     * rather than a harness quirk. */
    extern int sls_tls_memory_init(void);
    if (sls_tls_memory_init() != 0) { fprintf(stderr, "pool init failed\n"); return 1; }

    if (rtc_set_unix(ORACLE_NOW) != RTC_OK) {
        fprintf(stderr, "rtc_set_unix refused the time\n"); return 1;
    }

    rc = tls_cert_chain("CN=AeroSLS node 1 CA,O=AeroSLS",
                        "CN=AeroSLS node 1,O=AeroSLS",
                        sans, sizeof sans / sizeof sans[0],
                        90ULL * 24 * 60 * 60,
                        ca, sizeof ca, &ca_len,
                        crt, sizeof crt, &crt_len,
                        key, sizeof key, &key_len);
    if (rc != TLS_CERT_OK) {
        fprintf(stderr, "tls_cert_self_signed: rc=%d step=%s mbedtls=-0x%04x\n",
                rc, tls_cert_last_step(), (unsigned)(-tls_cert_last_mbedtls_ret()));
        return 1;
    }

    if (crt_path) {
        FILE *f = fopen(crt_path, "wb");
        if (!f) { perror("crt"); return 1; }
        fwrite(crt, 1, crt_len, f); fclose(f);
    }
    if (key_path) {
        FILE *f = fopen(key_path, "wb");
        if (!f) { perror("key"); return 1; }
        fwrite(key, 1, key_len, f); fclose(f);
    }
    if (ca_path) {
        FILE *f = fopen(ca_path, "wb");
        if (!f) { perror("ca"); return 1; }
        fwrite(ca, 1, ca_len, f); fclose(f);
    }
    fprintf(stderr, "generated: ca %zu, leaf %zu, key %zu bytes\n", ca_len, crt_len, key_len);
    return 0;
}
