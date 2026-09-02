/*
 * console.c — the non-blocking console's line editor.
 *
 * Separate from kernel_io.c on purpose. That file defines its own
 * `static inline` inb()/outb(), so every function in it executes real port
 * I/O and faults immediately in a host process -- the editing rules would
 * be untestable there. Nothing below touches a port: echo goes through
 * kernel_serial_putchar(), which a host test can stub.
 *
 * Same split boot_params.c uses: a pure state machine, and a thin hardware
 * wrapper (serial_console_poll(), in kernel_io.c) around it.
 *
 * Why a non-blocking editor exists at all: kernel.c enters
 * http_server_run() and never returns when a NIC is present, so
 * sls_shell_loop() and its blocking read_line() are unreachable on a
 * networked boot. Without this a clustered node has no control path --
 * there is no keyboard driver, and no host port forward is possible
 * because net/e1000.c binds a single NIC.
 */
#include "kernel_io.h"

#define CONSOLE_LINE_MAX 256

static char console_buf[CONSOLE_LINE_MAX];
static int  console_len = 0;

int console_feed(char c, char* out, size_t cap) {
    if (!out || cap == 0) return 0;

    if (c == '\r' || c == '\n') {
        kernel_serial_putchar('\r');
        kernel_serial_putchar('\n');
        size_t n = (size_t)console_len;
        if (n > cap - 1) n = cap - 1;
        for (size_t i = 0; i < n; i++) out[i] = console_buf[i];
        out[n] = '\0';
        console_len = 0;          /* reset even when truncated on copy-out */
        return 1;
    }

    if (c == 0x7F || c == '\b') {
        if (console_len > 0) {
            console_len--;
            kernel_serial_putchar('\b');
            kernel_serial_putchar(' ');
            kernel_serial_putchar('\b');
        }
        /* Backspace on an empty line does nothing and echoes nothing --
         * erasing the prompt itself would be worse than ignoring it. */
        return 0;
    }

    if (c >= 0x20 && console_len < CONSOLE_LINE_MAX - 1) {
        kernel_serial_putchar(c);   /* echo only what was actually taken */
        console_buf[console_len++] = c;
    }
    /* A printable byte past the limit, or a control byte, is dropped
     * WITHOUT echo. Echoing it would tell the operator a character was
     * accepted that will not be in the command. */
    return 0;
}

/* Drop the partial line (no echo). Used when the UART's loopback mode
 * clears: any bytes absorbed around the demo's loopback edges are
 * device-internal traffic, not console input — mixing them with the next
 * real line silently corrupts the first typed command. */
void console_reset_line(void) {
    console_len = 0;
}
