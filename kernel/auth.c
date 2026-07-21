#include "auth.h"
#include "kernel_io.h"
#include "dashboard.h"     // for read_tsc()
#include "secure_api.h"    // Architectural Phase 4 -- derive_user_key(), reused for account passwords

struct LeaseToken auth_tokens[AUTH_MAX_TOKENS];

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t au_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    au_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void au_strncpy(char* d, const char* s, int n) {
    int i; for (i=0;i<n-1&&s[i];i++) d[i]=s[i]; d[i]='\0';
}

// ─── FNV-1a ───────────────────────────────────────────────────────────────────
static uint64_t fnv1a(const char* s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i=0;i<n;i++) { h ^= (uint8_t)s[i]; h *= 0x00000100000001B3ULL; }
    return h;
}

// ─── Hex encoding ─────────────────────────────────────────────────────────────
static void hex16(uint64_t v, char* out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = hx[v & 0xF]; v >>= 4; }
}

// Generate a 32-char token from email + seed
static void make_token(const char* email, uint64_t seed, char* out) {
    uint64_t h1 = fnv1a(email, au_strlen(email)) ^ seed;
    uint64_t h2 = fnv1a(email, au_strlen(email)) ^ ~seed ^ 0xA5A5A5A5A5A5A5A5ULL;
    hex16(h1, out);       // first 16 hex chars
    hex16(h2, out + 16);  // last 16 hex chars
    out[32] = '\0';
}

// ─── au_token_expired ─────────────────────────────────────────────────────────
// Gap Remediation Phase E. Shared by every call site that decides whether an
// existing token slot is still good: auth_validate_token() (below), and the
// two "does this email already have a token" reuse checks in
// auth_create_token() and auth_http_issue(). Centralizing this matters
// because those two reuse checks originally only tested `active`, not
// expiry -- meaning once a token aged past AUTH_TOKEN_TTL_TICKS, POST
// /auth/token would keep re-handing out that same dead token forever with
// no way for a caller to get a working one short of an admin-initiated
// revoke. That's a real, concrete "denial looks like absence" bug this
// TTL feature itself would otherwise have introduced, caught and fixed in
// the same pass rather than shipped and found later.
static int au_token_expired(const struct LeaseToken* lt) {
    if (lt->no_expiry) return 0;
    return (kernel_tick_counter - lt->created_tick) > AUTH_TOKEN_TTL_TICKS;
}

// ─── Architectural Phase 4: account passwords ──────────────────────────────────
// See LeaseToken's own comment (auth.h) for why this salts with the email
// rather than calling derive_user_key() directly on the bare password.
static void au_derive_password_key(const char* email, const char* password,
                                    uint32_t password_len, uint32_t* out_key) {
    char salted[AUTH_EMAIL_LEN + 64 + 2];
    int p = 0;
    for (int i = 0; email[i] && p < AUTH_EMAIL_LEN; i++) salted[p++] = email[i];
    salted[p++] = ':';
    uint32_t n = password_len > 64 ? 64 : password_len;
    for (uint32_t i = 0; i < n; i++) salted[p++] = password[i];
    derive_user_key(salted, (uint32_t)p, out_key);
}

// lt->email must already be populated -- it's the salt input.
static void au_set_password(struct LeaseToken* lt, const char* password, uint32_t password_len) {
    if (!password || password_len == 0) { lt->has_password = 0; return; }
    au_derive_password_key(lt->email, password, password_len, lt->password_key);
    lt->has_password = 1;
}

static int au_check_password(const struct LeaseToken* lt, const char* password, uint32_t password_len) {
    if (!lt->has_password || !password || password_len == 0) return 0;
    uint32_t key[8];
    au_derive_password_key(lt->email, password, password_len, key);
    for (int i = 0; i < 8; i++) if (key[i] != lt->password_key[i]) return 0;
    return 1;
}

