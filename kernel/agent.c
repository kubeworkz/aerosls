#include "agent.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "agent_tools.h"
#include "../net/inference.h"

// ─── Static pools ─────────────────────────────────────────────────────────────
struct AgentDescriptor    agent_table[AGENT_MAX];
struct WorkflowDescriptor workflow_table[WORKFLOW_MAX];

// ─── Internal string helpers (mirrors object_catalog.c convention) ─────────────
static size_t ag_strlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static int ag_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void ag_strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void ag_buf_app(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}

// Decimal uint32 → string, writes into buf[0..max-1]
static void ag_uint_to_str(uint32_t v, char* buf, int max) {
    char tmp[12]; int n = 0;
    if (!v) { buf[0]='0'; buf[1]='\0'; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    for (int j = n - 1; j >= 0 && i < max - 1; j--) buf[i++] = tmp[j];
    buf[i] = '\0';
}

// ─── FNV-1a (matches object_catalog.c) ───────────────────────────────────────
static uint64_t ag_fnv1a(const char* s, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

// ─── Catalog helpers ──────────────────────────────────────────────────────────
extern uint64_t sys_sls_valloc(struct SLSVallocRequest* req);
extern int      catalog_find_by_name(const char* name); // not exported; look up manually

static int agent_find(const char* name) {
    for (int i = 0; i < AGENT_MAX; i++)
        if (agent_table[i].active && ag_streq(agent_table[i].name, name))
            return i;
    return -1;
}

static int agent_free_slot(void) {
    for (int i = 0; i < AGENT_MAX; i++)
        if (!agent_table[i].active) return i;
    return -1;
}

static int workflow_find(const char* name) {
    for (int i = 0; i < WORKFLOW_MAX; i++)
        if (workflow_table[i].active && ag_streq(workflow_table[i].name, name))
            return i;
    return -1;
}

static int workflow_free_slot(void) {
    for (int i = 0; i < WORKFLOW_MAX; i++)
        if (!workflow_table[i].active) return i;
    return -1;
}

// ─── Initialisation ───────────────────────────────────────────────────────────
void agent_init(void) {
    for (int i = 0; i < AGENT_MAX; i++)
        agent_table[i].active = 0;
    for (int i = 0; i < WORKFLOW_MAX; i++)
        workflow_table[i].active = 0;
    kernel_serial_printf("[AGENT] Agent engine initialised (max agents=%d, max workflows=%d)\n",
                         AGENT_MAX, WORKFLOW_MAX);
}

// ─── SYS_SLS_AGENT_CREATE (200) ───────────────────────────────────────────────
uint64_t sys_sls_agent_create(struct AgentCreateRequest* req) {
    if (!req || !req->name[0]) return 1;

    if (agent_find(req->name) >= 0) {
        kernel_serial_printf("[AGENT] CREATE failed: '%s' already exists\n", req->name);
        return 1;
    }

    int slot = agent_free_slot();
    if (slot < 0) {
        kernel_serial_printf("[AGENT] CREATE failed: agent table full (max %d)\n", AGENT_MAX);
        return 1;
    }

    // Register the agent as a first-class catalog object
    struct SLSVallocRequest vr;
    ag_strncpy(vr.name, req->name, OBJECT_NAME_LEN);
    vr.type       = OBJ_TYPE_AGENT;
    vr.size_pages = 1;
    vr.owner_uid  = req->owner_uid;
    vr.perm_mask  = 0x7;
    vr.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    vr.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
    uint64_t obj_id = sys_sls_valloc(&vr);
    if (!obj_id) {
        kernel_serial_printf("[AGENT] CREATE failed: catalog valloc error for '%s'\n", req->name);
        return 1;
    }

    struct AgentDescriptor* ag = &agent_table[slot];
    ag->object_id = obj_id;
    ag_strncpy(ag->name,                req->name,               OBJECT_NAME_LEN);
    ag_strncpy(ag->inference_endpoint,  req->inference_endpoint, AGENT_ENDPOINT_LEN);
    ag_strncpy(ag->model,               req->model,              AGENT_MODEL_LEN);
    ag_strncpy(ag->system_prompt,       req->system_prompt,      AGENT_PROMPT_LEN);
    ag->tool_mask            = req->tool_mask;
    ag->owner_uid             = req->owner_uid;  // (F)
    ag->memory_table_id       = 0;
    ag->memory_table_name[0]  = '\0';
    ag->history_stream_id     = 0;
    ag->state                 = AGENT_STATE_IDLE;
    ag->step_count            = 0;
    ag->run_count             = 0;              // (B)
    ag->schedule_ticks        = 0;              // (E)
    ag->schedule_msg[0]       = '\0';           // (E)
    ag->last_answer[0]        = '\0';
    ag->active                = 1;

    // ── Auto-create the persistent memory DB_TABLE ────────────────────────────
    // Name: "_<agentname>_mem" (fits in OBJECT_NAME_LEN=64 for names <= 58 chars)
    {
        static char mem_tbl[OBJECT_NAME_LEN];
        mem_tbl[0] = '_';
        int nlen = 0;
        while (req->name[nlen] && nlen < 58) { mem_tbl[1+nlen]=req->name[nlen]; nlen++; }
        mem_tbl[1+nlen]='_'; mem_tbl[2+nlen]='m'; mem_tbl[3+nlen]='e';
        mem_tbl[4+nlen]='m'; mem_tbl[5+nlen]='\0';

        struct SLSVallocRequest mv;
        ag_strncpy(mv.name, mem_tbl, OBJECT_NAME_LEN);
        mv.type       = OBJ_TYPE_DB_TABLE;
        mv.size_pages = 1;
        mv.owner_uid  = req->owner_uid;
        mv.perm_mask  = 0x7;
        mv.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
        mv.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
        uint64_t mem_id = sys_sls_valloc(&mv);
        if (mem_id) {
            ag->memory_table_id = mem_id;
            ag_strncpy(ag->memory_table_name, mem_tbl, OBJECT_NAME_LEN);
            // Seed initial records
            struct SLSRecordRequest mr;
            ag_strncpy(mr.name,  mem_tbl,     OBJECT_NAME_LEN);
            ag_strncpy(mr.key,   "step_count", RECORD_KEY_LEN);
            ag_strncpy(mr.value, "0",          RECORD_VAL_LEN);
            sys_sls_insert(&mr);
            kernel_serial_printf("[AGENT] Memory table '%s' created\n", mem_tbl);
        } else {
            kernel_serial_printf("[AGENT] WARNING: memory table creation failed for '%s'\n",
                                 req->name);
        }
    }

    kernel_serial_printf("[AGENT] Created agent '%s' model='%s' endpoint='%s' tools=0x%x\n",
                         ag->name, ag->model, ag->inference_endpoint, ag->tool_mask);
    return 0;
}

// ─── SYS_SLS_AGENT_RUN (201) — ReAct loop ────────────────────────────────────
uint64_t sys_sls_agent_run(struct AgentRunRequest* req) {
    if (!req || !req->name[0]) return 1;

    int slot = agent_find(req->name);
    if (slot < 0) {
        kernel_serial_printf("[AGENT] RUN failed: agent '%s' not found\n", req->name);
        return 1;
    }

    struct AgentDescriptor* ag = &agent_table[slot];
    if (ag->state == AGENT_STATE_RUNNING) {
        kernel_serial_printf("[AGENT] RUN failed: agent '%s' already running\n", req->name);
        return 1;
    }

    ag->state = AGENT_STATE_RUNNING;
    kernel_serial_printf("[AGENT] RUN '%s' message='%.60s'\n",
                         ag->name, req->message);

    // Static buffers keep large structs off the kernel stack
    static struct InferRequest  ir;
    static struct InferResponse infer_resp;
    static char react_msg[INFER_MESSAGE_LEN];
    static char tool_result[TOOL_RESULT_MAX];

    // Parse "ip:port" from ag->inference_endpoint into ir.endpoint_ip + ir.port
    ag_strncpy(ir.endpoint_ip, ag->inference_endpoint, INFER_ENDPOINT_LEN);
    ir.port = 11434;
    for (int i = 0; ir.endpoint_ip[i]; i++) {
        if (ir.endpoint_ip[i] == ':') {
            ir.endpoint_ip[i] = '\0';
            uint16_t p = 0;
            for (int j = i + 1; ag->inference_endpoint[j]; j++)
                p = (uint16_t)(p * 10 + (ag->inference_endpoint[j] - '0'));
            if (p) ir.port = p;
            break;
        }
    }

    ag_strncpy(ir.model, ag->model, INFER_MODEL_LEN);

    // ── Load memory: reload last_answer from DB if descriptor is blank (post-reboot) ──
    if (ag->last_answer[0] == '\0' && ag->memory_table_name[0]) {
        for (uint32_t ci = 0; ci < object_catalog_count; ci++) {
            if (!object_catalog[ci].active) continue;
            if (!ag_streq(object_catalog[ci].name, ag->memory_table_name)) continue;
            struct SLSObjectRecord* rec = &object_records[ci];
            for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
                if (!rec->fields[fi].active) continue;
                if (ag_streq(rec->fields[fi].key, "last_answer")) {
                    ag_strncpy(ag->last_answer, rec->fields[fi].value,
                               AGENT_LAST_ANSWER_LEN);
                    kernel_serial_printf("[AGENT] '%s' memory loaded: '%.60s'\n",
                                         ag->name, ag->last_answer);
                    break;
                }
            }
            break;
        }
    }

    // ── Build system prompt enriched with memory context (if available) ────────────
    static char enriched_prompt[INFER_PROMPT_LEN];
    static char mem_snippet[84];
    if (ag->memory_table_name[0] && ag->last_answer[0]) {
        int pos = 0;
        ag_buf_app(enriched_prompt, &pos, INFER_PROMPT_LEN, ag->system_prompt);
        if (pos > 0) ag_buf_app(enriched_prompt, &pos, INFER_PROMPT_LEN, " ");
        ag_buf_app(enriched_prompt, &pos, INFER_PROMPT_LEN,
                   "[Prior context \u2014 last answer: \"");
        ag_strncpy(mem_snippet, ag->last_answer, 81);
        ag_buf_app(enriched_prompt, &pos, INFER_PROMPT_LEN, mem_snippet);
        ag_buf_app(enriched_prompt, &pos, INFER_PROMPT_LEN, "\"]");
        enriched_prompt[pos] = '\0';
        ag_strncpy(ir.system_prompt, enriched_prompt, INFER_PROMPT_LEN);
    } else {
        ag_strncpy(ir.system_prompt, ag->system_prompt, INFER_PROMPT_LEN);
    }

    // Start with the original user message
    ag_strncpy(react_msg, req->message, INFER_MESSAGE_LEN);

    // ── ReAct loop (Reason + Act) ─────────────────────────────────────────────
    for (int step = 0; step < AGENT_MAX_STEPS; step++) {
        ag_strncpy(ir.user_message, react_msg, INFER_MESSAGE_LEN);

        // Build tool schemas for this agent's permitted tools
        if (ag->tool_mask)
            agent_tools_build_schema(ag->tool_mask, ir.tools_json, INFER_TOOLS_LEN);
        else
            ir.tools_json[0] = '\0';

        int rc = infer_call(&ir, &infer_resp);
        ag->step_count++;

        if (rc != 0) {
            kernel_serial_printf("[AGENT] '%s' inference error at step %d\n",
                                 ag->name, step + 1);
            ag->state = AGENT_STATE_ERROR;
            return 1;
        }

        if (!infer_resp.has_tool_call) {
            kernel_serial_printf("[AGENT] '%s' answer (step %d): %.120s\n",
                                 ag->name, step + 1, infer_resp.content);
            ag_strncpy(ag->last_answer, infer_resp.content, AGENT_LAST_ANSWER_LEN);
            // ── Persist to memory table (WAL-journaled, survives reboots) ────
            if (ag->memory_table_name[0]) {
                struct SLSRecordRequest mr;
                ag_strncpy(mr.name,  ag->memory_table_name, OBJECT_NAME_LEN);
                ag_strncpy(mr.key,   "last_answer",          RECORD_KEY_LEN);
                ag_strncpy(mr.value, ag->last_answer,         RECORD_VAL_LEN);
                if (sys_sls_insert(&mr) != 0) sys_sls_update(&mr);
                ag_strncpy(mr.key, "step_count", RECORD_KEY_LEN);
                ag_uint_to_str(ag->step_count, mr.value, RECORD_VAL_LEN);
                if (sys_sls_insert(&mr) != 0) sys_sls_update(&mr);
                // (B) Append run history entry
                ag->run_count++;
                static char run_key[16];
                run_key[0]='r'; run_key[1]='u'; run_key[2]='n'; run_key[3]='_';
                ag_uint_to_str(ag->run_count, run_key+4, 11);
                static char run_val[RECORD_VAL_LEN];
                {
                    int vp = 0;
                    static char qt[28]; ag_strncpy(qt, req->message, 26);
                    static char at_[28]; ag_strncpy(at_, ag->last_answer, 26);
                    ag_buf_app(run_val, &vp, RECORD_VAL_LEN, "Q:");
                    ag_buf_app(run_val, &vp, RECORD_VAL_LEN, qt);
                    ag_buf_app(run_val, &vp, RECORD_VAL_LEN, "|A:");
                    ag_buf_app(run_val, &vp, RECORD_VAL_LEN, at_);
                    run_val[vp] = '\0';
                }
                struct SLSRecordRequest mr2;
                ag_strncpy(mr2.name,  ag->memory_table_name, OBJECT_NAME_LEN);
                ag_strncpy(mr2.key,   run_key,               RECORD_KEY_LEN);
                ag_strncpy(mr2.value, run_val,               RECORD_VAL_LEN);
                sys_sls_insert(&mr2);
                // (B) update run_count record
                ag_strncpy(mr2.key, "run_count", RECORD_KEY_LEN);
                ag_uint_to_str(ag->run_count, mr2.value, RECORD_VAL_LEN);
                if (sys_sls_insert(&mr2) != 0) sys_sls_update(&mr2);
                kernel_serial_printf("[AGENT] '%s' memory persisted to '%s'\n",
                                     ag->name, ag->memory_table_name);
            }
            ag->state = AGENT_STATE_IDLE;
            return 0;
        }

        // Tool call — execute and feed result back
        kernel_serial_printf("[AGENT] '%s' step %d: tool='%s' args=%.60s\n",
                             ag->name, step + 1,
                             infer_resp.tool_name, infer_resp.tool_args_json);

        agent_tools_execute(infer_resp.tool_name,
                            infer_resp.tool_args_json,
                            tool_result, TOOL_RESULT_MAX,
                            ag->tool_mask,
                            ag->owner_uid);  // (F)

        kernel_serial_printf("[AGENT] Tool result: %.80s\n", tool_result);

        // Build the continuation message: original question + tool result
        int mpos = 0;
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, "Question: ");
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, req->message);
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, "\nTool '");
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, infer_resp.tool_name);
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, "' returned: ");
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN, tool_result);
        ag_buf_app(react_msg, &mpos, INFER_MESSAGE_LEN,
                   "\nUsing this data, answer the original question.");
        react_msg[mpos] = '\0';
    }

    kernel_serial_printf("[AGENT] '%s' reached max steps (%d)\n",
                         ag->name, AGENT_MAX_STEPS);
    // last_answer holds whatever content the final iteration produced
    ag_strncpy(ag->last_answer, infer_resp.content, AGENT_LAST_ANSWER_LEN);
    if (ag->memory_table_name[0]) {
        struct SLSRecordRequest mr;
        ag_strncpy(mr.name,  ag->memory_table_name, OBJECT_NAME_LEN);
        ag_strncpy(mr.key,   "last_answer",          RECORD_KEY_LEN);
        ag_strncpy(mr.value, ag->last_answer,         RECORD_VAL_LEN);
        if (sys_sls_insert(&mr) != 0) sys_sls_update(&mr);
        ag_strncpy(mr.key, "step_count", RECORD_KEY_LEN);
        ag_uint_to_str(ag->step_count, mr.value, RECORD_VAL_LEN);
        if (sys_sls_insert(&mr) != 0) sys_sls_update(&mr);
    }
    ag->state = AGENT_STATE_IDLE;
    return 0;
}

