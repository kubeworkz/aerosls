/*
 * serial_console_loopback_host_test.c — serial_console_poll()'s UART
 * loopback discipline. Links the REAL kernel/kernel_io.c AND the REAL
 * kernel/console.c, with port I/O substituted by
 * tests/kernel_io_panic_port.h's recording seam (same pattern as
 * kernel_panic_output_host_test.c).
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * irqtest's serial phase drives the 16550 in LOOPBACK (MCR bit 4): every
 * THR write loops back into the RBR. serial_console_poll() polls the RBR
 * every tick and raced the demo's own drains, absorbing stray loopback
 * bytes into console_feed()'s line editor. The first REAL line typed after
 * boot then merged with that stale partial line — caught live on target as
 * the sidecar receiving "Acaps" instead of "caps", which exec'd as a
 * nonexistent command and silently dropped the first typed input after
 * every full irqtest death/respawn boot.
 *
 * The fix has two halves, both asserted here:
 *   1. While loopback is on, the poll DISCARDS the RX FIFO — loopback
 *      bytes are device-internal traffic, never console input.
 *   2. When loopback clears, any residue is discarded AND the partial line
 *      is reset, so the next real input starts a fresh, clean line.
 *
 * The UART model below is deliberately small: an RX queue, the MCR
 * loopback bit, and an LSR that reports data-ready only while the queue is
 * nonempty. Nothing else about a 16550 matters to the code under test —
 * and a stub that emulated more than the code reads would risk the trap
 * this project has hit repeatedly: a stub more capable than the thing it
 * replaces turning a real bug into a passing test.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/serial_console_loopback_host_test \
 *       tests/serial_console_loopback_host_test.c \
 *       kernel/kernel_io.c kernel/console.c
 *   /tmp/serial_console_loopback_host_test
 */
#include "kernel/kernel_io.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 is not cosmetic -- tests/run_all.sh counts checks \
     * with grep -c '^ok:'. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* ─── stubs for what kernel_io.c pulls in beyond the UART seam ──────────── */
int  vga_is_ready(void)        { return 0; }
void vga_putchar(char c)       { (void)c; }
void vga_init(void)            { }
void vga_clear(void)           { }
void vga_print(const char *s)  { (void)s; }
int  smp_cpu_id(void)          { return 0; }
int  smp_cpu_count(void)       { return 1; }
void smp_uniprocessor_tick(void) { }

/* ─── the UART model (kio_test_* is declared by the force-included seam) ─── */
static uint8_t  g_rx[16];        /* bytes a host write has queued */
static int      g_rx_n, g_rx_i;  /* queue bounds */
static int      g_mcr_loopback;  /* MCR bit 4 */
static int      g_lsr_reads;     /* diagnostics: what the code under test */
static int      g_mcr_reads;     /*   actually probed, in order of the  */
static int      g_rbr_reads;     /*   asserts below */

void kio_test_outb(uint16_t port, uint8_t val) {
    (void)port; (void)val;   /* TX is not what this test asserts */
}

uint8_t kio_test_inb(uint16_t port) {
    switch ((int)(port - SERIAL_COM1_BASE)) {
    case 0: /* RBR: pop the next queued byte */
        g_rbr_reads++;
        return (g_rx_i < g_rx_n) ? g_rx[g_rx_i++] : 0;
    case 4: /* MCR: the loopback bit is all the poll reads */
        g_mcr_reads++;
        return g_mcr_loopback ? 0x10 : 0x00;
    case 5: /* LSR: data-ready only while the queue is nonempty; bit 5
             * (THR-empty) always set so TX-side waits never spin */
        g_lsr_reads++;
        return 0x20 | ((g_rx_i < g_rx_n) ? 0x01 : 0x00);
    default:
        return 0;
    }
}

static void uart_reset(void) {
    g_rx_n = g_rx_i = 0;
    g_mcr_loopback = 0;
    g_lsr_reads = g_mcr_reads = g_rbr_reads = 0;
}

static void uart_queue(const char* s) {
    for (; *s && g_rx_n < (int)sizeof(g_rx); s++) g_rx[g_rx_n++] = (uint8_t)*s;
}

