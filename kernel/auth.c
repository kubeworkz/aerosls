#include "auth.h"
#include "kernel_io.h"
#include "dashboard.h"     // for read_tsc()

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

// ─── auth_init ────────────────────────────────────────────────────────────────
// Pre-register the four simulator demo accounts with deterministic tokens
// (fixed seeds so they survive any TSC reading quirks).
// Also prints each token to the serial log so the developer can copy-paste
// them into a browser's Authorization header.
void auth_init(void) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) auth_tokens[i].active = 0;

    // Demo account definitions matching the simulator
    struct { const char* email; uint32_t uid; SLSRole role; uint64_t seed; } demos[] = {
        { "dave@gridworkz.com",   1000, ROLE_DB_ADMIN,      0xDEADBEEF01234567ULL },
        { "bob@vance.com",        1001, ROLE_APP_USER,      0xCAFEBABE76543210ULL },
        { "carol@gridworkz.com",  1002, ROLE_DB_ADMIN,      0xFEEDF00DABCDEF01ULL },
        { "guest@sandbox.com",    1003, ROLE_GUEST,         0xDEADC0DE99887766ULL },
    };

    kernel_serial_printf(
        "[AUTH] Token Registry\n"
        " %-30s  %-10s  %-13s  %s\n"
        " %-30s  %-10s  %-13s  %s\n",
        "Email", "UID", "Role", "Bearer Token",
        "------------------------------", "----------", "-------------",
        "--------------------------------");

    for (int i = 0; i < 4; i++) {
        struct AuthCreateRequest req;
        au_strncpy(req.email, demos[i].email, AUTH_EMAIL_LEN);
        req.uid  = demos[i].uid;
        req.role = demos[i].role;
        char tok[AUTH_TOKEN_LEN + 1];
        make_token(demos[i].email, demos[i].seed, tok);
        // Store
        struct LeaseToken* lt = &auth_tokens[i];
        au_strncpy(lt->email, req.email, AUTH_EMAIL_LEN);
        au_strncpy(lt->token, tok, AUTH_TOKEN_LEN + 1);
        lt->uid         = req.uid;
        lt->role        = req.role;
        lt->created_tsc = read_tsc();
        lt->active      = 1;

        kernel_serial_printf(
            " %-30s  %-10u  %-13s  %s\n",
            lt->email, lt->uid, role_name(lt->role), lt->token);
    }
    kernel_serial_print("\n");
}

// ─── auth_create_token ────────────────────────────────────────────────────────
int auth_create_token(struct AuthCreateRequest* req, char* out_token) {
    if (!req) return 0;

    // Check if this email already has a token
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (auth_tokens[i].active && au_streq(auth_tokens[i].email, req->email)) {
            if (out_token) au_strncpy(out_token, auth_tokens[i].token, AUTH_TOKEN_LEN+1);
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
            auth_tokens[i].uid         = req->uid;
            auth_tokens[i].role        = req->role;
            auth_tokens[i].created_tsc = read_tsc();
            auth_tokens[i].active      = 1;

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
int auth_validate_token(const char* token, uint32_t* out_uid, SLSRole* out_role) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!auth_tokens[i].active) continue;
        if (au_streq(auth_tokens[i].token, token)) {
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
        " %-30s  %-4s  %-13s  %s\n"
        " %-30s  ----  -------------  ----\n",
        "Email", "UID", "Role", "Token (first 8 chars...)",
        "------------------------------");
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
// Called by POST /auth/token.  Looks up the email; if a known address, returns
// its token.  Otherwise issues a new GUEST token.
int auth_http_issue(const char* email, char* resp_buf, int max) {
    if (!email || !email[0]) {
        const char* err = "{\"error\":\"email required\"}";
        int n = 0; while (err[n]&&n<max-1) resp_buf[n++]=err[n]; resp_buf[n]='\0';
        return n;
    }

    // Try existing token first
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (auth_tokens[i].active && au_streq(auth_tokens[i].email, email)) {
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

    // Unknown email — issue GUEST token
    struct AuthCreateRequest req;
    au_strncpy(req.email, email, AUTH_EMAIL_LEN);
    req.uid  = 9999;
    req.role = ROLE_GUEST;
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
