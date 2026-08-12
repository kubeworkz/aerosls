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
