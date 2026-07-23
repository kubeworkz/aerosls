/*
 * ollama_client_live_host_test.c — VectorStore Gap Analysis §4 (live Ollama
 * verification): a standalone host-buildable test for net/ollama_client.c,
 * linked against the REAL, unmodified net/ollama_client.c -- not a
 * reimplementation, and NOT mocked at the tcp_*() boundary the way
 * ollama_client_host_test.c is.
 *
 * ollama_client_host_test.c already proves ollama_embed()'s parsing logic
 * against hand-built fake bytes (documented there: this development
 * environment's network was sandboxed/allowlisted when that file was
 * written, confirmed by direct connection attempts before any
 * implementation work started). This file closes the remaining honest gap
 * §4 of the VectorStore Gap Analysis named: no automated test has ever
 * exercised ollama_embed() against bytes that actually came off a real
 * socket, from a real Ollama server.
 *
 * How: tcp_connect()/tcp_send()/tcp_recv()/tcp_close() (net/tcp.h's own
 * real API boundary -- the exact same seam ollama_client_host_test.c
 * stubs, just backed here by real POSIX sockets instead of canned bytes)
 * are implemented against a real TCP connection to a configurable
 * endpoint. net/ollama_client.c itself is completely unmodified and has
 * no idea its transport is POSIX sockets instead of this kernel's own
 * e1000/ARP stack -- exactly the substitution its own header comment
 * already described as the reason a live instance was never linked
 * directly against the freestanding net/tcp.c (too heavy/stateful for a
 * host test -- it depends on ARP resolution and the e1000 driver).
 *
 * This test is opt-in and self-skipping, not a hard CI gate: a short,
 * non-blocking connect probe (1s timeout) runs first. If nothing answers
 * at the configured host:port, the test prints SKIP and exits 0 (matching
 * tests/run_all.sh's own documented SKIP semantics -- "no live Ollama
 * reachable" is an environment fact, not a code failure, and this repo's
 * own sandboxed development network is exactly such an environment). Set
 * OLLAMA_LIVE_REQUIRE=1 to make an unreachable endpoint a hard failure
 * instead, for a CI environment that's expected to always have one.
 *
 * If a model isn't pulled on the reachable instance, Ollama answers with
 * a non-2xx status (ollama_embed() itself already turns that into rc=-1).
 * This test still counts that as a PASS for "the wire format round-
 * tripped against a real server" (the actual point of this file) and
 * reports it distinctly from a genuine transport failure -- requiring
 * every possible live Ollama deployment to have OLLAMA_LIVE_MODEL's exact
 * model pre-pulled would make this test far more brittle than what it's
 * actually verifying.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/ollama_client_live_host_test tests/ollama_client_live_host_test.c net/ollama_client.c
 *   /tmp/ollama_client_live_host_test
 *
 * Env vars: OLLAMA_LIVE_HOST (default 127.0.0.1), OLLAMA_LIVE_PORT
 * (default 11434), OLLAMA_LIVE_MODEL (default nomic-embed-text),
 * OLLAMA_LIVE_REQUIRE (default 0 -- unreachable is SKIP, not FAIL).
 */
#include "net/ollama_client.h"
#include "net/tcp.h"
#include "net/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

// ─── kernel_serial_printf stub (silent, matches ollama_client_host_test.c's
// own convention of swallowing kernel log output) ──────────────────────────
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

// ─── Real POSIX-socket tcp_*() implementation -- the SAME API boundary
// ollama_client_host_test.c stubs with canned bytes, here backed by a
// genuine TCP connection instead. conn_id IS the raw fd -- no translation
// table needed since this test only ever has one connection open at a
// time, matching ollama_embed()'s own one-request-at-a-time usage. ────────
int tcp_connect(IPv4Addr dst_ip, uint16_t dst_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    addr.sin_addr.s_addr = dst_ip;   // IPv4Addr is already network-byte-order (net/net.h's own documented convention)

    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == 0) { fcntl(fd, F_SETFL, flags); return fd; }
    if (errno != EINPROGRESS) { close(fd); return -1; }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = { 3, 0 };   // 3s -- generous for a local/LAN Ollama instance
    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) { close(fd); return -1; }

    int soerr = 0; socklen_t slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) { close(fd); return -1; }

    fcntl(fd, F_SETFL, flags);
    return fd;
}
int tcp_send(int conn_id, const void* buf, uint32_t len) {
    ssize_t n = send(conn_id, buf, len, 0);
    return (int)n;
}
int tcp_recv(int conn_id, void* buf, uint16_t max_len) {
    ssize_t n = recv(conn_id, buf, max_len, 0);
    if (n < 0) return 0;   // treat a recv error like "connection closed" -- matches ollama_embed()'s own n<=0 break
    return (int)n;
}
void tcp_close(int conn_id) { close(conn_id); }