// ─── Demo account roster ────────────────────────────────────────────────────────
// Shared by auth_init() (registers each account's bearer token, for login)
// and auth_seed_default_roles() (grants each account's object-catalog
// authority role, for the permission checks catalog_check_access() actually
// runs -- see that function's own comment for why these are two separate
// steps rather than one). One shared list so the two can't drift apart.
struct DemoAccount { const char* email; uint32_t uid; SLSRole role; uint64_t seed; };
static const struct DemoAccount g_demo_accounts[4] = {
    { "dave@gridworkz.com",   1000, ROLE_DB_ADMIN,      0xDEADBEEF01234567ULL },
    { "bob@vance.com",        1001, ROLE_APP_USER,      0xCAFEBABE76543210ULL },
    { "carol@gridworkz.com",  1002, ROLE_DB_ADMIN,      0xFEEDF00DABCDEF01ULL },
    { "guest@sandbox.com",    1003, ROLE_GUEST,         0xDEADC0DE99887766ULL },
};

// ─── auth_init ────────────────────────────────────────────────────────────────
// Pre-register the four simulator demo accounts with deterministic tokens
// (fixed seeds so they survive any TSC reading quirks).
// Also prints each token to the serial log so the developer can copy-paste
// them into a browser's Authorization header.
void auth_init(void) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) auth_tokens[i].active = 0;

    const struct DemoAccount* demos = g_demo_accounts;

    kernel_serial_print("[AUTH] Token Registry\n");
    kernel_serial_print(" Email                           UID   Role          Token                             Password\n");
    kernel_serial_print(" -------------------------------  ----  ------------  --------------------------------  --------\n");

    for (int i = 0; i < 4; i++) {
        struct AuthCreateRequest req;
        au_strncpy(req.email, demos[i].email, AUTH_EMAIL_LEN);
        req.uid  = demos[i].uid;
        req.role = demos[i].role;

        // Use fixed deterministic tokens — avoids any runtime token generation
        // that could trigger a fault before the IDT is fully set up.
        static const char* fixed_tokens[4] = {
            "deadbeef01234567cafebabe76543210",  // dave
            "cafebabe7654321089abcdef01234567",  // bob
            "feedf00dabcdef0112345678deadc0de",  // carol
            "deadc0de9988776655443322aabbccdd",  // guest
        };
        // Architectural Phase 4: fixed demo passwords, printed to the boot
        // log right alongside the token -- same "everything's public so the
        // developer can copy-paste it" convention this function already
        // used for the tokens themselves (see this function's own header
        // comment). These are standing local-dev demo credentials, not a
        // real security boundary, exactly like the tokens. Every account
        // created later via `auth create` (Phase G) requires a real caller-
        // supplied password instead -- see auth_create_token()'s own note.
        static const char* fixed_passwords[4] = {
            "demo-dave", "demo-bob", "demo-carol", "demo-guest",
        };

        struct LeaseToken* lt = &auth_tokens[i];
        au_strncpy(lt->email, req.email, AUTH_EMAIL_LEN);
        au_strncpy(lt->token, fixed_tokens[i], AUTH_TOKEN_LEN + 1);
        lt->uid          = req.uid;
        lt->role         = req.role;
        lt->created_tick = kernel_tick_counter;  // a plain global read, unlike
                                                   // rdtsc -- safe this early
        // Gap Remediation Phase E: these four are standing local-dev demo
        // credentials (see this function's own header comment), not
        // runtime-issued session tokens -- exempt from TTL expiry so the
        // simulator's login flow doesn't silently break after an hour of
        // uptime. Every token issued later via auth_create_token() still
        // gets real TTL enforcement.
        lt->no_expiry    = 1;
        lt->active       = 1;
        au_set_password(lt, fixed_passwords[i], (uint32_t)au_strlen(fixed_passwords[i]));

        kernel_serial_print(" ");
        kernel_serial_print(lt->email);
        kernel_serial_print("  uid=");
        // print uid manually (no variadic needed)
        uint32_t u = lt->uid;
        char ubuf[12]; int ul = 0;
        if (!u) { ubuf[ul++]='0'; } else { while(u){ubuf[ul++]=(char)('0'+u%10);u/=10;} }
        for (int k=ul-1;k>=0;k--) kernel_serial_putchar(ubuf[k]);
        kernel_serial_print("  ");
        kernel_serial_print(role_name(lt->role));
        kernel_serial_print("  ");
        kernel_serial_print(lt->token);
        kernel_serial_print("  ");
        kernel_serial_print(fixed_passwords[i]);
        kernel_serial_print("\n");
    }
    kernel_serial_print("\n");
}

