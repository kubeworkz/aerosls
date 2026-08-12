/* tls_server.c — server-side TLS 1.3, Phase 3.
 *
 * The pieces this joins all existed and none of them were called: the
 * certificate generator (tls_cert.c), the RNG hook and the cooperative BIO
 * bridge (tls_platform.c). This is the file that makes them a listener.
 *
 * ─── The shape, and why it is a poll step rather than a call ──────────────
 * net/http.c is ONE loop that also drives the serial console. A blocking
 * handshake would hand any client a trivial denial of service: connect, send
 * one byte of a ClientHello, and the node stops -- not just TLS, everything.
 * tls_platform.c's BIO already returns MBEDTLS_ERR_SSL_WANT_READ instead of
 * waiting, so mbedtls_ssl_handshake() returns rather than blocks, and this
 * file turns that into TLS_SRV_HANDSHAKING for the loop to come back to.
 *
 * ─── What is NOT proven by this file compiling ────────────────────────────
 * Nothing about a handshake. mbedTLS's own state machine, the TLS 1.3 record
 * layer and the certificate all get exercised for the first time by a real
 * client. That is the point of the Phase 3 gate being a browser: Chrome,
 * Firefox and curl are three independent implementations that share no
 * ancestor closer than SSLeay, and none of them is inclined to be generous.
 * Until one of them completes a handshake, this file is untested code that
 * links.
 */

#include "tls_server.h"
#include "tls_cert.h"
#include "tls_store.h"
#include "tls_platform.h"
#include "entropy.h"
#include "rtc.h"

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"

void kernel_serial_print(const char* s);
void kernel_serial_printf(const char* fmt, ...);

int sls_tls_bio_send(void *ctx, const unsigned char *buf, size_t len);
int sls_tls_bio_recv(void *ctx, unsigned char *buf, size_t len);

/* ─── Shared, built once ───────────────────────────────────────────────────
 * One config and one certificate for every session. mbedTLS is explicit that
 * a config may be shared across contexts, and the alternative -- a
 * certificate per connection -- would mean generating a P-256 key per
 * handshake, which is both slow and a fresh draw on the entropy pool for no
 * security benefit. */
static mbedtls_ssl_config   g_conf;
static mbedtls_x509_crt     g_crt;
static mbedtls_pk_context   g_key;
static int                  g_ready;

/* Kept so an operator (and tests/tls_cert_oracle_smoke.sh's sibling on a live
 * node) can see exactly what is being presented. 2 KiB is generous for a
 * P-256 self-signed leaf, which measured 430 bytes. */
static unsigned char g_crt_der[2048];      /* the leaf: what the node presents */
static size_t        g_crt_der_len;
static unsigned char g_ca_der[2048];       /* the CA: what an operator imports */
static size_t        g_ca_der_len;

struct tls_session {
    int                 in_use;
    int                 conn_id;
    int                 established;
    mbedtls_ssl_context ssl;
};
static struct tls_session g_sessions[TLS_SERVER_MAX_SESSIONS];

/* Refusals since boot, and peak concurrent sessions. Counted because 964
 * "no session slot" lines in a log is a symptom with no denominator: it does
 * not say whether the cap is one too low or ten too low, and grep cannot tell
 * a browser opening six parallel connections from a retry storm caused by the
 * refusals themselves. */
static unsigned long g_refused;
static unsigned      g_live;
static unsigned      g_live_peak;

static struct tls_session *find(int conn_id)
{
    for (int i = 0; i < TLS_SERVER_MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use && g_sessions[i].conn_id == conn_id) {
            return &g_sessions[i];
        }
    }
    return 0;
}

int tls_server_ready(void) { return g_ready != 0; }

const unsigned char *tls_server_cert_der(size_t *len)
{
    if (!g_ready) { return 0; }
    if (len) { *len = g_crt_der_len; }
    return g_crt_der;
}

const unsigned char *tls_server_ca_der(size_t *len)
{
    if (!g_ready) { return 0; }
    if (len) { *len = g_ca_der_len; }
    return g_ca_der;
}

