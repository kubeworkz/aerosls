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

int tls_cert_validity_window(uint64_t lifetime_seconds,
                             char *not_before, size_t nb_size,
                             char *not_after, size_t na_size)
{
    uint64_t now = 0, from = 0, to = 0;
    char nb[TLS_CERT_TIME_BUF], na[TLS_CERT_TIME_BUF];
    int rc;

    if (!not_before || !not_after ||
        nb_size < TLS_CERT_TIME_BUF || na_size < TLS_CERT_TIME_BUF) {
        return TLS_CERT_E_BADARG;
    }

    if (lifetime_seconds == 0 || lifetime_seconds > ((uint64_t)1 << 40)) {
        return TLS_CERT_E_BADARG;
    }

    /* Fail closed. §1.1: no trusted time means decline, never assume the epoch
     * and never fall back to boot time. This is the first real consumer of that
     * rule outside rtc.c itself. */
    if (rtc_get_unix(&now) != RTC_OK) {
        return TLS_CERT_E_NO_TIME;
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
    {
        const char *p = dn; int has_eq = 0;
        while (*p) { if (*p == '=') { has_eq = 1; break; } p++; }
        if (!has_eq) { return TLS_CERT_E_BADARG; }
    }
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

#endif /* TLS_CERT_HOST_TEST */
