/* tls_cert.c — self-signed certificate generation, Phase 3.
 *
 * This file holds the validity half. What it has to get right is narrow and
 * unforgiving: mbedTLS checks the LENGTH of the string and nothing else, so
 * every way of being wrong that keeps the length at 14 -- a month off by one,
 * a century mis-rounded, local time where UTC was meant -- passes silently
 * into a certificate and comes back as a browser error weeks later.
 *
 * Three decisions worth stating, because each had an obvious alternative:
 *
 * 1. NO snprintf. sls_tls_snprintf does implement "%04d%02u%02u%02u%02u%02u"
 *    correctly -- that was checked, not assumed, and the zero-pad width path
 *    in kernel/stubs.c handles it. But a fixed 14-column layout does not need
 *    a format-string parser: hand-placing the digits has no format string to
 *    get wrong, no varargs to mistype (%u takes unsigned int, and passing an
 *    int is the kind of thing that works until it does not), and no dependency
 *    on stubs.c, which is what lets the host test compile this file against
 *    rtc.c alone.
 *
 * 2. The calendar arithmetic is NOT repeated here. rtc_break_down() in rtc.c
 *    is the one implementation, and mbedtls_platform_gmtime_r() in
 *    tls_platform.c now delegates to it as well. Before this change the
 *    seconds-of-day split existed in tls_platform.c and would have been
 *    written a second time here -- and this project already has two snprintfs
 *    for exactly that reason. rtc.h's own comment asks for both directions of
 *    the conversion to live in rtc.c "tested together", so they now do.
 *
 * 3. Failure leaves the output buffer untouched, matching entropy_get() and
 *    rtc_get_unix(). A caller that ignores the return gets its own
 *    uninitialised stack rather than a plausible wrong date -- which is worse
 *    to debug and better to catch, and the exact-14 check downstream is a
 *    second net under it.
 */

#include "tls_cert.h"
#include "rtc.h"
#include "entropy.h"

/* Two zero-padded decimal digits at p[0..1]. v is already range-checked by the
 * caller; the % 10 on the tens digit is belt-and-braces so a value above 99
 * cannot write a non-digit into a certificate. */
static void put2(char *p, unsigned v)
{
    p[0] = (char)('0' + (v / 10u) % 10u);
    p[1] = (char)('0' + (v % 10u));
}

static void put4(char *p, unsigned v)
{
    p[0] = (char)('0' + (v / 1000u) % 10u);
    p[1] = (char)('0' + (v / 100u) % 10u);
    p[2] = (char)('0' + (v / 10u) % 10u);
    p[3] = (char)('0' + (v % 10u));
}

int tls_cert_format_time(uint64_t unix_seconds, char *out, size_t out_size)
{
    int64_t  year = 0;
    unsigned mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    char scratch[TLS_CERT_TIME_BUF];

    if (!out || out_size < TLS_CERT_TIME_BUF) {
        return TLS_CERT_E_BADARG;
    }

    rtc_break_down((int64_t)unix_seconds, &year, &mon, &day,
                   &hour, &min, &sec, (int *)0);

    /* The year must be four digits, and this is not cosmetic. x509_write_time()
     * in x509write_crt.c:393 chooses between ASN.1 UTCTime and GeneralizedTime
     * by reading the first three characters AS TEXT --
     *   t[0] < '2' || (t[0] == '2' && t[1] == '0' && t[2] < '5')
     * -- and then, for UTCTime, writes t+2 on the assumption that skipping two
     * characters skips a century. A three- or five-digit year does not merely
     * look wrong; it silently selects the wrong ASN.1 type and truncates at the
     * wrong offset. Refuse instead. */
    if (year < 1000 || year > 9999) {
        return TLS_CERT_E_RANGE;
    }

    /* Composed in scratch and copied out only on success, so a caller that
     * ignores the return code cannot find a half-written date in its buffer. */
    put4(&scratch[0],  (unsigned)year);
    put2(&scratch[4],  mon);
    put2(&scratch[6],  day);
    put2(&scratch[8],  hour);
    put2(&scratch[10], min);
    put2(&scratch[12], sec);
    scratch[TLS_CERT_TIME_LEN] = '\0';

    for (size_t i = 0; i < TLS_CERT_TIME_BUF; i++) {
        out[i] = scratch[i];
    }
    return TLS_CERT_OK;
}

/* The window, from a caller-supplied `now`.
 *
 * Split out of tls_cert_validity_window() when tls_cert_sign_leaf() needed
 * BOTH the window and the `now` it was derived from, to compare against a
 * stored CA's notAfter. Reading rtc_get_unix() twice would have given two
 * timestamps microseconds apart and no way to notice when they disagreed;
 * one read feeding both is a property, not a micro-optimisation. */
/* The argument checks, separately, so BOTH callers can run them BEFORE
 * reading the clock.
 *
 * That ordering is a property tests/tls_cert_host_test.c asserts, and it is
 * worth the extra function: a caller that passes a zero lifetime on a node
 * with no trusted clock must be told its argument was wrong, not that the
 * clock was missing. Reporting the environment's fault for the caller's is how
 * a two-minute fix becomes an afternoon spent on the RTC. This split was
 * originally lost when window_from() was factored out, and the host test
 * failed on exactly these two cases -- which is what it is for. */
