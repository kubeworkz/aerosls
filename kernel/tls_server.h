#ifndef SLS_TLS_SERVER_H
#define SLS_TLS_SERVER_H

/* tls_server.h — server-side TLS 1.3 sessions over the existing TCP bridge.
 *
 * Phase 3. The gate for this file is not a test in this repo: it is Chrome,
 * Firefox and curl completing a handshake and fetching the Navigator.
 */

#include <stdint.h>
#include <stddef.h>

#include "tls_cert.h"   /* struct tls_cert_san */

#define TLS_SRV_OK             0
#define TLS_SRV_HANDSHAKING    1   /* not an error: call again next poll */
#define TLS_SRV_E_NOT_READY  (-1)  /* tls_server_init() has not succeeded */
#define TLS_SRV_E_NO_SLOT    (-2)  /* at the concurrent-session cap */
#define TLS_SRV_E_BADARG     (-3)
#define TLS_SRV_E_FATAL      (-4)  /* session is dead; close it */

/* ─── The concurrent-session cap, and why it is this small ─────────────────
 * §3.2 of the design doc: TLS 1.3 records are up to 16 KB, and a server
 * context with full buffers is ~32-35 KB of I/O buffer per connection plus
 * ~25 KB of handshake context. MBEDTLS_SSL_MAX_FRAGMENT_LENGTH and
 * MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH are both unsupported under TLS 1.3, so
 * that is not tunable down -- the buffer is fixed at full record size.
 *
 * The pool is SLS_TLS_POOL_BYTES = 256 KiB, fixed, deliberately separate from
 * the database's arena. Two sessions is ~120 KB of that before fragmentation.
 *
 * TCP_MAX_CONNS is 512. Nothing about TCP's capacity has any bearing here, and
 * that mismatch is the entire reason this constant exists: the doc's
 * instruction is that the connection limit "must be enforced rather than
 * discovered", because discovering it means a pool allocation failing
 * mid-handshake under load.
 *
 * Raise this only against a measurement of real pool high-water under real
 * handshakes -- not against this arithmetic, which is an estimate carried over
 * from a document that has been wrong about magnitudes before. */
#define TLS_SERVER_MAX_SESSIONS 2

/* Generate a self-signed certificate and build the shared server config.
 * Safe to call more than once; the second call is a no-op. Returns TLS_SRV_OK
 * or negative -- and on failure TLS must not start, which is §2.4's
 * fail-closed rule reaching its last consumer. */
int tls_server_init(const char *ca_dn, const char *dn,
                    const struct tls_cert_san *sans, size_t san_count);

/* Non-zero once init has succeeded. */
int tls_server_ready(void);

/* Claim a session slot for a TCP connection id. TLS_SRV_E_NO_SLOT when the cap
 * is reached -- the caller must close the connection rather than fall back to
 * plaintext, which would be a downgrade a client cannot see. */
int tls_server_open(int conn_id);

/* Drive the handshake one step. TLS_SRV_HANDSHAKING means "no progress
 * possible right now, call again"; that is the cooperative contract the BIO
 * bridge was written for, and it is what keeps one silent client from halting
 * the single poll loop that also drives the serial console. */
int tls_server_handshake(int conn_id);

/* Application data, once the handshake is complete. Both return the byte
 * count, 0 for "nothing right now", or negative on a dead session. */
int tls_server_read(int conn_id, unsigned char *buf, size_t len);
int tls_server_write(int conn_id, const unsigned char *buf, size_t len);

/* Release the slot. Sends close_notify if the session got that far. */
void tls_server_close(int conn_id);

/* Certificate DER, for operators and for anything that wants to inspect what
 * this node actually presents. NULL before init succeeds. */
const unsigned char *tls_server_cert_der(size_t *len);

/* The CA certificate. THIS is the one an operator imports into a trust store;
 * importing the leaf achieves nothing. NULL before init succeeds. */
const unsigned char *tls_server_ca_der(size_t *len);

/* Refusals since boot, peak concurrent sessions, and the fixed pool's size and
 * peak usage. TLS_SERVER_MAX_SESSIONS above says to raise it only against
 * measured pool high-water; this is that measurement. Any argument may be
 * NULL. */
void tls_server_stats(unsigned long *refused, unsigned *live_peak,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks);

#endif /* SLS_TLS_SERVER_H */