int tls_server_init(const char *ca_dn, const char *dn,
                    const struct tls_cert_san *sans, size_t san_count)
{
    unsigned char key_der[512];
    unsigned char ca_key_der[512];
    size_t key_len = 0, ca_key_len = 0;
    int have_ca = 0;
    int rc;

    if (g_ready) { return TLS_SRV_OK; }
    if (!dn || !ca_dn) { return TLS_SRV_E_BADARG; }

    /* Fail closed, and say which prerequisite is missing rather than making
     * an operator bisect it. Both of these are real states on real hardware:
     * a Pi has no RTC at all, and a node whose entropy sources all failed
     * health tests must serve nothing rather than serve something insecure. */
    if (!rtc_is_trusted()) {
        kernel_serial_print("[TLS] no trusted wall clock -- refusing to issue a "
                            "certificate. TLS will not start.\n");
        return TLS_SRV_E_NOT_READY;
    }

    /* ─── The CA: from disk if it is there and usable, otherwise new ───────
     *
     * Two certificates, not one. A single self-signed certificate cannot
     * satisfy Chrome and Firefox at once -- see tls_cert.h.
     *
     * The CA is loaded rather than generated because an anchor that changes
     * every boot has to be re-imported every boot, in two trust stores, by
     * hand. The leaf is still generated every boot, with a fresh key. See
     * tls_store.h for what keeping the CA key on disk costs.
     *
     * Everything that can be wrong with a stored CA -- absent, written by an
     * older format, torn, paired with the wrong key, made with a DN this build
     * no longer uses, or simply expired -- converges on ONE repair: throw it
     * away and make a new one. The operator re-imports once. That is why the
     * cases below differ in what they LOG and not in what they do: a node that
     * silently serves nothing because its stored CA was unreadable is worse
     * than one that costs an import. */
    rc = tls_store_load(g_ca_der, sizeof g_ca_der, &g_ca_der_len,
                        ca_key_der, sizeof ca_key_der, &ca_key_len);
    if (rc == TLS_STORE_OK) {
        uint64_t left = 0;
        if (tls_cert_ca_seconds_remaining(g_ca_der, g_ca_der_len, &left)
                != TLS_CERT_OK) {
            kernel_serial_print("[TLS] stored CA has an unreadable validity "
                                "window -- replacing it.\n");
            have_ca = 0;
        } else if (left < TLS_SERVER_CA_RENEW_SECONDS) {
            /* Replaced on a schedule rather than at the moment it stops
             * working, so the re-import happens when someone is looking at it
             * and not in the middle of an outage. */
            kernel_serial_printf("[TLS] stored CA expires in %u day(s) -- "
                                 "replacing it now rather than mid-flight. "
                                 "Re-import the CA.\n",
                                 (unsigned)(left / (24ULL * 60ULL * 60ULL)));
            have_ca = 0;
        } else {
            have_ca = 1;
            kernel_serial_printf("[TLS] CA loaded from disk, %u day(s) left. "
                                 "An existing import is still good.\n",
                                 (unsigned)(left / (24ULL * 60ULL * 60ULL)));
        }
    } else if (rc == TLS_STORE_E_EMPTY) {
        kernel_serial_print("[TLS] no stored CA -- first boot on this disk.\n");
    } else if (rc == TLS_STORE_E_NO_NVME) {
        /* Not fatal, and worth being loud about: TLS still works, but this
         * boot's anchor dies with it and the operator will be re-importing
         * again next time without being told why. */
        kernel_serial_print("[TLS] no NVMe -- the CA cannot be stored and will "
                            "not survive this boot.\n");
    } else {
        kernel_serial_printf("[TLS] stored CA unusable (rc=%d) -- replacing "
                             "it. Re-import the CA.\n", rc);
    }

    if (have_ca) {
        rc = tls_cert_sign_leaf(g_ca_der, g_ca_der_len, ca_key_der, ca_key_len,
                                ca_dn, dn, sans, san_count,
                                TLS_SERVER_LEAF_SECONDS,
                                g_crt_der, sizeof g_crt_der, &g_crt_der_len,
                                key_der, sizeof key_der, &key_len);
        if (rc != TLS_CERT_OK) {
            /* The stored CA parsed and had time left, and still could not sign.
             * TLS_CERT_E_CA_MISMATCH here is the DN-changed-between-builds case
             * tls_cert.h describes; anything else is a stored key that is not
             * what it claims. Either way the frame is now known-bad, so wipe it
             * rather than leave a private key on disk that nothing will ever
             * use again. */
            kernel_serial_printf("[TLS] stored CA could not sign a leaf: rc=%d "
                                 "step=%s -- discarding it.\n",
                                 rc, tls_cert_last_step());
            tls_store_wipe();
            have_ca = 0;
        }
    }

    if (!have_ca) {
        mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
        ca_key_len = 0;
        rc = tls_cert_make_ca(ca_dn, TLS_SERVER_CA_SECONDS,
                              g_ca_der, sizeof g_ca_der, &g_ca_der_len,
                              ca_key_der, sizeof ca_key_der, &ca_key_len);
        if (rc == TLS_CERT_OK) {
            rc = tls_cert_sign_leaf(g_ca_der, g_ca_der_len,
                                    ca_key_der, ca_key_len,
                                    ca_dn, dn, sans, san_count,
                                    TLS_SERVER_LEAF_SECONDS,
                                    g_crt_der, sizeof g_crt_der, &g_crt_der_len,
                                    key_der, sizeof key_der, &key_len);
        }
        if (rc != TLS_CERT_OK) {
            kernel_serial_printf("[TLS] certificate generation failed: rc=%d step=%s\n",
                                 rc, tls_cert_last_step());
            mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
            mbedtls_platform_zeroize(key_der, sizeof key_der);
            return TLS_SRV_E_NOT_READY;
        }

        /* Store it only AFTER it has proved it can sign, so a CA that cannot
         * be used never becomes the CA that gets loaded next boot. A failure
         * here is not fatal -- the node serves fine this uptime -- but it does
         * mean another re-import, so it is said out loud. */
        if (tls_store_save(g_ca_der, g_ca_der_len,
                           ca_key_der, ca_key_len) != TLS_STORE_OK) {
            kernel_serial_print("[TLS] new CA could NOT be stored -- it will "
                                "not survive a reboot.\n");
        } else {
            kernel_serial_print("[TLS] new CA generated and stored. Import it "
                                "once; it now survives reboots.\n");
        }
    }

    /* The CA key's whole life in this file ends on this line. It was needed to
     * sign the leaf and is needed for nothing else: mbedTLS holds the LEAF key
     * for the handshake, and the copy on disk is where the next boot gets it
     * from. Anything below this point that wanted the CA key would be a bug,
     * and there is nothing left for it to read. */
    mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
    ca_key_len = 0;

    mbedtls_x509_crt_init(&g_crt);
    mbedtls_pk_init(&g_key);
    mbedtls_ssl_config_init(&g_conf);

    /* Both, in order: leaf first, then the CA that signed it. mbedTLS sends
     * the whole chain, and including a self-signed root is redundant for a
     * client that already trusts it -- but it is what lets `openssl s_client
     * -showcerts` hand a verifier the anchor, which is how every diagnostic
     * in tests/ gets one. Browsers ignore the extra. */
    if (mbedtls_x509_crt_parse_der(&g_crt, g_crt_der, g_crt_der_len) != 0) {
        kernel_serial_print("[TLS] our own leaf certificate did not parse back.\n");
        goto fail;
    }
    if (mbedtls_x509_crt_parse_der(&g_crt, g_ca_der, g_ca_der_len) != 0) {
        kernel_serial_print("[TLS] our own CA certificate did not parse back.\n");
        goto fail;
    }
    /* Parsing our own key back rather than keeping the mbedtls_pk_context from
     * generation is deliberate: it means the DER we would persist is the DER
     * we actually serve with, so a mistake in the key encoding surfaces here
     * at boot instead of after §6.1's storage lands. */
    if (mbedtls_pk_parse_key(&g_key, key_der, key_len, 0, 0,
                             sls_mbedtls_rng, 0) != 0) {
        kernel_serial_print("[TLS] our own private key did not parse back.\n");
        goto fail;
    }
    /* The plaintext key leaves this function here and nowhere else. */
    mbedtls_platform_zeroize(key_der, sizeof key_der);

    if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto fail;
    }
    mbedtls_ssl_conf_rng(&g_conf, sls_mbedtls_rng, 0);
    /* Phase 3 is server-side only. Client certificates are Phase 5, and
     * asking for one now would make every browser prompt for a certificate
     * it does not have. */
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    if (mbedtls_ssl_conf_own_cert(&g_conf, &g_crt, &g_key) != 0) {
        goto fail;
    }
    /* Advertise http/1.1 explicitly. curl offered h2 and http/1.1 on the first
     * live handshake and got "server did not agree on a protocol" -- harmless
     * for HTTP/1.1, but it leaves the client to guess, and a client that
     * guesses h2 against a server that only speaks HTTP/1.1 fails in a way
     * that looks like a TLS problem. This kernel has no HTTP/2, so say so.
     *
     * The array must outlive the config: mbedTLS stores the pointer. */
    static const char *const alpn[] = { "http/1.1", 0 };
    if (mbedtls_ssl_conf_alpn_protocols(&g_conf, (const char **)alpn) != 0) {
        goto fail;
    }

    g_ready = 1;
    kernel_serial_printf("[TLS] server ready: TLS 1.3, P-256. "
                         "leaf %u bytes, CA %u bytes.\n",
                         (unsigned)g_crt_der_len, (unsigned)g_ca_der_len);
    kernel_serial_print("[TLS] import the CA (not the leaf) to trust this node.\n");
    return TLS_SRV_OK;