static int window_args(uint64_t lifetime_seconds,
                       const char *not_before, size_t nb_size,
                       const char *not_after, size_t na_size)
{
    if (!not_before || !not_after ||
        nb_size < TLS_CERT_TIME_BUF || na_size < TLS_CERT_TIME_BUF) {
        return TLS_CERT_E_BADARG;
    }
    if (lifetime_seconds == 0 || lifetime_seconds > ((uint64_t)1 << 40)) {
        return TLS_CERT_E_BADARG;
    }
    return TLS_CERT_OK;
}

static int window_from(uint64_t now, uint64_t lifetime_seconds,
                       char *not_before, size_t nb_size,
                       char *not_after, size_t na_size)
{
    uint64_t from = 0, to = 0;
    char nb[TLS_CERT_TIME_BUF], na[TLS_CERT_TIME_BUF];
    int rc;

    if ((rc = window_args(lifetime_seconds, not_before, nb_size,
                          not_after, na_size)) != TLS_CERT_OK) {
        return rc;
    }

    /* Unsigned subtraction. rtc's sanity rules already refuse anything before
     * the build epoch, so this cannot fire today -- which is exactly why it is
     * here: the guard costs one comparison and the wrap it prevents produces a
     * notBefore in the year 584942417355. */
    if (now < TLS_CERT_BACKDATE_SECONDS) {
        return TLS_CERT_E_RANGE;
    }
    from = now - TLS_CERT_BACKDATE_SECONDS;

    to = now + lifetime_seconds;
    if (to < now) {
        return TLS_CERT_E_RANGE;   /* overflow */
    }

    if ((rc = tls_cert_format_time(from, nb, sizeof nb)) != TLS_CERT_OK) {
        return rc;
    }
    if ((rc = tls_cert_format_time(to, na, sizeof na)) != TLS_CERT_OK) {
        return rc;
    }

    /* Both formatted before either is published, so a failure on notAfter
     * cannot leave a valid notBefore next to a stale notAfter. */
    for (size_t i = 0; i < TLS_CERT_TIME_BUF; i++) {
        not_before[i] = nb[i];
        not_after[i]  = na[i];
    }
    return TLS_CERT_OK;
}

int tls_cert_validity_window(uint64_t lifetime_seconds,
                             char *not_before, size_t nb_size,
                             char *not_after, size_t na_size)
{
    uint64_t now = 0;
    int rc;

    /* Arguments before the clock -- see window_args(). */
    if ((rc = window_args(lifetime_seconds, not_before, nb_size,
                          not_after, na_size)) != TLS_CERT_OK) {
        return rc;
    }

    /* Fail closed. §1.1: no trusted time means decline, never assume the epoch
     * and never fall back to boot time. This is the first real consumer of that
     * rule outside rtc.c itself. */
    if (rtc_get_unix(&now) != RTC_OK) {
        return TLS_CERT_E_NO_TIME;
    }
    return window_from(now, lifetime_seconds, not_before, nb_size,
                       not_after, na_size);
}

/* Local rather than mbedtls_platform_zeroize(): that lives in tls_platform.c,
 * which the host test does not link, and the serial path must stay testable
 * without pulling 8 MB of library into a unit test. volatile so the store is
 * not optimised away on a buffer nothing reads again. */
static void secure_zero(unsigned char *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) { *v++ = 0; }
}

/* ─── Serial numbers ───────────────────────────────────────────────────────
 * Two things mbedTLS does NOT do for us, both from reading x509write_crt.c:
 *
 *   set_serial_raw() is a bounds check and a memcpy. It rejects more than 20
 *   octets and stores the rest verbatim.
 *
 *   mbedtls_x509write_crt_der() then prepends 0x00 when the top bit of the
 *   first byte is set, which is correct DER for a positive INTEGER and
 *   silently produces a 21-OCTET serial -- one over the RFC 5280 ceiling that
 *   set_serial_raw() just finished enforcing. Half of all random 20-byte
 *   serials do this.
 *
 * And a first byte of 0x00 would be written verbatim, giving a non-minimal
 * INTEGER: a DER violation with a 1-in-256 incidence, which is the sort of
 * defect that reproduces once a fortnight and gets blamed on the network.
 *
 * So: clear the top bit, force the low bit. 158 bits of entropy against the
 * 64 anyone asks for, and the encoding is positive, minimal and non-zero by
 * construction rather than by luck. */
int tls_cert_make_serial(unsigned char *out, size_t len)
{
    unsigned char tmp[TLS_CERT_SERIAL_LEN];

    if (!out || len != TLS_CERT_SERIAL_LEN) {
        return TLS_CERT_E_BADARG;
    }

    /* Into tmp, not out: entropy_get() leaves its buffer UNTOUCHED on failure,
     * so writing straight into the caller's buffer and returning an error
     * would leave them holding their own uninitialised stack -- which is a
     * disclosure bug, not a weak-serial bug, because the serial is published
     * to every client that connects. */
    if (entropy_get(tmp, sizeof tmp) != ENTROPY_OK) {
        return TLS_CERT_E_NO_ENTROPY;
    }

    tmp[0] = (unsigned char)((tmp[0] & 0x7f) | 0x01);

    for (size_t i = 0; i < sizeof tmp; i++) {
        out[i] = tmp[i];
    }
    secure_zero(tmp, sizeof tmp);
    return TLS_CERT_OK;
}


