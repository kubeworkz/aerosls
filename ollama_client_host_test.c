/*
 * ollama_client_host_test.c — Vector Store Roadmap Phase 3 verification: a
 * standalone host-buildable test for net/ollama_client.c, linked against
 * the REAL, unmodified net/ollama_client.c -- not a reimplementation.
 *
 * Live Ollama is unreachable from this development sandbox (confirmed via
 * three direct curl attempts to localhost/host.docker.internal/the Docker
 * gateway, all blocked by the network allowlist -- see the roadmap's own
 * Phase 3 findings addendum). This is the documented fallback: rather than
 * link the real net/tcp.c (too heavy/stateful for a host test -- it depends
 * on ARP resolution and the e1000 driver), tcp_connect/tcp_send/tcp_recv/
 * tcp_close are stubbed directly at their own real API boundary, and a
 * hand-constructed, byte-exact fake Ollama HTTP response is fed through
 * tcp_recv() in caller-chosen chunk sizes (proving ollama_embed()'s receive
 * loop correctly reassembles a response delivered across multiple reads,
 * not just a single one). This mirrors the project's established "stub the
 * heavy dependency at its own real API boundary" pattern (e.g.
 * catalog_check_access() in rowstore_host_test.c).
 *
 * This also exercises tcp_send()'s captured buffer to verify
 * ollama_embed() builds a correct HTTP request (method/path, Content-
 * Length matching the actual body, and the JSON body itself) -- not just
 * that it parses responses correctly.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers -I net \
 *       -o /tmp/ollama_client_host_test ollama_client_host_test.c net/ollama_client.c
 *   /tmp/ollama_client_host_test
 */
#include "net/ollama_client.h"
#include "net/tcp.h"
#include "net/net.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

// ─── kernel_serial_printf stub (silent, matches other host tests' own
// convention of swallowing kernel log output) ──────────────────────────────
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

// ─── tcp_* stubs -- fake a real Ollama server's wire behavior ─────────────
static char     g_sent_buf[8192];
static int      g_sent_len = 0;
static int      g_connect_calls = 0;
static int      g_close_calls = 0;
static int      g_force_connect_fail = 0;

static const char* g_fake_response = 0;   // set per-scenario
static int          g_fake_response_len = 0;
static int          g_fake_response_pos = 0;
static int          g_fake_chunk_size = 0; // 0 = deliver whole thing in one tcp_recv

int tcp_connect(IPv4Addr dst_ip, uint16_t dst_port) {
    (void)dst_ip; (void)dst_port;
    g_connect_calls++;
    if (g_force_connect_fail) return -1;
    return 7; // arbitrary fixed conn_id
}

int tcp_send(int conn_id, const void* buf, uint32_t len) {
    (void)conn_id;
    if (len < sizeof(g_sent_buf)) {
        memcpy(g_sent_buf, buf, len);
        g_sent_buf[len] = '\0';
        g_sent_len = (int)len;
    }
    return (int)len;
}

int tcp_recv(int conn_id, void* buf, uint16_t max_len) {
    (void)conn_id;
    if (!g_fake_response || g_fake_response_pos >= g_fake_response_len) return 0; // connection closed
    int chunk = g_fake_chunk_size > 0 ? g_fake_chunk_size : g_fake_response_len;
    int remaining = g_fake_response_len - g_fake_response_pos;
    if (chunk > remaining) chunk = remaining;
    if (chunk > max_len) chunk = max_len;
    memcpy(buf, g_fake_response + g_fake_response_pos, (size_t)chunk);
    g_fake_response_pos += chunk;
    return chunk;
}

void tcp_close(int conn_id) { (void)conn_id; g_close_calls++; }