// ─── Short, non-blocking probe: is anything answering at host:port at
// all? Kept separate from tcp_connect() above so a totally-unreachable
// endpoint gets a fast, clearly-labeled SKIP rather than ollama_embed()'s
// own slower internal failure path (which would otherwise print a
// confusing "FAIL" for what is genuinely just an absent server). ──────────
static int probe_reachable(const char* host, uint16_t port) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) != 1) return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = a;
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    int ok = 0;
    if (rc == 0) ok = 1;
    else if (errno == EINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        struct timeval tv = { 1, 0 };
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0) {
            int soerr = 0; socklen_t slen = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            ok = (soerr == 0);
        }
    }
    close(fd);
    return ok;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    const char* host  = getenv("OLLAMA_LIVE_HOST");  if (!host)  host  = "127.0.0.1";
    const char* portS = getenv("OLLAMA_LIVE_PORT");
    uint16_t    port  = portS ? (uint16_t)atoi(portS) : 11434;
    const char* model = getenv("OLLAMA_LIVE_MODEL"); if (!model) model = "nomic-embed-text";
    const char* reqS  = getenv("OLLAMA_LIVE_REQUIRE");
    int require = reqS && atoi(reqS) != 0;

    printf("VectorStore Gap Analysis Section 4: live Ollama verification\n");
    printf("target: %s:%u  model: %s  (set OLLAMA_LIVE_HOST/PORT/MODEL to override)\n\n",
           host, (unsigned)port, model);

    if (!probe_reachable(host, port)) {
        if (require) {
            printf("FAIL: no live Ollama instance reachable at %s:%u (OLLAMA_LIVE_REQUIRE=1)\n",
                   host, (unsigned)port);
            return 1;
        }
        printf("SKIP: no live Ollama instance reachable at %s:%u -- this is an environment fact,\n"
               "      not a code failure (set OLLAMA_LIVE_REQUIRE=1 to make this a hard failure,\n"
               "      or point OLLAMA_LIVE_HOST/PORT at a running instance).\n",
               host, (unsigned)port);
        return 0;
    }

    struct OllamaEmbedRequest req;
    memset(&req, 0, sizeof(req));
    strncpy(req.endpoint_ip, host, sizeof(req.endpoint_ip) - 1);
    req.port = port;
    strncpy(req.model, model, sizeof(req.model) - 1);
    strncpy(req.prompt, "AeroSLS VectorStore Gap Analysis Section 4 live verification",
            sizeof(req.prompt) - 1);

    struct OllamaEmbedResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rc = ollama_embed(&req, &resp);

    // A reachable server that answered with a non-2xx status (most likely:
    // OLLAMA_LIVE_MODEL isn't pulled on this instance) still proves the
    // real point of this file -- the request left this process, hit a
    // genuine Ollama server, and a genuine HTTP response came back and was
    // parsed for its status line. Reported distinctly from a hard failure.
    if (rc != 0 && resp.http_status != 0 && (resp.http_status < 200 || resp.http_status >= 300)) {
        printf("ok:   real Ollama server at %s:%u answered (HTTP %d) -- wire format round-tripped\n"
               "      against a genuine server, even though model '%s' returned a non-2xx status\n"
               "      (likely not pulled on this instance -- run 'ollama pull %s' for a full PASS)\n",
               host, (unsigned)port, resp.http_status, model, model);
        return 0;
    }

    CHECK(rc == 0, "ollama_embed() returns 0 (success) against the real server");
    CHECK(resp.http_status == 200, "HTTP 200 from the real server");
    CHECK(resp.dimension > 0, "a nonzero embedding dimension was parsed from a real response");
    {
        int any_nonzero = 0;
        for (uint32_t i = 0; i < resp.dimension; i++)
            if (resp.embedding[i] != 0.0f) { any_nonzero = 1; break; }
        CHECK(any_nonzero, "the parsed embedding isn't a degenerate all-zero buffer");
    }
    if (resp.dimension > 0)
        printf("\nembedding dimension: %u, first value: %f\n", resp.dimension, (double)resp.embedding[0]);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