// ─── SYS_SLS_AGENT_STATUS (202) ───────────────────────────────────────────────
void sys_sls_agent_status(const char* name) {
    if (!name || !name[0]) return;
    int slot = agent_find(name);
    if (slot < 0) {
        kernel_serial_printf("[AGENT] STATUS: agent '%s' not found\n", name);
        return;
    }
    struct AgentDescriptor* ag = &agent_table[slot];
    kernel_serial_printf(
        "[AGENT] '%s'\n"
        "  Object ID  : 0x%016lx\n"
        "  State      : %s\n"
        "  Model      : %s\n"
        "  Endpoint   : %s\n"
        "  Tools      : 0x%08x\n"
        "  Steps run  : %u\n"
        "  Mem table  : 0x%016lx\n"
        "  History    : 0x%016lx\n"
        "  Last answer: %.120s\n",
        ag->name,
        ag->object_id,
        agent_state_name(ag->state),
        ag->model,
        ag->inference_endpoint,
        ag->tool_mask,
        ag->step_count,
        ag->memory_table_id,
        ag->history_stream_id,
        ag->last_answer[0] ? ag->last_answer : "(none)");
}

// ─── SYS_SLS_AGENT_KILL (203) ─────────────────────────────────────────────────
uint64_t sys_sls_agent_kill(const char* name) {
    if (!name || !name[0]) return 1;
    int slot = agent_find(name);
    if (slot < 0) {
        kernel_serial_printf("[AGENT] KILL: agent '%s' not found\n", name);
        return 1;
    }
    struct AgentDescriptor* ag = &agent_table[slot];
    ag->state  = AGENT_STATE_IDLE;
    ag->active = 0;
    kernel_serial_printf("[AGENT] KILL: agent '%s' removed\n", name);
    return 0;
}

