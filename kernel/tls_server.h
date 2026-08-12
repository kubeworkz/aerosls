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

/* ─── The concurrent-session cap, now measured ─────────────────────────────
 * Was 2, derived from §3.2's estimate. It refused 964 connections on its first
 * real browser run, which measured the symptom and not the cause: refusals
 * cannot distinguish a cap one too low from ten too low, and they inflate
 * themselves because every refusal makes a browser retry.
 *
 * MEASURED, after both browsers loaded the Navigator:
 *   tls_pool_peak 99416 bytes at tls_sessions_peak 2  ->  ~49.7 KB/session
 *
 * 6 is what a browser opens per host, so it is the number that stops the
 * refusals rather than merely reducing them. 6 x 49.7 KB is ~298 KB, which is
 * why SLS_TLS_POOL_BYTES went to 512 KiB in the same change -- raising this
 * alone would have moved the failure from a clean refusal at the cap to a
 * pool allocation failing mid-handshake, which is exactly what the original
 * comment said must not happen.
 *
 * TCP_MAX_CONNS is still 512 and still has no bearing. The limit is the pool.
 *
 * Read tls_server_stats() / GET /api/health after changing either number.
 * That is not advice, it is how these two were arrived at. */
#define TLS_SERVER_MAX_SESSIONS 6

/* ─── Certificate lifetimes ────────────────────────────────────────────────
 * Three numbers, and the relationship between them matters more than any one
 * of them does.
 *
 * The CA is what an operator imports by hand into Chrome's store and again
 * into Firefox's. Its expiry is the ONLY thing that makes them do it again, so
 * it is long. Five years, not ten: the key sits in plaintext on an unencrypted
 * disk (tls_store.h says why), and an open-ended commitment to a secret in
 * that position is not one worth making.
 *
 * The leaf is regenerated with a fresh key on every boot, so its lifetime is
 * not about key exposure -- it is about UPTIME. Nothing re-issues the leaf
 * while the node is running, so a lifetime shorter than the longest expected
 * uptime would expire a certificate underneath a node that is serving happily,
 * with no log line at the moment it happens and browser errors afterwards. A
 * year is comfortably beyond any uptime this has seen. It is a bound, not a
 * solution: in-flight re-issue is the real fix and is not written yet.
 *
 * The renewal margin is deliberately EQUAL to the leaf lifetime rather than
 * being a fourth arbitrary number. It states the actual rule -- replace the CA
 * when it can no longer issue a full-length leaf -- so the two cannot drift
 * apart, and it means a leaf is never silently short-changed by
 * tls_cert_sign_leaf()'s clamp into a nearly-expired issuer's window. */
#define TLS_SERVER_CA_SECONDS      (5ULL * 365ULL * 24ULL * 60ULL * 60ULL)
#define TLS_SERVER_LEAF_SECONDS    (365ULL * 24ULL * 60ULL * 60ULL)
#define TLS_SERVER_CA_RENEW_SECONDS TLS_SERVER_LEAF_SECONDS

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
/* `live_peak` is the high-water mark since boot; `live` is how many slots are
 * occupied RIGHT NOW. Both, because they answer different questions and the
 * peak alone cannot answer either. A node reporting peak=6 with a stream of
 * refusals is EITHER saturated this second OR leaked its slots an hour ago,
 * and those need opposite responses -- wait, versus find the leak. Reading a
 * peak and inferring the present is the same mistake as counting refusals with
 * no denominator, which is what TLS_SERVER_MAX_SESSIONS above was raised to
 * fix. Peak said 6 and refusals said 1295 on a node whose slots were merely
 * busy; nothing in that pair distinguished it from a leak. */
void tls_server_stats(unsigned long *refused, unsigned *live_peak, unsigned *live,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks);

#endif /* SLS_TLS_SERVER_H */
