/*
 * auth_host_test.c — Gap Remediation Phase E verification: a standalone
 * host-buildable test for kernel/auth.c's new TTL-expiry logic, linked
 * against the REAL, unmodified kernel/auth.c -- not a reimplementation.
 *
 * kernel/timer.c itself isn't linked here (it depends on the LAPIC/IDT
 * arch layer, unavailable/undesirable on a host build) -- this test defines
 * kernel_tick_counter directly, exactly as timer.c itself does, and drives
 * it forward by hand to simulate uptime passing. That's the same "the real
 * value under test is provided by a plain global this test controls, not a
 * hardware-driven one" approach every prior phase's host tests already use
 * for NVMe (fake in-memory block store) and TSC-seeded token generation.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/auth_host_test tests/auth_host_test.c kernel/auth.c \
 *       kernel/secure_api.c
 *   /tmp/auth_host_test
 *
 * kernel/secure_api.c added for Architectural Phase 4 (account passwords --
 * auth.c now calls its derive_user_key()). This comment previously fell out
 * of sync with the actual command used to verify that phase by hand; caught
 * by tests/run_all.sh (Operational Phase A) actually running this comment's
 * command instead of a person's memory of it, which is the whole reason
 * that script reads the command from here rather than hardcoding its own copy.
 */
#include "kernel/auth.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* kernel_tick_counter — real definition, matching kernel/timer.c's own
 * (not linked here, see file header comment). This test advances it by
 * hand to simulate the LAPIC timer firing. */
volatile uint64_t kernel_tick_counter = 0;

/* ─── Stubs for auth.c's I/O side effects — this test only cares about the
 * real token-registry logic, not what gets printed to a serial console
 * that doesn't exist on a host build. ──────────────────────────────────── */