/* See tls_cert.h. Pure: no clock, no parsing, no allocation -- `now` and
 * `not_after` are supplied, so a test can drive the boundary at both sides of
 * the exact second without owning a clock. */
int tls_cert_renew_due(uint64_t now, uint64_t not_after, uint64_t lifetime_seconds)
{
    uint64_t remaining, margin;

    if (lifetime_seconds == 0) { return 1; }
    if (not_after <= now)      { return 1; }   /* expired, or expiring this second */

    remaining = not_after - now;
    margin    = lifetime_seconds / 3u;
    if (margin == 0) { return 1; }             /* a lifetime under 3s is not one */

    /* <=, not <. The boundary is CLOSED, and that is not a coin flip:
     * tls_server.c publishes `not_after - margin` to /api/health as
     * tls_leaf_renew_at, the second at which renewal becomes due. With a
     * strict < that published instant would be the one second at which it is
     * NOT due, and the number an operator reads would disagree with the
     * behaviour it describes. The check runs hourly, so nothing practical
     * turns on the single second -- but a number that contradicts the code
     * beside it is how the next person loses an afternoon. */
    return remaining <= margin;
}


#ifndef TLS_CERT_HOST_TEST
/* Everything below needs the vendored tree. Guarded the way tls_platform.c
 * guards its own mbedTLS half, so the validity and serial logic above stays
 * compilable against rtc.c alone. */
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/asn1.h"
#include "mbedtls/platform_util.h"
#include "tls_platform.h"

size_t strlen(const char *s);

static int has_eq(const char *s)
{
    while (*s) { if (*s == '=') { return 1; } s++; }
    return 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* mbedTLS writes DER at the END of the buffer and returns its length. Every
 * caller has to move it, and a caller that forgets gets a buffer whose first
 * bytes are zero and whose content is somewhere in the middle -- which parses
 * as nothing and looks like a generation failure rather than a copy failure. */
static void der_to_front(unsigned char *buf, size_t size, size_t len)
{
    unsigned char *start = buf + size - len;
    for (size_t i = 0; i < len; i++) {
        buf[i] = start[i];
    }
}

/* Chronological comparison of two TLS_CERT_TIME_LEN strings, by comparing
 * their bytes. That is not a shortcut: YYYYMMDDHHMMSS is fixed-width and
 * zero-padded in every field, so lexicographic order IS chronological order,
 * with nothing to parse and therefore nothing to parse wrong. It is one of the
 * reasons the format is worth the hand-placed digits above. Returns <0, 0, >0
 * like memcmp, and reads exactly TLS_CERT_TIME_LEN bytes -- never the NUL, so
 * it is safe on a buffer that has none. */
static int time_cmp(const char *a, const char *b)
{
    for (size_t i = 0; i < TLS_CERT_TIME_LEN; i++) {
        if (a[i] != b[i]) { return (a[i] < b[i]) ? -1 : 1; }
    }
    return 0;
}

static const char *g_step = "";
static int g_ret = 0;
const char *tls_cert_last_step(void) { return g_step; }
int tls_cert_last_mbedtls_ret(void) { return g_ret; }

/* Record the step name and mbedTLS's own return code, then bail. Every
 * mbedTLS call below goes through this, so TLS_CERT_E_MBEDTLS always comes
 * with the answer to "which one". */
#define TRY(name, call) do { \
    int r_ = (call); \
    if (r_ != 0) { g_step = (name); g_ret = r_; goto done; } \
} while (0)

