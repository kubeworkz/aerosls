#ifndef AGENT_H
#define AGENT_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS AI Agent Engine ──────────────────────────────────────────────────
//
// An AGENT is a first-class OS object that wraps an LLM inference endpoint,
// a persistent memory DB_TABLE, a conversation history STREAM, and a bitmask
// of permitted kernel tools (syscalls the agent may invoke on behalf of a
// caller).
//
// Lifecycle:
//   SYS_SLS_AGENT_CREATE  (200) — allocate descriptor + catalog entry
//   SYS_SLS_AGENT_RUN     (201) — submit a message; returns when idle
//   SYS_SLS_AGENT_STATUS  (202) — print descriptor to serial
//   SYS_SLS_AGENT_KILL    (203) — stop a running agent; free catalog slot
//   SYS_SLS_AGENT_LIST    (204) — list all agents to serial
//
// A WORKFLOW chains agents sequentially, piping the last_answer record of
// step N into the input message of step N+1 via a shared DB_TABLE.
//
//   SYS_SLS_WORKFLOW_CREATE (205) — define steps
//   SYS_SLS_WORKFLOW_RUN    (206) — execute all steps; blocks until done
//   SYS_SLS_WORKFLOW_STATUS (207) — print step progress to serial
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define AGENT_MAX              8
#define AGENT_ENDPOINT_LEN     128   // "host:port" of an OpenAI-compat server
#define AGENT_MODEL_LEN        64
#define AGENT_PROMPT_LEN       512
#define AGENT_MAX_TOOLS        16
#define AGENT_TOOL_NAME_LEN    32
#define AGENT_MAX_STEPS        8    // max ReAct iterations per agent_run call
#define AGENT_LAST_ANSWER_LEN  256  // persisted across ReAct loops; piped into workflows

#define WORKFLOW_MAX           4
#define WORKFLOW_MAX_STEPS     8

// ─── Tool permission flags (AgentDescriptor.tool_mask) ────────────────────────
#define AGENT_TOOL_DB_SELECT     (1u << 0)  // SYS_SLS_SELECT — read records
#define AGENT_TOOL_DB_INSERT     (1u << 1)  // SYS_SLS_INSERT — write records
#define AGENT_TOOL_DB_QUERY      (1u << 2)  // SYS_SLS_QUERY  — NL query engine
#define AGENT_TOOL_STREAM_READ   (1u << 3)  // read bytes from a STREAM object
#define AGENT_TOOL_STREAM_WRITE  (1u << 4)  // append bytes to a STREAM object
#define AGENT_TOOL_IPC_POST      (1u << 5)  // SYS_SLS_IPC_POST — send IPC msg
#define AGENT_TOOL_AGENT_RUN     (1u << 6)  // SYS_SLS_AGENT_RUN — call another agent
#define AGENT_TOOL_TIER_PROMOTE  (1u << 7)  // SYS_SLS_TIER_PROMOTE — pull hot data

// ─── Agent states ─────────────────────────────────────────────────────────────
typedef enum {
    AGENT_STATE_IDLE    = 0,
    AGENT_STATE_RUNNING = 1,
    AGENT_STATE_BLOCKED = 2,  // waiting on network I/O
    AGENT_STATE_ERROR   = 3,
} AgentState;

static inline const char* agent_state_name(AgentState s) {
    switch (s) {
        case AGENT_STATE_IDLE:    return "IDLE";
        case AGENT_STATE_RUNNING: return "RUNNING";
        case AGENT_STATE_BLOCKED: return "BLOCKED";
        case AGENT_STATE_ERROR:   return "ERROR";
        default:                  return "UNKNOWN";
    }
}

// ─── Agent descriptor ─────────────────────────────────────────────────────────
struct AgentDescriptor {
    uint64_t   object_id;                       // FNV-1a of name
    char       name[OBJECT_NAME_LEN];
    char       inference_endpoint[AGENT_ENDPOINT_LEN]; // "ip:port"
    char       model[AGENT_MODEL_LEN];          // e.g. "llama3.2"
    uint64_t   memory_table_id;                 // object_id of the auto-created DB_TABLE
    char       memory_table_name[OBJECT_NAME_LEN]; // "_<name>_mem" — used for DB record ops
    uint64_t   history_stream_id;               // object_id of a STREAM
    uint32_t   tool_mask;                       // AGENT_TOOL_* bitmask
    char       system_prompt[AGENT_PROMPT_LEN];
    AgentState state;
    uint32_t   step_count;                      // total ReAct steps executed
    uint32_t   run_count;                       // (B) successful end-to-end runs
    uint32_t   owner_uid;                       // (F) uid of creator
    uint32_t   schedule_ticks;                  // (E) 0=disabled; run every N service-poll ticks
    char       schedule_msg[128];               // (E) message for scheduled runs
    char       last_answer[AGENT_LAST_ANSWER_LEN]; // most recent text response
    uint8_t    active;
};