// ─── auth_seed_default_roles ───────────────────────────────────────────────────
// Grants each of the four demo accounts above a matching object-catalog
// authority role via sys_sls_role_set(). Deliberately NOT folded into
// auth_init() itself, and deliberately called separately by kernel.c AFTER
// persist_restore_all(), not before:
//
// Root cause this fixes: auth_tokens[] (this file) and role_table[]
// (object_catalog.c) are two independent uid -> role lookups. HTTP requests
// resolve identity via auth_http_extract(), which reads auth_tokens[] and
// correctly reports e.g. dave as ROLE_DB_ADMIN for the 401 gate -- but every
// catalog-gated operation (vecstore_insert/get/delete/collection_scan, and
// anything else that calls catalog_check_access()) separately re-resolves
// the caller's role via catalog_get_role(), which reads role_table[] instead.
// Nothing ever populated role_table[] for these four accounts, so
// catalog_get_role() fell through to its ROLE_GUEST default every time --
// silently downgrading a logged-in DB_ADMIN to GUEST for authority purposes,
// which only grants read access to HEAP_BLOB/STREAM objects. Any vector
// collection or table the demo accounts don't personally own (owner_uid at
// creation time is whatever POST /api/valloc hardcodes, not the caller) then
// fails every write/read that routes through catalog_check_access() with a
// permission-denied status, even though the account is a full admin.
//
// Boot ordering matters here: role_table[] is persisted/restored by the same
// mechanism as object_catalog[] (persist_catalog()/persist_restore_all(),
// see persist.c). auth_init() runs at boot step 4f, well before
// persist_restore_all() runs at step 7b. Seeding role_table[] inside
// auth_init() would just get silently overwritten the moment
// persist_restore_all() restores whatever (empty) role_table[] was
// previously persisted -- making the seed a no-op on every boot after the
// first. Calling this function AFTER persist_restore_all() instead avoids
// that race entirely: on a cold start it seeds role_table[] for real and
// sys_sls_role_set()'s own persist_catalog() call saves it, so every later
// boot's restore step loads the correct, already-seeded table; on a warm
// start (already persisted correctly) sys_sls_role_set() finds the existing
// active entry for each uid and just re-writes the same role, a harmless
// idempotent no-op.
void auth_seed_default_roles(void) {
    for (int i = 0; i < 4; i++) {
        struct SLSRoleRequest req;
        req.uid  = g_demo_accounts[i].uid;
        req.role = g_demo_accounts[i].role;
        sys_sls_role_set(&req);
    }
    kernel_serial_print("[AUTH] Default demo account roles seeded in object-catalog role_table.\n");
}

// ─── auth_create_token ────────────────────────────────────────────────────────
int auth_create_token(struct AuthCreateRequest* req, char* out_token) {
    if (!req) return 0;

    // Check if this email already has a token. Phase E: only reuse it if
    // it's still live -- an expired slot falls through to the reissue path
    // below instead, reusing the same slot rather than the free-slot search
    // (see au_token_expired()'s own comment for why this matters).
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (auth_tokens[i].active && au_streq(auth_tokens[i].email, req->email)) {
            if (!au_token_expired(&auth_tokens[i])) {
                if (out_token) au_strncpy(out_token, auth_tokens[i].token, AUTH_TOKEN_LEN+1);
                return 1;
            }
            char tok[AUTH_TOKEN_LEN + 1];
            make_token(req->email, read_tsc(), tok);
            au_strncpy(auth_tokens[i].token, tok, AUTH_TOKEN_LEN + 1);
            auth_tokens[i].uid          = req->uid;
            auth_tokens[i].role         = req->role;
            auth_tokens[i].created_tick = kernel_tick_counter;
            au_set_password(&auth_tokens[i], req->password, req->password_len);  // Architectural Phase 4
            if (out_token) au_strncpy(out_token, tok, AUTH_TOKEN_LEN + 1);
            kernel_serial_printf(
                "[AUTH] Token re-issued (previous expired): uid=%u role=%s email=%s\n",
                req->uid, role_name(req->role), req->email);
            return 1;
        }
    }

    // Find a free slot
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!auth_tokens[i].active) {
            char tok[AUTH_TOKEN_LEN + 1];
            make_token(req->email, read_tsc(), tok);

            au_strncpy(auth_tokens[i].email, req->email, AUTH_EMAIL_LEN);
            au_strncpy(auth_tokens[i].token, tok, AUTH_TOKEN_LEN + 1);
            auth_tokens[i].uid          = req->uid;
            auth_tokens[i].role         = req->role;
            auth_tokens[i].created_tick = kernel_tick_counter;  // Phase E
            auth_tokens[i].no_expiry    = 0;                    // Phase E: real TTL applies
            auth_tokens[i].active       = 1;
            au_set_password(&auth_tokens[i], req->password, req->password_len);  // Architectural Phase 4

            if (out_token) au_strncpy(out_token, tok, AUTH_TOKEN_LEN + 1);
            kernel_serial_printf(
                "[AUTH] Token issued: uid=%u role=%s email=%s\n",
                req->uid, role_name(req->role), req->email);
            return 1;
        }
    }
    kernel_serial_print("[AUTH] Token registry full.\n");
    return 0;
}

