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
#include "tls_server.h"   /* TLS_SERVER_CIPHERSUITE_LIST -- the list itself, not a copy */
/* Needed for mbedtls_ssl_list_ciphersuites(). Without it the implicit
 * declaration returns int, the 64-bit pointer is truncated, and --suites
 * segfaults before printing anything -- which is exactly what it did. */
#include "mbedtls/ssl_ciphersuites.h"
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


/* ─── The reboot scenario, without rebooting ────────────────────────────────
 * Everything above judges ONE generation. The change that made the CA survive
 * a reboot is not testable that way: what has to hold is that a CA made on one
 * boot still signs a usable leaf on a LATER one, and that the anchor an
 * operator imported does not change underneath them.
 *
 * So this mode makes a CA once, moves the clock forward twice, and signs a
 * leaf at each stop -- which is exactly what two reboots do, minus the reboot.
 * The DER goes to disk for OpenSSL to judge; the return codes are judged here,
 * because "this call must fail, with THIS code" is not something openssl can
 * be asked.
 *
 * The negative cases are the load-bearing ones. A stored CA can be paired with
 * the wrong key or made with a DN a later build no longer uses, and BOTH
 * produce a certificate that is internally valid and that no browser will
 * accept -- Firefox says SEC_ERROR_BAD_SIGNATURE for the first and nothing
 * intelligible for the second. Asserting they are refused at generation is the
 * difference between a named error in a boot log and an afternoon in a trust
 * store.
 */
static int p_ok, p_fail;
static void pok(const char *m)  { printf("ok:   %s\n", m); p_ok++; }
static void pbad(const char *m) { printf("FAIL: %s\n", m); p_fail++; }

static int dump(const char *dir, const char *name,
                const unsigned char *b, size_t n)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(b, 1, n, f);
    fclose(f);
    return 0;
}

#define DAY (24ULL * 60ULL * 60ULL)