int tls_cert_self_signed(const char *dn,
                         const struct tls_cert_san *sans, size_t san_count,
                         uint64_t lifetime_seconds,
                         unsigned char *crt_der, size_t crt_size, size_t *crt_len,
                         unsigned char *key_der, size_t key_size, size_t *key_len)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    /* The nodes must outlive the set_subject_alternative_name() call, which
     * walks the list, so they are locals of this function and not of the loop
     * that fills them. */
    mbedtls_x509_san_list san_node[TLS_CERT_MAX_SANS];
    unsigned char serial[TLS_CERT_SERIAL_LEN];
    char nb[TLS_CERT_TIME_BUF], na[TLS_CERT_TIME_BUF];
    int rc = TLS_CERT_E_MBEDTLS;
    int ret;

    if (!dn || !crt_der || !crt_len || !key_der || !key_len) {
        return TLS_CERT_E_BADARG;
    }
    /* Chrome 58 removed commonName matching entirely. A certificate whose
     * identity lives only in the CN is not "less good"; it is rejected, and
     * the failure looks like a TLS bug. Refuse to build one. */
    if (!sans || san_count == 0 || san_count > TLS_CERT_MAX_SANS) {
        return TLS_CERT_E_BADARG;
    }
    /* "node1" is not a DN. Catch it here rather than inside mbedTLS's name
     * parser, where it becomes a number. */
    if (!has_eq(dn)) { return TLS_CERT_E_BADARG; }
    g_step = ""; g_ret = 0;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    /* Order matters: everything that can fail WITHOUT generating key material
     * goes first, so a node with no clock never reaches the generator. */
    if ((rc = tls_cert_validity_window(lifetime_seconds,
                                       nb, sizeof nb, na, sizeof na)) != TLS_CERT_OK) {
        goto done;
    }
    if ((rc = tls_cert_make_serial(serial, sizeof serial)) != TLS_CERT_OK) {
        goto done;
    }

    rc = TLS_CERT_E_MBEDTLS;
    TRY("pk_setup", mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)));
    /* mbedtls_ecp_gen_key() takes the group id directly, so there is no
     * separate group_load step. The design doc inferred from a negative grep
     * that only mbedtls_ecp_gen_keypair_base existed; it is at ecp.h:1248. */
    TRY("ecp_gen_key", mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                           mbedtls_pk_ec(key), sls_mbedtls_rng, NULL));

    /* Self-signed: subject and issuer are the same name and the same key. */
    TRY("set_subject_name", mbedtls_x509write_crt_set_subject_name(&crt, dn));
    TRY("set_issuer_name",  mbedtls_x509write_crt_set_issuer_name(&crt, dn));
    /* These three return void. That inference in the design doc was correct;
     * unlike the ecp_gen_key one, it has now been confirmed by reading. */
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    TRY("set_validity", mbedtls_x509write_crt_set_validity(&crt, nb, na));
    TRY("set_serial_raw", mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial));
    /* ─── CA:TRUE, and it is a trade rather than a flag ───────────────────
     * This started as CA:FALSE, on the reasoning that a leaf which happens to
     * be its own issuer is not a certificate authority. curl --cacert agreed
     * and verified it happily.
     *
     * Windows did not. Importing it put "AeroSLS node" into Intermediate
     * Certification Authorities rather than Trusted Root, because the import
     * wizard classifies by certificate type and a certificate that does not
     * assert CA:TRUE is not a root. An intermediate is not a trust anchor, so
     * Chrome walked the chain, found nothing trusted at the end of it, and
     * reported ERR_CERT_AUTHORITY_INVALID -- correctly.
     *
     * What this costs, stated rather than discovered later: a certificate
     * that both serves traffic and asserts CA:TRUE can, if its key leaks,
     * mint certificates for any name a client trusts it for. pathlen 0 limits
     * it to signing leaves, not further CAs, which narrows the blast radius
     * without removing it.
     *
     * This is a Phase 3 expedient with a stated expiry. §4's private CA
     * removes it properly, by making the trust anchor a separate key that
     * never serves traffic -- which is the actual answer, and is why the
     * roadmap has that phase at all. */
    TRY("set_basic_constraints", mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0));
    TRY("set_subject_key_identifier", mbedtls_x509write_crt_set_subject_key_identifier(&crt));

    /* SubjectAltName. Read out of library/x509write.c rather than from the
     * struct's own field comment, which says only rfc822Name, dnsName and URI
     * are supported -- that comment describes the PARSE side and is wrong for
     * writing. mbedtls_x509_write_set_san_common() handles IP_ADDRESS, taking
     * the bytes from san.unstructured_name and writing them raw under the
     * context tag. So an IP entry is FOUR BYTES; a dotted-quad string would
     * encode the ASCII text under the iPAddress tag and every verifier would
     * reject it. */
    for (size_t i = 0; i < san_count; i++) {
        mbedtls_x509_san_list *n = &san_node[i];
        if (sans[i].dns) {
            n->node.type = MBEDTLS_X509_SAN_DNS_NAME;
            n->node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
            n->node.san.unstructured_name.p   = (unsigned char *)(uintptr_t)sans[i].dns;
            n->node.san.unstructured_name.len = strlen(sans[i].dns);
            /* An empty dNSName encodes as a zero-length name, which is not a
             * name and which some verifiers treat as matching nothing and
             * others as malformed. Refuse rather than find out which. */
            if (n->node.san.unstructured_name.len == 0) {
                rc = TLS_CERT_E_BADARG;
                goto done;
            }
        } else {
            n->node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            n->node.san.unstructured_name.tag = MBEDTLS_ASN1_OCTET_STRING;
            n->node.san.unstructured_name.p   = (unsigned char *)(uintptr_t)sans[i].ip4;
            n->node.san.unstructured_name.len = 4;
        }
        n->next = (i + 1 < san_count) ? &san_node[i + 1] : NULL;
    }
    TRY("set_subject_alternative_name", mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_node[0]));

    /* Signing needs randomness too: ECDSA draws a per-signature nonce, and a
     * reused or predictable one recovers the private key outright. Same RNG,
     * same fail-closed path. */
    ret = mbedtls_x509write_crt_der(&crt, crt_der, crt_size, sls_mbedtls_rng, NULL);
    if (ret < 0) { g_step = "crt_der"; g_ret = ret; goto done; }
    *crt_len = (size_t)ret;
    der_to_front(crt_der, crt_size, *crt_len);

    ret = mbedtls_pk_write_key_der(&key, key_der, key_size);
    if (ret < 0) { g_step = "pk_write_key_der"; g_ret = ret; goto done; }
    *key_len = (size_t)ret;
    der_to_front(key_der, key_size, *key_len);

    rc = TLS_CERT_OK;

