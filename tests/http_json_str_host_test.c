/*
 * http_json_str_host_test.c — net/http.c's json_str() escape decoding.
 *
 * `json_str()` is static, so this test INCLUDES the real net/http.c rather
 * than reimplementing its parser.  Only json_str() and str_find() survive
 * the link: -ffunction-sections/-fdata-sections plus --gc-sections discard
 * every other HTTP function, so the kernel's network and catalog layers are
 * not needed just to test a string decoder.
 *
 * This is the transport half of the `catalog exec` text-literal fix.  A
 * client correctly encodes one backslash as `\\` in JSON; the environment
 * shell needs that backslash to make `\'` a quote.  If json_str() copies the
 * two JSON bytes through, the shell sees `\\'` and strips the SQL quotes.
 *
 * The four cases are the escapes that matter on that path: escaped
 * backslash, escaped quote, newline, and an escape JSON does not define
 * (kept verbatim, as before).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I net -I kernel -I arch/x86 -I drivers -I user \
 *       -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *       -o /tmp/http_json_str_host_test tests/http_json_str_host_test.c
 *   /tmp/http_json_str_host_test
 */
#include <stdio.h>
#include <string.h>

#include "net/http.c"

static int failures = 0;

static void check(const char* label, const char* json,
                  const char* want, size_t want_len) {
    char out[64];
    memset(out, 0xAA, sizeof out);
    int n = json_str(json, "k", out, (int)sizeof out);
    if (n == (int)want_len && memcmp(out, want, want_len) == 0 &&
        out[want_len] == '\0') {
        printf("ok:   %s\n", label);
        return;
    }

    printf("FAIL: %s: n=%d out=", label, n);
    for (int i = 0; i < n && i < (int)sizeof out; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 32 && c < 127) putchar((int)c);
        else                    printf("\\x%02x", c);
    }
    printf(" (wanted %zu bytes)\n", want_len);
    failures++;
}

int main(void) {
    /* JSON "a\\b" is two backslashes; the decoded value is a\b. */
    check("escaped backslash", "{\"k\":\"a\\\\b\"}", "a\\b",
          sizeof("a\\b") - 1);

    /* JSON "a\"b" must not terminate the string at the escaped quote. */
    check("escaped quote", "{\"k\":\"a\\\"b\"}", "a\"b",
          sizeof("a\"b") - 1);

    /* JSON "\n" is one control character, not the two bytes '\' 'n'. */
    check("newline", "{\"k\":\"a\\nb\"}", "a\nb",
          sizeof("a\nb") - 1);

    /* JSON has no \q; the decoder preserves the backslash and the q. */
    check("unknown escape", "{\"k\":\"a\\qb\"}", "a\\qb",
          sizeof("a\\qb") - 1);

    if (failures) {
        printf("\nhttp_json_str_host_test: %d failed\n", failures);
        return 1;
    }
    printf("\nhttp_json_str_host_test: all cases held\n");
    return 0;
}
