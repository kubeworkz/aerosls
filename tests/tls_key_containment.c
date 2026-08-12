/*
 * tls_key_containment.c — the CA private key reaches ONE frame, and does not
 * stay in memory afterwards.
 *
 * ─── What this is guarding ─────────────────────────────────────────────────
 * kernel/tls_store.c writes the node's CA private key to NVMe in plaintext.
 * That is a deliberate trade (tls_store.h states it), and it is only
 * defensible while the key goes to exactly one place. A key that also reaches
 * a checkpoint, a snapshot or a migration stream has three lifetimes, three
 * sets of copies and three audiences, and no one reviewing the tls_store.h
 * comment would know.
 *
 * The comment says the staging frame is zeroized on every path. Comments do
 * not stay true. A `goto` added past the zeroize, an early return on an error
 * path, a future save() that writes a second frame -- each leaves a 4 KiB
 * static buffer holding a private key for the rest of the uptime, and none of
 * them changes anything observable. So this plants a key made of bytes nothing
 * else would produce and goes looking for it.
 *
 * ─── What it does NOT cover, said plainly ──────────────────────────────────
 * This is a host harness with a simulated disk. It proves the STORE writes the
 * key to one frame and holds none of it afterwards. It cannot prove that a
 * running kernel's checkpoint is clean, because that needs a live node and a
 * real disk image. The static half of tls_key_containment_check.sh covers the
 * structural side of that -- who is allowed to name the LBA and call the store
 * -- and the remaining gap is a live-node check that does not exist yet.
 * A green line here is not a claim that it does.
 *
 * Build and run: see tests/tls_key_containment_check.sh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "tls_store.h"
#include "persist.h"

/* ─── A simulated disk ─────────────────────────────────────────────────────
 * Wide enough to hold the TLS frame with room either side, so "the key is in
 * exactly one frame" is a statement about a disk and not about a single
 * buffer. Frames are indexed from PERSIST_TLS_LBA - 8 so an off-by-one-frame
 * write lands somewhere this can see rather than out of bounds.
 */
#define SIM_FRAMES   9
#define SIM_BASE_LBA (PERSIST_TLS_LBA - 32ULL)   /* four frames before */
static uint8_t sim_disk[SIM_FRAMES][4096];
static int sim_write_fails;
static int sim_read_fails;

/* nvme_io.h's externs, satisfied here. Non-NULL means "I/O queue is up". */
void *io_sq = (void *)1;
void *io_cq = (void *)1;

static int sim_index(uint64_t slba, size_t *out)
{
    if (slba < SIM_BASE_LBA) { return 0; }
    uint64_t off = slba - SIM_BASE_LBA;
    if (off % 8ULL != 0ULL) { return 0; }
    if (off / 8ULL >= SIM_FRAMES) { return 0; }
    *out = (size_t)(off / 8ULL);
    return 1;
}

int nvme_read_sync(uint64_t slba, void *buf)
{
    size_t i;
    if (sim_read_fails) { return 1; }
    if (!sim_index(slba, &i)) { return 1; }
    memcpy(buf, sim_disk[i], 4096);
    return 0;
}

int nvme_write_sync(uint64_t slba, const void *buf)
{
    size_t i;
    if (sim_write_fails) { return 1; }
    if (!sim_index(slba, &i)) { return 1; }
    memcpy(sim_disk[i], buf, 4096);
    return 0;
}

/* rtc.h's clock, stubbed. tls_store.c uses it only for the informational
 * timestamp, so a fixed value is honest here rather than convenient. */
int rtc_get_unix(uint64_t *out) { *out = 1786492800ULL; return 0; }

void kernel_serial_print(const char *s) { (void)s; }
void kernel_serial_printf(const char *fmt, ...) { (void)fmt; }

/* ─── The needle ───────────────────────────────────────────────────────────
 * Not random, and not zeroes: a fixed 48-byte pattern that no length field,
 * no ASN.1 header and no uninitialised page would ever contain. If this
 * sequence turns up somewhere, it got there by being copied.
 */
#define NEEDLE_LEN 48
static unsigned char needle[NEEDLE_LEN];
static void make_needle(void)
{
    for (int i = 0; i < NEEDLE_LEN; i++) {
        needle[i] = (unsigned char)(0xA5u ^ (unsigned)(i * 37 + 13));
    }
}

static int contains(const unsigned char *h, size_t hn,
                    const unsigned char *n, size_t nn)
{
    if (nn > hn) { return 0; }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(h + i, n, nn) == 0) { return 1; }
    }
    return 0;
}

static int pass, fail;
static void ok(const char *m)  { printf("ok:   %s\n", m); pass++; }
static void bad(const char *m) { printf("FAIL: %s\n", m); fail++; }
static void check(int cond, const char *m) { cond ? ok(m) : bad(m); }

/* How many of the simulated disk's frames contain the needle. */
static int frames_with_needle(void)
{
    int n = 0;
    for (int i = 0; i < SIM_FRAMES; i++) {
        if (contains(sim_disk[i], 4096, needle, NEEDLE_LEN)) { n++; }
    }
    return n;
}

static int staging_has_needle(void)
{
    size_t n = 0;
    const unsigned char *f = tls_store_test_frame(&n);
    return contains(f, n, needle, NEEDLE_LEN);
}

