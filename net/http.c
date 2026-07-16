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
    jb_str(&j,  "status",       "ok");                     jb_putc(&j, ',');
    jb_str(&j,  "system",       "AeroSLS 4.0");             jb_putc(&j, ',');
    jb_uint(&j, "uptime_ticks", kernel_tick_counter);       jb_putc(&j, ',');
    jb_uint(&j, "object_count", object_catalog_count);
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
                bin_fmt    = service_binaries[b].is_elf ? "ELF64" : "flat";
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
                    fmt_str   = service_binaries[b].is_elf ? "ELF64" : "flat";
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
static int api_program_spawn_handler(const char* body, char* buf, int max) {
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
    uint64_t pid = program_load(pname, 0);
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
    /* X-Binary-Format header so the downloader knows ELF vs flat */
    const char* xbf = "X-Binary-Format: ";
    while (*xbf) hdr[hp++] = *xbf++;
    const char* fmt = sb->is_elf ? "ELF64" : "flat";
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
            blen = api_program_spawn_handler(body_ptr, resp_body, (int)sizeof(resp_body));
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
