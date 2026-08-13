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
#include "timer.h"   /* kernel_tick_counter -- the renewal check's cheap gate */

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
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

/* ─── What renewal needs to remember ───────────────────────────────────────
 * The identity arguments, kept as POINTERS. They must outlive this module,
 * which is the same precondition the ALPN array below carries and for the same
 * mbedTLS-adjacent reason. net/http.c's are a `static const` array and string
 * literals, so this holds; a caller passing stack data would be handing this
 * file a dangling pointer to use a year later, when nothing would connect the
 * crash to the call. Stated in tls_server.h as a requirement rather than left
 * to be discovered. */
/* The header carries raw IANA numbers so net/http.c need not see mbedTLS.
 * These are what make them honest: a transposed digit is a build failure here
 * rather than a suite that silently never negotiates. */
_Static_assert(TLS_SERVER_SUITE_AES256 == MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
               "TLS_SERVER_SUITE_AES256 is not TLS_AES_256_GCM_SHA384");
_Static_assert(TLS_SERVER_SUITE_CHACHA == MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
               "TLS_SERVER_SUITE_CHACHA is not TLS_CHACHA20_POLY1305_SHA256");
_Static_assert(TLS_SERVER_SUITE_AES128 == MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
               "TLS_SERVER_SUITE_AES128 is not TLS_AES_128_GCM_SHA256");

static const char *g_ca_dn;
static const char *g_dn;
static const struct tls_cert_san *g_sans;
static size_t      g_san_count;

/* Absolute Unix seconds, not ticks -- see TLS_SERVER_RENEW_CHECK_TICKS. */
static uint64_t      g_leaf_not_after;
static uint64_t      g_leaf_renew_at;
static uint64_t      g_next_check_tick;
static int           g_renewable;
static int           g_unrenewable_warned;
static unsigned long g_renewals;
static unsigned long g_deferrals;

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

static void teardown_tls(void);
static int  install_leaf(const unsigned char *key_der, size_t key_len);
static void note_leaf_window(void);