void kernel_serial_putchar(char c) { (void)c; }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_print_hex64(uint64_t v) { (void)v; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* Architectural Phase 4: auth.c now links kernel/secure_api.c for
 * derive_user_key() (the real password-derivation logic under test, not a
 * reimplementation -- same "link the real file" convention this test
 * already follows for auth.c itself). secure_api.c's OTHER function,
 * sys_sls_secure_seal(), is never called here but still needs its own two
 * dependencies to link; stubbed with matching real signatures rather than
 * pulling in the object catalog they'd otherwise require. */
uint32_t kernel_get_current_thread_id(void) { return 0; }
int verify_expanded_matrix_access(uint32_t uid, uint32_t gid,
                                   uint64_t object_id, uint32_t needed_mask) {
    (void)uid; (void)gid; (void)object_id; (void)needed_mask;
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    auth_init();   // registers the 4 demo accounts, created_tick = kernel_tick_counter (0)

    /* ── Scenario 1: demo accounts validate correctly right after boot. ──── */
    uint32_t uid; SLSRole role;
    CHECK(auth_validate_token("deadbeef01234567cafebabe76543210", &uid, &role) == 1 &&
          uid == 1000 && role == ROLE_DB_ADMIN,
          "s1: dave's fixed demo token validates as uid=1000 DB_ADMIN");
    CHECK(auth_validate_token("deadc0de9988776655443322aabbccdd", &uid, &role) == 1 &&
          uid == 1003 && role == ROLE_GUEST,
          "s1: guest's fixed demo token validates as uid=1003 GUEST");

    /* ── Scenario 2: an unknown token fails cleanly, out params reset. ──── */
    uid = 999; role = ROLE_DB_ADMIN;
    CHECK(auth_validate_token("0000000000000000000000000000000000", &uid, &role) == 0 &&
          uid == 0 && role == ROLE_GUEST,
          "s2: an unknown token fails validation, out_uid=0 out_role=GUEST");

    /* ── Scenario 3: a freshly runtime-issued token validates immediately
     * (elapsed == 0, well under the TTL). ───────────────────────────────── */
    struct AuthCreateRequest req;
    strcpy(req.email, "newuser@example.com");
    req.uid = 42; req.role = ROLE_APP_USER;
    req.password_len = 0;  // Architectural Phase 4: no password for this scenario's account
    char tok[AUTH_TOKEN_LEN + 1];
    CHECK(auth_create_token(&req, tok) == 1, "s3: auth_create_token() issues a fresh token");
    CHECK(auth_validate_token(tok, &uid, &role) == 1 && uid == 42 && role == ROLE_APP_USER,
          "s3: the fresh token validates immediately as uid=42 APP_USER");

    /* ── Scenario 4: advance kernel_tick_counter past the TTL -- the
     * runtime-issued token now fails validation, but the demo tokens
     * (no_expiry) still pass unchanged. This is the core Phase E property:
     * TTL applies to session tokens, never to standing demo credentials. ── */
    kernel_tick_counter += AUTH_TOKEN_TTL_TICKS + 1;
    CHECK(auth_validate_token(tok, &uid, &role) == 0,
          "s4: the runtime token now fails validation once past AUTH_TOKEN_TTL_TICKS");
    CHECK(auth_validate_token("deadbeef01234567cafebabe76543210", &uid, &role) == 1 &&
          uid == 1000 && role == ROLE_DB_ADMIN,
          "s4: dave's demo token still validates fine at the same tick -- no_expiry holds");

    /* ── Scenario 5: re-issuing a token for the same (now-expired) email
     * mints a genuinely NEW token value, not the same dead one -- proving
     * the au_token_expired() reuse-path fix, not just the validate-path
     * fix. The old token must now be permanently dead even for the same
     * email. ─────────────────────────────────────────────────────────────── */
    char tok2[AUTH_TOKEN_LEN + 1];
    CHECK(auth_create_token(&req, tok2) == 1, "s5: re-issuing for the expired email succeeds");
    CHECK(strcmp(tok, tok2) != 0, "s5: the re-issued token is a genuinely different value, not the stale one");
    CHECK(auth_validate_token(tok2, &uid, &role) == 1 && uid == 42 && role == ROLE_APP_USER,
          "s5: the new token validates fine at the current tick");
    CHECK(auth_validate_token(tok, &uid, &role) == 0,
          "s5: the OLD (expired) token is still dead -- reissuing didn't resurrect it");

    /* ── Scenario 6: auth_http_issue() (the POST /auth/token path) shows the
     * same behavior end-to-end -- a brand-new email gets a working GUEST
     * token, and re-issuing before expiry returns the SAME token (real
     * reuse, not wasteful reissue every call). ──────────────────────────── */
    char resp[256];
    int n = auth_http_issue("fresh@example.com", "", resp, (int)sizeof(resp));
    CHECK(n > 0 && strstr(resp, "\"ok\":true") && strstr(resp, "\"role\":\"GUEST\""),
          "s6: auth_http_issue() for a brand-new email returns an ok GUEST token, no password needed");
    char resp2[256];
    int n2 = auth_http_issue("fresh@example.com", "", resp2, (int)sizeof(resp2));
    CHECK(n2 > 0 && strcmp(resp, resp2) == 0,
          "s6: re-issuing for the same email before expiry returns the identical response (real reuse)");

    /* ── Scenario 7: auth_revoke_by_email() still works exactly as before --
     * Phase E didn't touch this path, confirming no regression. ─────────── */
    char freshtok[AUTH_TOKEN_LEN + 1];
    const char* tp = strstr(resp, "\"token\":\"") + 9;
    memcpy(freshtok, tp, AUTH_TOKEN_LEN); freshtok[AUTH_TOKEN_LEN] = '\0';
    CHECK(auth_validate_token(freshtok, &uid, &role) == 1, "s7: the fresh@example.com token validates before revoke");
    CHECK(auth_revoke_by_email("fresh@example.com") == 1, "s7: revoke succeeds for a known email");
    CHECK(auth_validate_token(freshtok, &uid, &role) == 0, "s7: the same token fails validation immediately after revoke");
    CHECK(auth_revoke_by_email("nobody@example.com") == 0, "s7: revoke of an unregistered email returns 0");

    /* ── Scenario 8 (Architectural Phase 4): POST /auth/token now requires
     * a matching password for any account that has one set -- previously
     * knowing a live account's email alone was enough to get its real
     * token back. Covers: a fresh account created WITH a password, and one
     * of auth_init()'s own standing demo accounts (which now also carry
     * fixed demo passwords). ─────────────────────────────────────────────── */
    struct AuthCreateRequest sreq;
    strcpy(sreq.email, "secured@example.com");
    sreq.uid = 55; sreq.role = ROLE_APP_USER;
    strcpy(sreq.password, "hunter2");
    sreq.password_len = 7;
    char stok[AUTH_TOKEN_LEN + 1];
    CHECK(auth_create_token(&sreq, stok) == 1, "s8: auth_create_token() with a password succeeds");

    char sresp[256];
    int sn = auth_http_issue("secured@example.com", "wrong-password", sresp, (int)sizeof(sresp));
    CHECK(sn > 0 && strstr(sresp, "\"error\"") && !strstr(sresp, "\"ok\":true"),
          "s8: wrong password is rejected with a JSON error, no token leaked");

    char sresp2[256];
    int sn2 = auth_http_issue("secured@example.com", "", sresp2, (int)sizeof(sresp2));
    CHECK(sn2 > 0 && strstr(sresp2, "\"error\""),
          "s8: no password supplied at all is rejected the same way");

    char sresp3[256];
    int sn3 = auth_http_issue("secured@example.com", "hunter2", sresp3, (int)sizeof(sresp3));
    CHECK(sn3 > 0 && strstr(sresp3, "\"ok\":true") && strstr(sresp3, stok),
          "s8: the correct password returns the real token");

    char dresp[256];
    int dn = auth_http_issue("dave@gridworkz.com", "not-daves-password", dresp, (int)sizeof(dresp));
    CHECK(dn > 0 && strstr(dresp, "\"error\""),
          "s8: dave's standing demo account also now rejects a wrong password -- "
          "the exact gap that motivated this phase (email alone used to be enough)");
    char dresp2[256];
    int dn2 = auth_http_issue("dave@gridworkz.com", "demo-dave", dresp2, (int)sizeof(dresp2));
    CHECK(dn2 > 0 && strstr(dresp2, "\"ok\":true") && strstr(dresp2, "deadbeef01234567cafebabe76543210"),
          "s8: dave's correct fixed demo password returns his real fixed demo token");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