// ─── auth_validate_token ──────────────────────────────────────────────────────
// Gap Remediation Phase E: now also enforces TTL expiry for any token that
// isn't one of auth_init()'s standing demo accounts (no_expiry==1). An
// expired token fails validation exactly like an unknown one -- the slot is
// left active rather than reclaimed (bump-allocated, no-reclaim is this
// project's existing precedent for fixed-size registries under pressure,
// e.g. row_constraint.h's ROW_CONSTRAINT_MAX; freeing expired slots for
// reuse is a real future improvement, named in the Phase E findings rather
// than built here).
int auth_validate_token(const char* token, uint32_t* out_uid, SLSRole* out_role) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!auth_tokens[i].active) continue;
        if (au_streq(auth_tokens[i].token, token)) {
            if (au_token_expired(&auth_tokens[i])) break;  // expired -- fall through to failure
            if (out_uid)  *out_uid  = auth_tokens[i].uid;
            if (out_role) *out_role = auth_tokens[i].role;
            return 1;
        }
    }
    if (out_uid)  *out_uid  = 0;
    if (out_role) *out_role = ROLE_GUEST;
    return 0;
}

// ─── auth_revoke_by_email ─────────────────────────────────────────────────────
int auth_revoke_by_email(const char* email) {
    int revoked = 0;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (auth_tokens[i].active && au_streq(auth_tokens[i].email, email)) {
            auth_tokens[i].active = 0;
            revoked++;
            kernel_serial_printf("[AUTH] Revoked token for %s\n", email);
        }
    }
    return revoked;
}

// ─── sys_sls_auth_list ────────────────────────────────────────────────────────
void sys_sls_auth_list(void) {
    kernel_serial_printf(
        "\n[AUTH] Active Tokens\n"
        " %-30s  %-4s  %-13s  %s\n",
        "Email", "UID", "Role", "Token (first 8 chars...)");
    int shown = 0;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!auth_tokens[i].active) continue;
        // Print only first 8 chars of token for security
        char tok_preview[10];
        for (int k = 0; k < 8; k++) tok_preview[k] = auth_tokens[i].token[k];
        tok_preview[8] = '\0';
        kernel_serial_printf(
            " %-30s  %-4u  %-13s  %s...\n",
            auth_tokens[i].email, auth_tokens[i].uid,
            role_name(auth_tokens[i].role), tok_preview);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no active tokens)\n");
    kernel_serial_printf(" %d token(s).\n\n", shown);
}

