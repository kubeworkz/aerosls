/*
 * serial_tx_lock_host_test.c — the E1 serial TX lock, linked against the REAL,
 * unmodified kernel/kernel_io.c.
 *
 * ─── The property this exists to pin ──────────────────────────────────────
 * The lock makes the two writers that share COM1 — the BSP's kernel prints and
 * the AP's sidecar console drain — write whole lines instead of byte-interleaved
 * ones. It is the fix for logs like `[INIT] UNIF<kernel line>IED boot`, which
 * garbled the boot log for operators and made every grep-based boot guard
 * flaky.
 *
 * Its wait path, though, used to be a compare-and-swap retry loop: every
 * waiting iteration issued `lock cmpxchg`, up to the full spin budget per failed
 * acquisition. Under QEMU's multi-threaded TCG those are helper calls taking a
 * lock shared with the other vCPU thread's own memory operations — so a writer
 * waiting on a slow console line hammered the mechanism the core it was waiting
 * for needed to make progress at all. That is the leading suspect for the E1
 * boot wedge (a lost wake: guest alive, tick counter advancing, BSP parked in
 * cap_wait_chans for ever).
 *
 * The repair is a test-then-test-and-set wait: a plain load spins, and the
 * atomic is issued only when the load reports the port free. Nothing about that
 * is visible in the bytes on the wire, in the return codes, or in a boot log —
 * the two shapes are identical to every other kind of test. Hence the seam
 * (tests/kernel_serial_tx_seam.h) and hence this file: it counts the atomics.
 *
 * ─── What is asserted, and the control that makes it mean anything ────────
 *   1. an uncontended acquire costs EXACTLY ONE atomic;
 *   2. CONTROL: the counter really is on the path (with the hook forced to
 *      refuse, the lock must exhaust a bounded budget and report failure —
 *      without this, every "zero atomics" assertion below would pass vacuously
 *      on a test that cannot see the hook at all);
 *   3. a waiter behind another writer issues ZERO atomics — the repaired
 *      property, and 200 000 of them before;
 *   4. one line is written under ONE acquisition, with every byte of it inside
 *      that hold;
 *   5. the print family is flat: printf/print_hex64 acquire once, never
 *      nested, and the port is free again afterwards;
 *   6. the bounded fallback: a print that cannot acquire still reaches the
 *      wire, and does NOT release a hold it never took (only the acquirer may
 *      unlock — a double release would let the next writer in mid-line).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_serial_tx_seam.h \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/serial_tx_lock_host_test \
 *       tests/serial_tx_lock_host_test.c kernel/kernel_io.c
 *   /tmp/serial_tx_lock_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "kernel/kernel_io.h"

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 is not cosmetic -- tests/run_all.sh counts checks with
     * grep -c '^ok:', so a differently-formatted line reports "0 checks" and a
     * test that silently stopped asserting looks identical to one that passed. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* ─── the atomic seam (tests/kernel_serial_tx_seam.h) ────────────────────── */
/* Counts every atomic attempt the lock makes, and can be told to refuse them
 * so the bounded-failure path is reachable on a host. It performs the REAL
 * compare-and-swap: the lock must behave under this test exactly as it does in
 * the kernel, with a counter on the side. */
static int g_try_acquire_calls;
static int g_deny_acquire;

int kio_test_try_acquire(volatile int* busy) {
    g_try_acquire_calls++;
    if (g_deny_acquire) return 0;
    return __sync_bool_compare_and_swap(busy, 0, 1);
}

static void seam_reset(void) {
    g_try_acquire_calls = 0;
    g_deny_acquire = 0;
}

/* ─── the port seam (tests/kernel_io_panic_port.h) ───────────────────────── */
#define PORTLOG_MAX 4096
static char     g_port_bytes[PORTLOG_MAX];
static uint16_t g_port_addrs[PORTLOG_MAX];
/* The value of the atomic counter when each byte was written. A line written
 * under ONE acquisition shows the same count on every byte; a line written by
 * a lock that released and re-took the port part way through would show the
 * count stepping up mid-line. */
static int      g_byte_attempts[PORTLOG_MAX];
static int      g_port_count;