done:
    mbedtls_platform_zeroize(serial, sizeof serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return rc;
}


/* Two tiny predicates, named because the checks they express are the two
 * ways a chain silently degenerates: a DN with no '=' is not a DN at all, and
 * a leaf whose subject equals its issuer reads as self-signed to a path
 * builder no matter who signed it. */
/* ─── The two-certificate chain ────────────────────────────────────────────
 * See tls_cert.h for why one certificate cannot work. Briefly: Chrome needs
 * CA:TRUE to anchor it, Firefox refuses CA:TRUE at the end-entity position,
 * and those cannot both be satisfied by one certificate.
 *
 * Shared by both certificates so the two are consistent by construction
 * rather than by two call sites agreeing: the same validity window, the same
 * backdate, the same clock read. A leaf valid outside its issuer's window is
 * a failure every verifier reports as something else.
 */
static int write_one(mbedtls_x509write_cert *crt,
                     const char *subject_dn, const char *issuer_dn,
                     mbedtls_pk_context *subject_key,
                     mbedtls_pk_context *issuer_key,
                     const char *nb, const char *na,
                     int is_ca,
                     const struct tls_cert_san *sans, size_t san_count,
                     unsigned char *out, size_t out_size, size_t *out_len)
{
    unsigned char serial[TLS_CERT_SERIAL_LEN];
    mbedtls_x509_san_list san_node[TLS_CERT_MAX_SANS];
    int rc, ret;

    if ((rc = tls_cert_make_serial(serial, sizeof serial)) != TLS_CERT_OK) {
        return rc;
    }

    rc = TLS_CERT_E_MBEDTLS;
    TRY("set_subject_name", mbedtls_x509write_crt_set_subject_name(crt, subject_dn));
    TRY("set_issuer_name",  mbedtls_x509write_crt_set_issuer_name(crt, issuer_dn));
    mbedtls_x509write_crt_set_subject_key(crt, subject_key);
    mbedtls_x509write_crt_set_issuer_key(crt, issuer_key);
    mbedtls_x509write_crt_set_md_alg(crt, MBEDTLS_MD_SHA256);
    TRY("set_validity", mbedtls_x509write_crt_set_validity(crt, nb, na));
    TRY("set_serial_raw", mbedtls_x509write_crt_set_serial_raw(crt, serial, sizeof serial));
    TRY("set_basic_constraints",
        mbedtls_x509write_crt_set_basic_constraints(crt, is_ca, is_ca ? 0 : -1));
    TRY("set_subject_key_identifier", mbedtls_x509write_crt_set_subject_key_identifier(crt));
    /* The leaf needs to point at its issuer. Without an authorityKeyIdentifier
     * a path builder matches on name alone, which is enough here but stops
     * being enough the moment a second CA exists. */
    TRY("set_authority_key_identifier", mbedtls_x509write_crt_set_authority_key_identifier(crt));

    /* keyUsage, now that the two roles are separate and can be stated
     * honestly. The CA signs certificates and nothing else; the leaf signs
     * handshakes and never certificates. That separation is the entire point
     * of splitting them, and leaving it unstated would waste it. */
    if (is_ca) {
        TRY("set_key_usage",
            mbedtls_x509write_crt_set_key_usage(crt, MBEDTLS_X509_KU_KEY_CERT_SIGN |
                                                     MBEDTLS_X509_KU_CRL_SIGN));
    } else {
        TRY("set_key_usage",
            mbedtls_x509write_crt_set_key_usage(crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE));
    }

    if (san_count > 0) {
        for (size_t i = 0; i < san_count; i++) {
            mbedtls_x509_san_list *n = &san_node[i];
            if (sans[i].dns) {
                n->node.type = MBEDTLS_X509_SAN_DNS_NAME;
                n->node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
                n->node.san.unstructured_name.p   = (unsigned char *)(uintptr_t)sans[i].dns;
                n->node.san.unstructured_name.len = strlen(sans[i].dns);
                if (n->node.san.unstructured_name.len == 0) { rc = TLS_CERT_E_BADARG; goto done; }
            } else {
                n->node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
                n->node.san.unstructured_name.tag = MBEDTLS_ASN1_OCTET_STRING;
                n->node.san.unstructured_name.p   = (unsigned char *)(uintptr_t)sans[i].ip4;
                n->node.san.unstructured_name.len = 4;
            }
            n->next = (i + 1 < san_count) ? &san_node[i + 1] : NULL;
        }
        TRY("set_subject_alternative_name",
            mbedtls_x509write_crt_set_subject_alternative_name(crt, &san_node[0]));
    }

    ret = mbedtls_x509write_crt_der(crt, out, out_size, sls_mbedtls_rng, NULL);
    if (ret < 0) { g_step = "crt_der"; g_ret = ret; goto done; }
    *out_len = (size_t)ret;
    der_to_front(out, out_size, *out_len);
    rc = TLS_CERT_OK;

done:
    secure_zero(serial, sizeof serial);
    return rc;
}


