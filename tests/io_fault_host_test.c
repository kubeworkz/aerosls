/*
 * io_fault_host_test.c — every I/O failure must reach the caller that caused it.
 *
 * ─── The method, from SQLite ───────────────────────────────────────────────
 * SQLite's testing notes describe rigging the VFS "to simulate an I/O error
 * after a set number of I/O operations", then running the suite repeatedly
 * with the threshold raised each time, so every storage error path executes.
 *
 * ─── The sharpening the smoke forced ───────────────────────────────────────
 * The first version of this file swept a WORKLOAD -- save, load, wipe, load --
 * and asked whether anything in it reported a problem. That predicate passed a
 * deliberately mutated tls_store.c that ignored a write's status entirely,
 * because the swallowed write left the frame absent and the LATER load
 * correctly reported an empty store. Something reported a failure. It was the
 * wrong something.
 *
 * A sweep whose assertion is "an error surfaced somewhere" cannot distinguish
 * "the call handled it" from "a subsequent call tripped over the damage". The
 * second is not error handling; it is the corruption arriving later with a
 * different name on it.
 *
 * So each entry point is swept ALONE. Prerequisites run fault-free, the fault
 * lands inside the one call under test, and that call's own return value must
 * change. tests/io_fault_smoke.sh plants the same mutation and requires this
 * to catch it.
 *
 * ─── Why the first target is code that already gets this right ─────────────
 * kernel/tls_store.c checks every nvme_*_sync() return, so a correct sweep
 * reports zero swallowed failures. Zero is also what a broken sweep reports --
 * which is exactly what happened above. The control exists to make the smoke
 * meaningful; the eight unchecked call sites in persist.c, rowstore.c,
 * vecstore.c and qemu_sls_tcache.c are what it is for.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fake_nvme.h"
#include "tls_store.h"

int  rtc_get_unix(uint64_t *out) { *out = 1786492800ULL; return 0; }
void kernel_serial_print(const char *s)         { (void)s; }
void kernel_serial_printf(const char *fmt, ...) { (void)fmt; }

static int pass, fail;
static void ok(const char *m)  { printf("ok:   %s\n", m); pass++; }
static void bad(const char *m) { printf("FAIL: %s\n", m); fail++; }

/* 4 KiB aligned: these reach nvme_write_sync() and fake_nvme.c enforces the
 * alignment the real driver requires rather than quietly accepting it. */
static unsigned char g_ca[512]  __attribute__((aligned(4096)));
static unsigned char g_key[128] __attribute__((aligned(4096)));
static unsigned char g_out1[4096] __attribute__((aligned(4096)));
static unsigned char g_out2[4096] __attribute__((aligned(4096)));
static size_t g_len1, g_len2;

static void setup_none(void) { }
static void setup_saved(void)
{
    if (tls_store_save(g_ca, sizeof g_ca, g_key, sizeof g_key) != TLS_STORE_OK) {
        printf("FAIL: fault-free setup could not save -- the sweep below is "
               "measuring nothing\n");
        fail++;
    }
}

static int call_save(void)
{ return tls_store_save(g_ca, sizeof g_ca, g_key, sizeof g_key); }

static int call_load(void)
{
    g_len1 = g_len2 = 0;
    return tls_store_load(g_out1, sizeof g_out1, &g_len1,
                          g_out2, sizeof g_out2, &g_len2);
}

static int call_wipe(void) { return tls_store_wipe(); }

/* Sweep one entry point.
 *
 * `setup` runs with no faults and its I/O is not swept -- only the call under
 * test is. `ok_rc` is what the call returns when nothing went wrong; anything
 * else counts as "the caller was told". */