static int persist_mode(const char *dir, const struct tls_cert_san *sans,
                        size_t san_count)
{
    static unsigned char ca[4096], ca_key[2048];
    static unsigned char ca2[4096], ca2_key[2048];
    static unsigned char l1[4096], l1k[2048];
    static unsigned char l2[4096], l2k[2048];
    static unsigned char lc[4096], lck[2048];
    size_t ca_len = 0, ca_key_len = 0, ca2_len = 0, ca2_key_len = 0;
    size_t l1_len = 0, l1k_len = 0, l2_len = 0, l2k_len = 0;
    size_t lc_len = 0, lck_len = 0;
    const char *CA_DN   = "CN=AeroSLS node 1 CA,O=AeroSLS";
    const char *LEAF_DN = "CN=AeroSLS node 1,O=AeroSLS";
    int rc;

    /* Boot 1: no store on disk, so a CA is made. Five years, as
     * TLS_SERVER_CA_SECONDS specifies. */
    rc = tls_cert_make_ca(CA_DN, 5ULL * 365ULL * DAY,
                          ca, sizeof ca, &ca_len,
                          ca_key, sizeof ca_key, &ca_key_len);
    if (rc != TLS_CERT_OK) {
        fprintf(stderr, "make_ca: rc=%d step=%s\n", rc, tls_cert_last_step());
        return 1;
    }
    pok("a CA is generated with its key handed back, not destroyed");

    /* The CA key has to be small enough to share one 4 KiB frame with the CA
     * certificate. If a future key type blows that, this is where it should be
     * noticed -- not by a store that silently refuses to save. */
    if (ca_len + ca_key_len <= 4096u - 32u) {
        pok("CA certificate and key fit one 4 KiB frame with the header");
    } else {
        pbad("CA certificate and key do NOT fit one 4 KiB frame");
    }

    /* Boot 2, thirty days later. The CA comes off disk; only the leaf is new. */
    if (rtc_set_unix(ORACLE_NOW + 30ULL * DAY) != RTC_OK) {
        fprintf(stderr, "rtc_set_unix refused +30d\n"); return 1;
    }
    rc = tls_cert_sign_leaf(ca, ca_len, ca_key, ca_key_len, CA_DN, LEAF_DN,
                            sans, san_count, 365ULL * DAY,
                            l1, sizeof l1, &l1_len, l1k, sizeof l1k, &l1k_len);
    if (rc != TLS_CERT_OK) {
        fprintf(stderr, "sign_leaf #1: rc=%d step=%s\n", rc, tls_cert_last_step());
        return 1;
    }
    pok("a 30-day-old CA signs a fresh leaf");

    /* Boot 3, thirty days after that. Same CA, another new leaf. */
    if (rtc_set_unix(ORACLE_NOW + 60ULL * DAY) != RTC_OK) {
        fprintf(stderr, "rtc_set_unix refused +60d\n"); return 1;
    }
    rc = tls_cert_sign_leaf(ca, ca_len, ca_key, ca_key_len, CA_DN, LEAF_DN,
                            sans, san_count, 365ULL * DAY,
                            l2, sizeof l2, &l2_len, l2k, sizeof l2k, &l2k_len);
    if (rc != TLS_CERT_OK) {
        fprintf(stderr, "sign_leaf #2: rc=%d step=%s\n", rc, tls_cert_last_step());
        return 1;
    }
    pok("and signs a second one thirty days after that");

    if (l1_len != l2_len || memcmp(l1, l2, l1_len) != 0) {
        pok("the two leaves differ -- each boot really does mint a new one");
    } else {
        pbad("the two leaves are IDENTICAL -- the leaf is not being regenerated");
    }
    if (l1k_len != l2k_len || memcmp(l1k, l2k, l1k_len) != 0) {
        pok("and so do their private keys -- a fresh key per boot, as designed");
    } else {
        pbad("the two leaf KEYS are identical -- the key is being reused");
    }

    /* Clamping. A leaf asking for ten years from a CA with under five left
     * must come back with the CA's own notAfter, not its own. Without this the
     * leaf outlives its issuer and the chain fails at a moment nothing
     * explains. OpenSSL checks the actual dates; this checks it was accepted. */
    rc = tls_cert_sign_leaf(ca, ca_len, ca_key, ca_key_len, CA_DN, LEAF_DN,
                            sans, san_count, 10ULL * 365ULL * DAY,
                            lc, sizeof lc, &lc_len, lck, sizeof lck, &lck_len);
    if (rc == TLS_CERT_OK) {
        pok("a leaf asking for ten years from a five-year CA is issued, not refused");
    } else {
        pbad("a leaf asking beyond the CA's window was refused outright");
    }

    /* ─── The negatives ─────────────────────────────────────────────────── */

    /* A second CA, to borrow a wrong key from. */
    if (tls_cert_make_ca("CN=AeroSLS node 2 CA,O=AeroSLS", 5ULL * 365ULL * DAY,
                         ca2, sizeof ca2, &ca2_len,
                         ca2_key, sizeof ca2_key, &ca2_key_len) != TLS_CERT_OK) {
        fprintf(stderr, "make_ca #2 failed\n"); return 1;
    }

    /* CA #1's certificate with CA #2's key. This is what a torn 4 KiB write or
     * a half-updated store looks like from here, and mbedtls_pk_check_pair()
     * is what catches it. Left uncaught it produces leaves whose signature
     * does not verify -- Firefox's SEC_ERROR_BAD_SIGNATURE, which says nothing
     * about which of the two blobs on disk was wrong. */
    rc = tls_cert_sign_leaf(ca, ca_len, ca2_key, ca2_key_len, CA_DN, LEAF_DN,
                            sans, san_count, 365ULL * DAY,
                            l1, sizeof l1, &l1_len, l1k, sizeof l1k, &l1k_len);
    if (rc == TLS_CERT_E_CA_MISMATCH) {
        pok("a CA certificate paired with the wrong key is refused (E_CA_MISMATCH)");
    } else {
        pbad("a CA certificate paired with the WRONG KEY was accepted");
    }
    if (l1_len == 0) {
        pok("  and nothing was left in the caller's leaf buffer");
    } else {
        pbad("  but a leaf length was left set after the refusal");
    }

    /* The DN changed between builds while an older CA is still on disk. One
     * line in a header, no visible connection to certificates, and a chain no
     * path builder will join -- because issuer and subject are matched on
     * encoded bytes, not on meaning. */
    rc = tls_cert_sign_leaf(ca, ca_len, ca_key, ca_key_len,
                            "CN=AeroSLS node 1 CA,O=SomethingElse", LEAF_DN,
                            sans, san_count, 365ULL * DAY,
                            l1, sizeof l1, &l1_len, l1k, sizeof l1k, &l1k_len);
    if (rc == TLS_CERT_E_CA_MISMATCH) {
        pok("a DN that no longer matches the stored CA is refused (E_CA_MISMATCH)");
    } else {
        pbad("a leaf was signed with an issuer name the CA does not have");
    }

    /* An expired CA. Made with two days of life, asked to sign eight days
     * later. The clamp collapses the window and E_CA_EXPIRED falls out of it,
     * rather than a leaf whose notAfter is before its notBefore. */
    {
        static unsigned char sca[4096], sca_key[2048];
        size_t sca_len = 0, sca_key_len = 0;
        if (rtc_set_unix(ORACLE_NOW) != RTC_OK) {
            fprintf(stderr, "rtc_set_unix refused the base time\n"); return 1;
        }
        if (tls_cert_make_ca(CA_DN, 2ULL * DAY, sca, sizeof sca, &sca_len,
                             sca_key, sizeof sca_key, &sca_key_len) != TLS_CERT_OK) {
            fprintf(stderr, "make_ca (short) failed\n"); return 1;
        }
        if (rtc_set_unix(ORACLE_NOW + 8ULL * DAY) != RTC_OK) {
            fprintf(stderr, "rtc_set_unix refused +8d\n"); return 1;
        }
        rc = tls_cert_sign_leaf(sca, sca_len, sca_key, sca_key_len,
                                CA_DN, LEAF_DN, sans, san_count, 365ULL * DAY,
                                l1, sizeof l1, &l1_len, l1k, sizeof l1k, &l1k_len);
        if (rc == TLS_CERT_E_CA_EXPIRED) {
            pok("an expired CA is refused (E_CA_EXPIRED), not used to sign anyway");
        } else {
            pbad("an EXPIRED CA was used to sign a leaf");
        }
    }

    /* Seconds remaining, the number tls_server_init() renews on. Back at the
     * base clock the five-year CA should read close to five years; the
     * tolerance is a day because the certificate stores whole seconds and the
     * comparison is against a clock this test set itself. */
    {
        uint64_t left = 0;
        if (rtc_set_unix(ORACLE_NOW) != RTC_OK) { return 1; }
        rc = tls_cert_seconds_remaining(ca, ca_len, &left);
        if (rc == TLS_CERT_OK && left > (5ULL * 365ULL - 1ULL) * DAY &&
            left <= 5ULL * 365ULL * DAY) {
            pok("tls_cert_seconds_remaining reads the CA's own notAfter back");
        } else {
            pbad("tls_cert_seconds_remaining disagrees with the CA it read");
            fprintf(stderr, "      rc=%d left=%llu expected ~%llu\n",
                    rc, (unsigned long long)left,
                    (unsigned long long)(5ULL * 365ULL * DAY));
        }
    }

    /* l1 was reused as scratch by the negative cases above -- deliberately, to
     * prove they leave nothing usable behind -- so boot 2's leaf is signed
     * again here rather than writing whatever the last refusal left in the
     * buffer. l2 was never touched after boot 3 and goes out as it stands. */
    if (rtc_set_unix(ORACLE_NOW + 30ULL * DAY) != RTC_OK) { return 1; }
    if (tls_cert_sign_leaf(ca, ca_len, ca_key, ca_key_len, CA_DN, LEAF_DN,
                           sans, san_count, 365ULL * DAY,
                           l1, sizeof l1, &l1_len,
                           l1k, sizeof l1k, &l1k_len) != TLS_CERT_OK) {
        fprintf(stderr, "re-sign leaf1 failed\n"); return 1;
    }
    /* The simulated boot times, emitted rather than left for the shell to
     * recompute. OpenSSL judges validity against the REAL clock, so verifying
     * a leaf dated thirty days from now needs -attime -- and a second copy of
     * this arithmetic in the shell would be one edit away from testing a
     * different instant than the one the certificate was signed at. */
    printf("attime-boot2: %llu\n", (unsigned long long)(ORACLE_NOW + 30ULL * DAY));
    printf("attime-boot3: %llu\n", (unsigned long long)(ORACLE_NOW + 60ULL * DAY));

    if (dump(dir, "p_ca.der",      ca, ca_len)  ||
        dump(dir, "p_leaf1.der",   l1, l1_len)  ||
        dump(dir, "p_leaf2.der",   l2, l2_len)  ||
        dump(dir, "p_clamped.der", lc, lc_len)) {
        return 1;
    }

    printf("---- persist checks: passed=%d failed=%d\n", p_ok, p_fail);
    return p_fail == 0 ? 0 : 1;
}


