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
 * The fix has three halves, all asserted here (the ownership interlock of
 * kernel_io.h — the demo OWNS the port while MCR bit 4 is set):
 *   1. While loopback is on, the poll does NOT touch the RX FIFO — the
 *      bytes the demo deliberately leaves in RBR hold the IRQ4 line at a
 *      known level, and draining them destroys the edges it counts
 *      (caught live: the old discard rule turned phase 2 into 0/10 and
 *      manufactured the phantom stuck-driver ch=leak FAIL).
 *   2. Kernel TX does not REACH THE WIRE while the demo owns the port —
 *      every TX byte would loop back as a foreign RX byte (and never get
 *      out anyway; QEMU internalizes loopback TX before the host chardev).
 *      Deferred, NOT destroyed (test 6, and the overflow count in test 7):
 *      a window belongs to the port, not to the writer, so discarding here
 *      threw away other processes' lines — measured: both tenant POSIX
 *      sidecars lost their whole boot diagnostic batch to the irqtest's
 *      serial window, and the E3 boot check failed on a machine that had
 *      reported the property it asserts. An absence is not a measurement.
 *   3. When the ownership clears, any residue is discarded AND the partial
 *      line is reset, so the next real input starts a fresh, clean line.
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
/* POSIX-Environments E1: read_line()'s idle point yields the CPU to the unified
 * boot's Ring-3 work. No control plane exists on the host, and this test drives
 * the poll seam rather than that loop, so the documented no-op is the accurate
 * stub (kernel/process.c is the real implementation). */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

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
static char     g_cap[8192];   /* must hold a whole window's replayed batch:
                                 * the defer buffer is 2 KiB (test 7 asserts the
                                 * overflow report lands after all of it) */
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

    /* ═══ 3: loopback hands the port to the demo — poll leaves it alone ═ */
    printf("\n-- 3: loopback (MCR bit 4) is the demo's port, not console input --\n");
    {
        uart_reset();
        g_mcr_loopback = 1;
        uart_queue("AAAA");
        cap_start();
        out[0] = '\1'; out[1] = '\0';
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "loopback bytes never complete a line");
        CHECK(g_rbr_reads == 0,
              "*** the FIFO is left INTACT for the probe (ownership) ***");
        CHECK(out[0] == '\1', "the out buffer is untouched");
        kernel_serial_putchar('x');
        cap_stop_expect("",
                        "kernel TX does not reach the wire while the demo owns the port");
        /* That byte is DEFERRED, not dropped -- see test 6 for the delivery and
         * test 4 for the replay at release. The distinction matters: this write
         * used to be destroyed, and a boot guard reading the serial log cannot
         * tell a destroyed line from a line that was never written. */
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
        /* The RX straggler is not echoed -- and the 'x' test 3 wrote while the
         * demo held the port is replayed here, at the release, captured since
         * the capture is still armed. Byte-exact: nothing added, nothing lost. */
        cap_stop_expect("x",
                        "the window's deferred byte is replayed at release, not lost");

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
              "loopback bytes are left to the demo mid-line");
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

    /* ═══ 6: a whole line written inside a window arrives, whole ════════ */
    /* The E3 multi-instance boot failed on exactly this: two tenant sidecars
     * printed their boot diagnostics inside the irqtest's serial window, the
     * kernel destroyed every byte, and the boot check reported a machine that
     * had in fact reported the property it asserts. The contract this pins:
     * silent ON the wire during the demo, complete AFTER it. */
    printf("\n-- 6: output during a window is deferred, then delivered --\n");
    {
        const char* line = "[POSIX] aero state=00000000\n";
        uart_reset();
        g_mcr_loopback = 1;
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "the poll takes ownership when the demo turns loopback on");
        cap_start();
        /* Through kernel_serial_print(), the path a sidecar's klog takes
         * (syscall 165): it maps \n to \r\n, and that mapping happens INSIDE the
         * window here, so the deferred bytes are already wire-shaped. */
        kernel_serial_print(line);
        CHECK(kernel_serial_capture_stop() == 0,
              "*** nothing reaches the wire while the window is open ***");

        g_mcr_loopback = 0;
        cap_start();
        CHECK(serial_console_poll(out, sizeof(out)) == 0,
              "the release poll completes no console line");
        size_t n = kernel_serial_capture_stop();
        /* kernel_serial_putchar maps \n to \r\n, so the replay is 2 bytes longer. */
        CHECK(n == strlen(line) + 1,
              "*** the whole deferred line is replayed at release ***");
        CHECK(strcmp(g_cap, "[POSIX] aero state=00000000\r\n") == 0,
              "*** byte-for-byte, in order, with its line ending ***");
    }

    /* ═══ 7: the defer buffer is bounded, and says so ═══════════════════ */
    /* A writer that exceeds the window's buffer must not grow kernel .bss
     * without limit, and must not lose bytes SILENTLY — the overflow count is
     * reported on the way out, where a boot log can see it. */
    printf("\n-- 7: an overflowing window is bounded and reported --\n");
    {
        char big[2100];
        memset(big, 'A', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        uart_reset();
        g_mcr_loopback = 1;
        CHECK(serial_console_poll(out, sizeof(out)) == 0, "ownership taken");
        cap_start();
        kernel_serial_print(big);          /* 2099 bytes into a 2048-byte window */
        CHECK(kernel_serial_capture_stop() == 0, "still nothing on the wire");
        g_mcr_loopback = 0;
        cap_start();
        CHECK(serial_console_poll(out, sizeof(out)) == 0, "release poll");
        size_t n = kernel_serial_capture_stop();
        CHECK(n > 2048,
              "*** the 2048 deferred bytes are replayed, plus the overflow report ***");
        CHECK(strncmp(g_cap, big, 2048) == 0,
              "*** in order, capped at the buffer size (nothing beyond it) ***");
        CHECK(strstr(g_cap, "overflow") != NULL,
              "*** and the loss is STATED, not silent ***");
        CHECK(strstr(g_cap, "0x0000000000000033") != NULL,
              "*** with the exact number of bytes it could not hold (51) ***");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}