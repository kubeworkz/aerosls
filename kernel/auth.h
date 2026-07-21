#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include "object_catalog.h"
#include "timer.h"   // Gap Remediation Phase E -- kernel_tick_counter, see below

// ─── Token constants ──────────────────────────────────────────────────────────
#define AUTH_MAX_TOKENS  32
#define AUTH_EMAIL_LEN   64
#define AUTH_TOKEN_LEN   32   // 32 hex chars = 128 bits

// Gap Remediation Phase E: token TTL, expressed in LAPIC timer ticks rather
// than TSC cycles. This codebase has no calibrated TSC-to-wallclock ratio
// anywhere (dashboard.c's own "average_fault_latency_cycles" is deliberately
// left as raw, uncalibrated cycles -- fine for a relative latency metric,
// useless as an absolute expiry threshold across different CPUs). timer.c's
// kernel_tick_counter, by contrast, increments at a documented ~100 Hz
// (init_timer()'s own comment: "fires roughly every 10 ms"), so it's the
// only monotonic counter in the kernel with even an approximate real-time
// meaning. TTL is therefore approximate wall-clock, not exact -- the same
// honesty-over-precision tradeoff init_timer()'s own comment already makes,
// not a new one invented for this phase.
#define AUTH_TOKEN_TTL_TICKS  360000ULL   // ~1 hour at the documented ~100 Hz

// ─── Leaseholder token ────────────────────────────────────────────────────────
// Ties an email address to a uid + role in the AeroSLS capability model.
// The bearer token is included in HTTP requests as "Authorization: Bearer <token>".
struct LeaseToken {
    char     email[AUTH_EMAIL_LEN];
    char     token[AUTH_TOKEN_LEN + 1];  // NUL-terminated 32-char hex string
    uint32_t uid;
    SLSRole  role;
    uint64_t created_tick;  // kernel_tick_counter value at issuance (Phase E; was created_tsc)
    uint8_t  no_expiry;     // Phase E: 1 for the 4 standing auth_init() demo
                             // accounts (deliberate, permanent local-dev
                             // credentials, not runtime session tokens --
                             // see auth_init()'s own comment); 0 for every
                             // token issued at runtime via auth_create_token(),
                             // which always gets real TTL enforcement.
    uint8_t  active;

    // Architectural Phase 4 (docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md):
    // previously POST /auth/token handed back an existing account's live
    // token to anyone who supplied that account's email, no credential
    // check at all. password_key is derive_user_key() (kernel/secure_api.c
    // -- the same password-derived-key primitive `seal` already uses, not
    // a second scheme) applied to "<email>:<password>", not just the raw
    // password -- a poor-man's per-account salt, since derive_user_key()
    // itself has none, so two accounts sharing a password wouldn't
    // otherwise derive the same key. This is NOT a real cryptographic
    // password hash (no salt of its own, fixed non-standard iteration
    // scheme) -- reusing the existing primitive matches this codebase's
    // established honesty-over-false-security convention (see
    // secure_api.c's own comment on sys_sls_secure_seal()) rather than
    // pretending to a security level this kernel doesn't actually have.
    // has_password==0 means "no password ever set" -- auth_http_issue()
    // requires a password match only when has_password==1, so the
    // auto-provisioned anonymous ROLE_GUEST accounts (unknown email hitting
    // POST /auth/token) keep today's passwordless behavior, since they
    // claim no real identity to protect.
    uint32_t password_key[8];
    uint8_t  has_password;
};

// ─── Syscall numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_AUTH_CREATE 190
#define SYS_SLS_AUTH_LIST   191
#define SYS_SLS_AUTH_REVOKE 192

// ─── Syscall argument ─────────────────────────────────────────────────────────
struct AuthCreateRequest {
    char     email[AUTH_EMAIL_LEN];
    uint32_t uid;
    SLSRole  role;
    // Architectural Phase 4: password for the account being created/
    // reissued. password_len==0 leaves the account with no password set
    // (has_password stays 0 on LeaseToken) -- matches pre-Phase-4 behavior
    // for that one case, since there's nothing to check yet.
    char     password[64];
    uint32_t password_len;
};

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct LeaseToken auth_tokens[AUTH_MAX_TOKENS];

void  auth_init(void);

// Grants the four auth_init() demo accounts a matching object-catalog
// authority role (role_table[] in object_catalog.c) so catalog_check_access()
// resolves them the same way auth_http_extract() already does. Must be
// called AFTER persist_restore_all(), not from auth_init() itself -- see
// this function's own comment in auth.c for why boot ordering matters here.
void  auth_seed_default_roles(void);

// Create a token for an email/uid/role — writes the 32-char hex token into
// req->token if non-NULL.  Returns 1 on success, 0 on failure.
int   auth_create_token(struct AuthCreateRequest* req, char* out_token);

// Validate a bearer token string.
// Returns 1 if valid; fills out_uid/out_role.  Returns 0 otherwise.
int   auth_validate_token(const char* token,
                           uint32_t* out_uid, SLSRole* out_role);

// Revoke all tokens associated with email.  Returns 1 if any were removed.
int   auth_revoke_by_email(const char* email);

// Print the token registry to the serial port.
void  sys_sls_auth_list(void);

// Called by the HTTP layer for POST /auth/token.
// Looks up the email — if a registered address with has_password==1, the
// caller must supply the matching password (Architectural Phase 4) or this
// returns a JSON error instead of a token. Otherwise creates a new
// ROLE_GUEST token for the email (unknown addresses; no password required
// for that case, see LeaseToken's own comment on has_password==0).
// Writes a JSON response body into resp_buf.  Returns body length.
int   auth_http_issue(const char* email, const char* password, char* resp_buf, int max);

// Called by the HTTP layer: extract the "Authorization: Bearer <token>" header
// from raw_request and validate.  On success fills out_uid/out_role and returns 1.
// On failure sets *out_uid=0, *out_role=ROLE_GUEST and returns 0.
int   auth_http_extract(const char* raw_request,
                         uint32_t* out_uid, SLSRole* out_role);

#endif /* AUTH_H */