/* ─── Every pinned suite must actually exist in this build ─────────────────
 * kernel/tls_server.c hands mbedtls_ssl_conf_ciphersuites() a fixed list. A
 * suite in that list which is NOT compiled into this configuration is silently
 * dropped -- no error, no log, nothing. The server simply never offers it, and
 * a client that supports only that suite gets a handshake failure.
 *
 * That matters most for ChaCha20, which is in the list precisely as the
 * FALLBACK for parts without hardware AES. A fallback that was quietly absent
 * would be discovered on the one machine it was put there for, which is the
 * worst possible place to discover it. Nothing has ever negotiated ChaCha20 on
 * a real node -- every handshake so far picked AES-256 -- so until this check
 * existed the claim rested entirely on the list being written down.
 *
 * The list comes from tls_server.h, not from a copy here. A test with its own
 * copy of the list would agree with itself forever.
 */
static int suite_mode(void)
{
    static const int pinned[] = { TLS_SERVER_CIPHERSUITE_LIST };
    static const char *names[] = {
        "TLS_AES_256_GCM_SHA384", "TLS_CHACHA20_POLY1305_SHA256",
        "TLS_AES_128_GCM_SHA256"
    };
    const int *avail = mbedtls_ssl_list_ciphersuites();
    size_t n = sizeof pinned / sizeof pinned[0];
    int fails = 0;

    if (!avail) { printf("FAIL: mbedtls_ssl_list_ciphersuites() returned nothing\n"); return 1; }

    for (size_t i = 0; i < n; i++) {
        int found = 0;
        for (const int *p = avail; *p; p++) { if (*p == pinned[i]) { found = 1; break; } }
        if (found) {
            printf("ok:   %s (0x%04x) is compiled in and can be offered\n",
                   names[i], (unsigned)pinned[i]);
        } else {
            printf("FAIL: %s (0x%04x) is PINNED but not compiled into this "
                   "configuration -- it will be silently dropped\n",
                   names[i], (unsigned)pinned[i]);
            fails++;
        }
    }

    /* The CCM suites are excluded from the pinned list on purpose. Whether
     * they are compiled in is not the point -- what matters is that they are
     * not offered, and the pinned list is the whole of what is offered. Stated
     * here so a reader does not go looking for a check that would be
     * meaningless. */
    {
        const char *info = "";
        for (const int *p = avail; *p; p++) {
            if (*p == 0x1304 || *p == 0x1305) { info = " (compiled in, but not offered -- correct)"; break; }
        }
        printf("      CCM suites: excluded from the pinned list%s\n", info);
    }

    printf("---- suite checks: passed=%d failed=%d\n", (int)n - fails, fails);
    return fails == 0 ? 0 : 1;
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
    const char *persist_dir = NULL;
    int suites_mode = 0;
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
        else if (strcmp(argv[i], "--persist") == 0 && i + 1 < argc) { persist_dir = argv[++i]; }
        else if (strcmp(argv[i], "--suites") == 0) { suites_mode = 1; }
        else { fprintf(stderr, "usage: %s [--no-entropy] [--seed N] [--persist DIR] --crt F --key F\n", argv[0]); return 2; }
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

    if (suites_mode) { return suite_mode(); }

    if (persist_dir) {
        return persist_mode(persist_dir, sans, sizeof sans / sizeof sans[0]);
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