static void reset_stubs(void) {
    g_sent_len = 0; g_sent_buf[0] = '\0';
    g_connect_calls = 0; g_close_calls = 0; g_force_connect_fail = 0;
    g_fake_response = 0; g_fake_response_len = 0; g_fake_response_pos = 0; g_fake_chunk_size = 0;
}
static void set_fake_response(const char* resp, int chunk_size) {
    g_fake_response = resp;
    g_fake_response_len = (int)strlen(resp);
    g_fake_response_pos = 0;
    g_fake_chunk_size = chunk_size;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static int approx(float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 0.0001f; }

int main(void) {
    struct OllamaEmbedRequest req;
    struct OllamaEmbedResponse resp;

    /* ── Scenario 1: happy path, response delivered in ONE tcp_recv call.
     * Verifies request building (method/path/model/prompt/Content-Length)
     * AND response parsing together. ────────────────────────────────────── */
    reset_stubs();
    memset(&req, 0, sizeof(req));
    strcpy(req.endpoint_ip, "127.0.0.1");
    req.port = 11434;
    strcpy(req.model, "nomic-embed-text");
    strcpy(req.prompt, "hello world");

    const char* body1 = "{\"embedding\":[0.1,-0.2,0.30000001,4.5e-2,-1.25e2]}";
    char resp1[512];
    snprintf(resp1, sizeof(resp1),
             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(body1), body1);
    set_fake_response(resp1, 0);

    int rc = ollama_embed(&req, &resp);
    CHECK(rc == 0, "s1: ollama_embed returns 0 on a well-formed success response");
    CHECK(resp.http_status == 200, "s1: http_status parsed as 200");
    CHECK(resp.dimension == 5, "s1: dimension parsed as 5 (matches the fake embedding array length)");
    CHECK(approx(resp.embedding[0], 0.1f), "s1: embedding[0] == 0.1");
    CHECK(approx(resp.embedding[1], -0.2f), "s1: embedding[1] == -0.2 (negative number parses)");
    CHECK(approx(resp.embedding[2], 0.30000001f), "s1: embedding[2] == 0.30000001 (many fraction digits)");
    CHECK(approx(resp.embedding[3], 0.045f), "s1: embedding[3] == 4.5e-2 (positive exponent shorthand)");
    CHECK(approx(resp.embedding[4], -125.0f), "s1: embedding[4] == -1.25e2 (negative mantissa + explicit exponent)");
    CHECK(g_connect_calls == 1, "s1: tcp_connect called exactly once");
    CHECK(g_close_calls == 1, "s1: tcp_close called exactly once");
    CHECK(strstr(g_sent_buf, "POST /api/embeddings HTTP/1.1") == g_sent_buf, "s1: request starts with the correct method/path");
    CHECK(strstr(g_sent_buf, "\"model\":\"nomic-embed-text\"") != 0, "s1: request body contains the correct model");
    CHECK(strstr(g_sent_buf, "\"prompt\":\"hello world\"") != 0, "s1: request body contains the correct prompt");
    CHECK(strstr(g_sent_buf, "Connection: close") != 0, "s1: request declares Connection: close");
    {
        // Extract the actual body (after \r\n\r\n) and confirm Content-Length matches its real length --
        // proves build_http_request() isn't just emitting a plausible-looking but wrong header.
        const char* b = strstr(g_sent_buf, "\r\n\r\n");
        CHECK(b != 0, "s1: sent request has a header/body separator");
        int actual_body_len = b ? (int)strlen(b + 4) : -1;
        char clhdr[32]; snprintf(clhdr, sizeof(clhdr), "Content-Length: %d\r\n", actual_body_len);
        CHECK(strstr(g_sent_buf, clhdr) != 0, "s1: Content-Length header matches the real (sent) body length");
    }

    /* ── Scenario 2: same happy-path response, but delivered across MANY
     * small tcp_recv chunks -- proves the accumulate-until-close loop in
     * ollama_embed() correctly reassembles a fragmented response. ────────── */
    reset_stubs();
    set_fake_response(resp1, 7);   // deliver 7 bytes at a time
    rc = ollama_embed(&req, &resp);
    CHECK(rc == 0, "s2: ollama_embed succeeds when the response arrives in 7-byte chunks");
    CHECK(resp.dimension == 5, "s2: dimension still parses correctly across fragmented reads");
    CHECK(approx(resp.embedding[4], -125.0f), "s2: last element still parses correctly across fragmented reads");

    /* ── Scenario 3: tcp_connect() fails outright (e.g. connection refused --
     * no Ollama running on that port). ──────────────────────────────────────── */
    reset_stubs();
    g_force_connect_fail = 1;
    rc = ollama_embed(&req, &resp);
    CHECK(rc == -1, "s3: ollama_embed returns -1 when tcp_connect fails");
    CHECK(resp.dimension == 0, "s3: dimension is left at 0 on a connect failure");

    /* ── Scenario 4: non-2xx HTTP status (e.g. model not found -- Ollama
     * returns 404 for an unpulled model). ───────────────────────────────────── */
    reset_stubs();
    const char* err_body = "{\"error\":\"model 'nomic-embed-text' not found\"}";
    char resp4[256];
    snprintf(resp4, sizeof(resp4),
             "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(err_body), err_body);
    set_fake_response(resp4, 0);
    rc = ollama_embed(&req, &resp);
    CHECK(rc == -1, "s4: ollama_embed returns -1 on a 404 response");
    CHECK(resp.http_status == 404, "s4: http_status is still recorded as 404 even though the call failed");
    CHECK(resp.dimension == 0, "s4: dimension is left at 0 on a non-2xx response (no embedding parsed)");

    /* ── Scenario 5: 200 OK but with a malformed/missing "embedding" key --
     * e.g. hitting the newer /api/embed endpoint's {"embeddings":[[...]]}
     * shape by mistake, or a genuinely broken server response. ─────────────── */
    reset_stubs();
    const char* wrong_shape_body = "{\"embeddings\":[[0.1,0.2]]}";
    char resp5[256];
    snprintf(resp5, sizeof(resp5),
             "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(wrong_shape_body), wrong_shape_body);
    set_fake_response(resp5, 0);
    rc = ollama_embed(&req, &resp);
    CHECK(rc == -1, "s5: ollama_embed returns -1 when no \"embedding\" (singular) key is present");
    CHECK(resp.dimension == 0, "s5: dimension stays 0 when the expected key is absent");

    /* ── Scenario 6: connection closes with zero bytes ever sent back. ──────── */
    reset_stubs();
    // g_fake_response left null -> tcp_recv returns 0 immediately
    rc = ollama_embed(&req, &resp);
    CHECK(rc == -1, "s6: ollama_embed returns -1 on a completely empty response");

    /* ── Scenario 7: a large embedding (dimension near OLLAMA_MAX_EMBED_DIM)
     * to sanity check the response buffer sizing and array-parsing loop
     * don't break down at scale, not just on 5-element toy arrays. ─────────── */
    reset_stubs();
    {
        static char big_body[40000];
        int pos = 0;
        pos += sprintf(big_body + pos, "{\"embedding\":[");
        int dim = 768; // a real nomic-embed-text dimension, well within OLLAMA_MAX_EMBED_DIM
        for (int i = 0; i < dim; i++) {
            pos += sprintf(big_body + pos, "%s%d.%03d", i ? "," : "", (i % 2 == 0 ? 1 : -1) * (i % 10), i % 1000);
        }
        pos += sprintf(big_body + pos, "]}");
        static char big_resp[40200];
        snprintf(big_resp, sizeof(big_resp),
                 "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s", pos, big_body);
        set_fake_response(big_resp, 4096);
        rc = ollama_embed(&req, &resp);
        CHECK(rc == 0, "s7: ollama_embed succeeds on a realistic 768-dim embedding");
        CHECK(resp.dimension == 768, "s7: all 768 elements are parsed, not truncated");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
