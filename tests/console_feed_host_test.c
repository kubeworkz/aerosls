/*
 * console_feed_host_test.c — the non-blocking console's line editor.
 * Links the REAL kernel/console.c.
 *
 * ─── Why this exists ──────────────────────────────────────────────────────
 * kernel.c enters http_server_run() and never returns when a NIC is
 * present, so sls_shell_loop() -- and with it read_line() -- is unreachable
 * on any networked boot. A clustered node therefore had NO control path:
 * no keyboard driver, and no host port forward available because
 * net/e1000.c binds a single NIC. console_feed() is the line editor that
 * lets the HTTP loop present a prompt between sweeps instead.
 *
 * It has to behave like read_line() while being callable one byte at a
 * time, which is the whole difficulty: read_line() owns the CPU and can
 * loop until ENTER, this cannot. Every editing rule below is checked
 * against what read_line() does, because two consoles on one machine that
 * edit differently is a trap for whoever uses the second one.
 *
 * The rule worth the most is the overflow one. A line past the limit is
 * refused WITHOUT echo -- the echo stopping is the operator's only signal,
 * and a command silently truncated to something shorter that still parses
 * is the dangerous outcome. `partition destroy 12` truncated to
 * `partition destroy 1` is a different, valid, destructive command.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/console_feed_host_test \
 *       tests/console_feed_host_test.c kernel/console.c
 *   /tmp/console_feed_host_test
 */
#include "kernel/kernel_io.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* console.c's ONLY external call. Capturing it is how "what was echoed"
 * is checked -- and echo is not cosmetic here: it is the sole feedback an
 * operator gets about which keystrokes were accepted.
 *
 * This is also why the editor lives in console.c and not kernel_io.c: that
 * file defines its own `static inline` inb()/outb(), so linking it here
 * would run real port I/O instructions and fault in a host process. (Found
 * the direct way -- the first version of this test segfaulted.) */
static char echoed[4096];
static int  echoed_len = 0;
static void echo_reset(void) { echoed_len = 0; echoed[0] = '\0'; }

void kernel_serial_putchar(char c) {
    if (echoed_len < (int)sizeof(echoed) - 1) echoed[echoed_len++] = c;
    echoed[echoed_len] = '\0';
}

/* Feeds a whole string. Returns 1 if a line completed, with it in `out`. */
static int feed_str(const char* s, char* out, size_t cap) {
    int done = 0;
    for (const char* p = s; *p; p++) if (console_feed(*p, out, cap)) done = 1;
    return done;
}

int main(void) {
    static char line[256];
    printf("=== The console the HTTP loop drives ===\n\n");

    /* ═══ 1: a line is assembled across calls ═════════════════════════════ */
    printf("-- 1: one byte at a time --\n");
    {
        line[0] = '\1';
        CHECK(!console_feed('l', line, sizeof(line)), "a character alone does not end a line");
        CHECK(!console_feed('s', line, sizeof(line)), "...nor a second");
        CHECK(console_feed('\n', line, sizeof(line)), "*** LF completes the line ***");
        CHECK(!strcmp(line, "ls"), "*** and the assembled line is what was typed ***");

        CHECK(feed_str("help\r", line, sizeof(line)), "CR completes a line too");
        CHECK(!strcmp(line, "help"), "...with the terminator stripped");
    }

    /* ═══ 2: state resets between lines ═══════════════════════════════════ */
    printf("\n-- 2: consecutive lines --\n");
    {
        feed_str("first\n", line, sizeof(line));
        feed_str("second\n", line, sizeof(line));
        CHECK(!strcmp(line, "second"),
              "*** the second line is not the first with more on the end ***");

        CHECK(console_feed('\n', line, sizeof(line)), "a bare ENTER completes");
        CHECK(line[0] == '\0',
              "*** ...as an EMPTY line -- the shell treats that as a no-op prompt ***");
    }

    /* ═══ 3: editing matches read_line() ══════════════════════════════════ */
    printf("\n-- 3: backspace --\n");
    {
        feed_str("lz\b", line, sizeof(line));
        console_feed('s', line, sizeof(line));
        console_feed('\n', line, sizeof(line));
        CHECK(!strcmp(line, "ls"), "backspace removes the previous character");

        feed_str("ab\x7f", line, sizeof(line));
        console_feed('\n', line, sizeof(line));
        CHECK(!strcmp(line, "a"), "DEL (0x7F) does the same, as read_line() has it");

        echo_reset();
        console_feed('\b', line, sizeof(line));
        CHECK(echoed_len == 0,
              "*** backspace on an EMPTY line echoes nothing -- erasing the "
              "prompt itself would be worse than ignoring it ***");
        console_feed('\n', line, sizeof(line));
    }

    /* ═══ 4: overflow refuses rather than truncates silently ══════════════ */
    printf("\n-- 4: a line past the limit --\n");
    {
        char big[600];
        memset(big, 'x', sizeof(big)); big[sizeof(big) - 1] = '\0';
        echo_reset();
        feed_str(big, line, sizeof(line));
        int echoed_chars = echoed_len;
        console_feed('\n', line, sizeof(line));

        CHECK(strlen(line) < 256, "*** the line cannot overflow its buffer ***");
        CHECK(echoed_chars < (int)strlen(big),
              "*** echo STOPS at the limit -- that is the operator's only signal "
              "that the rest was refused ***");
        CHECK(echoed_chars == (int)strlen(line),
              "*** exactly what was echoed is what was kept: nothing accepted "
              "silently, nothing shown that was dropped ***");

        /* The dangerous case, spelled out: a truncated command must not be a
         * different valid command that the operator did not intend. Here the
         * refusal is visible, so it cannot be mistaken for acceptance. */
        for (int i = 0; i < 300; i++) console_feed('y', line, sizeof(line));
        console_feed('\n', line, sizeof(line));
        CHECK(strlen(line) == 255, "the cap is the buffer, consistently");
    }

    /* ═══ 5: control bytes are not smuggled into a command ════════════════ */
    printf("\n-- 5: control bytes --\n");
    {
        echo_reset();
        console_feed('a', line, sizeof(line));
        console_feed('\t', line, sizeof(line));    /* TAB */
        console_feed(0x1B, line, sizeof(line));    /* ESC */
        console_feed(0x03, line, sizeof(line));    /* Ctrl-C */
        console_feed('b', line, sizeof(line));
        console_feed('\n', line, sizeof(line));
        CHECK(!strcmp(line, "ab"),
              "*** TAB, ESC and Ctrl-C are dropped, not embedded in the command ***");
        CHECK(!strchr(echoed, '\x1b'),
              "...and an escape sequence is never echoed back to the terminal");
    }

    /* ═══ 6: a caller's small buffer is respected ═════════════════════════ */
    printf("\n-- 6: bounds on copy-out --\n");
    {
        char small[8];
        feed_str("abcdefghijklmnop\n", small, sizeof(small));
        CHECK(strlen(small) == 7, "*** the copy is bounded by the CALLER's cap ***");
        CHECK(small[7] == '\0', "...and NUL-terminated");

        /* Truncating on copy-out must still clear the accumulator, or the
         * next line would begin with the tail of this one. */
        feed_str("zz\n", small, sizeof(small));
        CHECK(!strcmp(small, "zz"),
              "*** ...and the next line is clean, not the leftover tail ***");

        CHECK(!console_feed('\n', NULL, 16), "a NULL out is safe");
        CHECK(!console_feed('\n', small, 0), "a zero cap is safe");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