static void sweep(const char *label, void (*setup)(void), int (*call)(void),
                  int ok_rc, int expect_len_cleared)
{
    unsigned long before, n_ops, i;
    int rc, swallowed = 0, kept_len = 0;

    fake_nvme_reset();
    setup();
    before = fake_nvme_ops();
    rc = call();
    n_ops = fake_nvme_ops() - before;

    if (rc != ok_rc) {
        printf("FAIL: %s: the fault-free call already returns %d\n", label, rc);
        fail++;
        return;
    }
    if (n_ops == 0) {
        printf("FAIL: %s: performs NO I/O -- a sweep over nothing reports a "
               "clean pass\n", label);
        fail++;
        return;
    }

    for (i = 1; i <= n_ops; i++) {
        fake_nvme_reset();
        setup();
        fake_nvme_fail_at(fake_nvme_ops() + i, 0);   /* the i'th op of THIS call */
        rc = call();
        if (rc == ok_rc) {
            printf("FAIL: %s: operation %lu of %lu failed and the call still "
                   "returned %d\n", label, i, n_ops, rc);
            swallowed++;
        }
        if (expect_len_cleared && rc != ok_rc && (g_len1 != 0 || g_len2 != 0)) {
            kept_len++;
        }
    }

    if (swallowed == 0) {
        printf("ok:   %s: all %lu injected failure(s) reached the caller\n",
               label, n_ops);
        pass++;
    } else {
        printf("FAIL: %s: %d of %lu injected failure(s) were SWALLOWED\n",
               label, swallowed, n_ops);
        fail++;
    }
    if (expect_len_cleared) {
        if (kept_len == 0) {
            printf("ok:     and left the caller holding no length for data it "
                   "did not get\n");
            pass++;
        } else {
            printf("FAIL:   but %d failure(s) left a non-zero length behind\n",
                   kept_len);
            fail++;
        }
    }
}

int main(void)
{
    for (size_t i = 0; i < sizeof g_ca;  i++) { g_ca[i]  = (unsigned char)(i * 3 + 1); }
    for (size_t i = 0; i < sizeof g_key; i++) { g_key[i] = (unsigned char)(i * 7 + 5); }

    printf("\n=== the disk works: does anything move at all? ===\n\n");
    fake_nvme_reset();
    {
        size_t a = 0, b = 0;
        int s = tls_store_save(g_ca, sizeof g_ca, g_key, sizeof g_key);
        int l = tls_store_load(g_out1, sizeof g_out1, &a, g_out2, sizeof g_out2, &b);
        if (s == TLS_STORE_OK && l == TLS_STORE_OK &&
            a == sizeof g_ca && b == sizeof g_key &&
            memcmp(g_out1, g_ca, sizeof g_ca) == 0 &&
            memcmp(g_out2, g_key, sizeof g_key) == 0) {
            ok("a fault-free save/load round-trips both blobs exactly");
        } else {
            bad("the fault-free round trip is already wrong -- fix that before "
                "reading anything below");
        }
        if (fake_nvme_violations() == 0) {
            ok("  with no contract violations (every buffer 4 KiB aligned)");
        } else {
            bad("  and violated the driver contract -- see stderr");
        }
        printf("      %lu I/O operation(s): %lu read(s), %lu write(s)\n",
               fake_nvme_ops(), fake_nvme_reads(), fake_nvme_writes());
    }

    printf("\n=== the sweep: each entry point, each of its operations ===\n\n");
    sweep("tls_store_save", setup_none,  call_save, TLS_STORE_OK, 0);
    sweep("tls_store_load", setup_saved, call_load, TLS_STORE_OK, 1);
    sweep("tls_store_wipe", setup_saved, call_wipe, TLS_STORE_OK, 0);

    /* A different question from the sweep: not "is one failure noticed" but
     * "does a disk that has stopped for good make something spin or wedge".
     * A module can pass every case above and hang on this. */
    printf("\n=== a disk that never comes back ===\n\n");
    fake_nvme_reset();
    fake_nvme_fail_at(1, 1);
    {
        int s = call_save();
        int l = call_load();
        if (s != TLS_STORE_OK && l != TLS_STORE_OK) {
            ok("a permanently failed disk is reported, not retried forever");
        } else {
            bad("a permanently failed disk was not reported by both calls");
        }
        if (g_len1 == 0 && g_len2 == 0) {
            ok("  and the caller is left holding nothing, not a partial read");
        } else {
            bad("  but the caller was handed a length for data it did not get");
        }
    }

    printf("\n---- checks=%d failed=%d\n", pass + fail, fail);
    return fail == 0 ? 0 : 1;
}