int main(void)
{
    /* A plausible-sized CA certificate, and a "key" that is the needle. The
     * certificate bytes are deliberately NOT the needle: the point is to tell
     * the key apart from everything else in the frame. */
    unsigned char cert[420];
    unsigned char key[NEEDLE_LEN];
    unsigned char rcert[4096], rkey[4096];
    size_t rcert_len = 0, rkey_len = 0;
    int rc;

    make_needle();
    for (size_t i = 0; i < sizeof cert; i++) { cert[i] = (unsigned char)(i & 0xFF); }
    memcpy(key, needle, NEEDLE_LEN);

    memset(sim_disk, 0, sizeof sim_disk);

    printf("\n=== the key reaches one frame and no other ===\n\n");

    check(frames_with_needle() == 0, "the simulated disk starts with no key on it");

    rc = tls_store_save(cert, sizeof cert, key, sizeof key);
    check(rc == TLS_STORE_OK, "the CA and key are stored");
    check(frames_with_needle() == 1,
          "the key lands in EXACTLY ONE frame -- not zero, not two");
    check(contains(sim_disk[4], 4096, needle, NEEDLE_LEN),
          "  and that frame is PERSIST_TLS_LBA, not a neighbour");

    check(!staging_has_needle(),
          "the staging buffer holds no key after save() returns");

    printf("\n=== and does not linger in memory ===\n\n");

    rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
    check(rc == TLS_STORE_OK, "the store loads back");
    check(rcert_len == sizeof cert && memcmp(rcert, cert, sizeof cert) == 0,
          "  the certificate round-trips byte for byte");
    check(rkey_len == sizeof key && memcmp(rkey, key, sizeof key) == 0,
          "  and so does the key");
    check(!staging_has_needle(),
          "the staging buffer holds no key after a SUCCESSFUL load either");

    /* The path most likely to skip a zeroize: an early return. Corrupt the
     * version so load() bails at the format check, with the key already read
     * into the staging frame from disk. */
    sim_disk[4][8] = 0xFF;
    rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
    check(rc == TLS_STORE_E_FORMAT, "a wrong format version is refused");
    check(!staging_has_needle(),
          "  and the staging buffer is still clear -- the early return zeroizes too");
    check(rcert_len == 0 && rkey_len == 0,
          "  and the caller is left holding nothing");
    sim_disk[4][8] = (uint8_t)TLS_STORE_VERSION;

    printf("\n=== the length fields are not taken on trust ===\n\n");

    /* A frame claiming more payload than a frame can hold. Left unchecked this
     * copies past the end of the caller's buffer -- from a region an attacker
     * with disk access controls. */
    {
        uint8_t save12 = sim_disk[4][12];
        sim_disk[4][12] = 0xFF; sim_disk[4][13] = 0xFF;
        rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
        check(rc == TLS_STORE_E_FORMAT, "an impossible ca_len is refused");
        sim_disk[4][12] = save12; sim_disk[4][13] = 0;
    }
    {
        uint8_t save16 = sim_disk[4][16];
        sim_disk[4][16] = 0; sim_disk[4][17] = 0;
        rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
        check(rc == TLS_STORE_E_FORMAT, "a zero key_len is refused");
        sim_disk[4][16] = save16;
    }
    /* Both lengths individually fine, their sum past the frame. The check that
     * catches this is separate from the two above and would not be reached by
     * either of them. */
    {
        uint8_t s12 = sim_disk[4][12], s13 = sim_disk[4][13];
        uint8_t s16 = sim_disk[4][16], s17 = sim_disk[4][17];
        sim_disk[4][12] = 0x00; sim_disk[4][13] = 0x0C;   /* 3072 */
        sim_disk[4][16] = 0x00; sim_disk[4][17] = 0x0C;   /* 3072 */
        rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
        check(rc == TLS_STORE_E_FORMAT,
              "two individually-legal lengths that overflow the frame together are refused");
        sim_disk[4][12] = s12; sim_disk[4][13] = s13;
        sim_disk[4][16] = s16; sim_disk[4][17] = s17;
    }
    /* And a destination smaller than a self-consistent frame. */
    {
        unsigned char small[16];
        size_t small_len = 0;
        rc = tls_store_load(small, sizeof small, &small_len, rkey, sizeof rkey, &rkey_len);
        check(rc == TLS_STORE_E_FORMAT,
              "a caller buffer too small for a valid frame is refused, not overrun");
        check(small_len == 0, "  with nothing written to it");
    }

    printf("\n=== wipe leaves nothing to read ===\n\n");

    rc = tls_store_wipe();
    check(rc == TLS_STORE_OK, "wipe() succeeds");
    check(frames_with_needle() == 0, "  the key is gone from the disk entirely");
    rc = tls_store_load(rcert, sizeof rcert, &rcert_len, rkey, sizeof rkey, &rkey_len);
    check(rc == TLS_STORE_E_EMPTY, "  and the store now reads as empty, not corrupt");

    printf("\n=== an absent disk is refused, not guessed at ===\n\n");
    {
        void *sq = io_sq, *cq = io_cq;
        io_sq = NULL; io_cq = NULL;
        check(tls_store_save(cert, sizeof cert, key, sizeof key) == TLS_STORE_E_NO_NVME,
              "save() without an I/O queue returns E_NO_NVME");
        check(tls_store_load(rcert, sizeof rcert, &rcert_len,
                             rkey, sizeof rkey, &rkey_len) == TLS_STORE_E_NO_NVME,
              "load() without an I/O queue returns E_NO_NVME");
        io_sq = sq; io_cq = cq;
    }

    /* A write that fails must not leave the module believing it stored
     * something -- and must still clear its staging frame. */
    sim_write_fails = 1;
    check(tls_store_save(cert, sizeof cert, key, sizeof key) == TLS_STORE_E_IO,
          "a failed NVMe write is reported, not swallowed");
    check(!staging_has_needle(),
          "  and the staging buffer is clear even on the I/O failure path");
    sim_write_fails = 0;

    printf("\n---- checks=%d failed=%d\n", pass + fail, fail);
    return fail == 0 ? 0 : 1;
}