/* Echo capture: kernel_serial_putchar (in the REAL kernel_io.c) diverts
 * into the capture buffer while active, exactly as the shell-capture path
 * uses it. This is how "was the byte echoed to the operator" is asserted —
 * echo is the operator's only feedback about which keystrokes were
 * accepted, so bytes discarded silently must not echo either. */
static char     g_cap[256];
static void     cap_start(void) { kernel_serial_capture_start(g_cap, sizeof g_cap); }
static void     cap_stop_expect(const char* expect, const char* msg) {
    size_t n = kernel_serial_capture_stop();
    g_cap[n < sizeof(g_cap) - 1 ? n : sizeof(g_cap) - 1] = '\0';
    CHECK(strcmp(g_cap, expect) == 0, msg);
}

int main(void) {
    static char out[64];
    printf("=== serial_console_poll loopback discipline ===\n\n");

    /* ═══ 1: normal operation is unchanged ══════════════════════════════ */
    printf("-- 1: a real line still assembles --\n");
    {
        uart_reset();
        cap_start();
        uart_queue("caps\n");
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 4,
              "a completed line returns its length");
        CHECK(strcmp(out, "caps") == 0,
              "*** and the line is exactly what was typed ***");
        cap_stop_expect("caps\r\n", "echo is per byte + terminator, as the editor does");
    }

    /* ═══ 2: empty FIFO is a no-op that probes LSR, not RBR ═════════════ */
    printf("\n-- 2: an empty FIFO --\n");
    {
        uart_reset();
        int lsr0 = g_lsr_reads, rbr0 = g_rbr_reads;
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 0, "no data -> no line");
        CHECK(g_lsr_reads > lsr0, "LSR is probed...");
        CHECK(g_rbr_reads == rbr0,
              "*** ...but RBR is NOT read when data-ready is clear ***");
        CHECK(out[0] == '\1', "the out buffer is left untouched");
    }

    /* ═══ 3: loopback discards the FIFO without feeding the editor ═════ */
    printf("\n-- 3: loopback (MCR bit 4) is device traffic, not input --\n");
    {
        uart_reset();
        g_mcr_loopback = 1;
        uart_queue("AAAA");
        cap_start();
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "loopback bytes never complete a line");
        CHECK(g_rbr_reads == 4,
              "*** the FIFO is drained, not left for later ***");
        CHECK(out[0] == '\1', "the out buffer is untouched");
        cap_stop_expect("", "nothing is echoed to the operator");
    }

    /* ═══ 4: loopback clearing drains residue and resets the editor ════ */
    printf("\n-- 4: loopback clearing --\n");
    {
        /* prev_loopback == 1 from test 3; a straggler byte is still in the
         * FIFO when the demo turns loopback off. */
        uart_reset();
        g_mcr_loopback = 0;
        uart_queue("Z");
        cap_start();
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "the transition poll returns nothing");
        CHECK(g_rbr_reads == 1,
              "*** the straggler is discarded, not fed to the editor ***");
        cap_stop_expect("", "and it is not echoed");

        /* Real input after the transition must be clean. */
        uart_queue("caps\n");
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 4,
              "real input assembles normally right after the transition");
        CHECK(strcmp(out, "caps") == 0,
              "*** with no residue merged into it ***");
    }

    /* ═══ 5: the regression — editor pollution across a loopback cycle ══ */
    printf("\n-- 5: the caught-on-target regression --\n");
    {
        /* A partial real line ("X") is sitting in the editor when the demo
         * starts; its loopback bytes must not merge into the next line. */
        uart_reset();
        g_mcr_loopback = 0;
        uart_queue("X");
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "a partial real line is in progress (no line completed)");
        g_mcr_loopback = 1;
        uart_queue("A");
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "loopback bytes are discarded mid-line");
        g_mcr_loopback = 0;
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "the transition resets the editor (the partial line is demo-era residue)");

        cap_start();
        uart_queue("caps\n");
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 4,
              "the first REAL line after the demo assembles");
        CHECK(strcmp(out, "caps") == 0,
              "*** and it is \"caps\", not \"Xcaps\" or \"Acaps\" ***");
        cap_stop_expect("caps\r\n",
                        "its echo shows only the real keystrokes");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}