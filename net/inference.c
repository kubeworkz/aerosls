#include "inference.h"
#include "tcp.h"
#include "net.h"
#include "arp.h"
#include "../kernel/kernel_io.h"
#include "../kernel/net_event.h"

// ─── Static working buffers (kept off the stack) ──────────────────────────────
#define INFER_BODY_BUF_SZ   5632   // max JSON request body
#define INFER_REQ_BUF_SZ    6144   // HTTP headers + body
#define INFER_RESP_BUF_SZ   8192   // full HTTP response (headers + JSON body)

static char infer_body_buf[INFER_BODY_BUF_SZ];
static char infer_req_buf [INFER_REQ_BUF_SZ];
static char infer_resp_buf[INFER_RESP_BUF_SZ];

// ─── String helpers (no libc in freestanding kernel) ─────────────────────────
static const char* inf_strfind(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char* h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

// ─── Buffer append helpers ────────────────────────────────────────────────────
static void buf_app(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}

static void buf_app_char(char* buf, int* pos, int max, char c) {
    if (*pos < max - 1) buf[(*pos)++] = c;
}

static void buf_app_uint(char* buf, int* pos, int max, uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) { buf_app_char(buf, pos, max, '0'); return; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) buf_app_char(buf, pos, max, tmp[i]);
}

// JSON-escape a string value and append to buf (handles \, ", \n, \r, \t)
static void buf_app_json_str(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 2) {
        unsigned char c = (unsigned char)*s++;
        if      (c == '"')  { buf_app(buf, pos, max, "\\\""); }
        else if (c == '\\') { buf_app(buf, pos, max, "\\\\"); }
        else if (c == '\n') { buf_app(buf, pos, max, "\\n");  }
        else if (c == '\r') { buf_app(buf, pos, max, "\\r");  }
        else if (c == '\t') { buf_app(buf, pos, max, "\\t");  }
        else if (c < 0x20)  { /* skip other control chars    */ }
        else                { buf_app_char(buf, pos, max, (char)c); }
    }
}

// ─── IPv4 dotted-decimal → network-byte-order uint32 ─────────────────────────
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

