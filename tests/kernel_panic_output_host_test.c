/*
 * kernel_panic_output_host_test.c — the panic-path output primitives, linked
 * against the REAL, unmodified kernel/kernel_io.c.
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * user/shell.c:457 calls kernel_serial_capture_start() before running a shell
 * command, so kernel_serial_putchar() diverts every character into a memory
 * buffer instead of the UART. A `qemu bench 8` issued over HTTP took a kernel
 * page fault and halted between that call and the matching _stop() at
 * shell.c:2812. The buffer was never flushed, the HTTP response was never
 * sent, and cluster/node2.log contained NOTHING -- no [SLS-BENCH], no
 * [QEMU-SLS MMU], no [FAULT] -- even though gdb later showed the CPU stopped
 * on the `cli; hlt` two instructions PAST the printf that reports the fault.
 *
 * Six consecutive hypotheses were built on "the log does not show X, therefore
 * X did not happen," against a log that was structurally incapable of showing
 * anything. The rule was already written down: AN ABSENCE IS NOT A MEASUREMENT.
 *
 * ─── What is asserted, and why it is the PORT and not the return code ──────
 * These functions return void. "kernel_panic_puts() did not crash" is true
 * whether it wrote to the UART, wrote to the capture buffer, or wrote nowhere
 * at all -- so nothing observable from the call site distinguishes the fix
 * from the bug. The kio_test_outb() hook below records every byte that reaches
 * a port, and the assertions are about what arrived THERE, with a capture
 * buffer deliberately left active: the far side of the boundary being crossed.
 *
 * Check 3 is the one that makes the rest mean anything. It drives
 * kernel_serial_print() through the same active capture and asserts the bytes
 * land in the BUFFER and not the port. Without it, every "went to the port"
 * assertion could pass on a test that simply cannot see the buffer.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/kernel_panic_output_host_test \
 *       tests/kernel_panic_output_host_test.c kernel/kernel_io.c
 *   /tmp/kernel_panic_output_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "kernel/kernel_io.h"

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 is not cosmetic -- tests/run_all.sh counts checks with
     * grep -c '^ok:', so a differently-formatted line reports "0 checks" and a
     * test that silently stopped asserting looks identical to one that passed. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* ─── the port hooks ─────────────────────────────────────────────────────── */

#define PORTLOG_MAX 4096
static char     g_port_bytes[PORTLOG_MAX];
static uint16_t g_port_addrs[PORTLOG_MAX];
static int      g_port_count;

/* Every inb() the code under test performs, so "did it poll the line-status
 * register before writing?" is answerable. A panic path that blasts the data
 * register without checking transmitter-empty would corrupt output on a real
 * UART, and would be indistinguishable from a correct one if only writes were
 * recorded. */
static int      g_inb_count;
static uint16_t g_last_inb_port;
static int      g_lsr_ready = 1;      /* what bit 5 of the LSR reports */

void kio_test_outb(uint16_t port, uint8_t val) {
    if (g_port_count < PORTLOG_MAX) {
        g_port_bytes[g_port_count] = (char)val;
        g_port_addrs[g_port_count] = port;
    }
    g_port_count++;
}

uint8_t kio_test_inb(uint16_t port) {
    g_inb_count++;
    g_last_inb_port = port;
    /* Bit 5 (0x20) is THR-empty. Everything else is left clear so a reader
     * that tests the wrong bit sees "not ready" forever and the bounded-wait
     * check below catches it. */
    return (uint8_t)(g_lsr_ready ? 0x20 : 0x00);
}

static void port_reset(void) {
    g_port_count = 0; g_inb_count = 0; g_last_inb_port = 0; g_lsr_ready = 1;
    memset(g_port_bytes, 0, sizeof g_port_bytes);
    memset(g_port_addrs, 0, sizeof g_port_addrs);
}

/* Bytes that reached the DATA register, as a C string. */
static const char *port_text(void) {
    static char out[PORTLOG_MAX + 1];
    int n = 0;
    for (int i = 0; i < g_port_count && i < PORTLOG_MAX; i++)
        if (g_port_addrs[i] == SERIAL_COM1_BASE) out[n++] = g_port_bytes[i];
    out[n] = '\0';
    return out;
}

/* ─── stubs for what kernel_io.c pulls in ────────────────────────────────── */
/* The VGA mirror is deliberately given a RECORDING stub rather than an empty
 * one: "the panic path must not touch VGA driver state" is a property worth
 * asserting, and an empty stub would make a violation invisible. */
static int g_vga_calls;
int  vga_is_ready(void)      { g_vga_calls++; return 1; }
void vga_putchar(char c)     { (void)c; g_vga_calls++; }
void vga_init(void)          { }
void vga_clear(void)         { }
void vga_print(const char *s){ (void)s; g_vga_calls++; }

int  smp_cpu_id(void)        { return 0; }
int  smp_cpu_count(void)     { return 1; }
void smp_uniprocessor_tick(void) { }
/* POSIX-Environments E1: read_line()'s idle point yields the CPU to the unified
 * boot's Ring-3 work. Nothing to schedule on the host and this test drives the
 * panic verbs, so the documented no-op is the accurate stub. */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

/* console_feed() lives in kernel/console.c and is not under test here.
 * Stubbed to "no line completed" so serial_console_poll() links; a stub that
 * claimed a line was ready would make read_line() behave differently from the
 * real thing, which is the failure mode this project has hit repeatedly --
 * a stub more capable than the thing it replaces turns a real bug into a
 * passing test. */
int console_feed(char c, char *out, size_t cap) {
    (void)c; (void)out; (void)cap; return 0;
}
void console_reset_line(void) { }