/* ─── The CA and the leaf, separately ──────────────────────────────────────
 * See tls_cert.h for why these are split. In one line: a CA whose key is
 * destroyed can only ever sign one leaf, so every reboot minted a new trust
 * anchor and cost a re-import in two trust stores.
 */

/* An mbedtls_x509_time from a parsed certificate, as the same 14-character
 * string everything else here speaks.
 *
 * Read straight out of the parsed fields rather than converted to Unix
 * seconds and back. The round trip would be two more chances to be wrong in
 * a way that keeps the length at 14 -- which is the exact failure mode this
 * whole file is written against. */
static int x509_time_str(const mbedtls_x509_time *t, char *out)
{
    if (t->year < 1000 || t->year > 9999 ||
        t->mon  < 1    || t->mon  > 12   ||
        t->day  < 1    || t->day  > 31   ||
        t->hour < 0    || t->hour > 23   ||
        t->min  < 0    || t->min  > 59   ||
        t->sec  < 0    || t->sec  > 60) {     /* 60: leap second, per X.509 */
        return TLS_CERT_E_RANGE;
    }
    put4(&out[0],  (unsigned)t->year);
    put2(&out[4],  (unsigned)t->mon);
    put2(&out[6],  (unsigned)t->day);
    put2(&out[8],  (unsigned)t->hour);
    put2(&out[10], (unsigned)t->min);
    put2(&out[12], (unsigned)t->sec);
    out[TLS_CERT_TIME_LEN] = '\0';
    return TLS_CERT_OK;
}

int tls_cert_make_ca(const char *ca_dn, uint64_t lifetime_seconds,
                     unsigned char *ca_der, size_t ca_size, size_t *ca_len,
                     unsigned char *ca_key_der, size_t ck_size, size_t *ck_len)
{
    mbedtls_pk_context ca_key;
    mbedtls_x509write_cert crt;
    char nb[TLS_CERT_TIME_BUF], na[TLS_CERT_TIME_BUF];
    int rc, ret;

    if (!ca_dn || !ca_der || !ca_len || !ca_key_der || !ck_len) {
        return TLS_CERT_E_BADARG;
    }
    if (!has_eq(ca_dn)) { return TLS_CERT_E_BADARG; }

    g_step = ""; g_ret = 0;
    mbedtls_pk_init(&ca_key);

    if ((rc = tls_cert_validity_window(lifetime_seconds,
                                       nb, sizeof nb, na, sizeof na)) != TLS_CERT_OK) {
        goto out;
    }

    rc = TLS_CERT_E_MBEDTLS;
    if (mbedtls_pk_setup(&ca_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ca_key),
                            sls_mbedtls_rng, NULL) != 0) {
        g_step = "ca_keygen"; goto out;
    }

    /* Self-signed, CA:TRUE, pathlen 0, no SAN. It never terminates a
     * connection, so it has no names to assert. */
    mbedtls_x509write_crt_init(&crt);
    rc = write_one(&crt, ca_dn, ca_dn, &ca_key, &ca_key, nb, na, 1, NULL, 0,
                   ca_der, ca_size, ca_len);
    mbedtls_x509write_crt_free(&crt);
    if (rc != TLS_CERT_OK) { goto out; }

    ret = mbedtls_pk_write_key_der(&ca_key, ca_key_der, ck_size);
    if (ret < 0) {
        g_step = "ca_pk_write_key_der"; g_ret = ret; rc = TLS_CERT_E_MBEDTLS;
        goto out;
    }
    *ck_len = (size_t)ret;
    der_to_front(ca_key_der, ck_size, *ck_len);
    rc = TLS_CERT_OK;

out:
    mbedtls_pk_free(&ca_key);
    /* On failure the caller gets nothing to mistake for a key. On success the
     * key is in ca_key_der and is now the caller's problem -- see §6.1. */
    if (rc != TLS_CERT_OK) {
        secure_zero(ca_key_der, ck_size);
        *ck_len = 0;
    }
    return rc;
}

int tls_cert_seconds_remaining(const unsigned char *der, size_t len,
                               uint64_t *out)
{
    mbedtls_x509_crt ca;
    uint64_t now = 0, expiry = 0;
    int rc;

    if (!der || len == 0 || !out) { return TLS_CERT_E_BADARG; }
    if (rtc_get_unix(&now) != RTC_OK) { return TLS_CERT_E_NO_TIME; }

    mbedtls_x509_crt_init(&ca);
    rc = TLS_CERT_E_MBEDTLS;
    if (mbedtls_x509_crt_parse_der(&ca, der, len) != 0) {
        g_step = "remaining_parse"; goto out;
    }
    /* The forward direction of the calendar, from rtc.c -- the same file that
     * owns the inverse used to write the string in the first place. */
    if (rtc_compose((unsigned)ca.valid_to.year, (unsigned)ca.valid_to.mon,
                    (unsigned)ca.valid_to.day,  (unsigned)ca.valid_to.hour,
                    (unsigned)ca.valid_to.min,  (unsigned)ca.valid_to.sec,
                    &expiry) != RTC_OK) {
        rc = TLS_CERT_E_RANGE; goto out;
    }
    *out = (expiry > now) ? (expiry - now) : 0;
    rc = TLS_CERT_OK;

out:
    mbedtls_x509_crt_free(&ca);
    return rc;
}