// ─── Workflow step — one agent invocation with named I/O keys ─────────────────
struct WorkflowStep {
    char agent_name[OBJECT_NAME_LEN];
    char input_key[RECORD_KEY_LEN];   // read this key from shared_state_table_id
    char output_key[RECORD_KEY_LEN];  // write result under this key
};

// ─── Workflow descriptor ──────────────────────────────────────────────────────
typedef enum {
    WORKFLOW_STATE_IDLE    = 0,
    WORKFLOW_STATE_RUNNING = 1,
    WORKFLOW_STATE_DONE    = 2,
    WORKFLOW_STATE_ERROR   = 3,
} WorkflowState;

static inline const char* workflow_state_name(WorkflowState s) {
    switch (s) {
        case WORKFLOW_STATE_IDLE:    return "IDLE";
        case WORKFLOW_STATE_RUNNING: return "RUNNING";
        case WORKFLOW_STATE_DONE:    return "DONE";
        case WORKFLOW_STATE_ERROR:   return "ERROR";
        default:                     return "UNKNOWN";
    }
}

struct WorkflowDescriptor {
    uint64_t              object_id;
    char                  name[OBJECT_NAME_LEN];
    uint64_t              shared_state_table_id;         // FNV-1a of table name
    char                  shared_state_table_name[OBJECT_NAME_LEN]; // used for DB ops
    struct WorkflowStep   steps[WORKFLOW_MAX_STEPS];
    uint8_t               step_count;
    uint8_t               current_step;
    WorkflowState         state;
    uint8_t               active;
};

// ─── Syscall argument structs ─────────────────────────────────────────────────
struct AgentCreateRequest {
    char     name[OBJECT_NAME_LEN];
    char     inference_endpoint[AGENT_ENDPOINT_LEN];
    char     model[AGENT_MODEL_LEN];
    char     system_prompt[AGENT_PROMPT_LEN];
    uint32_t tool_mask;
    uint32_t owner_uid;
};

struct AgentRunRequest {
    char name[OBJECT_NAME_LEN];
    char message[AGENT_PROMPT_LEN]; // user-turn input
};

struct WorkflowCreateRequest {
    char                name[OBJECT_NAME_LEN];
    char                shared_state_table[OBJECT_NAME_LEN]; // existing DB_TABLE name
    struct WorkflowStep steps[WORKFLOW_MAX_STEPS];
    uint8_t             step_count;
    uint32_t            owner_uid;
};

struct WorkflowRunRequest {
    char name[OBJECT_NAME_LEN];
    char input[AGENT_PROMPT_LEN]; // written to shared state as "input" key
};

// (E) Schedule request
struct AgentScheduleRequest {
    char     name[OBJECT_NAME_LEN];
    uint32_t ticks;       // 0 = disable scheduling
    char     message[128];
};

// ─── Syscall numbers (Phase H) ────────────────────────────────────────────────
#define SYS_SLS_AGENT_CREATE     200
#define SYS_SLS_AGENT_RUN        201
#define SYS_SLS_AGENT_STATUS     202
#define SYS_SLS_AGENT_KILL       203
#define SYS_SLS_AGENT_LIST       204
#define SYS_SLS_WORKFLOW_CREATE  205
#define SYS_SLS_WORKFLOW_RUN     206
#define SYS_SLS_WORKFLOW_STATUS  207
#define SYS_SLS_AGENT_SCHEDULE   208   // (E) set/clear scheduled run

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct AgentDescriptor    agent_table[AGENT_MAX];
extern struct WorkflowDescriptor workflow_table[WORKFLOW_MAX];

void     agent_init(void);

uint64_t sys_sls_agent_create(struct AgentCreateRequest* req);
uint64_t sys_sls_agent_run(struct AgentRunRequest* req);
void     sys_sls_agent_status(const char* name);
uint64_t sys_sls_agent_kill(const char* name);
void     sys_sls_agent_list(void);
uint64_t sys_sls_agent_schedule(struct AgentScheduleRequest* req); // (E)
void     agent_scheduler_tick(void);                               // (E)

uint64_t sys_sls_workflow_create(struct WorkflowCreateRequest* req);
uint64_t sys_sls_workflow_run(struct WorkflowRunRequest* req);
void     sys_sls_workflow_status(const char* name);
void     sys_sls_workflow_list(void);

#endif /* AGENT_H */
