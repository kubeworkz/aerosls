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

/* How often the renewal check bothers to read the clock, in TIMER TICKS.
 *
 * Ticks and only ticks, for the gate -- and never for the decision. The LAPIC
 * timer is programmed for ~100 Hz but kernel/timer.c says outright that "exact
 * rate is calibration-dependent", so a tick is not a unit of time this code
 * may reason about. It is a cheap monotonic counter, which is precisely what a
 * "should I bother looking?" gate needs and precisely what a "has it expired?"
 * decision must not use. The decision below reads rtc_get_unix().
 *
 * At the nominal rate this is about an hour. If the calibration is off by 3x
 * in either direction it becomes 20 minutes or 3 hours, and nothing cares --
 * which is the test for whether a tick-derived number is being used for the
 * right kind of thing. */
#define TLS_SERVER_RENEW_CHECK_TICKS  360000ULL

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
/* ─── In-flight leaf renewal ───────────────────────────────────────────────
 * Call from the server loop; it is cheap to call every sweep and does real
 * work at most once every TLS_SERVER_RENEW_CHECK_TICKS.
 *
 * Nothing re-issued the leaf while a node was running, so its lifetime was a
 * bet that no node runs longer than a year. A lost bet expires a certificate
 * underneath a server that is otherwise healthy, with no log line at the
 * moment it happens and the first symptom appearing in somebody's browser.
 *
 * Three things make this safe rather than clever:
 *
 *   The CA key is NOT held in memory for this. It is re-read from the store,
 *   used, and zeroized -- so the key's residency in RAM goes from "the whole
 *   uptime" to "milliseconds, once a year". A node with no stored CA
 *   therefore cannot renew at all, which is reported rather than discovered.
 *
 *   The new leaf is generated and validated BEFORE anything is torn down. A
 *   renewal that fails leaves the working certificate in place.
 *
 *   The swap only happens with ZERO live sessions. mbedtls_ssl_conf_own_cert()
 *   appends rather than replaces and 3.6 exposes no way to clear the list, so
 *   replacing the certificate means freeing and rebuilding the whole
 *   mbedtls_ssl_config -- which every live mbedtls_ssl_context points at. With
 *   a third of the leaf's life as the window there is no urgency worth a
 *   use-after-free.
 *
 * Returns TLS_SRV_OK when a renewal actually happened, and TLS_SRV_E_NOT_READY
 * otherwise -- including the ordinary "nothing to do", which is almost every
 * call. Callers are not expected to check it; the log and /api/health are how
 * this reports. */
int tls_server_maybe_renew(void);

/* Leaf renewal state, for /api/health.
 *   renew_at   Unix second the leaf becomes due for replacement, 0 if unknown.
 *   renewals   completed in-flight renewals since boot.
 *   deferrals  times a due renewal was postponed because sessions were live.
 *   renewable  1 if a renewal is possible at all. ZERO IS THE INTERESTING ONE:
 *              it means no CA is stored, so this node's leaf will expire and
 *              nothing will replace it. An operator has a year to see that,
 *              and only if something shows it to them. */
void tls_server_renewal_status(uint64_t *renew_at, unsigned long *renewals,
                               unsigned long *deferrals, int *renewable);

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