int main(void) {
    printf("kernel_panic_output_host_test\n");
    printf("=============================\n\n");

    char cap[512];

    /* ── 1. the property the whole file exists for ─────────────────────── */
    printf("1. panic output escapes an ACTIVE capture buffer\n");
    port_reset();
    memset(cap, 0, sizeof cap);
    kernel_serial_capture_start(cap, sizeof cap);
    kernel_panic_puts("PANIC-ESCAPES");
    size_t captured = kernel_serial_capture_stop();

    CHECK(strcmp(port_text(), "PANIC-ESCAPES") == 0,
          "bytes reached the UART data register");
    CHECK(captured == 0,
          "capture_len stayed 0 -- nothing was diverted into the buffer");
    CHECK(cap[0] == '\0',
          "the capture buffer itself is still empty");

    /* ── 2. the production port, asserted where relocating it cannot hide ─ */
    printf("\n2. the port numbers are the production ones\n");
    CHECK(SERIAL_COM1_BASE == 0x3F8u, "SERIAL_COM1_BASE is COM1 (0x3F8)");
    port_reset();
    kernel_panic_putchar('Z');
    CHECK(g_port_count == 1 && g_port_addrs[0] == 0x3F8u,
          "the data byte went to 0x3F8, not some other port");
    CHECK(g_last_inb_port == 0x3F8u + 5u,
          "the status poll read the line-status register at base+5");

    /* ── 3. the control that makes check 1 meaningful ──────────────────── */
    /* If this fails, the harness cannot observe the capture buffer at all and
     * every "went to the port" assertion above is vacuous -- true because
     * nothing could ever have shown otherwise. */
    printf("\n3. CONTROL: the ordinary path still goes INTO the capture\n");
    port_reset();
    memset(cap, 0, sizeof cap);
    kernel_serial_capture_start(cap, sizeof cap);
    kernel_serial_print("ORDINARY");
    captured = kernel_serial_capture_stop();
    CHECK(captured == 8 && strcmp(cap, "ORDINARY") == 0,
          "kernel_serial_print() filled the capture buffer");
    CHECK(strcmp(port_text(), "") == 0,
          "and wrote NOTHING to the port -- the two paths are distinguishable");

    /* ── 4. no VGA state touched ───────────────────────────────────────── */
    printf("\n4. the panic path touches no driver state\n");
    port_reset();
    g_vga_calls = 0;
    kernel_panic_puts("NO-VGA");
    CHECK(g_vga_calls == 0,
          "vga_is_ready()/vga_putchar() were never called");

    /* ── 5. newline translation ────────────────────────────────────────── */
    printf("\n5. formatting\n");
    port_reset();
    kernel_panic_puts("a\nb");
    CHECK(strcmp(port_text(), "a\r\nb") == 0, "'\\n' is sent as CR LF");

    port_reset();
    kernel_panic_puts(NULL);
    CHECK(strcmp(port_text(), "(null)") == 0, "NULL prints as (null), not a crash");

    port_reset();
    kernel_panic_hex64(0x0000200000000000ULL);
    CHECK(strcmp(port_text(), "0x0000200000000000") == 0,
          "hex64 is 0x + 16 zero-padded digits");

    port_reset();
    kernel_panic_hex64(0xDEADBEEFCAFEF00DULL);
    CHECK(strcmp(port_text(), "0xdeadbeefcafef00d") == 0,
          "hex64 digits a-f are lowercase and correct");

    port_reset();
    kernel_panic_dec(0);
    CHECK(strcmp(port_text(), "0") == 0, "dec 0 prints as 0");

    port_reset();
    kernel_panic_dec(212864);
    CHECK(strcmp(port_text(), "212864") == 0,
          "dec prints digits in order (the stack-overflow byte count)");

    /* ── 6. the bounded wait ───────────────────────────────────────────── */
    /* kernel_serial_putchar() spins forever on this bit. On a panic path that
     * turns "the kernel told you what happened" into "the kernel hung telling
     * you", which is strictly worse: the first is a diagnosis, the second is
     * indistinguishable from the hang being reported. */
    printf("\n6. the transmitter wait is bounded\n");
    port_reset();
    g_lsr_ready = 0;                     /* UART never reports ready */
    kernel_panic_putchar('X');
    CHECK(g_port_count == 1 && port_text()[0] == 'X',
          "the character is written even when the UART never drains");
    CHECK(g_inb_count > 1,
          "and the poll really happened -- it did not blast without checking");

    port_reset();
    g_lsr_ready = 1;
    kernel_panic_putchar('Y');
    CHECK(g_inb_count == 1,
          "a ready UART is polled exactly once, not spun on");

    /* ── 7. kernel_panic() itself ──────────────────────────────────────── */
    /* kernel_panic() called kernel_serial_print(), so every panic raised
     * during a shell command was swallowed too. It cannot be called here -- it
     * halts -- so assert the property that made it wrong: the message text is
     * emitted by the escaping path. This check is weaker than the others and
     * is labelled so rather than dressed up. */
    printf("\n7. kernel_panic()'s banner uses the escaping path\n");
    port_reset();
    memset(cap, 0, sizeof cap);
    kernel_serial_capture_start(cap, sizeof cap);
    kernel_panic_puts("\n[KERNEL PANIC] ");
    kernel_panic_puts("test message");
    kernel_panic_puts("\n-- System Halted --\n");
    captured = kernel_serial_capture_stop();
    CHECK(strstr(port_text(), "[KERNEL PANIC] test message") != NULL,
          "the panic banner reaches the wire through an active capture");
    CHECK(captured == 0, "and none of it landed in the buffer");

    printf("\n=============================\n");
    printf("passed %d, failed %d\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
