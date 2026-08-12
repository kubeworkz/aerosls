/* tls_store.c — one 4 KiB frame at PERSIST_TLS_LBA, holding the CA.
 *
 * See tls_store.h for why the CA is stored and the leaf is not, and for what
 * storing it costs.
 *
 * ─── Frame layout ──────────────────────────────────────────────────────────
 *   off  size  field
 *     0     8  magic       PERSIST_TLS_MAGIC ("SLSTLS01")
 *     8     4  version     TLS_STORE_VERSION
 *    12     4  ca_len
 *    16     4  key_len
 *    20     4  reserved    written 0, must read 0
 *    24     8  unix_written  diagnostics only, never trusted for a decision
 *    32   ...  CA certificate DER, ca_len bytes
 *         ...  CA private key DER, key_len bytes
 *
 * Every field is written and read a byte at a time through get32/put32 rather
 * than by casting the frame to a struct. That is not pedantry: this is an
 * on-disk format read by a kernel that may be built by a different compiler
 * than the one that wrote it, and a struct's padding is the compiler's to
 * choose. The one time that assumption breaks, it breaks on a private key.
 *
 * ─── What is deliberately NOT here ─────────────────────────────────────────
 * There is no CRC. That is a decision, not an omission.
 *
 * A CRC would catch a torn write. So does what already happens: both blobs are
 * handed to real parsers (mbedtls_x509_crt_parse_der, mbedtls_pk_parse_key),
 * and then to mbedtls_pk_check_pair(), which does a scalar multiply and proves
 * the key matches the certificate's public key. That check is STRICTLY
 * STRONGER than a checksum -- it rejects a torn write, and also a key from a
 * different CA, and also a certificate paired with a stale key, none of which
 * a CRC over the same bytes would notice. Adding a third crc32 implementation
 * to this tree to catch a subset of what the existing path already catches
 * would be cost with no coverage.
 *
 * What the parsers cannot catch is a frame whose header says one thing and
 * whose payload is another, so the lengths ARE checked here, against the frame
 * and against each other, before any byte reaches a parser.
 */

#include "tls_store.h"
#include "persist.h"
#include "rtc.h"
#include "../drivers/nvme_io.h"

void kernel_serial_print(const char* s);
void kernel_serial_printf(const char* fmt, ...);

/* Page-aligned: nvme_read_sync/nvme_write_sync build a PRP1-only command and
 * an unaligned buffer cannot be described by one at all. drivers/nvme_io.h
 * states the requirement; nvme_buf_aligned() enforces it. Getting this wrong
 * would fail loudly rather than silently, but only on real hardware. */
static uint8_t __attribute__((aligned(4096))) ts_frame[TLS_STORE_FRAME_BYTES];

static int  ts_loaded;
static uint64_t ts_written;

static void ts_zero(void *d, size_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)d;
    while (n--) { *p++ = 0; }
}

static void ts_copy(void *d, const void *s, size_t n)
{
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;
    while (n--) { *dp++ = *sp++; }
}

/* Little-endian, explicitly, on both directions. The kernel is x86-64 and
 * ARM64 little-endian today, so this is currently a no-op -- which is exactly
 * when it is worth writing down, because the day it stops being a no-op there
 * will be a disk full of frames written the other way. */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (8 * i)); }
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) { v = (v << 8) | (uint64_t)p[i]; }
    return v;
}

static int ts_nvme_available(void) { return io_sq && io_cq; }

