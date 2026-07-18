#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <stdint.h>
#include "agent.h"

// ─── AeroSLS Agent Tool Engine ────────────────────────────────────────────────
//
// Bridges LLM tool-call responses to kernel operations.
//
// Build flow (before each infer_call):
//   agent_tools_build_schema(mask, buf, max)
//     → fills buf with the JSON tools array for InferRequest.tools_json
//
// Execute flow (after an infer_call with has_tool_call == 1):
//   agent_tools_execute(name, args_json, result_buf, result_max, mask, uid)
//     → verifies the tool is in mask, calls the tool, returns JSON result
//
// Tools and their AGENT_TOOL_* flags:
//   db_select    — AGENT_TOOL_DB_SELECT   — read fields from a DB_TABLE
//   db_insert    — AGENT_TOOL_DB_INSERT   — write a field to a DB_TABLE
//   db_query     — AGENT_TOOL_DB_QUERY    — keyword scan of object catalog
//   tier_promote — AGENT_TOOL_TIER_PROMOTE — move object to faster tier
//   stream_read  — AGENT_TOOL_STREAM_READ  — read text from a STREAM
//   stream_write — AGENT_TOOL_STREAM_WRITE — append text to a STREAM
// ─────────────────────────────────────────────────────────────────────────────

// ─── Result buffer size ───────────────────────────────────────────────────────
#define TOOL_RESULT_MAX  1024

// ─── Public API ───────────────────────────────────────────────────────────────

// Build the JSON tools array (OpenAI function-calling format) for all tools
// whose flag bit is set in tool_mask.  Returns chars written.
int agent_tools_build_schema(uint32_t tool_mask, char* buf, int max);

// Execute a tool call by name.
// Fills result_buf with a JSON object (always, even on error).
// Returns 0 on success, -1 on error/permission denied.
int agent_tools_execute(const char* tool_name,
                        const char* args_json,
                        char*       result_buf,
                        int         result_max,
                        uint32_t    tool_mask,
                        uint32_t    caller_uid);

#endif /* AGENT_TOOLS_H */