// ─── SYS_SLS_AGENT_LIST (204) ─────────────────────────────────────────────────
void sys_sls_agent_list(void) {
    kernel_serial_printf("[AGENT] Agent table:\n");
    int found = 0;
    for (int i = 0; i < AGENT_MAX; i++) {
        if (!agent_table[i].active) continue;
        struct AgentDescriptor* ag = &agent_table[i];
        kernel_serial_printf("  [%d] %-24s  %-8s  model=%-20s  tools=0x%x  steps=%u\n",
                             i, ag->name,
                             agent_state_name(ag->state),
                             ag->model,
                             ag->tool_mask,
                             ag->step_count);
        found++;
    }
    if (!found)
        kernel_serial_printf("  (no agents)\n");
}

// ─── SYS_SLS_WORKFLOW_CREATE (205) ────────────────────────────────────────────
uint64_t sys_sls_workflow_create(struct WorkflowCreateRequest* req) {
    if (!req || !req->name[0]) return 1;
    if (req->step_count == 0 || req->step_count > WORKFLOW_MAX_STEPS) return 1;

    if (workflow_find(req->name) >= 0) {
        kernel_serial_printf("[WORKFLOW] CREATE failed: '%s' already exists\n", req->name);
        return 1;
    }

    int slot = workflow_free_slot();
    if (slot < 0) {
        kernel_serial_printf("[WORKFLOW] CREATE failed: table full (max %d)\n", WORKFLOW_MAX);
        return 1;
    }

    // Register the workflow as a catalog object
    struct SLSVallocRequest vr;
    ag_strncpy(vr.name, req->name, OBJECT_NAME_LEN);
    vr.type       = OBJ_TYPE_WORKFLOW;
    vr.size_pages = 1;
    vr.owner_uid  = req->owner_uid;
    vr.perm_mask  = 0x7;
    vr.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    vr.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
    uint64_t obj_id = sys_sls_valloc(&vr);
    if (!obj_id) {
        kernel_serial_printf("[WORKFLOW] CREATE failed: catalog valloc error for '%s'\n", req->name);
        return 1;
    }

    struct WorkflowDescriptor* wf = &workflow_table[slot];
    wf->object_id    = obj_id;
    ag_strncpy(wf->name, req->name, OBJECT_NAME_LEN);

    // Store both the ID (for fast lookup) and the name (for DB record ops)
    wf->shared_state_table_id = ag_fnv1a(req->shared_state_table,
                                          ag_strlen(req->shared_state_table));
    ag_strncpy(wf->shared_state_table_name, req->shared_state_table, OBJECT_NAME_LEN);

    for (uint8_t s = 0; s < req->step_count; s++) {
        ag_strncpy(wf->steps[s].agent_name,  req->steps[s].agent_name,  OBJECT_NAME_LEN);
        ag_strncpy(wf->steps[s].input_key,   req->steps[s].input_key,   RECORD_KEY_LEN);
        ag_strncpy(wf->steps[s].output_key,  req->steps[s].output_key,  RECORD_KEY_LEN);
    }
    wf->step_count   = req->step_count;
    wf->current_step = 0;
    wf->state        = WORKFLOW_STATE_IDLE;
    wf->active       = 1;

    kernel_serial_printf("[WORKFLOW] Created '%s' with %u steps\n",
                         wf->name, wf->step_count);
    return 0;
}

