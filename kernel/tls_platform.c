/*
 * tls_platform.c — the glue mbedTLS needs from this kernel.
 *
 * TLS Phase 2. See docs/AeroSLS-TLS-Design-v0.1.md (and its amendment: the
 * library is mbedTLS 3.6 LTS, not BearSSL).
 *
 * mbedTLS is transport- and platform-agnostic by design: it asks the host for
 * randomness, memory and time, and does everything else itself. This file is
 * those three answers and nothing more. Every signature here was read out of
 * vendor/mbedtls's own headers, not recalled:
 *
 *   mbedtls_hardware_poll   library/entropy_poll.h:46
 *   mbedtls_f_rng_t         include/mbedtls/platform_util.h:209
 *   set_calloc_free         include/mbedtls/platform.h:160
 *
 * That is not pedantry. Twice this session a claim written from memory turned
 * out wrong -- a library's protocol support, and three date constants -- and
 * both were one lookup away. The headers are in the tree now.
 */

#include <stdint.h>
#include <stddef.h>
#include "entropy.h"
#include "rtc.h"

#ifndef TLS_PLATFORM_HOST_TEST
/* The kernel build compiles against the vendored tree; the host test does not,
 * so that the fail-closed RNG contract can be exercised without pulling 8 MB
 * of library into a unit test. */
#include "mbedtls/memory_buffer_alloc.h"

#endif

#ifndef TLS_PLATFORM_HOST_TEST
#include "kernel_io.h"
#else
#include <stdio.h>
#define kernel_serial_print(s) fputs((s), stderr)
#endif

/* ─── 1. Randomness ────────────────────────────────────────────────────────
 * Two entry points, because mbedTLS asks for randomness in two different
 * ways and both must route to the same place.
 *
 * mbedtls_hardware_poll() feeds its entropy accumulator (MBEDTLS_ENTROPY_
 * HARDWARE_ALT). sls_mbedtls_rng() is the mbedtls_f_rng_t handed to
 * mbedtls_ssl_conf_rng() for per-handshake randomness.
 *
 * ─── The important part: both fail closed ─────────────────────────────────
 * entropy_get() returns non-zero when the pool never reached its threshold,
 * and leaves the caller's buffer untouched. The tempting bug here is to
 * report success with olen = 0, or to return whatever the stack held --
 * mbedTLS would then either stall in its accumulator or, worse, proceed with
 * a buffer nobody wrote. Returning an error propagates up through
 * mbedtls_ssl_handshake() and the connection is refused, which is the correct
 * outcome for a node that cannot generate keys safely.
 *
 * A node with no entropy must serve nothing. It must not serve TLS badly. */

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen);

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;
    if (olen) *olen = 0;
    if (!output || len == 0) return -1;

    if (entropy_get(output, len) != ENTROPY_OK) {
        /* Deliberately NOT partial credit. Reporting a short read would let
         * mbedTLS keep polling and eventually satisfy its threshold from a
         * source that has already said it has nothing. */
        kernel_serial_print("[TLS] entropy unavailable -- refusing to supply "
                            "randomness to mbedTLS.\n");
        return -1;
    }
    if (olen) *olen = len;
    return 0;
}

/* Shape is mbedtls_f_rng_t exactly: int (*)(void *p_rng, unsigned char *out,
 * size_t out_size). p_rng is unused -- our generator is a kernel singleton,
 * not a per-context object -- but the parameter must stay for the type to
 * match, and a wrong-arity function pointer here is a silent miscompile
 * rather than a diagnostic. */
int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size);

int sls_mbedtls_rng(void *p_rng, unsigned char *output, size_t output_size)
{
    (void)p_rng;
    if (!output || output_size == 0) return -1;
    return (entropy_get(output, output_size) == ENTROPY_OK) ? 0 : -1;
}