fail:
    mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
    mbedtls_platform_zeroize(key_der, sizeof key_der);
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_pk_free(&g_key);
    mbedtls_x509_crt_free(&g_crt);
    g_crt_der_len = 0;
    g_ca_der_len = 0;
    return TLS_SRV_E_NOT_READY;
}

int tls_server_open(int conn_id)
{
    struct tls_session *s = 0;

    if (!g_ready) { return TLS_SRV_E_NOT_READY; }
    if (conn_id < 0) { return TLS_SRV_E_BADARG; }
    if (find(conn_id)) { return TLS_SRV_OK; }   /* already open: idempotent */

    for (int i = 0; i < TLS_SERVER_MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) { s = &g_sessions[i]; break; }
    }
    /* At the cap. The caller closes the connection. It must NOT serve this
     * client over plaintext instead: a downgrade the client cannot see is
     * worse than a refusal it can. */
    if (!s) { g_refused++; return TLS_SRV_E_NO_SLOT; }

    s->in_use = 1;
    s->conn_id = conn_id;
    s->established = 0;
    mbedtls_ssl_init(&s->ssl);

    if (mbedtls_ssl_setup(&s->ssl, &g_conf) != 0) {
        /* The expected failure here is the 256 KiB pool, which is exactly what
         * TLS_SERVER_MAX_SESSIONS exists to prevent reaching. Say so, because
         * a silent slot leak would look like a client problem. */
        kernel_serial_print("[TLS] ssl_setup failed -- pool exhausted?\n");
        mbedtls_ssl_free(&s->ssl);
        s->in_use = 0;
        return TLS_SRV_E_FATAL;
    }

    /* The connection id travels as the BIO context, the same id http_conns[]
     * and tcp_conns[] are indexed by. */
    mbedtls_ssl_set_bio(&s->ssl, (void *)(long)conn_id,
                        sls_tls_bio_send, sls_tls_bio_recv, 0);
    if (++g_live > g_live_peak) { g_live_peak = g_live; }
    return TLS_SRV_OK;
}

