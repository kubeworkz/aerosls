#include "http.h"
#include "tcp.h"
#include "net.h"
#include "../kernel/kernel_io.h"
#include "../kernel/timer.h"
#include "../kernel/object_catalog.h"
#include "../kernel/transaction.h"
#include "../kernel/microkernel.h"
#include "../kernel/tier_mgr.h"
#include "../kernel/webapp.h"
#include "../kernel/bundle.h"
#include "../kernel/journal.h"
#include "../kernel/lock_mgr.h"
#include "../kernel/index_mgr.h"
#include "../kernel/constraint.h"
#include "../kernel/cursor.h"
#include "../kernel/aggregate.h"
#include "../kernel/mqt.h"
#include "../kernel/query_engine.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/auth.h"
#include "../kernel/loader.h"
#include "../kernel/stream.h"
#include "../user/permissions.h"
#include "../kernel/agent.h"
#include "../kernel/agent_tools.h"
#include "../kernel/rowstore.h"   // Gap Remediation Phase B -- POST /api/tables, GET /api/tables[/name/schema]
#include "../kernel/sql_exec.h"   // Gap Remediation Phase B -- POST /api/sql
#include "../kernel/vec_join.h"   // Gap Remediation Phase C -- POST /api/vec/join
#include "../kernel/vec_index.h"  // Gap Remediation Phase C -- POST /api/vec/indexes, /api/vec/index/search
#include "../kernel/partition.h"  // Gap Remediation Phase F -- partition create/list/destroy/assign/pause/resume
#include "../kernel/frame_pool.h" // Gap Remediation Phase F -- GET /api/partition/quotas, POST /api/partition/quota

// ─── Simple JSON builder ──────────────────────────────────────────────────────
static void jb_putc(JSONBuf* j, char c) {
    if (j->pos < j->max - 1) j->buf[j->pos++] = c;
}

void jb_raw(JSONBuf* j, const char* s) {
    while (*s) jb_putc(j, *s++);
}

static void jb_esc_str(JSONBuf* j, const char* s) {
    jb_putc(j, '"');
    while (*s) {
        if (*s == '"' || *s == '\\') jb_putc(j, '\\');
        jb_putc(j, *s++);
    }
    jb_putc(j, '"');
}

static void jb_key(JSONBuf* j, const char* k) {
    jb_esc_str(j, k);
    jb_putc(j, ':');
}

void jb_str(JSONBuf* j, const char* key, const char* val) {
    jb_key(j, key);
    jb_esc_str(j, val);
}

// jb_esc_str() above escapes '"' and '\' but not control characters -- fine
// for every existing call site in this file (none of them ever carried
// embedded newlines), but not fine for shell command output (Kernel-Side
// Shell Refactor, docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10.4), which
// routinely spans multiple lines ("ls", "journal dump", "mqt list", ...) --
// a raw '\n' inside a JSON string literal is invalid JSON and would break
// JSON.parse() on the client. Deliberately a new, narrowly-used helper
// rather than changing jb_esc_str() itself: that function has ~40 existing
// call sites in this file, none of which need this, so fixing it in place
// would be unscoped risk for zero benefit.
static void jb_str_multiline(JSONBuf* j, const char* key, const char* val) {
    jb_key(j, key);
    jb_putc(j, '"');
    while (*val) {
        char c = *val++;
        if      (c == '"')  { jb_putc(j, '\\'); jb_putc(j, '"'); }
        else if (c == '\\') { jb_putc(j, '\\'); jb_putc(j, '\\'); }
        else if (c == '\n') { jb_putc(j, '\\'); jb_putc(j, 'n'); }
        else if (c == '\r') { jb_putc(j, '\\'); jb_putc(j, 'r'); }
        else if (c == '\t') { jb_putc(j, '\\'); jb_putc(j, 't'); }
        else jb_putc(j, c);
    }
    jb_putc(j, '"');
}

void jb_uint(JSONBuf* j, const char* key, uint64_t val) {
    jb_key(j, key);
    if (val == 0) { jb_putc(j, '0'); return; }
    char tmp[21]; int len = 0;
    while (val) { tmp[len++] = (char)('0' + val % 10); val /= 10; }
    for (int i = len-1; i >= 0; i--) jb_putc(j, tmp[i]);
}

static void jb_hex(JSONBuf* j, const char* key, uint64_t val) {
    jb_key(j, key);
    jb_putc(j, '"'); jb_raw(j, "0x");
    char tmp[17]; int len = 0;
    if (val == 0) { jb_putc(j, '0'); jb_putc(j, '"'); return; }
    while (val) {
        int d = (int)(val & 0xF);
        tmp[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val >>= 4;
    }
    for (int i = len-1; i >= 0; i--) jb_putc(j, tmp[i]);
    jb_putc(j, '"');
}

void jb_obj_open(JSONBuf* j, const char* key) {
    if (key) { jb_key(j, key); }
    jb_putc(j, '{');
}

void jb_obj_close(JSONBuf* j) { jb_putc(j, '}'); }

void jb_arr_open(JSONBuf* j, const char* key) {
    if (key) { jb_key(j, key); }
    jb_putc(j, '[');
}

void jb_arr_close(JSONBuf* j) { jb_putc(j, ']'); }

// ─── API Handlers ─────────────────────────────────────────────────────────────

static int api_scan(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "build", "4.0-SLS"); jb_putc(&j, ',');
    jb_uint(&j, "object_count", object_catalog_count); jb_putc(&j, ',');
    jb_arr_open(&j, "objects");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_str(&j, "type", obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr); jb_putc(&j, ',');
        jb_str(&j, "tier", tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages); jb_putc(&j, ',');
        jb_uint(&j, "uid", e->owner_uid); jb_putc(&j, ',');
        jb_uint(&j, "field_count", object_records[i].field_count); jb_putc(&j, ',');
        jb_arr_open(&j, "fields");
        int ff = 1;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!object_records[i].fields[f].active) continue;
            if (!ff) jb_putc(&j, ','); ff = 0;
            jb_obj_open(&j, 0);
            jb_str(&j, "k", object_records[i].fields[f].key); jb_putc(&j, ',');
            jb_str(&j, "v", object_records[i].fields[f].value);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j);
    }
    jb_arr_close(&j); jb_putc(&j, ',');
    // WAL summary
    uint32_t committed=0, pending=0, aborted=0;
    for (uint32_t i = 0; i < wal_entry_count; i++) {
        if (wal_buffer[i].state == WAL_STATE_COMMITTED) committed++;
        else if (wal_buffer[i].state == WAL_STATE_PENDING)   pending++;
        else aborted++;
    }
    jb_obj_open(&j, "wal"); jb_putc(&j, '\0'); j.pos--;
    jb_uint(&j, "total", wal_entry_count); jb_putc(&j, ',');
    jb_uint(&j, "committed", committed); jb_putc(&j, ',');
    jb_uint(&j, "pending", pending); jb_putc(&j, ',');
    jb_uint(&j, "aborted", aborted);
    jb_obj_close(&j); jb_putc(&j, ',');
    // Service summary
    uint32_t svc_online=0, svc_crashed=0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (!services[i].active) continue;
        if (services[i].state == SVC_STATE_ONLINE) svc_online++;
        else svc_crashed++;
    }
    jb_obj_open(&j, "services"); jb_putc(&j, '\0'); j.pos--;
    jb_uint(&j, "total", service_count); jb_putc(&j, ',');
    jb_uint(&j, "online", svc_online); jb_putc(&j, ',');
    jb_uint(&j, "crashed", svc_crashed);
    jb_obj_close(&j);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

static int api_health(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j,  "status",       "ok");                     jb_putc(&j, ',');
    jb_str(&j,  "system",       "AeroSLS 4.0");             jb_putc(&j, ',');
    jb_uint(&j, "uptime_ticks", kernel_tick_counter);       jb_putc(&j, ',');
    jb_uint(&j, "object_count", object_catalog_count);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── GET /api/metrics ─────────────────────────────────────────────────────────
// Live kernel instrumentation: access events, tier promotions, IPC latency.
static int api_metrics(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "total_accesses",    tier_total_accesses);       jb_putc(&j, ',');
    jb_uint(&j, "total_promotions",  tier_total_promotions);     jb_putc(&j, ',');
    jb_uint(&j, "ipc_posted",        ipc_stats.total_posted);    jb_putc(&j, ',');
    jb_uint(&j, "ipc_dispatched",    ipc_stats.total_dispatched);jb_putc(&j, ',');
    jb_uint(&j, "ipc_avg_latency_ns",ipc_stats.avg_latency_ns);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── HTTP response helper ─────────────────────────────────────────────────────
static void http_respond(int conn, int status, const char* ctype,
                          const char* body, int blen) {
    char hdr[256];
    const char* reason = status == 200 ? "OK" :
                         status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" : "Error";
    // Build status line + headers into hdr[]
    int hpos = 0;
    const char* sl = "HTTP/1.1 ";
    while (*sl) hdr[hpos++] = *sl++;
    // status code as string
    hdr[hpos++] = (char)('0' + status/100);
    hdr[hpos++] = (char)('0' + (status/10)%10);
    hdr[hpos++] = (char)('0' + status%10);
    hdr[hpos++] = ' ';
    while (*reason) hdr[hpos++] = *reason++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    const char* hdrs =
        "Content-Type: ";
    while (*hdrs) hdr[hpos++] = *hdrs++;
    while (*ctype) hdr[hpos++] = *ctype++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    // Content-Length
    const char* cl = "Content-Length: ";
    while (*cl) hdr[hpos++] = *cl++;
    // write blen as decimal
    char tmp[12]; int tl = 0;
    int bl = blen;
    if (bl == 0) { tmp[tl++] = '0'; }
    else { while (bl) { tmp[tl++] = (char)('0' + bl%10); bl /= 10; } }
    for (int i = tl-1; i >= 0; i--) hdr[hpos++] = tmp[i];
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    const char* cors = "Access-Control-Allow-Origin: *\r\n";
    while (*cors) hdr[hpos++] = *cors++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';  // end of headers

    tcp_send(conn, hdr, (uint32_t)hpos);
    if (blen > 0) tcp_send(conn, body, (uint32_t)blen);
}

// Like http_respond but for binary/large assets from the compiled-in bundle.
// Accepts uint32_t length so files larger than 64 KiB (e.g. the JS bundle)
// are served correctly via tcp_send's internal chunking.
static void http_respond_raw(int conn, const char* ctype,
                              const uint8_t* data, uint32_t blen) {
    char hdr[256];
    int hpos = 0;
    // Status line
    const char* sl = "HTTP/1.1 200 OK\r\nContent-Type: ";
    while (*sl) hdr[hpos++] = *sl++;
    while (*ctype) hdr[hpos++] = *ctype++;
    const char* cl = "\r\nContent-Length: ";
    while (*cl) hdr[hpos++] = *cl++;
    // Write blen as decimal (uint32_t, up to 10 digits)
    char tmp[12]; int tl = 0;
    uint32_t bl = blen;
    if (bl == 0) { tmp[tl++] = '0'; }
    else { while (bl) { tmp[tl++] = (char)('0' + bl % 10); bl /= 10; } }
    for (int i = tl - 1; i >= 0; i--) hdr[hpos++] = tmp[i];
    const char* cors = "\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    while (*cors) hdr[hpos++] = *cors++;

    tcp_send(conn, hdr, (uint32_t)hpos);
    if (blen > 0) tcp_send(conn, data, blen);
}

// Forward declarations for helpers defined in the Phase F section below
static int json_str(const char* json, const char* key, char* out, int max);
static int json_int(const char* json, const char* key);

// ─── POST /auth/token — issue a bearer token for an email ─────────────────────
static int api_auth_token(const char* body, char* buf, int max) {
    if (!body) {
        const char* err = "{\"error\":\"missing body\"}";
        int n=0; while(err[n]&&n<max-1) buf[n]=err[n++]; buf[n]='\0'; return n;
    }
    char email[AUTH_EMAIL_LEN];
    json_str(body, "email", email, AUTH_EMAIL_LEN);
    return auth_http_issue(email, buf, max);
}

// ─── GET /auth/verify — decode and return token metadata ────────────────────
static int api_auth_verify(const char* raw_req, char* buf, int max) {
    uint32_t uid = 0; SLSRole role = ROLE_GUEST;
    int valid = auth_http_extract(raw_req, &uid, &role);
    // Find email for this uid
    const char* email = "(unknown)";
    for (int i=0;i<AUTH_MAX_TOKENS;i++) {
        if (auth_tokens[i].active && auth_tokens[i].uid==uid) {
            email = auth_tokens[i].email; break;
        }
    }
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "valid",   valid ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "uid",    uid);                      jb_putc(&j, ',');
    jb_str(&j, "role",    role_name(role));           jb_putc(&j, ',');
    jb_str(&j, "email",   email);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Phase F: HTTP/REST helpers ───────────────────────────────────────────────

// Substring search (freestanding — no libc strstr)
static const char* str_find(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char* h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

static int str_ncmp2(const char* a, const char* b, int n) {
    while (n-- > 0) {
        if (*a != *b) return *a - *b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

// Locate the HTTP request body (after the blank line \r\n\r\n)
static const char* http_body(const char* req) {
    for (const char* p = req; p[0]; p++)
        if (p[0]=='\r'&&p[1]=='\n'&&p[2]=='\r'&&p[3]=='\n') return p+4;
    return 0;
}

// Extract "key": "value" from a JSON string; returns chars copied
static int json_str(const char* json, const char* key, char* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p!='"') return 0;
    p++;
    int n = 0;
    while (*p && *p!='"' && n<max-1) out[n++] = *p++;
    out[n] = '\0';
    return n;
}

// Extract "key": N (integer) from a JSON string
static int json_int(const char* json, const char* key) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    int v = 0;
    while (*p>='0'&&*p<='9') { v = v*10 + (*p-'0'); p++; }
    return v;
}

// Extract "key": N (integer, widened to 64 bits) from a JSON string --
// json_int()'s own parse loop widened, needed for vector external_id values
// (Gap Remediation Phase C).
static uint64_t json_uint64(const char* json, const char* key) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    uint64_t v = 0;
    while (*p>='0'&&*p<='9') { v = v*10 + (uint64_t)(*p-'0'); p++; }
    return v;
}