/* ─── 2. Memory ────────────────────────────────────────────────────────────
 * mbedTLS wants calloc/free. This was the strongest argument for BearSSL,
 * which needs neither, and the design doc amendment says plainly that moving
 * to mbedTLS reduces rather than eliminates the problem.
 *
 * What reduces it is a FIXED POOL rather than the kernel's general heap: TLS
 * allocation cannot fragment or exhaust the arena the database lives in, so
 * the worst case is a refused handshake instead of a node dying somewhere
 * unrelated.
 *
 * The allocator itself is now upstream's MBEDTLS_MEMORY_BUFFER_ALLOC_C, not
 * ours. The previous version here was a bump allocator named PLACEHOLDER
 * because it never freed anything -- correct on overflow, correct on
 * exhaustion, and guaranteed to run out after N handshakes. Upstream's keeps a
 * real free list over the same buffer, so a connection's memory comes back
 * when it closes. Writing our own allocator to sit under a TLS stack was never
 * the right call when the library ships one built for exactly this.
 *
 * Size is a budget, not a tuning knob. TLS 1.3 records are up to 16 KB and
 * neither MAX_FRAGMENT_LENGTH nor VARIABLE_BUFFER_LENGTH is supported under
 * 1.3, so per-connection cost is fixed at full record size (design doc §3.2
 * and the amendment). Raising this number to make a failure go away is
 * choosing to run out later, in production, instead of now, in a test. */
#define SLS_TLS_POOL_BYTES (256u * 1024u)
#ifndef TLS_PLATFORM_HOST_TEST
static uint8_t tls_pool[SLS_TLS_POOL_BYTES] __attribute__((aligned(16)));
#endif
static uint8_t tls_pool_ready;

int  sls_tls_memory_init(void);
void sls_tls_memory_free(void);

int sls_tls_memory_init(void)
{
    if (tls_pool_ready) return 0;
#ifndef TLS_PLATFORM_HOST_TEST
    mbedtls_memory_buffer_alloc_init(tls_pool, sizeof tls_pool);
#endif
    tls_pool_ready = 1;
    kernel_serial_print("[TLS] memory pool initialised (256 KiB, fixed).\n");
    return 0;
}

void sls_tls_memory_free(void)
{
    if (!tls_pool_ready) return;
#ifndef TLS_PLATFORM_HOST_TEST
    mbedtls_memory_buffer_alloc_free();
#endif
    tls_pool_ready = 0;
}

size_t sls_tls_pool_bytes(void);
size_t sls_tls_pool_bytes(void) { return SLS_TLS_POOL_BYTES; }
int    sls_tls_memory_ready(void);
int    sls_tls_memory_ready(void) { return tls_pool_ready != 0; }

/* ─── 3. Time ──────────────────────────────────────────────────────────────
 * X.509 asks the platform for the current time to check notBefore/notAfter
 * (vendor/mbedtls/library/x509.c:1072 calls mbedtls_time(NULL)).
 * MBEDTLS_PLATFORM_TIME_MACRO binds that name to this function at COMPILE
 * time -- there is no pointer to install and no window in which one is unset.
 * Proof it is bound rather than merely declared: x509.o carries an undefined
 * reference to sls_mbedtls_time.
 *
 * ─── What happens with no trusted clock, and why 0 is not a fallback ──────
 * kernel/rtc.c refuses rather than guessing -- no RTC, a dead battery reading
 * 2000-01-01, a time before this kernel was built. When it refuses, this
 * returns 0, and 0 is the epoch: every certificate our private CA issues has
 * a notBefore far later than 1970, so X.509 rejects it as NOT YET VALID.
 *
 * That is fail-closed, but it is fail-closed by CONSEQUENCE, and relying on a
 * consequence is how a security property quietly stops holding -- a
 * certificate with an early enough notBefore would sail through. So it is the
 * second line, not the first. The first is sls_tls_time_init(), which REFUSES
 * on a node with no trusted clock and must be called before a listener is ever
 * opened. That is the check to keep honest. */

long long sls_mbedtls_time(long long *t);

long long sls_mbedtls_time(long long *t)
{
    uint64_t now = 0;
    long long v = 0;

    if (rtc_get_unix(&now) == RTC_OK) v = (long long)now;
    /* else v stays 0 -- see the note above; this is not a guess at the time,
     * it is a value chosen because it cannot pass a validity check. */

    if (t) *t = v;
    return v;
}

int sls_tls_time_init(void);
int sls_tls_time_trusted(void);