int tls_cert_sign_leaf(const unsigned char *ca_der, size_t ca_len,
                       const unsigned char *ca_key_der, size_t ca_key_len,
                       const char *ca_dn, const char *leaf_dn,
                       const struct tls_cert_san *sans, size_t san_count,
                       uint64_t lifetime_seconds,
                       unsigned char *leaf_der, size_t leaf_size, size_t *leaf_len,
                       unsigned char *leaf_key_der, size_t lk_size, size_t *lk_len)
{
    mbedtls_pk_context ca_key, leaf_key;
    mbedtls_x509_crt   ca, check;
    mbedtls_x509write_cert crt;
    char nb[TLS_CERT_TIME_BUF], na[TLS_CERT_TIME_BUF];
    char ca_nb[TLS_CERT_TIME_BUF], ca_na[TLS_CERT_TIME_BUF];
    uint64_t now = 0;
    int rc, ret;

    if (!ca_der || ca_len == 0 || !ca_key_der || ca_key_len == 0 ||
        !ca_dn || !leaf_dn || !leaf_der || !leaf_len ||
        !leaf_key_der || !lk_len) {
        return TLS_CERT_E_BADARG;
    }
    if (!sans || san_count == 0 || san_count > TLS_CERT_MAX_SANS) {
        return TLS_CERT_E_BADARG;
    }
    if (str_eq(ca_dn, leaf_dn)) { return TLS_CERT_E_BADARG; }
    if (!has_eq(ca_dn) || !has_eq(leaf_dn)) { return TLS_CERT_E_BADARG; }
    /* Before the clock read below, and before any parsing, for the reason
     * window_args() gives: a bad argument must not be reported as a missing
     * clock or an unparseable CA. */
    if ((rc = window_args(lifetime_seconds, nb, sizeof nb,
                          na, sizeof na)) != TLS_CERT_OK) {
        return rc;
    }

    g_step = ""; g_ret = 0;
    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&leaf_key);
    mbedtls_x509_crt_init(&ca);
    mbedtls_x509_crt_init(&check);

    rc = TLS_CERT_E_MBEDTLS;
    if (mbedtls_x509_crt_parse_der(&ca, ca_der, ca_len) != 0) {
        g_step = "parse_ca"; goto out;
    }
    if (mbedtls_pk_parse_key(&ca_key, ca_key_der, ca_key_len, NULL, 0,
                             sls_mbedtls_rng, NULL) != 0) {
        g_step = "parse_ca_key"; goto out;
    }

    /* The stored certificate and the stored key must actually be a pair.
     *
     * This is the load-bearing check on the persistence path and it replaces
     * the CRC that frame would otherwise carry -- it is strictly stronger.
     * A torn 4 KiB write, a half-updated frame, a key from a different CA:
     * every one of them ends here rather than in a leaf whose signature does
     * not verify. Firefox reports that as SEC_ERROR_BAD_SIGNATURE, which is
     * exactly right and says nothing whatsoever about which of the two blobs
     * on disk was wrong. */
    if (mbedtls_pk_check_pair(&ca.pk, &ca_key, sls_mbedtls_rng, NULL) != 0) {
        g_step = "ca_check_pair"; rc = TLS_CERT_E_CA_MISMATCH; goto out;
    }

    /* One clock read, shared by the window and the comparisons against the
     * CA's own window below. */
    if (rtc_get_unix(&now) != RTC_OK) { rc = TLS_CERT_E_NO_TIME; goto out; }
    if ((rc = window_from(now, lifetime_seconds,
                          nb, sizeof nb, na, sizeof na)) != TLS_CERT_OK) {
        goto out;
    }
    if ((rc = x509_time_str(&ca.valid_from, ca_nb)) != TLS_CERT_OK ||
        (rc = x509_time_str(&ca.valid_to,   ca_na)) != TLS_CERT_OK) {
        g_step = "ca_validity_unreadable"; goto out;
    }

    /* Clamp the leaf INTO the issuer's window, both ends.
     *
     * notAfter is the one that matters in practice: the CA is generated once
     * and the leaf every boot, so without this the leaf's expiry marches past
     * its issuer's and the chain becomes unverifiable at a moment nothing in
     * the logs explains.
     *
     * notBefore is clamped UP for a narrower case that is still real: an RTC
     * that has moved backwards. Without it the leaf claims validity from
     * before its issuer existed. Clamping up can put notBefore slightly in
     * the future as this node sees it -- which is correct, because the
     * verifier's clock is the one being satisfied and the CA's notBefore is
     * genuinely in its past. */
    if (time_cmp(na, ca_na) > 0) {
        for (size_t i = 0; i < TLS_CERT_TIME_BUF; i++) { na[i] = ca_na[i]; }
    }
    if (time_cmp(nb, ca_nb) < 0) {
        for (size_t i = 0; i < TLS_CERT_TIME_BUF; i++) { nb[i] = ca_nb[i]; }
    }
    if (time_cmp(nb, na) >= 0) {
        g_step = "ca_window_empty"; rc = TLS_CERT_E_CA_EXPIRED; goto out;
    }

    rc = TLS_CERT_E_MBEDTLS;
    if (mbedtls_pk_setup(&leaf_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(leaf_key),
                            sls_mbedtls_rng, NULL) != 0) {
        g_step = "leaf_keygen"; goto out;
    }

    mbedtls_x509write_crt_init(&crt);
    rc = write_one(&crt, leaf_dn, ca_dn, &leaf_key, &ca_key, nb, na, 0,
                   sans, san_count, leaf_der, leaf_size, leaf_len);
    mbedtls_x509write_crt_free(&crt);
    if (rc != TLS_CERT_OK) { goto out; }

    /* Parse the finished leaf back and check that its issuer field is the
     * CA's subject field BYTE FOR BYTE.
     *
     * `ca_dn` is a string that gets re-encoded into DER by
     * mbedtls_x509_string_to_names(). Two things can make that encoding differ
     * from the one inside the stored CA: a different string, or the same
     * string encoded by a different mbedTLS version. Either produces a leaf
     * that is internally valid, signed by the right key, and which no path
     * builder will join to its issuer -- because path building matches issuer
     * to subject on encoded bytes, not on meaning.
     *
     * The future this is actually for: someone edits the DN constant in a
     * later build while a CA made with the old one is still on disk. That is
     * a one-line change with no visible connection to certificates, and this
     * turns it into a named error at boot. tls_server_init() answers it by
     * discarding the stored CA, which is the correct repair. */
    if (mbedtls_x509_crt_parse_der(&check, leaf_der, *leaf_len) != 0) {
        g_step = "leaf_reparse"; rc = TLS_CERT_E_MBEDTLS; goto out;
    }
    if (check.issuer_raw.len != ca.subject_raw.len) {
        g_step = "issuer_len_mismatch"; rc = TLS_CERT_E_CA_MISMATCH; goto out;
    }
    for (size_t i = 0; i < check.issuer_raw.len; i++) {
        if (check.issuer_raw.p[i] != ca.subject_raw.p[i]) {
            g_step = "issuer_bytes_mismatch"; rc = TLS_CERT_E_CA_MISMATCH; goto out;
        }
    }

    ret = mbedtls_pk_write_key_der(&leaf_key, leaf_key_der, lk_size);
    if (ret < 0) {
        g_step = "pk_write_key_der"; g_ret = ret; rc = TLS_CERT_E_MBEDTLS; goto out;
    }
    *lk_len = (size_t)ret;
    der_to_front(leaf_key_der, lk_size, *lk_len);
    rc = TLS_CERT_OK;