int tls_server_handshake(int conn_id)
{
    struct tls_session *s = find(conn_id);
    int r;

    if (!s) { return TLS_SRV_E_BADARG; }
    if (s->established) { return TLS_SRV_OK; }

    r = mbedtls_ssl_handshake(&s->ssl);
    if (r == 0) {
        s->established = 1;
        {
            size_t used = 0, blocks = 0;
            sls_tls_pool_high_water(&used, &blocks);
            kernel_serial_printf("[TLS] conn %d: handshake complete, %s / %s "
                                 "(pool peak %u/%u bytes, %u live, peak %u, "
                                 "refused %u)\n",
                                 conn_id,
                                 mbedtls_ssl_get_version(&s->ssl),
                                 mbedtls_ssl_get_ciphersuite(&s->ssl),
                                 (unsigned)used, (unsigned)sls_tls_pool_bytes(),
                                 g_live, g_live_peak, (unsigned)g_refused);
        }
        return TLS_SRV_OK;
    }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return TLS_SRV_HANDSHAKING;
    }

    /* §6.4: the audit log may be detailed, the wire may not. mbedTLS has
     * already sent whatever alert it chose; this only records it locally. */
    kernel_serial_printf("[TLS] conn %d: handshake failed, -0x%04x\n",
                         conn_id, (unsigned)(-r));
    return TLS_SRV_E_FATAL;
}