// ─── JSON string extractor ────────────────────────────────────────────────────
// Finds "key":"value" (with optional whitespace after ':'), returns char count.
// Handles \" and \\ escapes in the value. Returns 0 if key not found or value
// is not a string (e.g. null).
static int infer_json_str(const char* json, const char* key,
                          char* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i] && si < 120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';

    const char* p = inf_strfind(json, srch);
    if (!p) return 0;
    p += si;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;   // value is not a string (null, number, etc.)
    p++;
    int n = 0;
    while (*p && n < max - 1) {
        if (*p == '\\' && *(p + 1)) {
            char e = *(p + 1);
            if      (e == '"')  { out[n++] = '"';  p += 2; }
            else if (e == '\\') { out[n++] = '\\'; p += 2; }
            else if (e == 'n')  { out[n++] = '\n'; p += 2; }
            else if (e == 'r')  { out[n++] = '\r'; p += 2; }
            else if (e == 't')  { out[n++] = '\t'; p += 2; }
            else                { out[n++] = e;    p += 2; }
        } else if (*p == '"') {
            break;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n;
}

// ─── Build JSON request body ──────────────────────────────────────────────────
// Format: {"model":"…","messages":[…],"tools":[…],"stream":false}
// tools key is omitted when tools_json is empty.
static int build_body(const struct InferRequest* req, char* buf, int max) {
    int pos = 0;
    buf_app (buf, &pos, max, "{\"model\":\"");
    buf_app_json_str(buf, &pos, max, req->model);
    buf_app (buf, &pos, max, "\",\"messages\":[");

    if (req->system_prompt[0]) {
        buf_app(buf, &pos, max, "{\"role\":\"system\",\"content\":\"");
        buf_app_json_str(buf, &pos, max, req->system_prompt);
        buf_app(buf, &pos, max, "\"},");
    }

    buf_app(buf, &pos, max, "{\"role\":\"user\",\"content\":\"");
    buf_app_json_str(buf, &pos, max, req->user_message);
    buf_app(buf, &pos, max, "\"}]");

    if (req->tools_json[0]) {
        buf_app(buf, &pos, max, ",\"tools\":");
        buf_app(buf, &pos, max, req->tools_json);
        buf_app(buf, &pos, max, ",\"tool_choice\":\"auto\"");
    }

    buf_app(buf, &pos, max, ",\"stream\":false}");
    buf[pos] = '\0';
    return pos;
}

// ─── Build full HTTP POST request ─────────────────────────────────────────────
// Builds the body first (into infer_body_buf) to get the Content-Length, then
// prepends the headers.
static int build_http_request(const struct InferRequest* req,
                               char* buf, int max) {
    int body_len = build_body(req, infer_body_buf, (int)sizeof(infer_body_buf));

    int pos = 0;
    buf_app (buf, &pos, max, "POST /v1/chat/completions HTTP/1.1\r\nHost: ");
    buf_app (buf, &pos, max, req->endpoint_ip);
    buf_app_char(buf, &pos, max, ':');
    buf_app_uint(buf, &pos, max, req->port);
    buf_app (buf, &pos, max, "\r\nContent-Type: application/json\r\nContent-Length: ");
    buf_app_uint(buf, &pos, max, (uint32_t)body_len);
    buf_app (buf, &pos, max, "\r\nConnection: close\r\n\r\n");

    for (int i = 0; i < body_len && pos < max - 1; i++)
        buf[pos++] = infer_body_buf[i];
    buf[pos] = '\0';
    return pos;
}

// ─── HTTP response helpers ────────────────────────────────────────────────────
// Parse "HTTP/1.x NNN …" → return NNN.
static int parse_http_status(const char* resp) {
    const char* p = resp;
    while (*p && *p != ' ') p++;   // skip "HTTP/1.x"
    if (!*p) return 0;
    p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    return code;
}

// Find body start (first byte after the blank line \r\n\r\n).
static const char* find_http_body(const char* resp) {
    for (const char* p = resp; p[0]; p++)
        if (p[0]=='\r' && p[1]=='\n' && p[2]=='\r' && p[3]=='\n')
            return p + 4;
    return 0;
}

// ─── infer_call ───────────────────────────────────────────────────────────────
int infer_call(const struct InferRequest* req, struct InferResponse* resp) {
    if (!req || !resp) return -1;

    resp->content[0]        = '\0';
    resp->tool_name[0]      = '\0';
    resp->tool_args_json[0] = '\0';
    resp->has_tool_call     = 0;
    resp->http_status       = 0;

    // 1. Parse endpoint IP
    IPv4Addr dst_ip = parse_ipv4_str(req->endpoint_ip);
    if (!dst_ip) {
        kernel_serial_printf("[INFER] Invalid endpoint IP: '%s'\n",
                             req->endpoint_ip);
        return -1;
    }

    kernel_serial_printf("[INFER] Connecting %s:%u model=%s\n",
                         req->endpoint_ip, req->port, req->model);

    // 2. TCP active open
    int conn = tcp_connect(dst_ip, req->port);
    if (conn < 0) {
        kernel_serial_printf("[INFER] tcp_connect failed\n");
        return -1;
    }

    // 3. Build and send HTTP POST
    int req_len = build_http_request(req, infer_req_buf,
                                      (int)sizeof(infer_req_buf));
    if (tcp_send(conn, infer_req_buf, (uint32_t)req_len) < 0) {
        kernel_serial_printf("[INFER] tcp_send failed\n");
        tcp_close(conn);
        return -1;
    }
    kernel_serial_printf("[INFER] Sent %d bytes, awaiting response...\n",
                         req_len);

    // 4. Accumulate response until the server closes the connection.
    //    We send "Connection: close" so the server MUST close after the reply.
    int total = 0, n;
    while (total < (int)(sizeof(infer_resp_buf) - 1)) {
        n = tcp_recv(conn,
                     infer_resp_buf + total,
                     (uint16_t)(sizeof(infer_resp_buf) - 1 - (uint16_t)total));
        if (n <= 0) break;
        total += n;
    }
    infer_resp_buf[total] = '\0';
    tcp_close(conn);

    if (total == 0) {
        kernel_serial_printf("[INFER] Empty response\n");
        return -1;
    }

    // 5. Parse HTTP status line
    resp->http_status = parse_http_status(infer_resp_buf);
    kernel_serial_printf("[INFER] HTTP %d, %d bytes\n",
                         resp->http_status, total);

    if (resp->http_status < 200 || resp->http_status >= 300) {
        kernel_serial_printf("[INFER] Non-2xx status\n");
        return -1;
    }

    // 6. Locate JSON body
    const char* body = find_http_body(infer_resp_buf);
    if (!body) {
        kernel_serial_printf("[INFER] Could not find response body\n");
        return -1;
    }

    // 7a. Extract text content (present when the model replied in text)
    if (infer_json_str(body, "content",
                       resp->content, INFER_CONTENT_LEN) > 0) {
        kernel_serial_printf("[INFER] Content (%.80s)\n", resp->content);
        return 0;
    }

    // 7b. No text content — look for a tool call
    //     OpenAI format: …"tool_calls":[{"function":{"name":"…","arguments":"…"}}]…
    const char* fn = inf_strfind(body, "\"function\":");
    if (fn) {
        infer_json_str(fn, "name",
                       resp->tool_name,     INFER_TOOL_NAME_LEN);
        infer_json_str(fn, "arguments",
                       resp->tool_args_json, INFER_TOOL_ARGS_LEN);
        if (resp->tool_name[0]) {
            resp->has_tool_call = 1;
            kernel_serial_printf("[INFER] Tool call: %s args=%.60s\n",
                                 resp->tool_name, resp->tool_args_json);
            return 0;
        }
    }

    kernel_serial_printf("[INFER] Could not parse content or tool call\n");
    return -1;
}