// ─── SYS_SLS_WORKFLOW_RUN (206) — sequential agent execution engine ───────────────
uint64_t sys_sls_workflow_run(struct WorkflowRunRequest* req) {
    if (!req || !req->name[0]) return 1;

    int slot = workflow_find(req->name);
    if (slot < 0) {
        kernel_serial_printf("[WORKFLOW] RUN failed: '%s' not found\n", req->name);
        return 1;
    }

    struct WorkflowDescriptor* wf = &workflow_table[slot];
    if (wf->state == WORKFLOW_STATE_RUNNING) {
        kernel_serial_printf("[WORKFLOW] RUN failed: '%s' already running\n", req->name);
        return 1;
    }
    if (wf->step_count == 0) {
        kernel_serial_printf("[WORKFLOW] RUN failed: '%s' has no steps\n", req->name);
        return 1;
    }

    wf->state        = WORKFLOW_STATE_RUNNING;
    wf->current_step = 0;
    kernel_serial_printf("[WORKFLOW] RUN '%s' (%u steps) input='%.60s'\n",
                         wf->name, (uint32_t)wf->step_count, req->input);

    // Write the initial input into the shared state table under "input" key
    if (wf->shared_state_table_name[0] && req->input[0]) {
        struct SLSRecordRequest wr;
        ag_strncpy(wr.name,  wf->shared_state_table_name, OBJECT_NAME_LEN);
        ag_strncpy(wr.key,   "input",                     RECORD_KEY_LEN);
        ag_strncpy(wr.value, req->input,                  RECORD_VAL_LEN);
        if (sys_sls_insert(&wr) != 0) sys_sls_update(&wr);
    }

    // ── Sequential step loop ─────────────────────────────────────────────────
    for (uint8_t s = 0; s < wf->step_count; s++) {
        wf->current_step = s;
        struct WorkflowStep* step = &wf->steps[s];

        kernel_serial_printf(
            "[WORKFLOW] '%s' step %u/%u: agent='%s' in='%s' out='%s'\n",
            wf->name, (uint32_t)(s+1), (uint32_t)wf->step_count,
            step->agent_name, step->input_key, step->output_key);

        // Read the input value from the shared state table
        static char step_input[RECORD_VAL_LEN];
        step_input[0] = '\0';

        if (wf->shared_state_table_name[0] && step->input_key[0]) {
            for (uint32_t ci = 0; ci < object_catalog_count; ci++) {
                if (!object_catalog[ci].active) continue;
                if (!ag_streq(object_catalog[ci].name,
                              wf->shared_state_table_name)) continue;
                struct SLSObjectRecord* rec = &object_records[ci];
                for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
                    if (!rec->fields[fi].active) continue;
                    if (ag_streq(rec->fields[fi].key, step->input_key)) {
                        ag_strncpy(step_input, rec->fields[fi].value,
                                   RECORD_VAL_LEN);
                        break;
                    }
                }
                break;
            }
        }

        // Fall back to the original workflow input if the key wasn't found
        if (!step_input[0] && req->input[0])
            ag_strncpy(step_input, req->input, RECORD_VAL_LEN);

        kernel_serial_printf("[WORKFLOW] step %u input: '%.80s'\n",
                             (uint32_t)(s+1), step_input);

        // Validate the agent exists
        int agent_slot = agent_find(step->agent_name);
        if (agent_slot < 0) {
            kernel_serial_printf(
                "[WORKFLOW] step %u: agent '%s' not found — aborting\n",
                (uint32_t)(s+1), step->agent_name);
            wf->state = WORKFLOW_STATE_ERROR;
            return 1;
        }

        // Run the agent
        struct AgentRunRequest ar;
        ag_strncpy(ar.name,    step->agent_name, OBJECT_NAME_LEN);
        ag_strncpy(ar.message, step_input,       AGENT_PROMPT_LEN);

        uint64_t rc = sys_sls_agent_run(&ar);
        if (rc != 0) {
            kernel_serial_printf(
                "[WORKFLOW] step %u: agent '%s' returned error — aborting\n",
                (uint32_t)(s+1), step->agent_name);
            wf->state = WORKFLOW_STATE_ERROR;
            return 1;
        }

        // Write the agent's last_answer to the output key in the shared table
        const char* answer = agent_table[agent_slot].last_answer;
        if (step->output_key[0] && wf->shared_state_table_name[0]) {
            struct SLSRecordRequest wr;
            ag_strncpy(wr.name,  wf->shared_state_table_name, OBJECT_NAME_LEN);
            ag_strncpy(wr.key,   step->output_key,             RECORD_KEY_LEN);
            ag_strncpy(wr.value, answer,                       RECORD_VAL_LEN);
            if (sys_sls_insert(&wr) != 0) sys_sls_update(&wr);
        }

        kernel_serial_printf("[WORKFLOW] step %u done. answer='%.80s'\n",
                             (uint32_t)(s+1), answer[0] ? answer : "(empty)");
    }

    wf->state        = WORKFLOW_STATE_DONE;
    wf->current_step = wf->step_count;
    kernel_serial_printf("[WORKFLOW] '%s' completed all %u steps\n",
                         wf->name, (uint32_t)wf->step_count);
    return 0;
}