int tls_server_read(int conn_id, unsigned char *buf, size_t len)
{
    struct tls_session *s = find(conn_id);
    int r;

    if (!s || !buf || len == 0) { return TLS_SRV_E_BADARG; }
    if (!s->established) { return TLS_SRV_E_BADARG; }

    r = mbedtls_ssl_read(&s->ssl, buf, len);
    if (r >= 0) { return r; }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;                     /* nothing yet; not an error */
    }
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { return TLS_SRV_E_FATAL; }
    return TLS_SRV_E_FATAL;
}

int tls_server_write(int conn_id, const unsigned char *buf, size_t len)
{
    struct tls_session *s = find(conn_id);
    int r;

    if (!s || !buf || len == 0) { return TLS_SRV_E_BADARG; }
    if (!s->established) { return TLS_SRV_E_BADARG; }

    r = mbedtls_ssl_write(&s->ssl, buf, len);
    if (r >= 0) { return r; }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;                     /* back-pressure; caller retries */
    }
    return TLS_SRV_E_FATAL;
}

void tls_server_close(int conn_id)
{
    struct tls_session *s = find(conn_id);
    if (!s) { return; }

    /* One attempt at close_notify, and only if there is a session to notify.
     * It is best-effort by design: retrying against a peer that has already
     * gone would spin the poll loop for a courtesy. */
    if (s->established) { (void)mbedtls_ssl_close_notify(&s->ssl); }

    mbedtls_ssl_free(&s->ssl);
    s->in_use = 0;
    s->conn_id = -1;
    s->established = 0;
    if (g_live > 0) { g_live--; }
}

/* ─── The numbers TLS_SERVER_MAX_SESSIONS should have been sized against ───
 * pool_peak is the honest per-session cost: peak bytes actually taken from
 * the 256 KiB pool, divided by the peak number of sessions that were live at
 * once. §3.2 estimated 50-60 KB from a document. This measures it.
 *
 * refused is the demand the cap turned away. Both are needed: refusals alone
 * cannot distinguish a cap one too low from one ten too low, and they inflate
 * themselves, because every refusal makes a browser retry. */
void tls_server_stats(unsigned long *refused, unsigned *live_peak,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks);
void tls_server_stats(unsigned long *refused, unsigned *live_peak,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks)
{
    size_t used = 0, blocks = 0;
    sls_tls_pool_high_water(&used, &blocks);
    if (refused)     { *refused = g_refused; }
    if (live_peak)   { *live_peak = g_live_peak; }
    if (pool_bytes)  { *pool_bytes = sls_tls_pool_bytes(); }
    if (pool_peak)   { *pool_peak = used; }
    if (pool_blocks) { *pool_blocks = blocks; }
}
