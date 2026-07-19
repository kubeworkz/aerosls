#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <stdint.h>

/*
 * ollama_client.h — Vector Store Roadmap Phase 3: an outbound HTTP client
 * that calls a local Ollama instance's embeddings endpoint. See
 * docs/AeroSLS-VectorStore-Roadmap-v0.1.md §5 for the full design writeup
 * and the "Findings addendum" for what's built here.
 *
 * ─── Why this lives in net/, not kernel/ (a correction to the roadmap's
 * own initially-assumed path) ────────────────────────────────────────────
 * The roadmap's own Phase 3 scope bullet said "kernel/ollama_client.c" --
 * written before this phase's own implementation looked at where its
 * direct template actually lives. `net/inference.c` (the proven
 * connect/send/recv/parse pattern this file adapts) is in `net/`, not
 * `kernel/`, for the obvious reason: it's fundamentally a network client,
 * grouped with `net/tcp.c`/`net/http.c`/`net/dhcp.c`, not a kernel
 * subsystem. This file follows that same real convention rather than the
 * roadmap doc's own untested guess -- the doc is updated to match, not the
 * other way around.
 *
 * ─── Adaptation, not new-HTTP-client work — confirmed by direct
 * investigation before writing any code (see the roadmap's own Phase 3
 * audit) ──────────────────────────────────────────────────────────────────
 * `net/inference.c`'s `infer_call()` already does the complete outbound
 * HTTP pattern this file needs, against a different endpoint:
 * `parse_ipv4_str()` -> `tcp_connect()` -> build a JSON request body with
 * headers -> `tcp_send()` -> loop `tcp_recv()` until the connection closes
 * -> `parse_http_status()` -> `find_http_body()` -> extract fields from
 * the JSON response. `ollama_embed()` below follows that exact control
 * flow, adapted to Ollama's `/api/embeddings` endpoint and response shape.
 * Per this codebase's own established convention (confirmed by `net/
 * inference.c` and `net/http.c` each keeping their own small string/buffer
 * helpers rather than sharing a common library), this file keeps its own
 * copies too (`oc_*` prefix) rather than extracting a shared net-client
 * helper module -- a real, deliberate choice to match existing practice,
 * not an oversight.
 *
 * ─── The one genuinely new piece of machinery: a JSON number-ARRAY parser
 * ──────────────────────────────────────────────────────────────────────────
 * Confirmed by the same audit: nothing anywhere in this tree parses a JSON
 * numeric array (`"embedding":[0.1,-0.2,...]`) or even a single bare JSON
 * number with a fractional/exponent part from external input -- `json_str
 * ()`/`json_int()` (net/http.c) and `infer_json_str()` (net/inference.c)
 * only extract string fields; `rowstore.c`'s/`row_constraint.c`'s own
 * float parsers (`rs_parse_f64()`/`rc_parse_f64()`) handle a narrower
 * grammar (`[-]digits[.digits]`, no exponent) sized for THIS codebase's
 * own internally-generated text, not for parsing arbitrary external JSON.
 * `oc_parse_json_number()` (ollama_client.c) implements the real JSON
 * number grammar (optional sign, integer part, optional fraction, optional
 * signed exponent) since this file is consuming genuinely external JSON
 * from a real server, not round-tripping this codebase's own text.
 *
 * ─── A real, honest, unverified assumption -- named, not hidden ─────────
 * This phase could not be verified against a live Ollama instance (see the
 * roadmap's own findings addendum for why: this development environment's
 * network is sandboxed/allowlisted, confirmed by direct connection
 * attempts that were blocked before any implementation work started, not
 * assumed). The request/response shape targeted here -- POST
 * `/api/embeddings` with `{"model":"...","prompt":"..."}`, response
 * `{"embedding":[...]}` -- is Ollama's documented legacy embeddings
 * endpoint (still functional in current Ollama releases, though a newer
 * `/api/embed` endpoint with batch input and a differently-shaped
 * `{"embeddings":[[...]]}` response also exists). This implementation
 * targets the legacy shape because it's simpler (one flat array, not an
 * array of arrays) and was what this roadmap already committed to before
 * this phase started. This is a real, load-bearing assumption to confirm
 * against a real running Ollama instance before depending on this code in
 * anger -- named explicitly here so it's never mistaken for a verified
 * fact.
 */

// ─── Field size limits ────────────────────────────────────────────────────
#define OLLAMA_ENDPOINT_LEN   128   // dotted-decimal "ip" component only (no port) -- mirrors InferRequest's own convention
#define OLLAMA_MODEL_LEN       64
#define OLLAMA_PROMPT_LEN    2048   // text to embed -- matches InferRequest's own INFER_MESSAGE_LEN
#define OLLAMA_MAX_EMBED_DIM 2048   // matches vecstore.h's own VECSTORE_MAX_DIMENSION value, independently
                                     // declared -- see header comment above on why these two subsystems
                                     // don't share a header (Phases 1 and 3 have no dependency on each
                                     // other per the roadmap's own suggested execution order)

// ─── Request ──────────────────────────────────────────────────────────────
struct OllamaEmbedRequest {
    char     endpoint_ip[OLLAMA_ENDPOINT_LEN];   // e.g. "10.0.2.2" or "127.0.0.1"
    uint16_t port;                               // e.g. 11434 (Ollama's default)
    char     model[OLLAMA_MODEL_LEN];            // e.g. "nomic-embed-text"
    char     prompt[OLLAMA_PROMPT_LEN];          // the text to embed
};

// ─── Response ─────────────────────────────────────────────────────────────
struct OllamaEmbedResponse {
    float    embedding[OLLAMA_MAX_EMBED_DIM];
    uint32_t dimension;     // number of floats actually written to embedding[]
    int      http_status;   // e.g. 200
};

// ─── Public API ───────────────────────────────────────────────────────────
// Returns 0 on success (embedding[0..dimension) populated), -1 on network
// or parse error -- same (req, resp) -> int convention as infer_call(),
// deliberately, not a coincidence.
int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp);

#endif /* OLLAMA_CLIENT_H */