out:
    mbedtls_x509_crt_free(&check);
    mbedtls_x509_crt_free(&ca);
    mbedtls_pk_free(&ca_key);
    mbedtls_pk_free(&leaf_key);
    if (rc != TLS_CERT_OK) {
        secure_zero(leaf_key_der, lk_size);
        *lk_len = 0;
        *leaf_len = 0;
    }
    return rc;
}

int tls_cert_chain(const char *ca_dn, const char *leaf_dn,
                   const struct tls_cert_san *sans, size_t san_count,
                   uint64_t lifetime_seconds,
                   unsigned char *ca_der, size_t ca_size, size_t *ca_len,
                   unsigned char *leaf_der, size_t leaf_size, size_t *leaf_len,
                   unsigned char *leaf_key_der, size_t lk_size, size_t *lk_len)
{
    /* Now a composition of the two halves rather than a third implementation.
     * That matters beyond tidiness: tests/tls_cert_oracle.c drives this
     * function, so writing it in terms of make_ca() and sign_leaf() is what
     * keeps the oracle testing the code the kernel actually runs. A separate
     * one-shot path would have been an oracle for a function nothing calls. */
    unsigned char ca_key_der[512];
    size_t ca_key_len = 0;
    int rc;

    if (!ca_dn || !leaf_dn) { return TLS_CERT_E_BADARG; }
    if (str_eq(ca_dn, leaf_dn)) { return TLS_CERT_E_BADARG; }

    rc = tls_cert_make_ca(ca_dn, lifetime_seconds,
                          ca_der, ca_size, ca_len,
                          ca_key_der, sizeof ca_key_der, &ca_key_len);
    if (rc != TLS_CERT_OK) { goto out; }

    rc = tls_cert_sign_leaf(ca_der, *ca_len, ca_key_der, ca_key_len,
                            ca_dn, leaf_dn, sans, san_count, lifetime_seconds,
                            leaf_der, leaf_size, leaf_len,
                            leaf_key_der, lk_size, lk_len);

out:
    /* The CA key dies here, on every path, exactly as it did before -- this
     * function's contract is unchanged and a caller that uses it cannot
     * persist an anchor. tls_server_init() no longer calls it for that reason.
     */
    secure_zero(ca_key_der, sizeof ca_key_der);
    return rc;
}
#endif /* TLS_CERT_HOST_TEST */