// Extract a JSON array of numbers "key": [n0, n1, ...] into out[] as floats
// -- Gap Remediation Phase C, needed for vector query/insert values. No
// libc strtof (freestanding) -- a hand-rolled decimal/sign/fraction parse,
// the same shape rowstore.c's own rs_parse_f64()/net/ollama_client.c's own
// oc_parse_json_number() already use elsewhere, kept as its own small copy
// here rather than shared (matching this project's established "each file
// keeps its own small helpers" convention -- see vec_join.h's own header
// comment on why). Returns the number of floats written (0..max).
static int json_float_array(const char* json, const char* key, float* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p==' '||*p=='\t'||*p==',') p++;
        if (*p == ']' || !*p) break;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        double v = 0.0;
        while (*p>='0'&&*p<='9') { v = v*10.0 + (double)(*p-'0'); p++; }
        if (*p == '.') {
            p++;
            double frac = 0.1;
            while (*p>='0'&&*p<='9') { v += (double)(*p-'0') * frac; frac *= 0.1; p++; }
        }
        out[n++] = (float)(neg ? -v : v);
        while (*p==' '||*p=='\t') p++;
    }
    return n;
}

// Fills out[] with json_str(json, key, ...)'s result, or with def if the
// key was absent/empty -- json_str() itself leaves out[] untouched (not
// even null-terminated) on failure, so a caller that needs a guaranteed
// default must pre-clear first. No libc strcpy (freestanding, and not used
// anywhere else in this file) -- a small hand-rolled copy loop instead,
// matching this codebase's established per-file *_strcpy() convention.
static void json_str_or_default(const char* json, const char* key, char* out, int max, const char* def) {
    out[0] = '\0';
    json_str(json, key, out, max);
    if (!out[0]) {
        int i = 0;
        for (; i < max - 1 && def[i]; i++) out[i] = def[i];
        out[i] = '\0';
    }
}

// Extract "key": F (a single decimal number) as a float -- Gap Remediation
// Phase C, the scalar counterpart to json_float_array() above, needed for
// distance values inside a "matches" array element (POST /api/vec/join).
// Gap Remediation (post-roadmap x86 boot-build fix): out-parameter, not a
// by-value float return -- see kernel/vecstore.c's own header comment on
// why (the real x86-64 cross-build disables SSE, which breaks float
// BY-VALUE RETURN specifically; local double/float math is unaffected).
// Behavior unchanged from the original by-value version: *out is set to
// 0.0f if `key` isn't found, same as the old "return 0.0f" default.
static void json_float(const char* json, const char* key, float* out) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) { *out = 0.0f; return; }
    p += si;
    while (*p==' '||*p=='\t') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    double v = 0.0;
    while (*p>='0'&&*p<='9') { v = v*10.0 + (double)(*p-'0'); p++; }
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (*p>='0'&&*p<='9') { v += (double)(*p-'0') * frac; frac *= 0.1; p++; }
    }
    *out = (float)(neg ? -v : v);
}

// Extracts the n-th top-level JSON object from a "key": [ {...}, {...} ]
// array into out (a plain, null-terminated copy of just that one object's
// text, braces included) -- Gap Remediation Phase C, needed for POST
// /api/vec/join, the one route on this surface taking an array of OBJECTS
// rather than an array of plain numbers (json_float_array already covers
// those). Assumes no nested objects inside a match entry (true for the
// {external_id, page_id, slot_index, distance} shape this route expects --
// matching exactly what GET .../api/vec/search's own response already
// emits, so a client can round-trip one response straight into the next
// request's body). Returns 1 if index n existed and was copied, 0
// otherwise (array too short, key missing, malformed).
static int json_array_object_at(const char* json, const char* key, int n, char* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p != '[') return 0;
    p++;
    int idx = 0;
    while (*p && *p != ']') {
        while (*p==' '||*p=='\t'||*p==',') p++;
        if (*p != '{') break;
        const char* start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (idx == n) {
            int len = (int)(p - start);
            if (len >= max) len = max - 1;
            for (int i = 0; i < len; i++) out[i] = start[i];
            out[len] = '\0';
            return 1;
        }
        idx++;
    }
    return 0;
}

// URL-decode: replace + with space and %XX with the byte value
static int url_decode(const char* src, char* dst, int max) {
    int n = 0;
    while (*src && n<max-1) {
        if (*src=='+') { dst[n++]=' '; src++; }
        else if (*src=='%'&&src[1]&&src[2]) {
            uint8_t hi=(src[1]>='a')?src[1]-'a'+10:(src[1]>='A')?src[1]-'A'+10:src[1]-'0';
            uint8_t lo=(src[2]>='a')?src[2]-'a'+10:(src[2]>='A')?src[2]-'A'+10:src[2]-'0';
            dst[n++]=(char)((hi<<4)|lo); src+=3;
        } else { dst[n++]=*src++; }
    }
    dst[n]='\0';
    return n;
}

// Extract ?key=value from a query string (URL-decoded into out)
static int url_param(const char* qs, const char* key, char* out, int max) {
    int klen = 0; while (key[klen]) klen++;
    while (*qs) {
        if (str_ncmp2(qs, key, klen)==0 && qs[klen]=='=') {
            const char* v = qs+klen+1;
            const char* e = v; while (*e&&*e!='&') e++;
            int vl = (int)(e-v); if (vl>=511) vl=510;
            char tmp[512]; for (int i=0;i<vl;i++) tmp[i]=v[i]; tmp[vl]='\0';
            return url_decode(tmp, out, max);
        }
        while (*qs&&*qs!='&') qs++;
        if (*qs=='&') qs++;
    }
    return 0;
}

// ─── CORS preflight ───────────────────────────────────────────────────────────
static void http_options(int conn) {
    const char* h = "HTTP/1.1 204 No Content\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                    "Content-Length: 0\r\n\r\n";
    tcp_send(conn, h, (uint32_t)strlen(h));
}

// ─── GET /api/objects ─────────────────────────────────────────────────────────
static int api_objects(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", object_catalog_count); jb_putc(&j, ',');
    jb_arr_open(&j, "objects");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name);            jb_putc(&j, ',');
        jb_str(&j, "type", obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr);     jb_putc(&j, ',');
        jb_str(&j, "tier", tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);    jb_putc(&j, ',');
        jb_uint(&j, "uid", e->owner_uid);       jb_putc(&j, ',');
        jb_str(&j, "role", role_name(e->owner_role)); jb_putc(&j, ',');
        jb_uint(&j, "field_count", object_records[i].field_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tables — Gap Remediation Phase B ────────────────────────────────
// Enumerates row-set tables (object_catalog[] entries with uses_rowstore
// set -- distinct from OBJ_TYPE_DB_TABLE, which also covers legacy
// single-record KV objects that never called rowstore_create_table()).
// Needed so a Navigator-style table browser can list what exists without
// already knowing a name -- no such enumeration had any HTTP route before
// this (docs/AeroSLS-Gap-Analysis-v0.1.md §5).
static int api_tables_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "tables");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !e->uses_rowstore) continue;
        struct RowTableHeader* h = &table_headers[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_uint(&j, "column_count", h->layout.column_count); jb_putc(&j, ',');
        jb_uint(&j, "row_count", h->row_count); jb_putc(&j, ',');
        jb_uint(&j, "page_count", h->page_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tables/<name>/schema — Gap Remediation Phase B ──────────────────
// Column names + types for one row-set table, derived from its
// RowTableLayout (computed once at rowstore_create_table() time -- see
// rowstore.h). Distinct from the legacy /api/objects/<name>'s own
// best-effort per-field type lookup (which walks object_schemas[] matching
// on live record field keys); this reads the table's own fixed layout
// directly, the authoritative source for a row-set table's real columns.
static int api_table_schema(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !e->uses_rowstore || strcmp(e->name, name) != 0) continue;
        struct RowTableLayout* layout = &table_headers[i].layout;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_arr_open(&j, "columns");
        for (uint32_t c = 0; c < layout->column_count; c++) {
            if (c) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_str(&j, "name", layout->column_names[c]); jb_putc(&j, ',');
            jb_str(&j, "type", field_type_name(layout->column_types[c]));
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j, 0); jb_str(&j, "error", "table not found"); jb_obj_close(&j);
    j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/objects/<name> ──────────────────────────────────────────────────
static int api_object_detail(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !strcmp(e->name, name)) {
            if (!e->active) continue;
        }
        if (strcmp(e->name, name) != 0) continue;
        jb_obj_open(&j, 0);
        jb_str(&j, "name",  e->name);           jb_putc(&j, ',');
        jb_str(&j, "type",  obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr);     jb_putc(&j, ',');
        jb_str(&j, "tier",  tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);    jb_putc(&j, ',');
        jb_uint(&j, "uid",  e->owner_uid);      jb_putc(&j, ',');
        jb_uint(&j, "perm", e->perm_mask);      jb_putc(&j, ',');
        // Records
        struct SLSObjectRecord* rec = &object_records[i];
        jb_arr_open(&j, "records");
        int ff = 1;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!rec->fields[f].active) continue;
            if (!ff) jb_putc(&j, ','); ff = 0;
            // find schema type
            const char* tname = "STRING";
            for (uint32_t s = 0; s < SCHEMA_MAX_FIELDS; s++) {
                if (object_schemas[i].fields[s].active &&
                    !strcmp(object_schemas[i].fields[s].key,
                                      rec->fields[f].key)) {
                    tname = field_type_name(object_schemas[i].fields[s].type);
                    break;
                }
            }
            jb_obj_open(&j, 0);
            jb_str(&j, "key",   rec->fields[f].key);   jb_putc(&j, ',');
            jb_str(&j, "value", rec->fields[f].value);  jb_putc(&j, ',');
            jb_str(&j, "type",  tname);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j, 0);
    jb_str(&j, "error", "object not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/services ────────────────────────────────────────────────────────
static int api_services_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", service_count); jb_putc(&j, ',');
    jb_obj_open(&j, "ipc");
    jb_uint(&j, "posted",     ipc_stats.total_posted);     jb_putc(&j, ',');
    jb_uint(&j, "dispatched", ipc_stats.total_dispatched); jb_putc(&j, ',');
    jb_uint(&j, "dropped",    ipc_stats.total_dropped);
    jb_obj_close(&j); jb_putc(&j, ',');
    jb_arr_open(&j, "services");
    for (uint32_t i = 0; i < service_count; i++) {
        struct ServiceDescriptor* s = &services[i];
        if (!s->active) continue;
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_str(&j,  "name",    s->name);           jb_putc(&j, ',');
        jb_uint(&j, "pid",     s->pid);             jb_putc(&j, ',');
        jb_uint(&j, "port",    s->port);            jb_putc(&j, ',');
        jb_str(&j,  "state",   svc_state_name(s->state)); jb_putc(&j, ',');
        jb_uint(&j, "reboots", s->reboot_count);   jb_putc(&j, ',');
        jb_uint(&j, "msgs",    (uint64_t)s->msgs_processed);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/wal ─────────────────────────────────────────────────────────────
static int api_wal_json(char* buf, int max) {
    uint32_t committed=0, pending=0, aborted=0;
    for (uint32_t i=0;i<wal_entry_count;i++) {
        if (wal_buffer[i].state==WAL_STATE_COMMITTED) committed++;
        else if (wal_buffer[i].state==WAL_STATE_PENDING) pending++;
        else aborted++;
    }
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "total",     wal_entry_count); jb_putc(&j, ',');
    jb_uint(&j, "committed", committed);       jb_putc(&j, ',');
    jb_uint(&j, "pending",   pending);         jb_putc(&j, ',');
    jb_uint(&j, "aborted",   aborted);         jb_putc(&j, ',');
    jb_arr_open(&j, "entries");
    for (uint32_t i=0;i<wal_entry_count;i++) {
        struct WALEntry* w = &wal_buffer[i];
        if (i) jb_putc(&j, ',');
        const char* st = w->state==WAL_STATE_COMMITTED ? "COMMITTED" :
                         w->state==WAL_STATE_PENDING    ? "PENDING"   : "ABORTED";
        jb_obj_open(&j, 0);
        jb_uint(&j, "id",    w->entry_id); jb_putc(&j, ',');
        jb_uint(&j, "tx",    w->tx_id);    jb_putc(&j, ',');
        jb_str(&j,  "key",   w->key);      jb_putc(&j, ',');
        jb_str(&j,  "state", st);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tiers ───────────────────────────────────────────────────────────
static int api_tiers_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    static const SLSStorageTier tiers[]  = { STORAGE_TIER_L1_CACHE, STORAGE_TIER_L2_DRAM, STORAGE_TIER_L3_SSD };
    static const char* tier_keys[] = { "l1_cache", "l2_dram", "l3_ssd" };
    for (int t = 0; t < 3; t++) {
        if (t) jb_putc(&j, ',');
        jb_arr_open(&j, tier_keys[t]);
        int first = 1;
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            struct SLSObjectEntry* e = &object_catalog[i];
            if (!e->active || e->storage_tier != tiers[t]) continue;
            if (!first) jb_putc(&j, ','); first = 0;
            uint32_t acc=0, idle=0;
            for (int s=0;s<TIER_MAX_TRACKED;s++) {
                if (tier_stats[s].active && tier_stats[s].object_id==e->object_id)
                    { acc=tier_stats[s].access_count; idle=tier_stats[s].idle_ticks; break; }
            }
            jb_obj_open(&j, 0);
            jb_str(&j,  "name",     e->name);           jb_putc(&j, ',');
            jb_uint(&j, "accesses", acc);               jb_putc(&j, ',');
            jb_uint(&j, "idle",     idle);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/processes ───────────────────────────────────────────────────────
static int api_processes_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", proc_count); jb_putc(&j, ',');
    jb_arr_open(&j, "processes");
    int first = 1;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "pid",   pd->pid);                        jb_putc(&j, ',');
        jb_str(&j,  "name",  pd->name);                       jb_putc(&j, ',');
        jb_str(&j,  "state", proc_state_name(pd->state));     jb_putc(&j, ',');
        jb_uint(&j, "uid",   pd->owner_uid);                  jb_putc(&j, ',');
        jb_hex(&j,  "rip",   pd->user_rip);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/query?q=<text> ─────────────────────────────────────────────────
static int api_query_json(const char* q, char* buf, int max) {
    QueryDomain d = query_domain_for(q);
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "query",  q);                       jb_putc(&j, ',');
    jb_str(&j, "domain", query_domain_name(d));    jb_putc(&j, ',');
    // Embed full scan as the data payload
    jb_raw(&j, "\"data\":");
    api_scan(buf + j.pos, max - j.pos - 32);
    int scan_len = 0; while (buf[j.pos + scan_len]) scan_len++;
    j.pos += scan_len;
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── uint64 → decimal string (freestanding; no printf) ─────────────────────
static void prog_u64_to_str(uint64_t v, char* out, int max) {
    if (max < 2) { out[0] = '\0'; return; }
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[21]; int len = 0;
    while (v && len < 20) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
    int i; for (i = 0; i < len && i < max - 1; i++) out[i] = tmp[len - 1 - i];
    out[i] = '\0';
}

// ─── Hex decode helper (used by program upload) ─────────────────────────────
static size_t hex_decode(const char* hex, uint8_t* out, size_t max_bytes) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < max_bytes) {
        char c;
        uint8_t hi, lo;
        c = hex[0];
        hi = (c>='0'&&c<='9') ? (uint8_t)(c-'0')
           : (c>='a'&&c<='f') ? (uint8_t)(c-'a'+10)
           : (c>='A'&&c<='F') ? (uint8_t)(c-'A'+10) : 0xFF;
        c = hex[1];
        lo = (c>='0'&&c<='9') ? (uint8_t)(c-'0')
           : (c>='a'&&c<='f') ? (uint8_t)(c-'a'+10)
           : (c>='A'&&c<='F') ? (uint8_t)(c-'A'+10) : 0xFF;
        if (hi == 0xFF || lo == 0xFF) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

// ─── GET /api/programs ────────────────────────────────────────────────────────
static int api_programs_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "programs");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || e->type != OBJ_TYPE_PROGRAM) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name",  e->name);         jb_putc(&j, ',');
        jb_hex(&j,  "vaddr", e->base_vaddr);   jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);   jb_putc(&j, ',');
        jb_str(&j,  "tier",  tier_name(e->storage_tier)); jb_putc(&j, ',');
        int bin_loaded = 0;
        uint32_t bin_size = 0;
        const char* bin_fmt = "none";
        for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
            if (service_binaries[b].active &&
                !strcmp(service_binaries[b].object_name, e->name)) {
                bin_loaded = 1;
                bin_size   = service_binaries[b].size;
                bin_fmt    = binary_format_name(&service_binaries[b]);
                break;
            }
        }
        jb_str(&j,  "binary",       bin_loaded ? "yes" : "no"); jb_putc(&j, ',');
        jb_uint(&j, "binary_bytes", (uint64_t)bin_size);        jb_putc(&j, ',');
        jb_str(&j,  "format",       bin_fmt);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── GET /api/simi/<name> — Gap Remediation Phase G ────────────────────────────
