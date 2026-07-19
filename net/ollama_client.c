#include "ollama_client.h"
#include "tcp.h"
#include "net.h"
#include "arp.h"
#include "../kernel/kernel_io.h"
#include "../kernel/net_event.h"

/*
 * ollama_client.c — Vector Store Roadmap Phase 3 Ollama embedding client.
 * See ollama_client.h for the full design writeup.
 */

// ─── Static working buffers (kept off the stack, matching inference.c's
// own convention) ───────────────────────────────────────────────────────────
#define OLLAMA_BODY_BUF_SZ  2560    // JSON request body: model + prompt(2048) + escaping/wrapper headroom
#define OLLAMA_REQ_BUF_SZ   2816    // HTTP headers + body
// Response must hold up to OLLAMA_MAX_EMBED_DIM (2048) floats as JSON
// text. Worst case per float (Go's float64->JSON encoder, which is what
// Ollama's server uses, can emit up to ~17 significant digits for
// round-trip precision, e.g. "-0.12345678901234568") is ~22 bytes
// including the separating comma; 2048 * 22 = 45056, plus HTTP headers and
// JSON wrapping -- 65536 (64 KiB) covers this with real margin, a stated
// number not a guess.
#define OLLAMA_RESP_BUF_SZ 65536

static char oc_body_buf[OLLAMA_BODY_BUF_SZ];
static char oc_req_buf [OLLAMA_REQ_BUF_SZ];
static char oc_resp_buf[OLLAMA_RESP_BUF_SZ];

// ─── String / buffer helpers (no libc, oc_* here -- see ollama_client.h's
// header comment on why this duplicates rather than shares inference.c's
// own near-identical helpers). ─────────────────────────────────────────────
static const char* oc_strfind(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char* h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}
static void oc_app(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}
static void oc_app_char(char* buf, int* pos, int max, char c) {
    if (*pos < max - 1) buf[(*pos)++] = c;
}
static void oc_app_uint(char* buf, int* pos, int max, uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) { oc_app_char(buf, pos, max, '0'); return; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) oc_app_char(buf, pos, max, tmp[i]);
}
static void oc_app_json_str(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 2) {
        unsigned char c = (unsigned char)*s++;
        if      (c == '"')  { oc_app(buf, pos, max, "\\\""); }
        else if (c == '\\') { oc_app(buf, pos, max, "\\\\"); }
        else if (c == '\n') { oc_app(buf, pos, max, "\\n");  }
        else if (c == '\r') { oc_app(buf, pos, max, "\\r");  }
        else if (c == '\t') { oc_app(buf, pos, max, "\\t");  }
        else if (c < 0x20)  { /* skip other control chars */ }
        else                { oc_app_char(buf, pos, max, (char)c); }
    }
}

static IPv4Addr parse_ipv4_str(const char* s) {
    uint32_t oct[4] = {0,0,0,0};
    int oi = 0;
    while (*s && oi < 4) {
        while (*s >= '0' && *s <= '9')
            oct[oi] = oct[oi] * 10 + (uint32_t)(*s++ - '0');
        oi++;
        if (*s == '.') s++;
    }
    return htonl((oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3]);
}

// ─── JSON number parsing -- the genuinely new machinery this phase adds
// (see ollama_client.h's header comment: nothing in this tree parses a
// full JSON number grammar, or any JSON array, before this file). ─────────

// Parses one JSON number (the real grammar: [-]int[.frac][(e|E)[+-]digits])
// starting at s. Writes the value to *out. Returns the number of
// characters consumed, or 0 on parse failure (not a valid JSON number at
// this position).
static int oc_parse_json_number(const char* s, float* out) {
    const char* start = s;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!(*s >= '0' && *s <= '9')) return 0;   // JSON requires at least one integer digit

    double v = 0.0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (double)(*s - '0'); s++; }

    if (*s == '.') {
        const char* dot = s;
        s++;
        if (!(*s >= '0' && *s <= '9')) { s = dot; }   // "1." with no digits after -- not valid JSON, rewind
        else {
            double frac = 0.1;
            while (*s >= '0' && *s <= '9') { v += (double)(*s - '0') * frac; frac *= 0.1; s++; }
        }
    }

    if (*s == 'e' || *s == 'E') {
        const char* exp_start = s;
        s++;
        int exp_neg = 0;
        if (*s == '+') s++;
        else if (*s == '-') { exp_neg = 1; s++; }
        if (!(*s >= '0' && *s <= '9')) {
            s = exp_start;   // "1e" with no exponent digits -- not valid, rewind, exponent is not part of the number
        } else {
            int exp = 0;
            while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
            double mult = 1.0, base = 10.0;
            while (exp > 0) { if (exp & 1) mult *= base; base *= base; exp >>= 1; }
            v = exp_neg ? v / mult : v * mult;
        }
    }

    *out = (float)(neg ? -v : v);
    return (int)(s - start);
}