// ─── SYS_SLS_WORKFLOW_STATUS (207) ────────────────────────────────────────────
void sys_sls_workflow_status(const char* name) {
    if (!name || !name[0]) return;
    int slot = workflow_find(name);
    if (slot < 0) {
        kernel_serial_printf("[WORKFLOW] STATUS: '%s' not found\n", name);
        return;
    }
    struct WorkflowDescriptor* wf = &workflow_table[slot];
    kernel_serial_printf(
        "[WORKFLOW] '%s'\n"
        "  Object ID   : 0x%016lx\n"
        "  State       : %s\n"
        "  Steps       : %u total, %u current\n"
        "  State table : 0x%016lx\n",
        wf->name,
        wf->object_id,
        workflow_state_name(wf->state),
        wf->step_count, wf->current_step,
        wf->shared_state_table_id);

    for (uint8_t s = 0; s < wf->step_count; s++) {
        kernel_serial_printf("  [%u] agent=%-20s  in=%-20s  out=%s\n",
                             s,
                             wf->steps[s].agent_name,
                             wf->steps[s].input_key,
                             wf->steps[s].output_key);
    }
}

// ─── sys_sls_workflow_list ────────────────────────────────────────────────────
void sys_sls_workflow_list(void) {
    kernel_serial_printf("[WORKFLOW] Workflow table:\n");
    int found = 0;
    for (int i = 0; i < WORKFLOW_MAX; i++) {
        if (!workflow_table[i].active) continue;
        struct WorkflowDescriptor* wf = &workflow_table[i];
        kernel_serial_printf("  [%d] %-24s  %-8s  steps=%u  current=%u\n",
                             i, wf->name,
                             workflow_state_name(wf->state),
                             wf->step_count, wf->current_step);
        found++;
    }
    if (!found) kernel_serial_printf("  (no workflows)\n");
}