int tls_store_load(unsigned char *ca_der, size_t ca_size, size_t *ca_len,
                   unsigned char *ca_key_der, size_t ck_size, size_t *ck_len)
{
    uint32_t version, clen, klen, reserved;
    int rc;

    if (!ca_der || !ca_len || !ca_key_der || !ck_len) {
        return TLS_STORE_E_BADARG;
    }
    if (!ts_nvme_available()) { return TLS_STORE_E_NO_NVME; }

    ts_zero(ts_frame, sizeof ts_frame);
    if (nvme_read_sync(PERSIST_TLS_LBA, ts_frame) != 0) {
        rc = TLS_STORE_E_IO; goto out;
    }

    if (get64(&ts_frame[0]) != PERSIST_TLS_MAGIC) {
        /* First boot, or a wiped store. Not an error and not logged as one:
         * the caller's response is to generate a CA, which it announces. */
        rc = TLS_STORE_E_EMPTY; goto out;
    }

    version  = get32(&ts_frame[8]);
    clen     = get32(&ts_frame[12]);
    klen     = get32(&ts_frame[16]);
    reserved = get32(&ts_frame[20]);

    if (version != TLS_STORE_VERSION) {
        kernel_serial_printf("[TLS] stored CA is format v%u, this build reads "
                             "v%u -- discarding it.\n",
                             (unsigned)version, (unsigned)TLS_STORE_VERSION);
        rc = TLS_STORE_E_FORMAT; goto out;
    }
    if (reserved != 0) { rc = TLS_STORE_E_FORMAT; goto out; }

    /* Every length check before any byte is copied out. The additions are
     * against TLS_STORE_MAX_PAYLOAD, which is far below UINT32_MAX, so
     * clen + klen cannot wrap -- but the individual checks come first anyway,
     * so the sum is only ever computed on two values already known small. */
    if (clen == 0 || klen == 0) { rc = TLS_STORE_E_FORMAT; goto out; }
    if (clen > TLS_STORE_MAX_PAYLOAD || klen > TLS_STORE_MAX_PAYLOAD) {
        rc = TLS_STORE_E_FORMAT; goto out;
    }
    if (clen + klen > TLS_STORE_MAX_PAYLOAD) { rc = TLS_STORE_E_FORMAT; goto out; }

    /* And against the caller's buffers, separately -- a frame that is
     * self-consistent can still be too big for the destination. */
    if (clen > ca_size || klen > ck_size) { rc = TLS_STORE_E_FORMAT; goto out; }

    ts_copy(ca_der,     &ts_frame[TLS_STORE_HDR_BYTES],        clen);
    ts_copy(ca_key_der, &ts_frame[TLS_STORE_HDR_BYTES + clen], klen);
    *ca_len = clen;
    *ck_len = klen;
    ts_written = get64(&ts_frame[24]);
    ts_loaded  = 1;
    rc = TLS_STORE_OK;

out:
    /* The frame held the plaintext CA key. It does not hold it now, on any
     * path -- including the successful one, where the key lives on only in the
     * caller's buffer and only until it has been parsed. */
    ts_zero(ts_frame, sizeof ts_frame);
    if (rc != TLS_STORE_OK) { *ca_len = 0; *ck_len = 0; }
    return rc;
}

int tls_store_save(const unsigned char *ca_der, size_t ca_len,
                   const unsigned char *ca_key_der, size_t ck_len)
{
    uint64_t now = 0;
    int rc;

    if (!ca_der || !ca_key_der || ca_len == 0 || ck_len == 0) {
        return TLS_STORE_E_BADARG;
    }
    if (ca_len + ck_len > TLS_STORE_MAX_PAYLOAD) { return TLS_STORE_E_BADARG; }
    if (!ts_nvme_available()) { return TLS_STORE_E_NO_NVME; }

    /* A store with no timestamp is fine; a node that cannot read its clock has
     * already been refused a certificate upstream, so this is belt and braces
     * rather than a path that runs. */
    if (rtc_get_unix(&now) != RTC_OK) { now = 0; }

    ts_zero(ts_frame, sizeof ts_frame);
    put64(&ts_frame[0], PERSIST_TLS_MAGIC);
    put32(&ts_frame[8],  TLS_STORE_VERSION);
    put32(&ts_frame[12], (uint32_t)ca_len);
    put32(&ts_frame[16], (uint32_t)ck_len);
    put32(&ts_frame[20], 0u);
    put64(&ts_frame[24], now);
    ts_copy(&ts_frame[TLS_STORE_HDR_BYTES],          ca_der,     ca_len);
    ts_copy(&ts_frame[TLS_STORE_HDR_BYTES + ca_len], ca_key_der, ck_len);

    rc = (nvme_write_sync(PERSIST_TLS_LBA, ts_frame) == 0)
             ? TLS_STORE_OK : TLS_STORE_E_IO;
    if (rc == TLS_STORE_OK) { ts_written = now; }

    ts_zero(ts_frame, sizeof ts_frame);
    return rc;
}

int tls_store_wipe(void)
{
    int rc;

    if (!ts_nvme_available()) { return TLS_STORE_E_NO_NVME; }

    ts_zero(ts_frame, sizeof ts_frame);
    rc = (nvme_write_sync(PERSIST_TLS_LBA, ts_frame) == 0)
             ? TLS_STORE_OK : TLS_STORE_E_IO;
    ts_loaded  = 0;
    ts_written = 0;
    return rc;
}

void tls_store_stats(int *loaded, uint64_t *unix_written)
{
    if (loaded)       { *loaded = ts_loaded; }
    if (unix_written) { *unix_written = ts_written; }
}

#ifdef TLS_STORE_TEST_HOOKS
/* The staging frame, for tests/tls_key_containment_check.sh to look at
 * directly. It is the one place in this module that ever holds the plaintext
 * CA key, and the claim that it does not hold it afterwards is exactly the
 * kind of claim that stays true in a comment long after a `goto` skips the
 * zeroize. Guarded like rtc.c's RTC_TEST_HOOKS, so nothing in the shipping
 * kernel can reach it. */
const unsigned char *tls_store_test_frame(size_t *len)
{
    if (len) { *len = sizeof ts_frame; }
    return (const unsigned char *)ts_frame;
}
#endif