void kio_test_outb(uint16_t port, uint8_t val) {
    if (g_port_count < PORTLOG_MAX) {
        g_port_bytes[g_port_count] = (char)val;
        g_port_addrs[g_port_count] = port;
        g_byte_attempts[g_port_count] = g_try_acquire_calls;
    }
    g_port_count++;
}

uint8_t kio_test_inb(uint16_t port) {
    (void)port;
    return 0x20;   /* bit 5 set: THR empty, so putchar never waits */
}

static void port_reset(void) {
    g_port_count = 0;
    memset(g_port_bytes, 0, sizeof g_port_bytes);
    memset(g_port_addrs, 0, sizeof g_port_addrs);
    memset(g_byte_attempts, 0, sizeof g_byte_attempts);
}

/* Bytes that reached the DATA register, as a C string. */
static const char* port_text(void) {
    static char out[PORTLOG_MAX + 1];
    int n = 0;
    for (int i = 0; i < g_port_count && i < PORTLOG_MAX; i++)
        if (g_port_addrs[i] == SERIAL_COM1_BASE) out[n++] = g_port_bytes[i];
    out[n] = '\0';
    return out;
}

/* 1 when every byte written since the last reset was written while the atomic
 * counter stood at `n` -- i.e. under a single, unbroken hold. */
static int bytes_all_under(int n) {
    if (g_port_count == 0) return 0;
    for (int i = 0; i < g_port_count && i < PORTLOG_MAX; i++)
        if (g_byte_attempts[i] != n) return 0;
    return 1;
}

/* ─── stubs for what kernel_io.c pulls in ────────────────────────────────── */
int  vga_is_ready(void)      { return 0; }   /* keep VGA out of the port log */
void vga_putchar(char c)     { (void)c; }
void vga_init(void)          { }
void vga_clear(void)         { }
void vga_print(const char* s){ (void)s; }

int  smp_cpu_id(void)        { return 0; }
int  smp_cpu_count(void)     { return 1; }
void smp_uniprocessor_tick(void) { }
/* POSIX-Environments E1: read_line()'s idle point. Nothing to schedule on the
 * host, and this test drives the print path, so the documented no-op is the
 * accurate stub. */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

/* console_feed() lives in kernel/console.c and is not under test here. Stubbed
 * to "no line completed" so serial_console_poll() links; a stub claiming a line
 * was ready would make the poll behave differently from the real thing. */
int console_feed(char c, char* out, size_t cap) {
    (void)c; (void)out; (void)cap; return 0;
}
void console_reset_line(void) { }

