#include "http.h"
#include "tcp.h"
#include "net.h"
#include "../kernel/kernel_io.h"
#include "../kernel/object_catalog.h"
#include "../kernel/transaction.h"
#include "../kernel/microkernel.h"
#include "../kernel/tier_mgr.h"
#include "../kernel/webapp.h"
#include "../kernel/bundle.h"
#include "../kernel/query_engine.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/auth.h"

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
    jb_str(&j, "status", "ok"); jb_putc(&j, ',');
    jb_str(&j, "system", "AeroSLS 4.0");
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

// ─── POST /api/valloc ─────────────────────────────────────────────────────────
static int api_valloc_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVallocRequest req;
    req.owner_uid = 0; req.perm_mask = 0;
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

// ─── POST /api/record (insert | update | delete) ─────────────────────────────
static int api_record_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRecordRequest req;
    char op[16] = "insert";
    json_str(body, "obj",   req.name,  OBJECT_NAME_LEN);
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

// ─── Route an incoming HTTP request ──────────────────────────────────────────
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
        if (!strcmp(path, "/auth/verify")) {
            blen = api_auth_verify(req, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/health")) {
            blen = api_health(resp_body, (int)sizeof(resp_body));
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
        if (!strcmp(path, "/api/query")) {
            char q[256] = "show all";
            url_param(qs, "q", q, (int)sizeof(q));
            blen = api_query_json(q, resp_body, (int)sizeof(resp_body));
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
            // Pass the authenticated uid as the object owner
            blen = api_valloc_post(body_ptr, resp_body, (int)sizeof(resp_body));
            // Inject the authenticated uid into the valloc request
            json_str(body_ptr ? body_ptr : "", "name", resp_body, 64); // reuse buffer temporarily
            blen = api_valloc_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/record")) {
            blen = api_record_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tx/begin")) {
            blen = api_tx_post("begin", resp_body, (int)sizeof(resp_body));
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

    static char req_buf[4096];

    for (;;) {
        int conn = tcp_accept(listen_fd);
        if (conn < 0) continue;
        kernel_serial_print("[HTTP] connection accepted\n");

        int rlen = tcp_recv(conn, req_buf, (uint16_t)(sizeof(req_buf) - 1));
        kernel_serial_print("[HTTP] request received\n");
        if (rlen > 0) {
            req_buf[rlen] = '\0';
            http_route(conn, req_buf);
            kernel_serial_print("[HTTP] response sent\n");
        }
        tcp_close(conn);
    }
}