/* No pointer to install: MBEDTLS_PLATFORM_TIME_MACRO binds mbedtls_time to
 * sls_mbedtls_time at compile time. This exists purely to make the refusal
 * explicit and early, at a place a caller must pass through. */
int sls_tls_time_init(void)
{
    if (!rtc_is_trusted()) {
        kernel_serial_print("[TLS] no trusted wall clock -- certificate "
                            "validity cannot be checked. TLS must not start.\n");
        return -1;
    }
    return 0;
}

int sls_tls_time_trusted(void) { return rtc_is_trusted(); }

/* ─── 4. The two remaining platform hooks ──────────────────────────────────
 * Both exist because a freestanding link named them, not because a design
 * document predicted them. See sls_mbedtls_config.h §4b.
 */

#ifndef TLS_PLATFORM_HOST_TEST
extern volatile uint64_t kernel_tick_counter;
#else
static uint64_t kernel_tick_counter;
#endif

/* mbedtls_exit(). There is nothing to exit to in a kernel, and returning would
 * continue executing on state mbedTLS has already declared unusable. Say so
 * loudly and stop. Not a panic() call: this is reached only from library
 * internals, and a halt that names its cause is easier to diagnose than a
 * generic fault several frames away. */
void sls_mbedtls_exit(int status);

void sls_mbedtls_exit(int status)
{
    (void)status;
    kernel_serial_print("[TLS] mbedTLS called exit() -- unrecoverable library "
                        "state. Halting rather than continuing on it.\n");
#ifndef TLS_PLATFORM_HOST_TEST
    for (;;) { __asm__ volatile("cli; hlt"); }
#endif
}

/* mbedtls_ms_time(): a MONOTONIC millisecond counter for timeouts. Explicitly
 * NOT the wall clock -- §3 is that, and using this for certificate validity
 * would put expiry on a counter that restarts at every boot. ~100 ticks per
 * second, matching net/http.c's own uptime conversion. */
/* Return type is mbedtls_ms_time_t, not `long long`. They are not the same:
 * the typedef is int64_t, which on LP64 is `long`, and declaring it `long long`
 * is a hard compile error at the header -- caught by building, after I wrote
 * the obvious-looking type instead of the one the header names. */
#ifndef TLS_PLATFORM_HOST_TEST
#include "mbedtls/platform_time.h"

mbedtls_ms_time_t mbedtls_ms_time(void);

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(kernel_tick_counter * 10ull);
}
#endif

/* ─── 5. Zeroization ───────────────────────────────────────────────────────
 * MBEDTLS_PLATFORM_ZEROIZE_ALT. See sls_mbedtls_config.h for why upstream's
 * version cannot be used here.
 *
 * A plain memset() would be wrong, not merely different: a compiler is
 * entitled to delete a store to memory it can prove nothing reads again, and
 * a buffer being wiped just before it goes out of scope is precisely that
 * case. Writing through a volatile pointer removes that licence. */
void mbedtls_platform_zeroize(void *buf, size_t len);

void mbedtls_platform_zeroize(void *buf, size_t len)
{
    if (!buf || len == 0) return;
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) *p++ = 0;
}

/* ─── 6. The TCP bridge ────────────────────────────────────────────────────
 * mbedTLS never sees a socket. It calls two callbacks and this file is where
 * net/tcp.h meets them. Shapes are mbedtls_ssl_send_t and mbedtls_ssl_recv_t
 * exactly (include/mbedtls/ssl.h:813 and :837): int (void*, buf, size_t).
 *
 * ─── The thing that had to be discovered before writing this ──────────────
 * tcp_recv() BLOCKS. net/tcp.c:281 spins in net_event_hlt_wait() until data
 * arrives or the connection closes; there is no "nothing yet" return.
 *
 * That is fine for the existing HTTP loop because it never actually lets it
 * block -- net/http.c:6169 guards every call with `if (c->rbuf_used > 0)`, so
 * tcp_recv() is only entered when the data is already buffered. The blocking
 * path exists and is never taken.
 *
 * mbedTLS cannot do that. It decides when it needs bytes, and it needs them
 * mid-record. A naive bridge that just called tcp_recv() would hand any client
 * a trivial denial of service: open a connection, send one byte of a
 * ClientHello, and the node stops -- not just TLS, EVERYTHING, because
 * http_server_run() is a single loop that also drives the serial console
 * (net/http.c:11).
 *
 * So the bridge applies the same guard the HTTP loop does and returns
 * MBEDTLS_ERR_SSL_WANT_READ when the buffer is empty. That is precisely what
 * that error code is for, it needs no change to net/tcp.c, and it keeps the
 * handshake cooperative with the poll loop it lives inside.
 *
 * ctx is the connection id, passed as an integer through a void* -- the same
 * id http_conns[] and tcp_conns[] are both indexed by. */

