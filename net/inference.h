#ifndef INFERENCE_H
#define INFERENCE_H

#include <stdint.h>

// ─── AeroSLS Inference Client ──────────────────────────────────────────────────
//
// Sends a single HTTP/1.1 POST to an OpenAI-compatible /v1/chat/completions
// endpoint (e.g. Ollama, vLLM) over the kernel TCP stack, then parses the
// JSON response into an InferResponse.
//
// Usage:
//   struct InferRequest  req  = {0};
//   struct InferResponse resp = {0};
//   strncpy(req.endpoint_ip, "10.0.2.2", sizeof(req.endpoint_ip));
//   req.port = 11434;
//   strncpy(req.model, "llama3.2", sizeof(req.model));
//   strncpy(req.user_message, "Hello!", sizeof(req.user_message));
//   int rc = infer_call(&req, &resp);
//   // rc == 0 → resp.content or resp.has_tool_call is set
// ─────────────────────────────────────────────────────────────────────────────

// ─── Field size limits ────────────────────────────────────────────────────────
#define INFER_ENDPOINT_LEN    128   // dotted-decimal "ip" component only (no port)
#define INFER_MODEL_LEN        64
#define INFER_PROMPT_LEN      512   // system prompt
#define INFER_MESSAGE_LEN    2048   // user turn text
#define INFER_TOOLS_LEN      2048   // raw JSON array of tool schemas; empty = no tools
#define INFER_CONTENT_LEN    4096   // assistant text response
#define INFER_TOOL_NAME_LEN    64
#define INFER_TOOL_ARGS_LEN  1024   // tool arguments JSON string

// ─── Request ──────────────────────────────────────────────────────────────────
struct InferRequest {
    char     endpoint_ip[INFER_ENDPOINT_LEN]; // e.g. "10.0.2.2"
    uint16_t port;                            // e.g. 11434
    char     model[INFER_MODEL_LEN];
    char     system_prompt[INFER_PROMPT_LEN];
    char     user_message[INFER_MESSAGE_LEN];
    char     tools_json[INFER_TOOLS_LEN];     // JSON array; empty string = omit
};

// ─── Response ─────────────────────────────────────────────────────────────────
struct InferResponse {
    char    content[INFER_CONTENT_LEN];           // set when has_tool_call == 0
    char    tool_name[INFER_TOOL_NAME_LEN];       // set when has_tool_call == 1
    char    tool_args_json[INFER_TOOL_ARGS_LEN];  // set when has_tool_call == 1
    uint8_t has_tool_call;
    int     http_status;                          // e.g. 200
};

// ─── Public API ───────────────────────────────────────────────────────────────
// Returns 0 on success (response parsed), -1 on network or parse error.
int infer_call(const struct InferRequest* req, struct InferResponse* resp);

#endif /* INFERENCE_H */