int tls_server_init(const char *ca_dn, const char *dn,
                    const struct tls_cert_san *sans, size_t san_count)
{
    unsigned char key_der[512];
    unsigned char ca_key_der[512];
    size_t key_len = 0, ca_key_len = 0;
    int have_ca = 0, stored_ok = 0;
    int rc;

    if (g_ready) { return TLS_SRV_OK; }
    if (!dn || !ca_dn) { return TLS_SRV_E_BADARG; }
    if (!sans || san_count == 0) { return TLS_SRV_E_BADARG; }

    /* Kept for renewal. See the note on g_ca_dn: these must outlive the
     * module, and net/http.c's do. */
    g_ca_dn = ca_dn; g_dn = dn; g_sans = sans; g_san_count = san_count;

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
        if (tls_cert_seconds_remaining(g_ca_der, g_ca_der_len, &left)
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
            stored_ok = 1;
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
            stored_ok = 1;
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

    if (install_leaf(key_der, key_len) != 0) { goto fail; }
    /* The plaintext leaf key leaves this function here and nowhere else. */
    mbedtls_platform_zeroize(key_der, sizeof key_der);

    /* Renewal is only possible if the CA can be read back later. A node whose
     * CA lives only in this boot's RAM cannot re-sign anything, and the honest
     * time to say so is now -- a year before it matters -- not when the leaf
     * expires. */
    g_renewable = stored_ok;
    note_leaf_window();

    g_ready = 1;
    kernel_serial_printf("[TLS] server ready: TLS 1.3, P-256. "
                         "leaf %u bytes, CA %u bytes.\n",
                         (unsigned)g_crt_der_len, (unsigned)g_ca_der_len);
    if (!g_renewable) {
        kernel_serial_print("[TLS] this leaf CANNOT be renewed in flight -- no "
                            "CA on disk. It expires when it expires.\n");
    }
    kernel_serial_print("[TLS] import the CA (not the leaf) to trust this node.\n");
    return TLS_SRV_OK;

fail:
    mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
    mbedtls_platform_zeroize(key_der, sizeof key_der);
    teardown_tls();
    g_crt_der_len = 0;
    g_ca_der_len = 0;
    return TLS_SRV_E_NOT_READY;
}

/* ─── The parts renewal reuses ─────────────────────────────────────────────
 * install_leaf() is the second half of what tls_server_init() used to do
 * inline. It is factored out rather than duplicated because a renewal that
 * built its config even slightly differently from boot would be a difference
 * that shows up once a year, in production, on one node. */
static void teardown_tls(void)
{
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_pk_free(&g_key);
    mbedtls_x509_crt_free(&g_crt);
}

static int install_leaf(const unsigned char *key_der, size_t key_len)
{
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

    /* ─── The ciphersuites, decided rather than defaulted ──────────────────
     * Nothing ever called this before, so mbedTLS's built-in preference won
     * and the design doc's "suites we want" table described a choice that was
     * never made. The table was not wrong about what is AVAILABLE; it read as
     * a decision, and there was no decision. This is the decision.
     *
     * Order is server preference: ssl.h states the server picks its own
     * favourite among those the client offers, unless
     * mbedtls_ssl_conf_preference_order() says otherwise, and it is not
     * called. So this list is read top-down.
     *
     *   AES_256_GCM_SHA384 first. Every target this runs on today has
     *   hardware AES -- AESNI on x86-64, AESCE on the ARM64 parts -- so it is
     *   both the fastest and constant-time. It is also what curl, Chrome and
     *   Firefox already negotiated through the Phase 3 gate, which is the
     *   point: pinning changes NOTHING that has been tested. It makes the
     *   accidental deliberate, and leaves the tested path exactly where it is.
     *
     *   CHACHA20_POLY1305_SHA256 second, and this is the one that earns its
     *   place. On a part without AES acceleration mbedTLS falls back to
     *   table-driven AES, which is cache-timing vulnerable -- §6's item 2,
     *   "timing side channels through the record layer", arriving through the
     *   cipher rather than through the glue. ChaCha20 is constant-time in
     *   software by construction. This is the same argument §2 already makes
     *   for choosing ChaCha20 over AES as the DRBG; it applies to the record
     *   layer for the same reason, and having made it once it would be odd to
     *   leave the fallback to chance.
     *
     *   AES_128_GCM_SHA256 third, so a client that has only that still
     *   connects.
     *
     * The two CCM suites are left out on purpose. CCM exists for constrained
     * devices and nothing here is constrained; CCM_8 additionally truncates
     * the tag to 64 bits, which is a real reduction in forgery resistance
     * accepted elsewhere to save six bytes a record. Neither trade is one this
     * node needs to offer, and a suite that is offered can be selected.
     *
     * Static, because ssl.h warns the array is NOT copied and must outlive the
     * config -- the same footgun as the ALPN list directly above. */
    static const int suites[] = { TLS_SERVER_CIPHERSUITE_LIST, 0 };
    mbedtls_ssl_conf_ciphersuites(&g_conf, suites);

    return 0;

fail:
    teardown_tls();
    return -1;
}

/* Record when the leaf now installed becomes due for replacement. Derived from
 * the certificate itself rather than from "now plus the lifetime we asked
 * for", because the two differ whenever tls_cert_sign_leaf() clamped the
 * window into the CA's -- and the clamped case is exactly the one where
 * getting this wrong means a leaf that expires before anything looks at it. */
static void note_leaf_window(void)
{
    uint64_t now = 0, left = 0, margin = TLS_SERVER_LEAF_SECONDS / 3u;

    g_leaf_not_after = 0;
    g_leaf_renew_at  = 0;
    if (rtc_get_unix(&now) != RTC_OK) { return; }
    if (tls_cert_seconds_remaining(g_crt_der, g_crt_der_len, &left) != TLS_CERT_OK) {
        return;
    }
    g_leaf_not_after = now + left;
    g_leaf_renew_at  = (g_leaf_not_after > margin) ? (g_leaf_not_after - margin) : now;
}


/* ─── In-flight leaf renewal ───────────────────────────────────────────────
 * See tls_server.h for the shape and the three safety properties. The order of
 * the guards below is the design: cheapest first, and nothing is torn down
 * until a replacement exists.
 */
int tls_server_maybe_renew(void)
{
    /* All locals, deliberately. A static buffer for the CA key would put the
     * node's one long-lived secret back in RAM for the whole uptime, which is
     * the property tls_server_init() ends with a zeroize to establish. Here it
     * lives for the length of this call, once a year. The frame is ~5 KiB
     * against a 256 KiB advisory. */
    unsigned char ca_der[2048], ca_key_der[512];
    unsigned char new_leaf[2048], new_key[512];
    size_t ca_len = 0, ca_key_len = 0, nl_len = 0, nk_len = 0;
    uint64_t now = 0;
    int rc, same;

    if (!g_ready) { return TLS_SRV_E_NOT_READY; }

    /* The gate. Ticks, because this runs on every sweep of the server loop and
     * an RTC read is port I/O. Never the decision -- see the header. */
    if (kernel_tick_counter < g_next_check_tick) { return TLS_SRV_E_NOT_READY; }
    g_next_check_tick = kernel_tick_counter + TLS_SERVER_RENEW_CHECK_TICKS;

    if (rtc_get_unix(&now) != RTC_OK) {
        /* A node that has lost its clock cannot judge expiry. It also could
         * not have issued this certificate. Say nothing and try again in an
         * hour -- the clock is somebody else's problem and shouting about it
         * hourly would bury the line that matters. */
        return TLS_SRV_E_NOT_READY;
    }
    if (!tls_cert_renew_due(now, g_leaf_not_after, TLS_SERVER_LEAF_SECONDS)) {
        return TLS_SRV_E_NOT_READY;
    }

    if (!g_renewable) {
        if (!g_unrenewable_warned) {
            g_unrenewable_warned = 1;
            kernel_serial_print("[TLS] the leaf is due for renewal and this node "
                                "has NO CA on disk to sign a new one. It will "
                                "expire and stay expired. Reboot to reissue.\n");
        }
        return TLS_SRV_E_NOT_READY;
    }

    /* Live sessions hold pointers into g_conf, and replacing the certificate
     * means freeing and rebuilding it -- mbedtls_ssl_conf_own_cert() appends
     * rather than replaces, and 3.6 exposes no way to clear the list. With a
     * third of the leaf's life as the window, waiting for an idle moment costs
     * nothing and a use-after-free costs everything. */
    if (g_live > 0) {
        g_deferrals++;
        kernel_serial_printf("[TLS] leaf renewal due, deferred: %u session(s) "
                             "live. Retrying in about an hour.\n", g_live);
        return TLS_SRV_E_NOT_READY;
    }

    /* The CA key comes off the disk, is used, and is gone before this function
     * returns -- on every path, including the failures. It is never held
     * between renewals. */
    rc = tls_store_load(ca_der, sizeof ca_der, &ca_len,
                        ca_key_der, sizeof ca_key_der, &ca_key_len);
    if (rc != TLS_STORE_OK) {
        kernel_serial_printf("[TLS] leaf renewal: the stored CA could not be "
                             "read (rc=%d). Serving the old leaf.\n", rc);
        mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
        return TLS_SRV_E_NOT_READY;
    }

    /* The CA on disk must still be the CA we are presenting. If it is not,
     * something rewrote the store underneath a running node, and signing with
     * it would hand clients a chain that does not lead to the anchor they
     * imported. Cheap to check, and the alternative is discovering it in a
     * browser. */
    same = (ca_len == g_ca_der_len);
    if (same) {
        for (size_t i = 0; i < ca_len; i++) {
            if (ca_der[i] != g_ca_der[i]) { same = 0; break; }
        }
    }
    if (!same) {
        kernel_serial_print("[TLS] leaf renewal: the CA on disk is NOT the one "
                            "this node is serving. Refusing to sign. Reboot to "
                            "pick up the stored CA deliberately.\n");
        mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
        return TLS_SRV_E_NOT_READY;
    }

    rc = tls_cert_sign_leaf(ca_der, ca_len, ca_key_der, ca_key_len,
                            g_ca_dn, g_dn, g_sans, g_san_count,
                            TLS_SERVER_LEAF_SECONDS,
                            new_leaf, sizeof new_leaf, &nl_len,
                            new_key, sizeof new_key, &nk_len);
    mbedtls_platform_zeroize(ca_key_der, sizeof ca_key_der);
    if (rc != TLS_CERT_OK) {
        /* Nothing has been torn down. The old leaf is still installed and
         * still being served; we simply try again next hour, and there are
         * thousands of hours left in the margin. */
        kernel_serial_printf("[TLS] leaf renewal failed: rc=%d step=%s. Still "
                             "serving the previous leaf.\n",
                             rc, tls_cert_last_step());
        mbedtls_platform_zeroize(new_key, sizeof new_key);
        return TLS_SRV_E_NOT_READY;
    }

    /* ─── The swap ──────────────────────────────────────────────────────────
     * Past this line there is no way back, and it is worth being explicit
     * about why rather than leaving it to be discovered: rolling back would
     * mean reinstalling the OLD leaf, which needs the old leaf's private key,
     * which was zeroized the moment it was installed. Keeping it would mean a
     * second private key resident for a year to cover a failure that
     * tls_cert_sign_leaf() has already ruled out -- it parses the finished
     * certificate back before returning, so "the DER does not parse" cannot
     * reach here.
     *
     * What remains is an allocation failure inside the rebuild, at the one
     * moment the pool is at its emptiest (zero live sessions, by the guard
     * above) and doing exactly what succeeded at boot. If it happens anyway,
     * g_ready goes to 0 and every TLS connection is refused -- loudly, and
     * visibly in /api/health -- rather than served with something broken. */
    teardown_tls();
    for (size_t i = 0; i < nl_len; i++) { g_crt_der[i] = new_leaf[i]; }
    g_crt_der_len = nl_len;

    if (install_leaf(new_key, nk_len) != 0) {
        g_ready = 0;
        g_crt_der_len = 0;
        kernel_serial_print("[TLS] CATASTROPHIC: the renewed leaf could not be "
                            "installed and the previous one is gone. TLS is "
                            "down on this node until it reboots.\n");
        mbedtls_platform_zeroize(new_key, sizeof new_key);
        return TLS_SRV_E_NOT_READY;
    }
    mbedtls_platform_zeroize(new_key, sizeof new_key);

    g_renewals++;
    g_unrenewable_warned = 0;
    note_leaf_window();
    kernel_serial_printf("[TLS] leaf renewed in flight (%lu since boot), %u "
                         "bytes. No re-import is needed -- the CA is unchanged.\n",
                         g_renewals, (unsigned)g_crt_der_len);

    /* And while we have the CA parsed anyway: the anchor itself expires, and
     * that one DOES cost a re-import. Only tls_server_init() replaces it, so
     * the useful thing here is warning far enough ahead that the reboot can be
     * scheduled rather than forced. */
    {
        uint64_t ca_left = 0;
        if (tls_cert_seconds_remaining(g_ca_der, g_ca_der_len, &ca_left) == TLS_CERT_OK &&
            ca_left < TLS_SERVER_CA_RENEW_SECONDS) {
            kernel_serial_printf("[TLS] note: the CA has %u day(s) left. The next "
                                 "REBOOT will replace it, and that one does need "
                                 "a re-import in every trust store.\n",
                                 (unsigned)(ca_left / (24ULL * 60ULL * 60ULL)));
        }
    }
    return TLS_SRV_OK;
}

void tls_server_renewal_status(uint64_t *renew_at, unsigned long *renewals,
                               unsigned long *deferrals, int *renewable)
{
    if (renew_at)  { *renew_at  = g_leaf_renew_at; }
    if (renewals)  { *renewals  = g_renewals; }
    if (deferrals) { *deferrals = g_deferrals; }
    if (renewable) { *renewable = g_renewable; }
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
void tls_server_stats(unsigned long *refused, unsigned *live_peak, unsigned *live,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks);
void tls_server_stats(unsigned long *refused, unsigned *live_peak, unsigned *live,
                      size_t *pool_bytes, size_t *pool_peak, size_t *pool_blocks)
{
    size_t used = 0, blocks = 0;
    sls_tls_pool_high_water(&used, &blocks);
    if (refused)     { *refused = g_refused; }
    if (live_peak)   { *live_peak = g_live_peak; }
    if (live)        { *live = g_live; }
    if (pool_bytes)  { *pool_bytes = sls_tls_pool_bytes(); }
    if (pool_peak)   { *pool_peak = used; }
    if (pool_blocks) { *pool_blocks = blocks; }
}