// ─── auth_http_issue ─────────────────────────────────────────────────────────
// Called by POST /auth/token. Looks up the email; if a known address with a
// password set (Architectural Phase 4), the supplied password must match or
// this returns a JSON error instead of the token -- previously ANY caller
// who supplied a known email got that account's live token back, no
// credential check at all, which meant knowing e.g. dave@gridworkz.com's
// address was enough to obtain his real DB_ADMIN token. Unknown emails
// still get a new ROLE_GUEST token with no password required, since they
// claim no existing identity to protect (see LeaseToken's own comment on
// has_password==0).
int auth_http_issue(const char* email, const char* password, char* resp_buf, int max) {
    if (!email || !email[0]) {
        const char* err = "{\"error\":\"email required\"}";
        int n = 0; while (err[n]&&n<max-1) resp_buf[n++]=err[n]; resp_buf[n]='\0';
        return n;
    }
    uint32_t password_len = password ? (uint32_t)au_strlen(password) : 0;

    // Try existing token first. Phase E: skip (and fall through to the
    // "unknown email" reissue path below) if it's expired -- see
    // au_token_expired()'s own comment for why an expired slot can't just
    // be handed back as-is.
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (auth_tokens[i].active && au_streq(auth_tokens[i].email, email) &&
            !au_token_expired(&auth_tokens[i])) {
            // Architectural Phase 4: an account with a password set must
            // present it. Accounts without one (only the auto-provisioned
            // GUEST accounts below can currently reach this state) fall
            // through unchanged, matching pre-Phase-4 behavior for that case.
            if (auth_tokens[i].has_password &&
                !au_check_password(&auth_tokens[i], password, password_len)) {
                const char* err = "{\"error\":\"invalid credentials\"}";
                int n = 0; while (err[n]&&n<max-1) { resp_buf[n]=err[n]; n++; } resp_buf[n]='\0';
                return n;
            }
            // Found — return it
            int pos = 0;
            const char* pre = "{\"ok\":true,\"token\":\"";
            while (*pre && pos<max-1) resp_buf[pos++] = *pre++;
            for (int k = 0; auth_tokens[i].token[k] && pos<max-2; k++)
                resp_buf[pos++] = auth_tokens[i].token[k];
            const char* mid = "\",\"uid\":";
            while (*mid && pos<max-1) resp_buf[pos++] = *mid++;
            // write uid as decimal
            uint32_t u = auth_tokens[i].uid;
            char tmp[12]; int tl=0;
            if (!u) { tmp[tl++]='0'; }
            else { while(u){tmp[tl++]=(char)('0'+u%10);u/=10;} }
            for (int k=tl-1;k>=0;k--) { if(pos<max-1) resp_buf[pos++]=tmp[k]; }
            const char* suf = ",\"role\":\"";
            while (*suf && pos<max-1) resp_buf[pos++] = *suf++;
            const char* rn = role_name(auth_tokens[i].role);
            while (*rn && pos<max-1) resp_buf[pos++] = *rn++;
            const char* end = "\"}";
            while (*end && pos<max-1) resp_buf[pos++] = *end++;
            resp_buf[pos] = '\0';
            return pos;
        }
    }

    // Unknown email — issue GUEST token, no password (see this function's
    // own header comment for why that's fine: no existing identity to protect).
    struct AuthCreateRequest req;
    au_strncpy(req.email, email, AUTH_EMAIL_LEN);
    req.uid  = 9999;
    req.role = ROLE_GUEST;
    req.password_len = 0;
    char tok[AUTH_TOKEN_LEN + 1];
    auth_create_token(&req, tok);

    int pos = 0;
    const char* pre = "{\"ok\":true,\"token\":\"";
    while (*pre && pos<max-1) resp_buf[pos++] = *pre++;
    for (int k = 0; tok[k] && pos<max-2; k++) resp_buf[pos++] = tok[k];
    const char* suf = "\",\"uid\":9999,\"role\":\"GUEST\"}";
    while (*suf && pos<max-1) resp_buf[pos++] = *suf++;
    resp_buf[pos] = '\0';
    return pos;
}

// ─── auth_http_extract ────────────────────────────────────────────────────────
// Find "Authorization: Bearer <token>" in raw HTTP headers and validate it.
int auth_http_extract(const char* raw, uint32_t* out_uid, SLSRole* out_role) {
    *out_uid  = 0;
    *out_role = ROLE_GUEST;

    // Search for the header (case-sensitive, standard HTTP)
    const char* needle = "Authorization: Bearer ";
    const char* p = raw;
    int nlen = 0; while (needle[nlen]) nlen++;

    for (; *p; p++) {
        // Quick match check
        int match = 1;
        for (int i = 0; i < nlen; i++) {
            if (!p[i] || p[i] != needle[i]) { match = 0; break; }
        }
        if (!match) continue;

        p += nlen;  // skip "Authorization: Bearer "
        char tok[AUTH_TOKEN_LEN + 2];
        int tl = 0;
        while (*p && *p != '\r' && *p != '\n' && *p != ' ' && tl <= AUTH_TOKEN_LEN)
            tok[tl++] = *p++;
        tok[tl] = '\0';
        return auth_validate_token(tok, out_uid, out_role);
    }
    return 0;  // No Authorization header found
}
