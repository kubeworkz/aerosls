#ifndef SLS_TLS_CERT_H
#define SLS_TLS_CERT_H

/* tls_cert.h — self-signed certificate generation for Phase 3.
 *
 * Currently this is the validity half only. Key generation, the SAN and the
 * serial follow; see docs/AeroSLS-TLS-Design-v0.1.md, "The three, settled".
 */

#include <stdint.h>
#include <stddef.h>

#define TLS_CERT_OK           0
#define TLS_CERT_E_BADARG   (-1)
#define TLS_CERT_E_NO_TIME  (-2)  /* rtc_get_unix() declined -- no trusted clock */
#define TLS_CERT_E_RANGE    (-3)  /* year not four digits, or clock too early to backdate */

/* mbedtls_x509write_crt_set_validity() requires strlen(s) == exactly
 * MBEDTLS_X509_RFC5280_UTC_TIME_LEN - 1, which is 14 in the vendored 3.6.7
 * tree (x509_crt.h:140), and returns MBEDTLS_ERR_X509_BAD_INPUT_DATA for any
 * other length. It appends the 'Z' itself. So: 14 characters, no 'Z', and a
 * 15-byte buffer to hold them plus the NUL. */
#define TLS_CERT_TIME_LEN   14
#define TLS_CERT_TIME_BUF   (TLS_CERT_TIME_LEN + 1)

/* Backdating notBefore. Not one second -- see the design doc. The risk that
 * actually bites is our own CMOS RTC being read as UTC on a machine whose
 * firmware holds local time, which is a whole timezone, and which is invisible
 * in QEMU where host and guest agree. A day covers it and costs nothing on a
 * certificate we can reissue. */
#define TLS_CERT_BACKDATE_SECONDS  (24ULL * 60ULL * 60ULL)

/* Format a Unix timestamp as the exactly-14-character UTC string described
 * above. Returns TLS_CERT_OK, or negative with `out` UNTOUCHED -- the same
 * contract as entropy_get() and rtc_get_unix(), and for the same reason.
 *
 * Pure: no clock read, no allocation, no I/O. That is deliberate, so the host
 * test can drive it across dates no machine here will ever hold. */
int tls_cert_format_time(uint64_t unix_seconds, char *out, size_t out_size);

/* Fill a notBefore/notAfter pair around the current trusted time, backdated by
 * TLS_CERT_BACKDATE_SECONDS. Returns TLS_CERT_E_NO_TIME and touches neither
 * buffer when there is no trusted clock: a node that cannot say when it is
 * must not issue a certificate claiming to know. */
int tls_cert_validity_window(uint64_t lifetime_seconds,
                             char *not_before, size_t nb_size,
                             char *not_after, size_t na_size);

#endif /* SLS_TLS_CERT_H */