// ─── (E) agent_scheduler_tick — called from microkernel_service_poll() ────────
// Runs every time the AP service loop polls.  Uses a static tick counter so
// agents fire at the requested intervals without needing a real wall clock.
void agent_scheduler_tick(void) {
    static uint32_t sched_tick = 0;
    sched_tick++;
    static struct AgentRunRequest sched_req;
    for (int i = 0; i < AGENT_MAX; i++) {
        struct AgentDescriptor* ag = &agent_table[i];
        if (!ag->active)                         continue;
        if (!ag->schedule_ticks)                 continue;
        if (ag->state != AGENT_STATE_IDLE)        continue;
        if (sched_tick % ag->schedule_ticks != 0) continue;
        kernel_serial_printf("[SCHED] Firing scheduled run for agent '%s'\n",
                             ag->name);
        ag_strncpy(sched_req.name,    ag->name,         OBJECT_NAME_LEN);
        ag_strncpy(sched_req.message, ag->schedule_msg, AGENT_PROMPT_LEN);
        sys_sls_agent_run(&sched_req);
    }
}

// ─── (E) sys_sls_agent_schedule (208) ────────────────────────────────────────
uint64_t sys_sls_agent_schedule(struct AgentScheduleRequest* req) {
    if (!req || !req->name[0]) return 1;
    int slot = agent_find(req->name);
    if (slot < 0) {
        kernel_serial_printf("[AGENT] SCHEDULE: agent '%s' not found\n",
                             req->name);
        return 1;
    }
    struct AgentDescriptor* ag = &agent_table[slot];
    ag->schedule_ticks = req->ticks;
    if (req->ticks > 0) {
        ag_strncpy(ag->schedule_msg, req->message, sizeof(ag->schedule_msg));
        kernel_serial_printf("[AGENT] '%s' scheduled every %u ticks: '%.60s'\n",
                             ag->name, req->ticks, ag->schedule_msg);
    } else {
        ag->schedule_msg[0] = '\0';
        kernel_serial_printf("[AGENT] '%s' schedule cleared\n", ag->name);
    }
    return 0;
}
