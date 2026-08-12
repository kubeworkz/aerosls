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


/* RFC 5280 caps the serial at 20 octets and MBEDTLS_X509_RFC5280_MAX_SERIAL_LEN
 * is 20 to match. Note that mbedTLS prepends a 0x00 when the top bit is set, so
 * 20 random bytes can become a 21-octet INTEGER on the wire -- one over the
 * limit set_serial_raw() just enforced. tls_cert_make_serial() shapes the bytes
 * so that cannot happen. */
#define TLS_CERT_SERIAL_LEN 20

/* Fill `out` with a positive, minimally-encoded, non-zero serial from
 * entropy_get(). Returns TLS_CERT_OK, or TLS_CERT_E_NO_ENTROPY with `out`
 * UNTOUCHED -- which is the whole point: entropy_get() does not zero the
 * buffer on failure, so a caller that ignores this return publishes its own
 * uninitialised stack in a certificate. */
#define TLS_CERT_E_NO_ENTROPY (-4)
#define TLS_CERT_E_MBEDTLS    (-5)  /* an mbedTLS call failed; see the log */
int tls_cert_make_serial(unsigned char *out, size_t len);

/* ─── Subject alternative names ────────────────────────────────────────────
 * One entry per name the certificate should be valid for. `dns` non-NULL
 * makes it a dNSName; `dns == NULL` makes it an iPAddress from `ip4`.
 *
 * A list rather than one of each, because a node is reached by more than one
 * name and a verifier checks the name the CLIENT used, not the one the node
 * believes it has. The first live handshake made that concrete: the
 * certificate carried only IP Address:10.0.2.15 -- slirp's guest address --
 * while every client reaches the node as localhost through a QEMU port
 * forward. `curl -k` did not care, because -k skips verification entirely.
 * `curl --cacert` and both browsers would have rejected it, with an error
 * that reads as a certificate fault and is really an addressing one. */
#define TLS_CERT_MAX_SANS 8

struct tls_cert_san {
    const char   *dns;        /* dNSName, or NULL for an iPAddress entry */
    unsigned char ip4[4];     /* used only when dns == NULL */
};

/* Generate an EC P-256 key and a self-signed certificate over it.
 *
 * `dn` is a full distinguished name in mbedTLS's string form -- "CN=node1" or
 * "CN=node1,O=AeroSLS" -- NOT a bare common name. mbedtls_x509write_crt_set_
 * subject_name() parses it with mbedtls_x509_string_to_names(), which needs
 * the attribute prefix; a bare "node1" fails deep inside that parser and
 * surfaces as an opaque error. This function rejects a `dn` with no '=' at
 * the boundary instead, where the message can say which argument was wrong.
 *
 * At least one SAN must be given and at most TLS_CERT_MAX_SANS: Chrome has
 * ignored commonName since Chrome 58, so a certificate whose identity lives
 * only in the CN fails the Phase 3 gate outright.
 *
 * Both outputs are DER. mbedTLS writes DER at the END of the buffer it is
 * given; this function moves it to the front, so crt_der[0..*crt_len) and
 * key_der[0..*key_len) are the whole encodings.
 *
 * The key DER is private key material. It must not be logged, and §6.1's
 * checkpoint question applies to wherever the caller puts it. */
int tls_cert_self_signed(const char *dn,
                         const struct tls_cert_san *sans, size_t san_count,
                         uint64_t lifetime_seconds,
                         unsigned char *crt_der, size_t crt_size, size_t *crt_len,
                         unsigned char *key_der, size_t key_size, size_t *key_len);

/* Which mbedTLS call failed, and its return code, after TLS_CERT_E_MBEDTLS.
 * Exists because working out that a bare CN was the problem took building a
 * separate probe that replayed the whole sequence with the codes printed --
 * a diagnostic round that a two-line accessor removes. Not thread-safe; there
 * is one certificate generator and it runs at provisioning. */
const char *tls_cert_last_step(void);
int tls_cert_last_mbedtls_ret(void);

#endif /* SLS_TLS_CERT_H */