int main(void) {
    printf("serial_tx_lock_host_test\n");
    printf("========================\n\n");

    kernel_serial_tx_unlock();   /* known state: free */

    /* ── 1. the uncontended acquire is one atomic ─────────────────────── */
    printf("1. an uncontended acquire costs exactly one atomic\n");
    seam_reset();
    int held = kernel_serial_tx_lock();
    CHECK(held == 1, "the port is acquired when it is free");
    CHECK(g_try_acquire_calls == 1,
          "and the acquire cost exactly ONE atomic read-modify-write");
    seam_reset();
    kernel_serial_tx_unlock();
    CHECK(kernel_serial_tx_lock() == 1,
          "unlock() releases, so the next acquire succeeds");
    CHECK(g_try_acquire_calls == 1, "again at the cost of one atomic");
    kernel_serial_tx_unlock();

    /* ── 2. CONTROL: the seam is on the path ──────────────────────────── */
    /* Without this section, every "zero atomics" assertion below could pass on
     * a test that simply cannot observe the hook. Refusing every attempt also
     * exercises the bounded give-up: the lock must stop, not spin for ever. */
    printf("\n2. CONTROL: the atomic counter is live, and the budget is bounded\n");
    seam_reset();
    g_deny_acquire = 1;
    held = kernel_serial_tx_lock();
    CHECK(g_try_acquire_calls > 0,
          "CONTROL: attempts really are counted through the seam");
    CHECK(held == 0,
          "an acquire that can never win reports failure instead of hanging");
    CHECK(g_try_acquire_calls >= 1000 && g_try_acquire_calls <= 10000000,
          "and it retried a bounded number of times (not once, not for ever)");
    seam_reset();

    /* ── 3. the repaired property: a waiter costs NO atomics ──────────── */
    printf("\n3. a waiter behind another writer issues zero atomics\n");
    seam_reset();
    CHECK(kernel_serial_tx_lock() == 1, "writer A takes the port");
    seam_reset();
    held = kernel_serial_tx_lock();     /* writer B waits on the held port */
    CHECK(held == 0, "writer B gives up after its budget (bounded, no hang)");
    CHECK(g_try_acquire_calls == 0,
          "while waiting, writer B issued ZERO atomic read-modify-writes");
    kernel_serial_tx_unlock();

    /* ── 4. one line is one acquisition ───────────────────────────────── */
    printf("\n4. a line is written under a single, unbroken hold\n");
    port_reset(); seam_reset();
    kernel_serial_print("WHOLE-LINE\n");
    CHECK(strcmp(port_text(), "WHOLE-LINE\r\n") == 0,
          "the line reached the wire with '\\n' sent as CR LF");
    CHECK(g_try_acquire_calls == 1,
          "under exactly one acquisition");
    CHECK(bytes_all_under(1),
          "and every byte of it was written inside that hold (no mid-line release)");

    /* ── 5. the print family is flat ──────────────────────────────────── */
    /* kernel_serial_printf() documents that its %p case must NOT delegate to
     * kernel_serial_print(), because that would try to take a lock the printf
     * already holds; with a bounded (non-reentrant) lock that nested acquire
     * would burn its whole budget and still print unlocked. That is exactly
     * one extra atomic attempt, so the counter catches it. */
    printf("\n5. printf/hex64 acquire once, and never nest\n");
    port_reset(); seam_reset();
    kernel_serial_printf("%s %d %x %p\n", "s", 7, 0x2a, (void*)(uintptr_t)0x1000);
    CHECK(g_try_acquire_calls == 1,
          "printf acquired the port exactly once (no nested acquisition)");
    /* Note the literal '\n': kernel_serial_printf() does not translate newlines
     * (kernel_serial_print()/print_hex64() do). Pre-existing behaviour, asserted
     * so a change to the lock cannot silently change it either. */
    CHECK(strcmp(port_text(), "s 7 2a 0x0000000000001000\n") == 0,
          "and its formatting and literal bytes are unchanged");
    CHECK(bytes_all_under(1), "with every byte inside that one hold");

    port_reset(); seam_reset();
    kernel_serial_print_hex64(0xDEADBEEFCAFEF00DULL);
    CHECK(g_try_acquire_calls == 1, "print_hex64 acquired exactly once");
    CHECK(strcmp(port_text(), "deadbeefcafef00d") == 0, "and printed 16 lowercase digits");

    seam_reset();
    CHECK(kernel_serial_tx_lock() == 1 && g_try_acquire_calls == 1,
          "the port is free again after each print (no leaked hold)");
    kernel_serial_tx_unlock();

    /* ── 6. the bounded fallback, and who may unlock ──────────────────── */
    printf("\n6. a print that cannot acquire still prints, and releases nothing\n");
    CHECK(kernel_serial_tx_lock() == 1, "the holder takes the port");
    port_reset(); seam_reset();
    kernel_serial_print("FROM-ISR\n");
    CHECK(strcmp(port_text(), "FROM-ISR\r\n") == 0,
          "a caller that cannot acquire still reaches the wire (no dropped log)");
    CHECK(g_try_acquire_calls == 0,
          "having issued no atomic attempt at all while it waited");
    CHECK(kernel_serial_tx_lock() == 0,
          "it did NOT release a hold it never took (the holder still owns the port)");
    kernel_serial_tx_unlock();          /* the real holder releases */
    seam_reset();
    CHECK(kernel_serial_tx_lock() == 1,
          "after the holder's release the port is acquirable again");
    CHECK(g_try_acquire_calls == 1, "at one atomic");
    kernel_serial_tx_unlock();

    printf("\n========================\n");
    printf("passed %d, failed %d\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