// Structured counterpart to loader_simi_info()'s own console dump -- reads
// through loader_simi_info_query() directly (the same real logic
// loader_simi_info() itself now wraps, see loader.c's own comment), not the
// SYS_SLS_SIMI_INFO syscall -- matching Phase B/C/F's established "read
// kernel state / call the query function directly for a JSON response
// rather than repurposing a console-dump-shaped syscall" precedent.
static int api_simi_info(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    struct SimiInfoResult r;
    loader_simi_info_query(name, &r);

    jb_obj_open(&j, 0);
    jb_str(&j, "name", name); jb_putc(&j, ',');
    const char* status_str =
        r.status == SIMI_INFO_STATUS_OK        ? "ok" :
        r.status == SIMI_INFO_STATUS_NOT_FOUND ? "not_found" :
        r.status == SIMI_INFO_STATUS_NOT_SIMI  ? "not_simi" : "corrupt";
    jb_str(&j, "status", status_str);
    if (r.status == SIMI_INFO_STATUS_NOT_SIMI) {
        jb_putc(&j, ','); jb_str(&j, "format", r.format_name);
    }
    if (r.status == SIMI_INFO_STATUS_OK) {
        jb_putc(&j, ',');
        jb_uint(&j, "num_instr",    r.num_instr);    jb_putc(&j, ',');
        jb_uint(&j, "num_literals", r.num_literals);  jb_putc(&j, ',');
        jb_uint(&j, "num_entries",  r.num_entries);   jb_putc(&j, ',');
        jb_uint(&j, "num_names",    r.num_names);     jb_putc(&j, ',');
        jb_arr_open(&j, "entries");
        for (uint32_t i = 0; i < r.entries_returned; i++) {
            if (i) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_str(&j, "name", r.entries[i].name); jb_putc(&j, ',');
            jb_uint(&j, "offset", r.entries[i].offset);
            jb_obj_close(&j);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_str(&j, "entries_truncated", r.entries_truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_arr_open(&j, "names");
        for (uint32_t i = 0; i < r.names_returned; i++) {
            if (i) jb_putc(&j, ',');
            jb_esc_str(&j, r.names[i].name);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_str(&j, "names_truncated", r.names_truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_obj_open(&j, "activation");
        jb_str(&j, "cached", r.activation.cached ? "true" : "false");
        if (r.activation.cached) {
            jb_putc(&j, ',');
            jb_uint(&j, "code_pages",   r.activation.code_pages);   jb_putc(&j, ',');
            jb_uint(&j, "entry_offset", r.activation.entry_offset); jb_putc(&j, ',');
            jb_hex(&j,  "content_hash", r.activation.content_hash);
        }
        jb_obj_close(&j);
    }
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── POST /api/program/create ─────────────────────────────────────────────────
static int api_program_create(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    struct SLSVallocRequest req;
    req.owner_uid = 0;
    req.perm_mask = PERM_READ | PERM_EXECUTE | PERM_OWNER;
    req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    req.type      = OBJ_TYPE_PROGRAM;
    req.name[0]   = '\0';
    json_str(body, "name", req.name, OBJECT_NAME_LEN);
    req.size_pages = (uint32_t)json_int(body, "pages");
    if (!req.name[0] || !req.size_pages) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name and pages required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    uint64_t id = sys_sls_valloc(&req);
    jb_obj_open(&j, 0);
    if (id) {
        /* Step 4: seed metadata records so the full DB1-DB7 hook chain
           (journal, lock, index, constraint, MQT) fires on every
           subsequent upload and spawn lifecycle event. */
        struct SLSRecordRequest mr;
        int i; for (i=0;i<OBJECT_NAME_LEN-1&&req.name[i];i++) mr.name[i]=req.name[i]; mr.name[i]='\0';
        mr.key[0]='\0'; mr.value[0]='\0';
        /* status */
        mr.key[0]='s'; mr.key[1]='t'; mr.key[2]='a'; mr.key[3]='t'; mr.key[4]='u'; mr.key[5]='s'; mr.key[6]='\0';
        mr.value[0]='c'; mr.value[1]='r'; mr.value[2]='e'; mr.value[3]='a'; mr.value[4]='t'; mr.value[5]='e'; mr.value[6]='d'; mr.value[7]='\0';
        sys_sls_insert(&mr);
        /* binary_size */
        const char* bsk = "binary_size"; for (i=0;bsk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=bsk[i]; mr.key[i]='\0';
        mr.value[0]='0'; mr.value[1]='\0';
        sys_sls_insert(&mr);
        /* format */
        const char* fmk = "format"; for (i=0;fmk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=fmk[i]; mr.key[i]='\0';
        mr.value[0]='n'; mr.value[1]='o'; mr.value[2]='n'; mr.value[3]='e'; mr.value[4]='\0';
        sys_sls_insert(&mr);
        /* last_pid */
        const char* lpk = "last_pid"; for (i=0;lpk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=lpk[i]; mr.key[i]='\0';
        mr.value[0]='0'; mr.value[1]='\0';
        sys_sls_insert(&mr);
        jb_str(&j, "ok",   "true");     jb_putc(&j, ',');
        jb_hex(&j, "object_id", id);    jb_putc(&j, ',');
        jb_str(&j, "type", "PROGRAM");
    } else {
        jb_str(&j, "ok",    "false");   jb_putc(&j, ',');
        jb_str(&j, "error", "valloc failed");
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/program/upload ─────────────────────────────────────────────────
// Body: {"name":"<obj>","hex":"<hex-bytes>","offset":N,"last":0|1}
//   hex:    lower- or upper-case hex pairs of raw binary bytes
//   offset: byte offset into the program binary this chunk starts at
//   last:   1 = final chunk; triggers size finalisation in binary store
static int api_program_upload(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    /* UPLOAD_CHUNK_MAX = 1024 bytes = 2048 hex chars; static avoids stack bloat */
    static char hex_buf[UPLOAD_CHUNK_MAX * 2 + 4];
    static struct SLSUploadRequest ureq;
    hex_buf[0] = '\0';
    ureq.object_name[0] = '\0';
    json_str(body, "name", ureq.object_name, PROC_NAME_LEN);
    json_str(body, "hex",  hex_buf, (int)sizeof(hex_buf));
    int offset = json_int(body, "offset");
    int last   = json_int(body, "last");
    if (!ureq.object_name[0] || !hex_buf[0]) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name and hex required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    ureq.byte_offset = (uint32_t)(offset >= 0 ? offset : 0);
    ureq.is_last     = (uint8_t)(last ? 1 : 0);
    ureq.chunk_len   = (uint32_t)hex_decode(hex_buf, ureq.chunk, UPLOAD_CHUNK_MAX);
    jb_obj_open(&j, 0);
    if (ureq.chunk_len == 0) {
        jb_str(&j, "ok",    "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "hex decode produced zero bytes");
    } else {
        uint64_t rc = sys_sls_upload_binary(&ureq);
        jb_str(&j,  "ok",           rc == 0 ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "bytes_written", (uint64_t)ureq.chunk_len);   jb_putc(&j, ',');
        jb_uint(&j, "offset",        (uint64_t)ureq.byte_offset); jb_putc(&j, ',');
        jb_str(&j,  "final",         ureq.is_last ? "true" : "false");
        /* Step 4: on the final chunk, write metadata records so journal,
           index, and MQT machinery see the completed upload. */
        if (rc == 0 && ureq.is_last) {
            /* Discover binary size + format from the store */
            uint32_t tot_bytes = 0;
            const char* fmt_str = "flat";
            for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
                if (service_binaries[b].active &&
                    !strcmp(service_binaries[b].object_name, ureq.object_name)) {
                    tot_bytes = service_binaries[b].size;
                    fmt_str   = binary_format_name(&service_binaries[b]);
                    break;
                }
            }
            struct SLSRecordRequest mr;
            int i; for (i=0;i<PROC_NAME_LEN-1&&ureq.object_name[i];i++) mr.name[i]=ureq.object_name[i]; mr.name[i]='\0';
            /* binary_size */
            const char* bsk="binary_size"; for (i=0;bsk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=bsk[i]; mr.key[i]='\0';
            prog_u64_to_str((uint64_t)tot_bytes, mr.value, RECORD_VAL_LEN);
            sys_sls_update(&mr);
            /* format */
            const char* fmk="format"; for (i=0;fmk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=fmk[i]; mr.key[i]='\0';
            for (i=0;fmt_str[i]&&i<RECORD_VAL_LEN-1;i++) mr.value[i]=fmt_str[i]; mr.value[i]='\0';
            sys_sls_update(&mr);
            /* status -> ready */
            const char* stk="status"; for (i=0;stk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=stk[i]; mr.key[i]='\0';
            mr.value[0]='r'; mr.value[1]='e'; mr.value[2]='a'; mr.value[3]='d'; mr.value[4]='y'; mr.value[5]='\0';
            sys_sls_update(&mr);
        }
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/program/spawn ──────────────────────────────────────────────────
// Gap Remediation Phase F: req_uid now threads through to program_load() as
// the real caller identity, replacing a hardcoded 0 (PARTITION_SYSTEM/kernel
// identity) named as its own separate gap in docs/AeroSLS-Gap-Analysis-v0.1.md
// §5 and again in the Phase B addendum's own /api/sql comment -- every
// spawned process was silently owned by the kernel regardless of who
// actually authenticated the request, meaning catalog_check_access()'s
// PERM_EXECUTE check inside program_spawn() (kernel/process.c) was checking
// the wrong identity, and every spawned process landed in PARTITION_SYSTEM
// instead of the caller's own partition (kernel/partition.c's
// partition_get_for_uid()). Same req_uid plumbing api_table_create_post()/
// api_sql_post()/api_agent_create() already established -- copying an
// existing pattern, not inventing one.
static int api_program_spawn_handler(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    char pname[OBJECT_NAME_LEN];
    pname[0] = '\0';
    json_str(body, "name", pname, OBJECT_NAME_LEN);
    if (!pname[0]) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    /* program_load() validates OBJ_TYPE_PROGRAM, calls program_spawn()
       which maps pages into a fresh PML4 and enters Ring-3. */
    uint64_t pid = program_load(pname, req_uid);
    jb_obj_open(&j, 0);
    jb_str(&j,  "ok",  pid ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "pid", pid);
    /* Step 4: on successful spawn, update metadata records so journal
       captures the before/after audit trail and MQTs auto-refresh. */
    if (pid) {
        struct SLSRecordRequest mr;
        int i; for (i=0;i<OBJECT_NAME_LEN-1&&pname[i];i++) mr.name[i]=pname[i]; mr.name[i]='\0';
        /* status -> running */
        const char* stk="status"; for (i=0;stk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=stk[i]; mr.key[i]='\0';
        mr.value[0]='r'; mr.value[1]='u'; mr.value[2]='n'; mr.value[3]='n'; mr.value[4]='i'; mr.value[5]='n'; mr.value[6]='g'; mr.value[7]='\0';
        sys_sls_update(&mr);
        /* last_pid */
        const char* lpk="last_pid"; for (i=0;lpk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=lpk[i]; mr.key[i]='\0';
        prog_u64_to_str(pid, mr.value, RECORD_VAL_LEN);
        sys_sls_update(&mr);
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── Stream binary download ────────────────────────────────────────────────────
static void http_respond_stream(int conn, struct StreamEntry* se) {
    char hdr[384]; int hp=0;
    const char* sl="HTTP/1.1 200 OK\r\n"; while(*sl) hdr[hp++]=*sl++;
    const char* ct="Content-Type: "; while(*ct) hdr[hp++]=*ct++;
    const char* mt=se->mime_type[0] ? se->mime_type : "application/octet-stream";
    while(*mt) hdr[hp++]=*mt++;
    hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* cd="Content-Disposition: attachment; filename=\"";
    while(*cd) hdr[hp++]=*cd++;
    const char* fn=se->name; while(*fn) hdr[hp++]=*fn++;
    hdr[hp++]='"'; hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* cl="Content-Length: "; while(*cl) hdr[hp++]=*cl++;
    char tmp[12]; int tl=0; uint32_t v=se->size;
    if (!v){tmp[tl++]='0';} else {while(v){tmp[tl++]=(char)('0'+v%10);v/=10;}}
    for(int i=tl-1;i>=0;i--) hdr[hp++]=tmp[i];
    hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* co="Access-Control-Allow-Origin: *\r\n"; while(*co) hdr[hp++]=*co++;
    hdr[hp++]='\r'; hdr[hp++]='\n';
    tcp_send(conn, hdr, (uint32_t)hp);
    /* Send content frame-by-frame; lazy-load from NVMe if frame not in RAM
       (happens on first download after a reboot). */
    if (se->size > 0) {
        uint32_t remaining = se->size;
        for (uint32_t fi = 0; fi < STREAM_MAX_FRAMES && remaining > 0; fi++) {
            uint8_t* frame_data = se->frames[fi];
            if (!frame_data) {
                frame_data = stream_lazy_load_frame(se, fi);
                if (!frame_data) break;  /* NVMe read failed — truncate response */
            }
            uint32_t to_send = remaining < 4096u ? remaining : 4096u;
            tcp_send(conn, (const char*)frame_data, to_send);
            remaining -= to_send;
        }
    }
}

// ─── Program binary download ───────────────────────────────────────────────────
static void http_respond_program_binary(int conn, struct ServiceBinary* sb) {
    char hdr[512]; int hp = 0;
    const char* sl = "HTTP/1.1 200 OK\r\n"; while (*sl) hdr[hp++] = *sl++;
    const char* ct = "Content-Type: application/octet-stream\r\n";
    while (*ct) hdr[hp++] = *ct++;
    const char* cd = "Content-Disposition: attachment; filename=\"";
    while (*cd) hdr[hp++] = *cd++;
    const char* fn = sb->object_name; while (*fn) hdr[hp++] = *fn++;
    hdr[hp++] = '"'; hdr[hp++] = '\r'; hdr[hp++] = '\n';
    /* X-Binary-Format header so the downloader knows ELF vs flat vs SIMI */
    const char* xbf = "X-Binary-Format: ";
    while (*xbf) hdr[hp++] = *xbf++;
    const char* fmt = binary_format_name(sb);
    while (*fmt) hdr[hp++] = *fmt++;
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    const char* cl = "Content-Length: "; while (*cl) hdr[hp++] = *cl++;
    char tmp[12]; int tl = 0; uint32_t v = sb->size;
    if (!v) { tmp[tl++] = '0'; } else { while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = tl - 1; i >= 0; i--) hdr[hp++] = tmp[i];
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    const char* co = "Access-Control-Allow-Origin: *\r\n"; while (*co) hdr[hp++] = *co++;
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    tcp_send(conn, hdr, (uint32_t)hp);
    if (sb->size > 0)
        tcp_send(conn, (const char*)sb->data, sb->size);
}

// ─── POST /api/stream/create ──────────────────────────────────────────────────
static int api_stream_create(const char* body, char* buf, int max) {
    JSONBuf j={buf,0,max};
    if (!body){jb_obj_open(&j,0);jb_str(&j,"error","missing body");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    char sname[STREAM_NAME_LEN], smime[STREAM_MIME_LEN];
    sname[0]=smime[0]='\0';
    json_str(body,"name",sname,STREAM_NAME_LEN);
    json_str(body,"mime",smime,STREAM_MIME_LEN);
    if (!sname[0]){jb_obj_open(&j,0);jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","name required");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    int rc=stream_create(sname,smime);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false");jb_putc(&j,',');
    jb_str(&j,"name",sname);
    if(rc!=0){jb_putc(&j,',');jb_str(&j,"error",rc==2?"already exists":"store full");}
    jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;
}

// ─── POST /api/stream/upload ──────────────────────────────────────────────────
static int api_stream_upload(const char* body, char* buf, int max) {
    JSONBuf j={buf,0,max};
    if (!body){jb_obj_open(&j,0);jb_str(&j,"error","missing body");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    static char st_hex[UPLOAD_CHUNK_MAX*2+4];
    static uint8_t st_chunk[UPLOAD_CHUNK_MAX];
    char sname[STREAM_NAME_LEN]; sname[0]=st_hex[0]='\0';
    json_str(body,"name",sname,STREAM_NAME_LEN);
    json_str(body,"hex", st_hex,(int)sizeof(st_hex));
    int offset=json_int(body,"offset"), last=json_int(body,"last");
    if (!sname[0]||!st_hex[0]){jb_obj_open(&j,0);jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","name and hex required");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    size_t decoded=hex_decode(st_hex,st_chunk,UPLOAD_CHUNK_MAX);
    jb_obj_open(&j,0);
    if(decoded==0){jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","hex decode produced zero bytes");}
    else{
        int rc=stream_write_chunk(sname,st_chunk,(uint32_t)decoded,(uint32_t)(offset>=0?offset:0),(uint8_t)(last?1:0));
        jb_str(&j,"ok",rc==0?"true":"false");jb_putc(&j,',');
        jb_uint(&j,"bytes_written",(uint64_t)decoded);jb_putc(&j,',');
        jb_str(&j,"final",last?"true":"false");
    }
    jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;
}

// ─── POST /api/valloc ─────────────────────────────────────────────────────────
static int api_valloc_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVallocRequest req;
    req.owner_uid = 0; req.perm_mask = 0;
    req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    json_str(body, "name", req.name, OBJECT_NAME_LEN);
    req.type       = (SLSObjectType)json_int(body, "type");
    req.size_pages = (uint32_t)json_int(body, "pages");
    if (!req.name[0] || !req.size_pages) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name and pages required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t id = sys_sls_valloc(&req);
    jb_obj_open(&j,0);
    if (id) {
        jb_str(&j,"ok","true"); jb_putc(&j,',');
        jb_hex(&j,"object_id",id);
    } else {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","valloc failed");
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/schema — Gap Remediation Phase H ────────────────────────────────
// Closes the "smaller, separate, still-open gap" api_table_create_post()'s
// own comment (right below) has flagged since Phase B: sys_sls_schema_set()
// (SYS_SLS_SCHEMA_SET, object_catalog.c) was already syscall/shell-reachable
// ("schema set <object> <key> <type>", user/shell.c) but had no HTTP route,
// meaning the three-step "create a real SQL table" flow (valloc -> schema
// set (one field at a time) -> POST /api/tables) had its middle step missing
// over HTTP -- the frontend could valloc an empty object and promote it, but
// never actually define its columns. Body: {"name": "<object_name>",
// "columns": [{"name":"<col>", "type":"STRING|UINT64|INT|FLOAT|BOOL"}, ...]}.
// Loops sys_sls_schema_set() once per column (that syscall's own one-field-
// at-a-time contract, not reinvented here), same json_array_object_at()
// array-of-objects idiom /api/vec/join already established, and the same
// type-string aliasing shell.c's own "schema set" command already uses
// (INT accepted as UINT64's alias; unrecognized/absent type defaults to
// STRING). Stops at the first column that fails (object not found, or
// object already promoted to row-store -- schema is frozen post-promotion,
// see object_catalog.c's own guard comment) rather than silently applying a
// partial schema and calling it success.
static int api_schema_set_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSSchemaRequest req;
    json_str(body, "name", req.object_name, OBJECT_NAME_LEN);
    if (!req.object_name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    char colbuf[192];
    char typ[16];
    uint32_t applied = 0;
    int failed = 0;
    for (int n = 0; n < SCHEMA_MAX_FIELDS; n++) {
        if (!json_array_object_at(body, "columns", n, colbuf, (int)sizeof(colbuf))) break;
        json_str(colbuf, "name", req.key, RECORD_KEY_LEN);
        typ[0] = '\0';
        json_str(colbuf, "type", typ, (int)sizeof(typ));
        if      (!strcmp(typ, "UINT64") || !strcmp(typ, "INT")) req.type = FIELD_TYPE_UINT64;
        else if (!strcmp(typ, "FLOAT"))                          req.type = FIELD_TYPE_FLOAT;
        else if (!strcmp(typ, "BOOL"))                           req.type = FIELD_TYPE_BOOL;
        else                                                      req.type = FIELD_TYPE_STRING;
        if (!req.key[0]) { failed = 1; break; }
        uint64_t rc = sys_sls_schema_set(&req);
        if (rc != 0) { failed = 1; break; }
        applied++;
    }
    jb_obj_open(&j,0);
    jb_str(&j, "ok", (!failed && applied) ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.object_name); jb_putc(&j,',');
    jb_uint(&j, "columns_set", applied);
    if (failed) {
        jb_putc(&j,',');
        jb_str(&j, "error", applied == 0 && !req.key[0]
                             ? "malformed or empty columns array"
                             : "schema_set failed (object not found, or already promoted to row-store)");
        jb_putc(&j,',');
        jb_str(&j, "failed_column", req.key);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tables — Gap Remediation Phase B ───────────────────────────────
// The live path rowstore_create_table() never had: before this, the only
// way to promote a valloc'd + schema'd catalog object into a real row-set
// table was a host test calling rowstore_create_table() directly (see
// docs/AeroSLS-Gap-Analysis-v0.1.md §2/§5). Body: {"name": "<table_name>"}.
// Does not valloc or schema-set for the caller -- POST /api/valloc
// (type=1/DB_TABLE) covers the first step, POST /api/schema (Gap
// Remediation Phase H, above) covers the second.
static int api_table_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRowstoreCreateTableRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.table_name, OBJECT_NAME_LEN);
    if (!req.table_name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_rowstore_create_table(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.table_name);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/sql — Gap Remediation Phase B ──────────────────────────────────
// The live path sql_execute() never had over HTTP: SYS_SLS_SQL_EXECUTE
// (Phase 22) was already syscall/shell-reachable, but net/http.c had zero
// "sql" routes at all (docs/AeroSLS-Gap-Analysis-v0.1.md §5) -- confirmed
// by direct inspection before this phase, not assumed. Body:
// {"query": "<statement>"}. Runs one autocommit statement under req_uid
// (the real bearer-token identity, matching the agent/workflow POST routes'
// own req_uid convention -- not a hardcoded uid, unlike the program-spawn
// routes' own separately-named gap in the analysis doc). SELECT results are
// fetched and embedded directly in this one response (up to however many
// rows fit in the shared resp_body buffer -- this file's existing, established
// truncate-silently-at-the-byte-buffer convention, same as every other
// dump-a-collection endpoint here; no new pagination route is added).
struct sql_row_json_ctx { JSONBuf* j; int first; };
static void sql_row_to_json_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct sql_row_json_ctx* ctx = (struct sql_row_json_ctx*)ctxp;
    if (!ctx->first) jb_putc(ctx->j, ',');
    ctx->first = 0;
    jb_arr_open(ctx->j, 0);
    for (uint32_t i = 0; i < v->count; i++) {
        if (i) jb_putc(ctx->j, ',');
        jb_esc_str(ctx->j, v->values[i]);
    }
    jb_arr_close(ctx->j);
}
// ─── Kernel-Side Shell Refactor (docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10.4) ─
// extern, not a new header -- matches how kernel.c declares sls_shell_loop()
// today (a plain extern at the one call site, no shell.h for one function).
extern int sls_shell_execute(const char* input, char* out_buf, size_t out_cap);

// Must match the constant of the same name in user/shell.c's sls_shell_loop()
// -- no shared header exists for this one value, so both copies carry this
// cross-reference instead (see shell.c's own comment on the same constant).
#define SHELL_EXEC_OUT_CAP 8192

// Runs the FULL ~90-command shell.c dispatch, not a curated subset -- this
// is what closes every command on §3's "Missing" list (login, role set,
// grant, revoke, chmod, auth create/list/revoke, seal, write, demo,
// upload, load, loader list, svc crash/restart, proc kill, ipc post/stat,
// journal create/purge, tier demote/promote, vfree, workflow addstep,
// webapp set/list/append) without 24 individual new routes -- see §10.6.
// req_uid is unused: shell session state (current_session_uid/gid/
// current_tx_id) is shared, global, file-scope state in shell.c, the same
// single-simulated-machine model this app already has everywhere else
// (one DEMO_TOKEN, no per-request session) -- see §10.3.
static int api_shell_exec_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    (void)req_uid;
    char command[256];
    json_str(body, "command", command, sizeof(command));
    static char shell_out[SHELL_EXEC_OUT_CAP];
    int recognized = sls_shell_execute(command, shell_out, sizeof(shell_out));
    jb_obj_open(&j, 0);
    jb_str(&j, "ok", recognized ? "true" : "false"); jb_putc(&j, ',');
    jb_str_multiline(&j, "output", shell_out);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_sql_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSSqlRequest req;
    req.caller_uid = req_uid;
    json_str(body, "query", req.sql_text, SQL_MAX_TEXT_LEN);
    uint64_t rc = sys_sls_sql_execute(&req);

    jb_obj_open(&j, 0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j, ',');
    if (rc != 0) {
        jb_uint(&j, "error_code", (uint64_t)req.result.error); jb_putc(&j, ',');
        jb_str(&j, "error", req.result.error_msg);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (req.result.kind == SQL_STMT_SELECT) {
        jb_uint(&j, "row_count", req.result.row_count); jb_putc(&j, ',');
        jb_str(&j, "truncated", req.result.truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_arr_open(&j, "columns");
        for (uint32_t c = 0; c < req.result.column_count; c++) {
            if (c) jb_putc(&j, ',');
            jb_esc_str(&j, req.result.columns[c]);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_arr_open(&j, "rows");
        struct sql_row_json_ctx ctx = { &j, 1 };
        cursor_fetch_rows(req.result.cursor_id, req.result.row_count, sql_row_to_json_cb, &ctx);
        cursor_close(req.result.cursor_id);
        jb_arr_close(&j);
    } else {
        jb_uint(&j, "affected_rows", req.result.affected_rows);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/collections — Gap Remediation Phase C ───────────────────────
// Enumerates vector collections -- reads vector_collections[] directly
// (index-aligned with object_catalog[], same idiom api_tables_list() uses
// for row-set tables) rather than going through SYS_SLS_VEC_LIST, which
// dumps to the serial console, not a return buffer -- matching Phase B's
// own GET-route precedent of reading kernel state directly for a JSON
// response rather than repurposing a console-dump syscall.
static int api_vec_collections_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "collections");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active || !vector_collections[i].active) continue;
        struct VecCollectionHeader* h = &vector_collections[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", object_catalog[i].name); jb_putc(&j, ',');
        jb_uint(&j, "dimension", h->dimension); jb_putc(&j, ',');
        jb_uint(&j, "entry_count", h->entry_count); jb_putc(&j, ',');
        jb_uint(&j, "page_count", h->page_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/indexes — Gap Remediation Phase C ────────────────────────────
// Enumerates HNSW indexes -- reads vec_indexes[] directly, same reasoning
// as api_vec_collections_list() above.
static int api_vec_indexes_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "indexes");
    int first = 1;
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        struct VecIndex* idx = &vec_indexes[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", idx->index_name); jb_putc(&j, ',');
        jb_str(&j, "collection", idx->collection_name); jb_putc(&j, ',');
        jb_str(&j, "metric", idx->metric == VEC_METRIC_L2 ? "l2" : "cosine"); jb_putc(&j, ',');
        jb_uint(&j, "active_count", idx->active_count); jb_putc(&j, ',');
        jb_uint(&j, "node_count", idx->node_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partitions — Gap Remediation Phase F ─────────────────────────────
// Enumerates defined partitions -- reads partition_table[] directly, same
// "read kernel state, don't repurpose a console-dump syscall" idiom as
// api_vec_collections_list() above. Every partition syscall (create, list,
// destroy, assign, pause, resume) was already correctly wired at the
// syscall/dispatch layer since Phase 8/14 -- this is the first time any of
// it is reachable outside a host test or the raw shell (docs/AeroSLS-Gap-
// Remediation-Roadmap-v0.1.md Phase F).
static int api_partitions_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "partitions");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "id", partition_table[i].partition_id); jb_putc(&j, ',');
        jb_str(&j, "name", partition_table[i].name); jb_putc(&j, ',');
        jb_uint(&j, "frame_usage", (uint32_t)partition_get_frame_usage(i)); jb_putc(&j, ',');
        uint64_t quota = partition_get_frame_quota(i);
        jb_uint(&j, "frame_quota", (uint32_t)quota); jb_putc(&j, ',');
        jb_str(&j, "quota_unlimited", quota == 0 ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partition/quotas — Gap Remediation Phase F ───────────────────────
// Same per-partition usage/quota data folded into api_partitions_list() above
// (added there directly, the same "one list, everything about it" shape
// api_tables_list() already established) -- this route exists as a distinct,
// narrower endpoint anyway, mirroring the syscall-level split between
// sys_sls_partition_list() and sys_sls_partition_quota_list() (Phase F also
// gave the latter its first syscall number -- frame_pool.h), so a caller who
// only cares about quota pressure doesn't have to parse the wider payload.
static int api_partition_quotas_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "quotas");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        uint64_t usage = partition_get_frame_usage(i);
        uint64_t quota = partition_get_frame_quota(i);
        if (usage == 0 && quota == 0) continue;   // mirrors sys_sls_partition_quota_list()'s own skip rule
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", i); jb_putc(&j, ',');
        jb_uint(&j, "usage", (uint32_t)usage); jb_putc(&j, ',');
        jb_uint(&j, "quota", (uint32_t)quota); jb_putc(&j, ',');
        jb_str(&j, "unlimited", quota == 0 ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partitions — Gap Remediation Phase F ────────────────────────────
// Body: {"name": "<partition_name>"}. Thin wrapper over
// sys_sls_partition_create(), the same shape as api_table_create_post().
static int api_partition_create_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionCreateRequest req;
    req.name[0] = '\0';
    json_str(body, "name", req.name, PARTITION_NAME_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t id = sys_sls_partition_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", id != 0xFFFFFFFFu ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "partition_id", (uint32_t)id);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/assign — Gap Remediation Phase F ─────────────────────
// Body: {"uid": N, "partition_id": N}.
static int api_partition_assign_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionAssignRequest req;
    req.uid          = (uint32_t)json_int(body, "uid");
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = sys_sls_partition_assign(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/destroy — Gap Remediation Phase F ────────────────────
// Body: {"partition_id": N}.
static int api_partition_destroy_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    uint32_t partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = sys_sls_partition_destroy(partition_id);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/pause | /api/partition/resume — Gap Remediation
// Phase F ─────────────────────────────────────────────────────────────────────
// Body: {"partition_id": N}. `resume` selects sys_sls_partition_resume()
// instead of sys_sls_partition_pause() -- same one-function-two-routes shape
// api_tx_post() already established for commit/rollback.
static int api_partition_pause_post(const char* body, char* buf, int max, int resume) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    uint32_t partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = resume ? sys_sls_partition_resume(partition_id)
                         : sys_sls_partition_pause(partition_id);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/quota — Gap Remediation Phase F ──────────────────────
// Body: {"partition_id": N, "frame_quota": N}. frame_quota=0 means unlimited
// (frame_pool.h's own convention) -- passing it explicitly re-uncaps a
// previously-limited partition, not just an omission default.
static int api_partition_quota_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionQuotaSetRequest req;
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    req.frame_quota  = json_uint64(body, "frame_quota");
    uint64_t rc = sys_sls_partition_quota_set(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/collections — Gap Remediation Phase C ──────────────────────
// The live path Vector Store Phase 4's SYS_SLS_VEC_CREATE never had over
// HTTP -- syscall/shell-reachable since Phase 4, zero HTTP routes for any
// vector-store capability before this (docs/AeroSLS-Gap-Analysis-v0.1.md
// §7). Body: {"name": "<collection>", "dimension": N}. Requires the
// catalog object to already exist via POST /api/valloc, matching
// SYS_SLS_VEC_CREATE's own precondition.
static int api_vec_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecCreateRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.collection_name, OBJECT_NAME_LEN);
    req.dimension = (uint32_t)json_int(body, "dimension");
    uint64_t rc = sys_sls_vec_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.collection_name); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/insert — Gap Remediation Phase C ────────────────────────────
// Body: {"collection": "<name>", "external_id": N, "values": [f0, f1, ...]}.
static int api_vec_insert_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecInsertRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.external_id = json_uint64(body, "external_id");
    req.values.count = (uint32_t)json_float_array(body, "values", req.values.values, VECSTORE_MAX_DIMENSION);
    uint64_t rc = sys_sls_vec_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status); jb_putc(&j,',');
    jb_uint(&j, "page_id", req.out_id.page_id); jb_putc(&j,',');
    jb_uint(&j, "slot_index", req.out_id.slot_index);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/embed-insert — Gap Remediation Phase C ──────────────────────
// Body: {"collection": "<name>", "external_id": N, "endpoint_ip": "...",
// "port": N, "model": "...", "prompt": "..."}. endpoint_ip/port default to
// 127.0.0.1:11434 (local Ollama) if omitted, matching the shell command's
// own "embed-insert always targets a local Ollama instance" convention
// (user/shell.c).
static int api_vec_embed_insert_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecEmbedInsertRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.external_id = json_uint64(body, "external_id");
    json_str_or_default(body, "endpoint_ip", req.ollama_req.endpoint_ip, OLLAMA_ENDPOINT_LEN, "127.0.0.1");
    int port = json_int(body, "port");
    req.ollama_req.port = (uint16_t)(port ? port : 11434);
    json_str_or_default(body, "model", req.ollama_req.model, OLLAMA_MODEL_LEN, "nomic-embed-text");
    req.ollama_req.prompt[0] = '\0';
    json_str(body, "prompt", req.ollama_req.prompt, OLLAMA_PROMPT_LEN);
    uint64_t rc = sys_sls_vec_embed_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "ollama_status", (uint64_t)req.ollama_status); jb_putc(&j,',');
    jb_uint(&j, "insert_status", (uint64_t)req.insert_status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/search — Gap Remediation Phase C ────────────────────────────
// Body: {"collection": "<name>", "query": [f0, f1, ...], "metric":
// "cosine"|"l2" (default cosine), "k": N}.
static int api_vec_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.query.count = (uint32_t)json_float_array(body, "query", req.query.values, VECSTORE_MAX_DIMENSION);
    char metric_s[16]; metric_s[0] = '\0';
    json_str(body, "metric", metric_s, (int)sizeof(metric_s));
    req.metric = (!strcmp(metric_s, "l2") || !strcmp(metric_s, "L2")) ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    uint64_t rc = sys_sls_vec_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        // distance is a float -- no float formatter in this file's JSONBuf
        // yet; round-trip via the same fixed-6-decimal shape rowstore.c's
        // own rs_f64_to_str() uses, inlined here since it's the only float
        // field this whole API surface has ever needed to emit.
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/indexes — Gap Remediation Phase C ───────────────────────────
// Live path for vec_index_create() (Vector Store Phase 6, HNSW), which had
// no syscall/shell/HTTP surface at all before this pass (vec_index.h's own
// point 6 named it a deliberate "revisit only when a real caller needs it"
// cut). Body: {"name": "<index>", "collection": "<collection>", "metric":
// "cosine"|"l2" (default cosine)}. Does not backfill an already-populated
// collection -- see vec_index.h's own point 7.
static int api_vec_index_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecIndexCreateRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.index_name, OBJECT_NAME_LEN);
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    char metric_s[16]; metric_s[0] = '\0';
    json_str(body, "metric", metric_s, (int)sizeof(metric_s));
    req.metric = (!strcmp(metric_s, "l2") || !strcmp(metric_s, "L2")) ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
    uint64_t rc = sys_sls_vec_index_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.index_name); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/index/search — Gap Remediation Phase C ──────────────────────
// Live path for vec_index_search() (approximate top-K over the HNSW
// index). Body: {"index": "<name>", "query": [f0, f1, ...], "k": N,
// "ef": N (default = k)}. Response shape matches POST /api/vec/search's own
// (external_id/page_id/slot_index/distance per match) so a client can treat
// exact and approximate search as interchangeable result consumers.
static int api_vec_index_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecIndexSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "index", req.index_name, OBJECT_NAME_LEN);
    req.query.count = (uint32_t)json_float_array(body, "query", req.query.values, VECSTORE_MAX_DIMENSION);
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    req.ef = (uint32_t)json_int(body, "ef");
    if (!req.ef) req.ef = req.k;
    uint64_t rc = sys_sls_vec_index_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/join — Gap Remediation Phase C ──────────────────────────────
// Live path for vec_join_resolve() (Vector Store Phase 5), which had no
// syscall/shell/HTTP surface at all before this pass. Body: {"table":
// "<name>", "id_column": "<col>", "matches": [{"external_id": N,
// "page_id": N, "slot_index": N, "distance": F}, ...]} -- takes matches
// directly (typically the "matches" array from a prior POST /api/vec/search
// or /api/vec/index/search response, round-tripped as-is) rather than
// re-running a search itself, matching sys_sls_vec_join()'s own real
// contract: this is the join primitive alone, composable with either search
// path via ordinary REST calls rather than a hidden second search.
static int api_vec_join_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecJoinRequest req;
    req.caller_uid = req_uid;
    json_str(body, "table", req.table_name, OBJECT_NAME_LEN);
    json_str(body, "id_column", req.id_column, RECORD_KEY_LEN);
    uint32_t n = 0;
    char objbuf[256];
    while (n < VEC_SEARCH_MAX_K && json_array_object_at(body, "matches", (int)n, objbuf, (int)sizeof(objbuf))) {
        req.matches[n].external_id  = json_uint64(objbuf, "external_id");
        req.matches[n].id.page_id    = (uint32_t)json_int(objbuf, "page_id");
        req.matches[n].id.slot_index = (uint32_t)json_int(objbuf, "slot_index");
        json_float(objbuf, "distance", &req.matches[n].distance);
        n++;
    }
    req.match_count = n;
    uint64_t rc = sys_sls_vec_join(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "match_count", n); jb_putc(&j,',');
    jb_uint(&j, "result_count", req.result_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "results");
    uint32_t nshown = req.result_count < VEC_JOIN_MAX_RESULTS ? req.result_count : VEC_JOIN_MAX_RESULTS;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.results[i].match.external_id); jb_putc(&j, ',');
        jb_arr_open(&j, "row");
        for (uint32_t c = 0; c < req.results[i].row.count; c++) {
            if (c) jb_putc(&j, ',');
            jb_esc_str(&j, req.results[i].row.values[c]);
        }
        jb_arr_close(&j);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/record (insert | update | delete) ─────────────────────────────
static int api_record_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRecordRequest req;
    char op[16] = "insert";
    json_str(body, "object", req.name, OBJECT_NAME_LEN);
    if (!req.name[0]) json_str(body, "obj", req.name, OBJECT_NAME_LEN);
    json_str(body, "key",   req.key,   RECORD_KEY_LEN);
    json_str(body, "value", req.value, RECORD_VAL_LEN);
    json_str(body, "op",    op, 16);
    uint64_t rc = 1;
    if (!strcmp(op, "update")) rc = sys_sls_update(&req);
    else if (!strcmp(op, "delete")) rc = sys_sls_delete(&req);
    else rc = sys_sls_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tx/begin|commit|rollback ──────────────────────────────────────
static int api_tx_post(const char* op, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    uint32_t tid = kernel_get_current_thread_id();
    uint64_t rc = 1;
    jb_obj_open(&j, 0);
    if (!strcmp(op, "begin")) {
        uint64_t tx = sys_sls_tx_begin(tid);
        jb_str(&j, "ok", tx ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "tx_id", tx);
    } else if (!strcmp(op, "commit")) {
        rc = sys_sls_tx_commit(tid);
        jb_str(&j, "ok", rc==0 ? "true" : "false");
    } else if (!strcmp(op, "rollback")) {
        rc = sys_sls_tx_rollback(tid);
        jb_str(&j, "ok", rc==0 ? "true" : "false");
    } else {
        jb_str(&j, "error", "unknown tx op");
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Phase H: Agent / Workflow REST API ──────────────────────────────────────

// Convert a comma-separated tool name string to an AGENT_TOOL_* bitmask.
// Accepts both "db_select,db_query" and ["db_select","db_query"] formats.
static uint32_t parse_tool_mask(const char* s) {
    uint32_t m = 0;
    if (!s || !s[0]) return 0;
    if (str_find(s, "db_select"))    m |= AGENT_TOOL_DB_SELECT;
    if (str_find(s, "db_insert"))    m |= AGENT_TOOL_DB_INSERT;
    if (str_find(s, "db_query"))     m |= AGENT_TOOL_DB_QUERY;
    if (str_find(s, "stream_read"))  m |= AGENT_TOOL_STREAM_READ;
    if (str_find(s, "stream_write")) m |= AGENT_TOOL_STREAM_WRITE;
    if (str_find(s, "tier_promote")) m |= AGENT_TOOL_TIER_PROMOTE;
    if (str_find(s, "ipc_post"))     m |= AGENT_TOOL_IPC_POST;
    if (str_find(s, "agent_run"))    m |= AGENT_TOOL_AGENT_RUN;
    return m;
}

// ─── GET /api/agents ──────────────────────────────────────────────────────────
static int api_agents_list(char* buf, int max) {
    JSONBuf j = {buf,0,max};
    uint32_t cnt = 0;
    for (int i = 0; i < AGENT_MAX; i++) cnt += agent_table[i].active ? 1 : 0;
    jb_obj_open(&j,0);
    jb_uint(&j,"count",(uint64_t)cnt); jb_putc(&j,',');
    jb_arr_open(&j,"agents");
    int first = 1;
    for (int i = 0; i < AGENT_MAX; i++) {
        if (!agent_table[i].active) continue;
        struct AgentDescriptor* ag = &agent_table[i];
        if (!first) jb_putc(&j,','); first=0;
        jb_obj_open(&j,0);
        jb_str (&j,"name",     ag->name);                        jb_putc(&j,',');
        jb_str (&j,"model",    ag->model);                       jb_putc(&j,',');
        jb_str (&j,"endpoint", ag->inference_endpoint);          jb_putc(&j,',');
        jb_str (&j,"state",    agent_state_name(ag->state));     jb_putc(&j,',');
        jb_uint(&j,"steps",    (uint64_t)ag->step_count);        jb_putc(&j,',');
        jb_uint(&j,"tool_mask",(uint64_t)ag->tool_mask);          jb_putc(&j,',');
        jb_str (&j,"last_answer",ag->last_answer[0]?ag->last_answer:"");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/agent/<name> ────────────────────────────────────────────────────
static int api_agent_status(const char* name, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    for (int i = 0; i < AGENT_MAX; i++) {
        if (!agent_table[i].active) continue;
        struct AgentDescriptor* ag = &agent_table[i];
        int match = 1;
        for (int k = 0; ag->name[k] || name[k]; k++)
            if (ag->name[k] != name[k]) { match=0; break; }
        if (!match) continue;
        jb_obj_open(&j,0);
        jb_str (&j,"name",      ag->name);                       jb_putc(&j,',');
        jb_str (&j,"model",     ag->model);                      jb_putc(&j,',');
        jb_str (&j,"endpoint",  ag->inference_endpoint);         jb_putc(&j,',');
        jb_str (&j,"state",     agent_state_name(ag->state));    jb_putc(&j,',');
        jb_uint(&j,"steps",     (uint64_t)ag->step_count);       jb_putc(&j,',');
        jb_uint(&j,"tool_mask", (uint64_t)ag->tool_mask);        jb_putc(&j,',');
        jb_uint(&j,"object_id", ag->object_id);                  jb_putc(&j,',');
        jb_uint(&j,"run_count", (uint64_t)ag->run_count);        jb_putc(&j,',');
        jb_uint(&j,"sched_ticks",(uint64_t)ag->schedule_ticks);  jb_putc(&j,',');
        jb_str (&j,"memory_table",ag->memory_table_name[0]?ag->memory_table_name:""); jb_putc(&j,',');
        jb_str (&j,"last_answer",ag->last_answer[0]?ag->last_answer:"");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j,0); jb_str(&j,"error","agent not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/create ───────────────────────────────────────────────────
static int api_agent_create(const char* body, char* buf, int max,
                             uint32_t uid) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct AgentCreateRequest req;
    req.name[0]=req.inference_endpoint[0]=req.model[0]=req.system_prompt[0]='\0';
    req.tool_mask=0; req.owner_uid=uid;

    json_str(body,"name",          req.name,               OBJECT_NAME_LEN);
    json_str(body,"endpoint",      req.inference_endpoint, AGENT_ENDPOINT_LEN);
    json_str(body,"model",         req.model,              AGENT_MODEL_LEN);
    json_str(body,"system_prompt", req.system_prompt,      AGENT_PROMPT_LEN);

    static char tools_str[256]; tools_str[0]='\0';
    json_str(body,"tools",tools_str,sizeof(tools_str));
    req.tool_mask = parse_tool_mask(tools_str);

    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_create(&req);
    jb_obj_open(&j,0);
    jb_str (&j,"ok",       rc==0?"true":"false");  jb_putc(&j,',');
    jb_str (&j,"name",     req.name);              jb_putc(&j,',');
    jb_uint(&j,"tool_mask",(uint64_t)req.tool_mask);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/run ──────────────────────────────────────────────────────
// Blocks until the ReAct loop completes (may take several seconds for inference).
static int api_agent_run(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct AgentRunRequest req;
    req.name[0]=req.message[0]='\0';
    json_str(body,"name",    req.name,    OBJECT_NAME_LEN);
    json_str(body,"message", req.message, AGENT_PROMPT_LEN);

    if (!req.name[0]||!req.message[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name and message required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_run(&req);

    // Report step_count from the descriptor
    uint32_t steps = 0;
    for (int i = 0; i < AGENT_MAX; i++) {
        if (agent_table[i].active) {
            int m=1;
            for (int k=0; agent_table[i].name[k]||req.name[k]; k++)
                if (agent_table[i].name[k]!=req.name[k]){m=0;break;}
            if (m) { steps=agent_table[i].step_count; break; }
        }
    }
    jb_obj_open(&j,0);
    jb_str (&j,"ok",    rc==0?"true":"false"); jb_putc(&j,',');
    jb_str (&j,"agent", req.name);             jb_putc(&j,',');
    jb_uint(&j,"steps", (uint64_t)steps);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/drop ─────────────────────────────────────────────────────
static int api_agent_drop(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body,"name",name,OBJECT_NAME_LEN);
    if (!name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_kill(name);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/workflows ───────────────────────────────────────────────────────
static int api_workflows_list(char* buf, int max) {
    JSONBuf j = {buf,0,max};
    uint32_t cnt = 0;
    for (int i=0;i<WORKFLOW_MAX;i++) cnt += workflow_table[i].active?1:0;
    jb_obj_open(&j,0);
    jb_uint(&j,"count",(uint64_t)cnt); jb_putc(&j,',');
    jb_arr_open(&j,"workflows");
    int first=1;
    for (int i=0;i<WORKFLOW_MAX;i++) {
        if (!workflow_table[i].active) continue;
        struct WorkflowDescriptor* wf = &workflow_table[i];
        if (!first) jb_putc(&j,','); first=0;
        jb_obj_open(&j,0);
        jb_str (&j,"name",  wf->name);                          jb_putc(&j,',');
        jb_str (&j,"state", workflow_state_name(wf->state));    jb_putc(&j,',');
        jb_uint(&j,"steps", (uint64_t)wf->step_count);          jb_putc(&j,',');
        jb_uint(&j,"current_step",(uint64_t)wf->current_step);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/workflow/<name> ─────────────────────────────────────────────────
static int api_workflow_status(const char* name, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    for (int i=0;i<WORKFLOW_MAX;i++) {
        if (!workflow_table[i].active) continue;
        struct WorkflowDescriptor* wf = &workflow_table[i];
        int m=1;
        for (int k=0;wf->name[k]||name[k];k++)
            if (wf->name[k]!=name[k]){m=0;break;}
        if (!m) continue;
        jb_obj_open(&j,0);
        jb_str (&j,"name",         wf->name);                       jb_putc(&j,',');
        jb_str (&j,"state",        workflow_state_name(wf->state)); jb_putc(&j,',');
        jb_uint(&j,"step_count",   (uint64_t)wf->step_count);       jb_putc(&j,',');
        jb_uint(&j,"current_step", (uint64_t)wf->current_step);     jb_putc(&j,',');
        jb_arr_open(&j,"steps");
        for (uint8_t s=0;s<wf->step_count;s++) {
            if (s) jb_putc(&j,',');
            jb_obj_open(&j,0);
            jb_str(&j,"agent",  wf->steps[s].agent_name); jb_putc(&j,',');
            jb_str(&j,"input",  wf->steps[s].input_key);  jb_putc(&j,',');
            jb_str(&j,"output", wf->steps[s].output_key);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j,0); jb_str(&j,"error","workflow not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/workflow/create ────────────────────────────────────────────────
// Body: {"name":"wf","shared_table":"tbl","step_count":2,
//        "step0_agent":"a1","step0_in":"q","step0_out":"r1",
//        "step1_agent":"a2","step1_in":"r1","step1_out":"ans"}
static int api_workflow_create(const char* body, char* buf, int max,
                                uint32_t uid) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct WorkflowCreateRequest req;
    req.name[0]=req.shared_state_table[0]='\0';
    req.step_count=0; req.owner_uid=uid;

    json_str(body,"name",         req.name,               OBJECT_NAME_LEN);
    json_str(body,"shared_table", req.shared_state_table, OBJECT_NAME_LEN);
    int sc = json_int(body,"step_count");
    if (sc < 0) sc = 0;
    if (sc > WORKFLOW_MAX_STEPS) sc = WORKFLOW_MAX_STEPS;
    req.step_count = (uint8_t)sc;

    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    // Parse per-step fields: step0_agent, step0_in, step0_out, ...
    for (int s=0; s<sc; s++) {
        char ka[32],ki[32],ko[32];
        // build key names manually (no sprintf available)
        ka[0]='s';ka[1]='t';ka[2]='e';ka[3]='p';ka[4]=(char)('0'+s);
        ka[5]='_';ka[6]='a';ka[7]='g';ka[8]='e';ka[9]='n';ka[10]='t';ka[11]='\0';
        ki[0]='s';ki[1]='t';ki[2]='e';ki[3]='p';ki[4]=(char)('0'+s);
        ki[5]='_';ki[6]='i';ki[7]='n';ki[8]='\0';
        ko[0]='s';ko[1]='t';ko[2]='e';ko[3]='p';ko[4]=(char)('0'+s);
        ko[5]='_';ko[6]='o';ko[7]='u';ko[8]='t';ko[9]='\0';
        req.steps[s].agent_name[0]=req.steps[s].input_key[0]=req.steps[s].output_key[0]='\0';
        json_str(body,ka,req.steps[s].agent_name,OBJECT_NAME_LEN);
        json_str(body,ki,req.steps[s].input_key, RECORD_KEY_LEN);
        json_str(body,ko,req.steps[s].output_key,RECORD_KEY_LEN);
    }
    uint64_t rc = sys_sls_workflow_create(&req);
    jb_obj_open(&j,0);
    jb_str (&j,"ok",  rc==0?"true":"false"); jb_putc(&j,',');
    jb_str (&j,"name",req.name);             jb_putc(&j,',');
    jb_uint(&j,"steps",(uint64_t)sc);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/workflow/run ───────────────────────────────────────────────────
static int api_workflow_run(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct WorkflowRunRequest req;
    req.name[0]=req.input[0]='\0';
    json_str(body,"name",  req.name,  OBJECT_NAME_LEN);
    json_str(body,"input", req.input, AGENT_PROMPT_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_workflow_run(&req);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false"); jb_putc(&j,',');
    jb_str(&j,"name",req.name);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// req[] is a NUL-terminated string of the raw HTTP request.
static void http_route(int conn, char* req) {
    // Parse: METHOD /path[?qs] HTTP/x.x
    char method[8], path[128], qs[256];
    method[0] = path[0] = qs[0] = '\0';
    char* p = req;
    int mi = 0;
    while (*p && *p != ' ' && mi < 7) method[mi++] = *p++;
    method[mi] = '\0';
    while (*p == ' ') p++;
    int pi = 0;
    while (*p && *p != ' ' && *p != '?' && *p != '\r' && pi < 127) path[pi++] = *p++;
    path[pi] = '\0';
    if (*p == '?') {
        p++; int qi = 0;
        while (*p && *p != ' ' && *p != '\r' && qi < 255) qs[qi++] = *p++;
        qs[qi] = '\0';
    }

    const char* body_ptr = http_body(req);
    static char resp_body[16384];
    int blen = 0;

    // OPTIONS: CORS preflight
    if (method[0] == 'O') { http_options(conn); return; }

    int is_post = (method[0] == 'P');

    // ── GET routes ────────────────────────────────────────────────────────────
    if (!is_post) {
        // Gap Remediation Phase E: bearer-token gate for GET routes, mirroring
        // the POST gate further below. Scoped to /api/* data routes only --
        // /auth/token and /auth/verify stay public (issuing or checking a
        // token can't itself require a valid one), /api/health stays public
        // as a conventional unauthenticated liveness probe, and everything
        // NOT under /api/ (the compiled-in Navigator SPA bundle + dynamic
        // WEB_APP assets, matched further down) stays public too -- the
        // SPA's own login screen has to be reachable before a caller has a
        // token to present in the first place.
        if (str_find(path, "/api/") == path && strcmp(path, "/api/health") != 0) {
            uint32_t get_uid = 0; SLSRole get_role = ROLE_GUEST;
            auth_http_extract(req, &get_uid, &get_role);
            if (get_role == ROLE_GUEST) {
                const char* e401 = "{\"error\":\"Unauthorized — include Authorization: Bearer <token>\"}";
                http_respond(conn, 401, "application/json", e401, (int)strlen(e401));
                return;
            }
        }
        if (!strcmp(path, "/auth/verify")) {
            blen = api_auth_verify(req, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/health")) {
            blen = api_health(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/metrics")) {
            blen = api_metrics(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/scan")) {
            blen = api_scan(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/objects")) {
            blen = api_objects(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/objects/") == path) {
            blen = api_object_detail(path + 13, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase C: GET /api/vec/collections, /api/vec/indexes ──
        if (!strcmp(path, "/api/vec/collections")) {
            blen = api_vec_collections_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/indexes")) {
            blen = api_vec_indexes_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase F: GET /api/partitions, /api/partition/quotas ──
        if (!strcmp(path, "/api/partitions")) {
            blen = api_partitions_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/quotas")) {
            blen = api_partition_quotas_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase G: GET /api/simi/<name> ──────────────────────
        if (str_find(path, "/api/simi/") == path) {
            blen = api_simi_info(path + 10, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase B: GET /api/tables[/<name>/schema] ──────────
        if (!strcmp(path, "/api/tables")) {
            blen = api_tables_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/tables/") == path) {
            const char* rest = path + 12;   // after "/api/tables/"
            const char* suffix = str_find(rest, "/schema");
            char tname[OBJECT_NAME_LEN]; tname[0] = '\0';
            if (suffix && suffix[7] == '\0') {   // exact trailing "/schema", nothing after
                uint32_t n = (uint32_t)(suffix - rest);
                if (n >= OBJECT_NAME_LEN) n = OBJECT_NAME_LEN - 1;
                for (uint32_t k = 0; k < n; k++) tname[k] = rest[k];
                tname[n] = '\0';
            }
            if (tname[0]) {
                blen = api_table_schema(tname, resp_body, (int)sizeof(resp_body));
                http_respond(conn, 200, "application/json", resp_body, blen); return;
            }
            const char* e = "{\"error\":\"unknown /api/tables/ route -- try /api/tables/<name>/schema\"}";
            http_respond(conn, 404, "application/json", e, (int)strlen(e)); return;
        }
        if (!strcmp(path, "/api/services")) {
            blen = api_services_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/wal")) {
            blen = api_wal_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tiers")) {
            blen = api_tiers_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/processes")) {
            blen = api_processes_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/programs")) {
            blen = api_programs_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/streams")) {
            blen = stream_list_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/stream/") == path) {
            const char* sname = path + 12; /* skip "/api/stream/" */
            struct StreamEntry* se = stream_find(sname);
            if (!se) {
                const char* e = "{\"error\":\"stream not found\"}";
                http_respond(conn, 404, "application/json", e, (int)strlen(e));
            } else {
                http_respond_stream(conn, se);
            }
            return;
        }
        if (str_find(path, "/api/program/") == path) {
            const char* pname = path + 13; /* skip "/api/program/" */
            struct ServiceBinary* sb = 0;
            for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
                if (service_binaries[b].active &&
                    !str_ncmp2(service_binaries[b].object_name, pname,
                               (int)strlen(pname) + 1)) {
                    sb = &service_binaries[b]; break;
                }
            }
            if (!sb) {
                const char* e = "{\"error\":\"program not found\"}";
                http_respond(conn, 404, "application/json", e, (int)strlen(e));
            } else {
                http_respond_program_binary(conn, sb);
            }
            return;
        }
        if (!strcmp(path, "/api/query")) {
            char q[256] = "show all";
            url_param(qs, "q", q, (int)sizeof(q));
            blen = api_query_json(q, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/constraints[?table=<name>] ───────────────────────────────
        if (!strcmp(path, "/api/constraints")) {
            char tbl[OBJECT_NAME_LEN];
            tbl[0] = '\0';
            url_param(qs, "table", tbl, (int)sizeof(tbl));
            blen = constraints_to_json(tbl, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursors — list open cursors ──────────────────────────────
        if (!strcmp(path, "/api/cursors")) {
            blen = cursors_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursor/fetch?id=N&n=M ────────────────────────────────────
        if (!strcmp(path, "/api/cursor/fetch")) {
            char sid[16], sn[8];
            sid[0] = sn[0] = '\0';
            url_param(qs, "id", sid, (int)sizeof(sid));
            url_param(qs, "n",  sn,  (int)sizeof(sn));
            uint32_t cid = 0; const char* sp = sid;
            while (*sp >= '0' && *sp <= '9') { cid = cid * 10 + (uint32_t)(*sp - '0'); sp++; }
            uint32_t nrows = 10; sp = sn;
            if (*sp) { nrows = 0; while (*sp >= '0' && *sp <= '9') { nrows = nrows * 10 + (uint32_t)(*sp - '0'); sp++; } }
            if (!nrows) nrows = 10;
            blen = cursor_fetch(cid, nrows, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursor/close?id=N ────────────────────────────────────────
        if (!strcmp(path, "/api/cursor/close")) {
            char sid[16]; sid[0] = '\0';
            url_param(qs, "id", sid, (int)sizeof(sid));
            uint32_t cid = 0; const char* sp = sid;
            while (*sp >= '0' && *sp <= '9') { cid = cid * 10 + (uint32_t)(*sp - '0'); sp++; }
            cursor_close(cid);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", "true");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/locks")) {
            blen = lock_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/mqts — list all MQTs ────────────────────────────────────
        if (!strcmp(path, "/api/mqts")) {
            blen = mqts_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/mqt/<name> — read current MQT result table ──────────────
        if (str_find(path, "/api/mqt/") == path) {
            const char* mname = path + 9;
            blen = api_object_detail(mname, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/indexes — list all indexes ───────────────────────────────
        if (!strcmp(path, "/api/indexes")) {
            blen = indexes_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/index/<name>[?q=<value>] ─────────────────────────────────
        if (str_find(path, "/api/index/") == path) {
            const char* iname = path + 11;
            char q_val[64];
            q_val[0] = '\0';
            url_param(qs, "q", q_val, (int)sizeof(q_val));
            if (q_val[0]) {
                // Exact lookup — return matching record key
                char rec_key[64];
                rec_key[0] = '\0';
                int hit = index_lookup(iname, q_val, rec_key);
                JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
                jb_obj_open(&j, 0);
                jb_str(&j, "hit", hit ? "true" : "false");
                if (hit) { jb_putc(&j, ','); jb_str(&j, "key", rec_key); }
                jb_obj_close(&j); j.buf[j.pos] = '\0';
                http_respond(conn, 200, "application/json", resp_body, j.pos); return;
            }
            blen = index_to_json(iname, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/journal/<name>[?since=N] ─────────────────────────────────
        if (str_find(path, "/api/journal/") == path) {
            const char* jname = path + 13;
            uint64_t since = 0;
            char since_s[32] = "0";
            url_param(qs, "since", since_s, (int)sizeof(since_s));
            const char* sp = since_s;
            while (*sp >= '0' && *sp <= '9') { since = since * 10 + (uint64_t)(*sp - '0'); sp++; }
            blen = journal_to_json(jname, since, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/journals")) {
            // List all active journal attachments
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_putc(&j, '[');
            int first = 1;
            for (uint32_t i = 0; i < journal_attachment_count; i++) {
                if (!journal_attachments[i].active) continue;
                if (!first) jb_putc(&j, ',');
                first = 0;
                jb_obj_open(&j, 0);
                jb_str(&j, "journal", journal_attachments[i].journal_name); jb_putc(&j, ',');
                jb_str(&j, "table",   journal_attachments[i].object_name);
                jb_obj_close(&j);
            }
            jb_putc(&j, ']');
            j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── Phase H: Agent & Workflow GET routes ─────────────────────────────
        if (!strcmp(path, "/api/agents")) {
            blen = api_agents_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/agent/") == path) {
            blen = api_agent_status(path + 11, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/workflows")) {
            blen = api_workflows_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/workflow/") == path) {
            blen = api_workflow_status(path + 14, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Compiled-in bundle (full Navigator SPA) — checked first
        const struct BundleAsset* ba = bundle_find(path);
        if (ba) {
            http_respond_raw(conn, ba->mime, ba->data, ba->len); return;
        }
        // WEB_APP asset lookup (dynamic assets stored via syscall)
        struct WebAsset* asset = webapp_find(path);
        if (asset) {
            http_respond(conn, 200, asset->mime,
                         asset->content, (int)asset->content_len); return;
        }
    }

    // ── POST routes ───────────────────────────────────────────────────────────
    if (is_post) {        // POST /auth/token — public, no auth required
        if (!strcmp(path, "/auth/token")) {
            blen = api_auth_token(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }

        // All other POST routes require at least APP_USER or DB_ADMIN
        uint32_t req_uid = 0; SLSRole req_role = ROLE_GUEST;
        auth_http_extract(req, &req_uid, &req_role);
        if (req_role == ROLE_GUEST) {
            const char* e401 = "{\"error\":\"Unauthorized — include Authorization: Bearer <token>\"}";
            http_respond(conn, 401, "application/json", e401,
                         (int)strlen(e401));
            return;
        }        if (!strcmp(path, "/api/valloc")) {
            blen = api_valloc_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase H ─────────────────────────────────────────────
        if (!strcmp(path, "/api/schema")) {
            blen = api_schema_set_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase B ────────────────────────────────────────────
        if (!strcmp(path, "/api/tables")) {
            blen = api_table_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/sql")) {
            blen = api_sql_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Kernel-Side Shell Refactor ──────────────────────────────────────
        if (!strcmp(path, "/api/shell/exec")) {
            blen = api_shell_exec_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase C: Vector Store HTTP reachability ────────────
        if (!strcmp(path, "/api/vec/collections")) {
            blen = api_vec_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/insert")) {
            blen = api_vec_insert_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/embed-insert")) {
            blen = api_vec_embed_insert_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/search")) {
            blen = api_vec_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/indexes")) {
            blen = api_vec_index_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/index/search")) {
            blen = api_vec_index_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/join")) {
            blen = api_vec_join_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/record")) {
            blen = api_record_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/journal/attach|detach ───────────────────────────────────
        if (!strcmp(path, "/api/journal/attach") ||
            !strcmp(path, "/api/journal/detach")) {
            char jname[32];
            char tname[64];
            jname[0] = '\0';
            tname[0] = '\0';
            json_str(body_ptr, "journal", jname, (int)sizeof(jname));
            json_str(body_ptr, "table",   tname, (int)sizeof(tname));
            int rc = (!strcmp(path, "/api/journal/attach"))
                   ? journal_attach(jname, tname)
                   : journal_detach(jname, tname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", rc == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/tx/begin")) {
            blen = api_tx_post("begin", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/index/create|drop|rebuild ───────────────────────────────
        if (!strcmp(path, "/api/index/create")) {
            char iname[OBJECT_NAME_LEN], tname[OBJECT_NAME_LEN], fname[RECORD_KEY_LEN];
            iname[0] = tname[0] = fname[0] = '\0';
            json_str(body_ptr, "name",  iname, (int)sizeof(iname));
            json_str(body_ptr, "table", tname, (int)sizeof(tname));
            json_str(body_ptr, "field", fname, (int)sizeof(fname));
            int rc = index_create(iname, tname, fname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/index/drop")) {
            char iname[OBJECT_NAME_LEN]; iname[0] = '\0';
            json_str(body_ptr, "name", iname, (int)sizeof(iname));
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", index_drop(iname) == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/index/rebuild")) {
            char iname[OBJECT_NAME_LEN]; iname[0] = '\0';
            json_str(body_ptr, "name", iname, (int)sizeof(iname));
            index_rebuild(iname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", "true");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/constraint/add|remove ───────────────────────────────────
        if (!strcmp(path, "/api/constraint/add")) {
            char tbl[OBJECT_NAME_LEN], fld[RECORD_KEY_LEN], typ[16], ref[OBJECT_NAME_LEN];
            tbl[0] = fld[0] = typ[0] = ref[0] = '\0';
            json_str(body_ptr, "table", tbl, (int)sizeof(tbl));
            json_str(body_ptr, "field", fld, (int)sizeof(fld));
            json_str(body_ptr, "type",  typ, (int)sizeof(typ));
            json_str(body_ptr, "ref",   ref, (int)sizeof(ref));
            int rc = 1;
            if (!strcmp(typ, "UNIQUE"))    rc = constraint_add_unique(tbl, fld);
            else if (!strcmp(typ, "NOT_NULL"))  rc = constraint_add_not_null(tbl, fld);
            else if (!strcmp(typ, "REFERENCE")) rc = constraint_add_reference(tbl, fld, ref);
            else if (!strcmp(typ, "RANGE")) {
                char smin[24], smax[24]; smin[0] = smax[0] = '\0';
                json_str(body_ptr, "min", smin, (int)sizeof(smin));
                json_str(body_ptr, "max", smax, (int)sizeof(smax));
                // Parse min/max with simple atoi
                int64_t mn = 0, mx = 0;
                const char* p = smin; if (*p=='-'){mn=-1;p++;} int neg=mn<0; mn=0;
                while(*p>='0'&&*p<='9'){mn=mn*10+(*p-'0');p++;} if(neg) mn=-mn;
                p = smax; neg=0; if (*p=='-'){neg=1;p++;} mx=0;
                while(*p>='0'&&*p<='9'){mx=mx*10+(*p-'0');p++;} if(neg) mx=-mx;
                rc = constraint_add_range(tbl, fld, mn, mx);
            }
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc==0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/constraint/remove")) {
            char tbl[OBJECT_NAME_LEN], fld[RECORD_KEY_LEN], typ[16];
            tbl[0] = fld[0] = typ[0] = '\0';
            json_str(body_ptr, "table", tbl, (int)sizeof(tbl));
            json_str(body_ptr, "field", fld, (int)sizeof(fld));
            json_str(body_ptr, "type",  typ, (int)sizeof(typ));
            int t = -1;
            if      (!strcmp(typ, "UNIQUE"))    t = 0;
            else if (!strcmp(typ, "NOT_NULL"))  t = 1;
            else if (!strcmp(typ, "RANGE"))     t = 2;
            else if (!strcmp(typ, "REFERENCE")) t = 3;
            int rc = constraint_remove(tbl, fld, t);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc==0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/mqt/create|drop|refresh ────────────────────────────────
        if (!strcmp(path, "/api/mqt/create")) {
            char mname[OBJECT_NAME_LEN], btable[OBJECT_NAME_LEN];
            char fn_s[8], afld[RECORD_KEY_LEN], wfld[RECORD_KEY_LEN];
            char weq[RECORD_VAL_LEN], gfld[RECORD_KEY_LEN];
            mname[0]=btable[0]=fn_s[0]=afld[0]=wfld[0]=weq[0]=gfld[0]='\0';
            json_str(body_ptr, "name",     mname,  OBJECT_NAME_LEN);
            json_str(body_ptr, "table",    btable, OBJECT_NAME_LEN);
            json_str(body_ptr, "fn",       fn_s,   (int)sizeof(fn_s));
            json_str(body_ptr, "field",    afld,   RECORD_KEY_LEN);
            json_str(body_ptr, "where",    wfld,   RECORD_KEY_LEN);
            json_str(body_ptr, "eq",       weq,    RECORD_VAL_LEN);
            json_str(body_ptr, "group_by", gfld,   RECORD_KEY_LEN);
            uint8_t fn = (uint8_t)AGG_COUNT;
            if      (!strcmp(fn_s,"SUM")) fn=(uint8_t)AGG_SUM;
            else if (!strcmp(fn_s,"AVG")) fn=(uint8_t)AGG_AVG;
            else if (!strcmp(fn_s,"MIN")) fn=(uint8_t)AGG_MIN;
            else if (!strcmp(fn_s,"MAX")) fn=(uint8_t)AGG_MAX;
            int rc = mqt_create(mname, btable, fn, afld, wfld, weq, gfld);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/mqt/drop")) {
            char mname[OBJECT_NAME_LEN]; mname[0]='\0';
            json_str(body_ptr, "name", mname, OBJECT_NAME_LEN);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",mqt_drop(mname)==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/mqt/refresh")) {
            char mname[OBJECT_NAME_LEN]; mname[0]='\0';
            json_str(body_ptr, "name", mname, OBJECT_NAME_LEN);
            mqt_refresh(mname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok","true");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/aggregate ────────────────────────────────────────────────        // Body: {"table":"...","fn":"COUNT|SUM|AVG|MIN|MAX",
        //        "field":"...","where":"...","eq":"...",
        //        "group_by":"...","having":N,"order_by":"...","order":"ASC|DESC"}
        // fn="" or omitted → AGG_NONE (ORDER BY only)
        if (!strcmp(path, "/api/aggregate")) {
            struct AggQuery q;
            char fn_s[8];
            q.table[0] = q.agg_field[0] = q.where_field[0] = q.where_eq[0] = '\0';
            q.group_field[0] = q.order_field[0] = fn_s[0] = '\0';
            q.having_min_count = 0; q.order_desc = 0;
            json_str(body_ptr, "table",    q.table,       OBJECT_NAME_LEN);
            json_str(body_ptr, "fn",       fn_s,          (int)sizeof(fn_s));
            json_str(body_ptr, "field",    q.agg_field,   RECORD_KEY_LEN);
            json_str(body_ptr, "where",    q.where_field, RECORD_KEY_LEN);
            json_str(body_ptr, "eq",       q.where_eq,    RECORD_VAL_LEN);
            json_str(body_ptr, "group_by", q.group_field, RECORD_KEY_LEN);
            json_str(body_ptr, "order_by", q.order_field, RECORD_KEY_LEN);
            char ord_dir[8]; ord_dir[0] = '\0';
            json_str(body_ptr, "order",    ord_dir,       (int)sizeof(ord_dir));
            if (ord_dir[0] == 'D' || ord_dir[0] == 'd') q.order_desc = 1;
            int hav = json_int(body_ptr, "having");
            q.having_min_count = (int64_t)(hav > 0 ? hav : 0);
            // Map fn string to enum
            if      (!strcmp(fn_s,"COUNT")) q.fn = (uint8_t)AGG_COUNT;
            else if (!strcmp(fn_s,"SUM"))   q.fn = (uint8_t)AGG_SUM;
            else if (!strcmp(fn_s,"AVG"))   q.fn = (uint8_t)AGG_AVG;
            else if (!strcmp(fn_s,"MIN"))   q.fn = (uint8_t)AGG_MIN;
            else if (!strcmp(fn_s,"MAX"))   q.fn = (uint8_t)AGG_MAX;
            else                            q.fn = (uint8_t)AGG_NONE;
            blen = aggregate_exec(&q, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/cursor/open ─────────────────────────────────────────────
        if (!strcmp(path, "/api/cursor/open")) {
            char tbl[OBJECT_NAME_LEN], wfld[RECORD_KEY_LEN];
            char wval[RECORD_VAL_LEN], oidx[OBJECT_NAME_LEN];
            tbl[0] = wfld[0] = wval[0] = oidx[0] = '\0';
            json_str(body_ptr, "table",  tbl,  (int)sizeof(tbl));
            json_str(body_ptr, "where",  wfld, (int)sizeof(wfld));
            json_str(body_ptr, "eq",     wval, (int)sizeof(wval));
            json_str(body_ptr, "order",  oidx, (int)sizeof(oidx));
            uint32_t cid = cursor_open(tbl, wfld, wval, oidx);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", cid > 0 ? "true" : "false"); jb_putc(&j, ',');
            jb_uint(&j, "cursor_id", cid);
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/program/create|upload|spawn ───────────────────────────────────
        if (!strcmp(path, "/api/program/create")) {
            blen = api_program_create(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/program/upload")) {
            blen = api_program_upload(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/program/spawn")) {
            blen = api_program_spawn_handler(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase F: partition create/assign/destroy/pause/
        // resume/quota — every one of these syscalls was already correctly
        // wired at the dispatch layer; this is the first HTTP surface for any
        // of them. ────────────────────────────────────────────────────────────
        if (!strcmp(path, "/api/partitions")) {
            blen = api_partition_create_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/assign")) {
            blen = api_partition_assign_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/destroy")) {
            blen = api_partition_destroy_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/pause")) {
            blen = api_partition_pause_post(body_ptr, resp_body, (int)sizeof(resp_body), 0);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/resume")) {
            blen = api_partition_pause_post(body_ptr, resp_body, (int)sizeof(resp_body), 1);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/quota")) {
            blen = api_partition_quota_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/stream/create|upload ──────────────────────────────────────
        if (!strcmp(path, "/api/stream/create")) {
            blen = api_stream_create(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/stream/upload")) {
            blen = api_stream_upload(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tx/commit")) {
            blen = api_tx_post("commit", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tx/rollback")) {
            blen = api_tx_post("rollback", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Phase H: Agent & Workflow POST routes ─────────────────────────────
        if (!strcmp(path, "/api/agent/create")) {
            blen = api_agent_create(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/run")) {
            blen = api_agent_run(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/drop")) {
            blen = api_agent_drop(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/schedule")) {
            char aname[OBJECT_NAME_LEN]; aname[0]='\0';
            char amsg[128];             amsg[0]='\0';
            json_str(body_ptr, "name",    aname, OBJECT_NAME_LEN);
            json_str(body_ptr, "message", amsg,  sizeof(amsg));
            int aticks = json_int(body_ptr, "ticks");
            struct AgentScheduleRequest sr;
            for(int i=0;aname[i]&&i<(int)(sizeof(sr.name)-1);i++) sr.name[i]=aname[i]; sr.name[sizeof(sr.name)-1]='\0';
            for(int i=0;amsg[i] &&i<(int)(sizeof(sr.message)-1);i++) sr.message[i]=amsg[i]; sr.message[sizeof(sr.message)-1]='\0';
            sr.ticks = (uint32_t)(aticks > 0 ? aticks : 0);
            uint64_t rc = sys_sls_agent_schedule(&sr);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/agent/unschedule")) {
            char aname[OBJECT_NAME_LEN]; aname[0]='\0';
            json_str(body_ptr, "name", aname, OBJECT_NAME_LEN);
            struct AgentScheduleRequest sr;
            for(int i=0;aname[i]&&i<(int)(sizeof(sr.name)-1);i++) sr.name[i]=aname[i]; sr.name[sizeof(sr.name)-1]='\0';
            sr.message[0]='\0'; sr.ticks=0;
            uint64_t rc = sys_sls_agent_schedule(&sr);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/workflow/create")) {
            blen = api_workflow_create(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/workflow/run")) {
            blen = api_workflow_run(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
    }

    // 404 fallback
    const char* e404 = "{\"error\":\"not found\"}";
    http_respond(conn, 404, "application/json", e404, (int)strlen(e404));
}

// ─── HTTP server main loop ────────────────────────────────────────────────────
void http_server_run(void) {
    tcp_init();
    int listen_fd = tcp_listen(NET_HTTP_PORT);
    if (listen_fd < 0) {
        kernel_serial_print("[HTTP] Failed to bind port 3000.\n");
        return;
    }
    kernel_serial_printf("[HTTP] Listening on port %u. "
                         "GET http://10.0.2.15:3000/api/scan\n",
                         NET_HTTP_PORT);

    static char req_buf[65536];  // 64 KiB — fits a 16 KiB binary chunk (32 KiB hex) + headers

    for (;;) {
        int conn = tcp_accept(listen_fd);
        if (conn < 0) continue;

        /* Accumulate the full HTTP request, respecting Content-Length. */
        int rlen = 0;
        for (;;) {
            int got = tcp_recv(conn, req_buf + rlen,
                               (uint16_t)(sizeof(req_buf) - 1 - rlen));
            if (got <= 0) break;
            rlen += got;
            req_buf[rlen] = '\0';
            /* Must have complete headers before we can parse Content-Length. */
            const char *body_start = str_find(req_buf, "\r\n\r\n");
            if (!body_start) { if (rlen < (int)(sizeof(req_buf) - 1)) continue; else break; }
            /* No Content-Length → GET / HEAD / no body; done. */
            const char *cl = str_find(req_buf, "Content-Length:");
            if (!cl) cl = str_find(req_buf, "content-length:");
            if (!cl) break;
            int clen = 0;
            const char *cp = cl + 15;
            while (*cp == ' ') cp++;
            while (*cp >= '0' && *cp <= '9') { clen = clen * 10 + (*cp++ - '0'); }
            int hdr_end = (int)(body_start - req_buf) + 4;
            if (rlen >= hdr_end + clen) break;  /* full body received */
            if (rlen >= (int)(sizeof(req_buf) - 1)) break;  /* buffer full */
        }
        if (rlen > 0) {
            http_route(conn, req_buf);
        }
        tcp_close(conn);
    }
}