#ifndef TLS_PLATFORM_HOST_TEST
#include "../net/tcp.h"
#include "mbedtls/ssl.h"

int sls_tls_bio_send(void *ctx, const unsigned char *buf, size_t len);
int sls_tls_bio_recv(void *ctx, unsigned char *buf, size_t len);

int sls_tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int id = (int)(long)ctx;
    if (!buf || len == 0) return 0;

    /* tcp_send takes uint32_t; a TLS record is at most ~16 KB so this cannot
     * truncate, but the clamp is here rather than assumed because the day the
     * types change silently is the day it does. */
    uint32_t want = (len > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)len;
    int sent = tcp_send(id, buf, want);
    if (sent < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

    /* A short write is reported honestly. mbedTLS will call again with the
     * remainder; claiming the full length would silently drop record bytes and
     * the peer would fail a MAC check somewhere unrelated. */
    return sent;
}

int sls_tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int id = (int)(long)ctx;
    if (!buf || len == 0) return 0;
    if (id < 0 || id >= TCP_MAX_CONNS) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

    struct TCPConn *c = &tcp_conns[id];

    /* The guard that stops one silent client halting the node. See above. */
    if (c->rbuf_used == 0) {
        if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED)
            return 0;                      /* peer closed: a real EOF */
        return MBEDTLS_ERR_SSL_WANT_READ;  /* nothing yet: come back later */
    }

    uint16_t want = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
    int got = tcp_recv(id, buf, want);
    if (got < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return got;
}
#endif /* !TLS_PLATFORM_HOST_TEST */

/* ─── 7. gmtime_r for X.509 validity ───────────────────────────────────────
 * MBEDTLS_PLATFORM_GMTIME_R_ALT, so this is the implementation the library
 * uses. x509.c calls it directly from mbedtls_x509_time_is_past() and
 * _is_future() -- the two functions that decide whether a certificate is
 * currently valid.
 *
 * The calendar arithmetic lives in kernel/rtc.c next to its inverse, where
 * both directions are tested against Python's datetime. This is the struct
 * marshalling and nothing else.
 *
 * struct tm's conventions are a well-known source of off-by-one: tm_year is
 * years since 1900 and tm_mon is 0-based, while tm_mday is 1-based. Getting
 * either wrong shifts every certificate's validity window by a year or a
 * month, in the permissive direction as often as not. */
#ifndef TLS_PLATFORM_HOST_TEST
#include <time.h>

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf);

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    if (!tt || !tm_buf) return 0;

    /* The arithmetic moved to rtc_break_down() in rtc.c so that this and
     * tls_cert_format_time() share one calendar rather than two. rtc.h asked
     * for exactly this: both directions of the conversion in one file, tested
     * together. Behaviour here is unchanged -- same floor division, same
     * civil_from_days, same Thursday. */
    int64_t  y = 0; unsigned m = 0, d = 0, hh = 0, mi = 0, ss = 0; int wd = 0;
    rtc_break_down((int64_t)*tt, &y, &m, &d, &hh, &mi, &ss, &wd);

    tm_buf->tm_year = (int)(y - 1900);   /* years since 1900 */
    tm_buf->tm_mon  = (int)m - 1;        /* 0-based */
    tm_buf->tm_mday = (int)d;            /* 1-based */
    tm_buf->tm_hour = (int)hh;
    tm_buf->tm_min  = (int)mi;
    tm_buf->tm_sec  = (int)ss;
    tm_buf->tm_wday = wd;
    tm_buf->tm_yday = 0;                 /* not computed; mbedTLS does not read it */
    tm_buf->tm_isdst = 0;                /* UTC has no DST, by definition */

    return tm_buf;
}
#endif