// Finds "key":[...] and parses up to max_count comma-separated JSON
// numbers into out[]. Returns the count actually parsed (0 if the key
// isn't found, isn't followed by '[', or the array is empty). Stops (does
// NOT fail the whole call) at the first element that doesn't parse as a
// number, or at max_count -- returns whatever was successfully parsed so
// far, matching this codebase's own "return what's valid, don't discard
// partial real data" posture elsewhere (e.g. row_journal.c's ring buffer).
static uint32_t oc_json_float_array(const char* json, const char* key, float* out, uint32_t max_count) {
    char srch[64]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i] && si < 56; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';

    const char* p = oc_strfind(json, srch);
    if (!p) return 0;
    p += si;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;

    uint32_t count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        float v;
        int consumed = oc_parse_json_number(p, &v);
        if (consumed == 0) break;   // not a valid number here -- stop, keep what's already parsed
        out[count++] = v;
        p += consumed;
    }
    return count;
}

// ─── Build JSON request body: {"model":"...","prompt":"..."} ──────────────
static int build_body(const struct OllamaEmbedRequest* req, char* buf, int max) {
    int pos = 0;
    oc_app(buf, &pos, max, "{\"model\":\"");
    oc_app_json_str(buf, &pos, max, req->model);
    oc_app(buf, &pos, max, "\",\"prompt\":\"");
    oc_app_json_str(buf, &pos, max, req->prompt);
    oc_app(buf, &pos, max, "\"}");
    buf[pos] = '\0';
    return pos;
}

// ─── Build full HTTP POST request ─────────────────────────────────────────
static int build_http_request(const struct OllamaEmbedRequest* req, char* buf, int max) {
    int body_len = build_body(req, oc_body_buf, (int)sizeof(oc_body_buf));

    int pos = 0;
    oc_app(buf, &pos, max, "POST /api/embeddings HTTP/1.1\r\nHost: ");
    oc_app(buf, &pos, max, req->endpoint_ip);
    oc_app_char(buf, &pos, max, ':');
    oc_app_uint(buf, &pos, max, req->port);
    oc_app(buf, &pos, max, "\r\nContent-Type: application/json\r\nContent-Length: ");
    oc_app_uint(buf, &pos, max, (uint32_t)body_len);
    oc_app(buf, &pos, max, "\r\nConnection: close\r\n\r\n");

    for (int i = 0; i < body_len && pos < max - 1; i++)
        buf[pos++] = oc_body_buf[i];
    buf[pos] = '\0';
    return pos;
}

// ─── HTTP response helpers (identical shape to inference.c's own, see
// header comment on why this duplicates rather than shares). ──────────────
static int parse_http_status(const char* resp) {
    const char* p = resp;
    while (*p && *p != ' ') p++;
    if (!*p) return 0;
    p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    return code;
}
static const char* find_http_body(const char* resp) {
    for (const char* p = resp; p[0]; p++)
        if (p[0]=='\r' && p[1]=='\n' && p[2]=='\r' && p[3]=='\n')
            return p + 4;
    return 0;
}

// ─── ollama_embed ───────────────────────────────────────────────────────────
int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    if (!req || !resp) return -1;

    resp->dimension   = 0;
    resp->http_status = 0;

    IPv4Addr dst_ip = parse_ipv4_str(req->endpoint_ip);
    if (!dst_ip) {
        kernel_serial_printf("[OLLAMA] Invalid endpoint IP: '%s'\n", req->endpoint_ip);
        return -1;
    }

    kernel_serial_printf("[OLLAMA] Connecting %s:%u model=%s\n",
                         req->endpoint_ip, req->port, req->model);

    int conn = tcp_connect(dst_ip, req->port);
    if (conn < 0) {
        kernel_serial_printf("[OLLAMA] tcp_connect failed\n");
        return -1;
    }

    int req_len = build_http_request(req, oc_req_buf, (int)sizeof(oc_req_buf));
    if (tcp_send(conn, oc_req_buf, (uint32_t)req_len) < 0) {
        kernel_serial_printf("[OLLAMA] tcp_send failed\n");
        tcp_close(conn);
        return -1;
    }
    kernel_serial_printf("[OLLAMA] Sent %d bytes, awaiting response...\n", req_len);

    // Accumulate response until the server closes the connection --
    // "Connection: close" above means the server MUST close after replying.
    int total = 0, n;
    while (total < (int)(sizeof(oc_resp_buf) - 1)) {
        n = tcp_recv(conn, oc_resp_buf + total, (uint16_t)(sizeof(oc_resp_buf) - 1 - (uint16_t)total));
        if (n <= 0) break;
        total += n;
    }
    oc_resp_buf[total] = '\0';
    tcp_close(conn);

    if (total == 0) {
        kernel_serial_printf("[OLLAMA] Empty response\n");
        return -1;
    }

    resp->http_status = parse_http_status(oc_resp_buf);
    kernel_serial_printf("[OLLAMA] HTTP %d, %d bytes\n", resp->http_status, total);

    if (resp->http_status < 200 || resp->http_status >= 300) {
        kernel_serial_printf("[OLLAMA] Non-2xx status\n");
        return -1;
    }

    const char* body = find_http_body(oc_resp_buf);
    if (!body) {
        kernel_serial_printf("[OLLAMA] Could not find response body\n");
        return -1;
    }

    uint32_t n_parsed = oc_json_float_array(body, "embedding", resp->embedding, OLLAMA_MAX_EMBED_DIM);
    if (n_parsed == 0) {
        kernel_serial_printf("[OLLAMA] Could not parse an \"embedding\" array from the response\n");
        return -1;
    }

    resp->dimension = n_parsed;
    kernel_serial_printf("[OLLAMA] Parsed embedding, dimension=%u\n", resp->dimension);
    return 0;
}
