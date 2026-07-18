#include "agent_tools.h"
#include "object_catalog.h"
#include "tier_mgr.h"
#include "stream.h"
#include "kernel_io.h"
#include "../user/permissions.h"   // PERM_READ, PERM_WRITE (F)

// ─── Tool JSON schemas (OpenAI function-calling format, no outer brackets) ────
#define SCHEMA_DB_SELECT \
    "{\"type\":\"function\",\"function\":{\"name\":\"db_select\"," \
    "\"description\":\"Read fields from a DB_TABLE object.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"table\":{\"type\":\"string\",\"description\":\"table name\"}," \
    "\"key\":{\"type\":\"string\",\"description\":\"field key; empty returns all fields\"}}," \
    "\"required\":[\"table\"]}}}"

#define SCHEMA_DB_INSERT \
    "{\"type\":\"function\",\"function\":{\"name\":\"db_insert\"," \
    "\"description\":\"Write a key-value field to a DB_TABLE object.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"table\":{\"type\":\"string\",\"description\":\"table name\"}," \
    "\"key\":{\"type\":\"string\",\"description\":\"field key\"}," \
    "\"value\":{\"type\":\"string\",\"description\":\"field value\"}}," \
    "\"required\":[\"table\",\"key\",\"value\"]}}}"

#define SCHEMA_DB_QUERY \
    "{\"type\":\"function\",\"function\":{\"name\":\"db_query\"," \
    "\"description\":\"Keyword scan of the live object catalog and record fields.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"query\":{\"type\":\"string\",\"description\":\"search keyword\"}}," \
    "\"required\":[\"query\"]}}}"

#define SCHEMA_TIER_PROMOTE \
    "{\"type\":\"function\",\"function\":{\"name\":\"tier_promote\"," \
    "\"description\":\"Promote an object to a faster storage tier (L3->L2->L1).\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"object name\"}}," \
    "\"required\":[\"name\"]}}}"

#define SCHEMA_STREAM_READ \
    "{\"type\":\"function\",\"function\":{\"name\":\"stream_read\"," \
    "\"description\":\"Read text content from a STREAM object.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"stream name\"}," \
    "\"max_bytes\":{\"type\":\"string\",\"description\":\"max bytes to read (default 256)\"}}," \
    "\"required\":[\"name\"]}}}"

#define SCHEMA_STREAM_WRITE \
    "{\"type\":\"function\",\"function\":{\"name\":\"stream_write\"," \
    "\"description\":\"Append text to a STREAM object.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"stream name\"}," \
    "\"data\":{\"type\":\"string\",\"description\":\"text to append\"}}," \
    "\"required\":[\"name\",\"data\"]}}}"

// ─── Internal string / buffer helpers ─────────────────────────────────────────
static int at_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Case-insensitive substring search
static int at_strcontains(const char* hay, const char* ndl) {
    for (; *hay; hay++) {
        const char* h = hay, *n = ndl;
        while (*h && *n) {
            char hc = (*h>='A'&&*h<='Z') ? (char)(*h+32) : *h;
            char nc = (*n>='A'&&*n<='Z') ? (char)(*n+32) : *n;
            if (hc != nc) break;
            h++; n++;
        }
        if (!*n) return 1;
    }
    return 0;
}

static const char* at_strfind(const char* hay, const char* ndl) {
    if (!*ndl) return hay;
    for (; *hay; hay++) {
        const char* h = hay, *n = ndl;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

static void at_app(char* b, int* p, int m, const char* s) {
    while (*s && *p < m-1) b[(*p)++] = *s++;
}
static void at_char(char* b, int* p, int m, char c) {
    if (*p < m-1) b[(*p)++] = c;
}
static void at_uint(char* b, int* p, int m, uint32_t v) {
    char t[12]; int n=0;
    if (!v) { at_char(b,p,m,'0'); return; }
    while (v&&n<11) { t[n++]=(char)('0'+v%10); v/=10; }
    for (int i=n-1;i>=0;i--) at_char(b,p,m,t[i]);
}
// JSON-escape a string value into the buffer
static void at_json(char* b, int* p, int m, const char* s) {
    while (*s && *p < m-2) {
        unsigned char c = (unsigned char)*s++;
        if      (c=='"')  { at_app(b,p,m,"\\\""); }
        else if (c=='\\') { at_app(b,p,m,"\\\\"); }
        else if (c=='\n') { at_app(b,p,m,"\\n");  }
        else if (c=='\r') { at_app(b,p,m,"\\r");  }
        else if (c<0x20)  { /* skip control chars */ }
        else              { at_char(b,p,m,(char)c); }
    }
}
// strncpy-style copy
static void at_copy(char* d, const char* s, int n) {
    int i;
    for (i=0; i<n-1 && s[i]; i++) d[i]=s[i];
    d[i]='\0';
}

// JSON string extractor: "key":"value" → out; returns chars copied
static int at_json_str(const char* json, const char* key, char* out, int max) {
    char srch[128]; int si=0;
    srch[si++]='"';
    for (int i=0; key[i]&&si<120; i++) srch[si++]=key[i];
    srch[si++]='"'; srch[si++]=':'; srch[si]='\0';
    const char* p = at_strfind(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p!='"') return 0;
    p++;
    int n=0;
    while (*p && n<max-1) {
        if (*p=='\\'&&*(p+1)) {
            char e=*(p+1);
            if      (e=='"')  { out[n++]='"';  p+=2; }
            else if (e=='\\') { out[n++]='\\'; p+=2; }
            else if (e=='n')  { out[n++]='\n'; p+=2; }
            else              { out[n++]=e;    p+=2; }
        } else if (*p=='"') break;
        else out[n++]=*p++;
    }
    out[n]='\0'; return n;
}

// Build an error result JSON
static int at_err(char* buf, int max, const char* msg) {
    int p=0;
    at_app(buf,&p,max,"{\"ok\":false,\"error\":\"");
    at_json(buf,&p,max,msg);
    at_app(buf,&p,max,"\"}");
    buf[p]='\0'; return -1;
}

// ─── Tool: db_select ──────────────────────────────────────────────────────────
static int tool_db_select(const char* args, char* buf, int max, uint32_t uid) {
    char table[OBJECT_NAME_LEN]={0};
    char key  [RECORD_KEY_LEN] ={0};
    at_json_str(args, "table", table, OBJECT_NAME_LEN);
    at_json_str(args, "key",   key,   RECORD_KEY_LEN);
    if (!table[0]) return at_err(buf, max, "table required");
    if (!catalog_check_access(uid, table, PERM_READ))   // (F)
        return at_err(buf, max, "access denied");

    int oi = -1;
    for (uint32_t i=0; i<object_catalog_count; i++) {
        if (object_catalog[i].active && at_streq(object_catalog[i].name, table)) {
            oi=(int)i; break;
        }
    }
    if (oi < 0) return at_err(buf, max, "table not found");

    struct SLSObjectRecord* rec = &object_records[oi];
    int pos=0;
    at_app(buf,&pos,max,"{\"ok\":true,\"records\":[");
    int first=1;
    for (uint32_t i=0; i<RECORD_MAX_FIELDS; i++) {
        if (!rec->fields[i].active) continue;
        if (key[0] && !at_streq(rec->fields[i].key, key)) continue;
        if (!first) at_char(buf,&pos,max,',');
        first=0;
        at_app (buf,&pos,max,"{\"key\":\"");
        at_json(buf,&pos,max,rec->fields[i].key);
        at_app (buf,&pos,max,"\",\"value\":\"");
        at_json(buf,&pos,max,rec->fields[i].value);
        at_app (buf,&pos,max,"\"}");
    }
    at_app(buf,&pos,max,"]}");
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] db_select %s%s%s → %d fields\n",
                         table, key[0]?".":"", key[0]?key:"", rec->field_count);
    return 0;
}

// ─── Tool: db_insert ──────────────────────────────────────────────────────────
static int tool_db_insert(const char* args, char* buf, int max, uint32_t uid) {
    char table[OBJECT_NAME_LEN]={0};
    char key  [RECORD_KEY_LEN] ={0};
    char value[RECORD_VAL_LEN] ={0};
    at_json_str(args, "table", table, OBJECT_NAME_LEN);
    at_json_str(args, "key",   key,   RECORD_KEY_LEN);
    at_json_str(args, "value", value, RECORD_VAL_LEN);
    if (!table[0]||!key[0]) return at_err(buf, max, "table and key required");
    if (!catalog_check_access(uid, table, PERM_WRITE))  // (F)
        return at_err(buf, max, "access denied");

    struct SLSRecordRequest req;
    at_copy(req.name,  table, OBJECT_NAME_LEN);
    at_copy(req.key,   key,   RECORD_KEY_LEN);
    at_copy(req.value, value, RECORD_VAL_LEN);

    uint64_t rc = sys_sls_insert(&req);
    int pos=0;
    if (rc==0) at_app(buf,&pos,max,"{\"ok\":true}");
    else       at_app(buf,&pos,max,"{\"ok\":false,\"error\":\"insert failed\"}");
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] db_insert %s.%s=%s → %s\n",
                         table, key, value, rc==0?"ok":"fail");
    return (rc==0) ? 0 : -1;
}

// ─── Tool: db_query ───────────────────────────────────────────────────────────
static int tool_db_query(const char* args, char* buf, int max, uint32_t uid) {
    char query[128]={0};
    at_json_str(args, "query", query, sizeof(query));
    if (!query[0]) return at_err(buf, max, "query required");
    if (catalog_get_role(uid) == ROLE_GUEST)              // (F)
        return at_err(buf, max, "access denied");

    int pos=0;
    at_app(buf,&pos,max,"{\"ok\":true,\"matches\":[");
    int first=1;

    for (uint32_t i=0; i<object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        int hit = at_strcontains(object_catalog[i].name, query);
        if (!hit) {
            struct SLSObjectRecord* rec = &object_records[i];
            for (uint32_t f=0; f<RECORD_MAX_FIELDS&&!hit; f++) {
                if (!rec->fields[f].active) continue;
                hit = at_strcontains(rec->fields[f].key,   query) ||
                      at_strcontains(rec->fields[f].value, query);
            }
        }
        if (!hit) continue;
        if (!first) at_char(buf,&pos,max,',');
        first=0;
        at_app (buf,&pos,max,"{\"name\":\"");
        at_json(buf,&pos,max,object_catalog[i].name);
        at_app (buf,&pos,max,"\",\"type\":\"");
        at_app (buf,&pos,max,obj_type_name(object_catalog[i].type));
        at_app (buf,&pos,max,"\",\"fields\":");
        at_uint(buf,&pos,max,object_records[i].field_count);
        at_app (buf,&pos,max,"}");
    }
    at_app(buf,&pos,max,"]}");
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] db_query '%s'\n", query);
    return 0;
}

// ─── Tool: tier_promote ───────────────────────────────────────────────────────
static int tool_tier_promote(const char* args, char* buf, int max, uint32_t uid) {
    char name[OBJECT_NAME_LEN]={0};
    at_json_str(args, "name", name, OBJECT_NAME_LEN);
    if (!name[0]) return at_err(buf, max, "name required");
    if (!catalog_check_access(uid, name, PERM_WRITE))    // (F)
        return at_err(buf, max, "access denied");

    uint64_t rc = sys_sls_tier_promote(name);
    int pos=0;
    if (rc == 0) {
        const char* tier = "UNKNOWN";
        for (uint32_t i=0; i<object_catalog_count; i++) {
            if (object_catalog[i].active && at_streq(object_catalog[i].name, name)) {
                tier = tier_name(object_catalog[i].storage_tier); break;
            }
        }
        at_app(buf,&pos,max,"{\"ok\":true,\"tier\":\"");
        at_app(buf,&pos,max,tier);
        at_app(buf,&pos,max,"\"}");
    } else {
        at_app(buf,&pos,max,"{\"ok\":false,\"error\":\"promote failed\"}");
    }
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] tier_promote %s → %s\n",
                         name, rc==0?"ok":"fail");
    return (rc==0) ? 0 : -1;
}

// ─── Tool: stream_read ────────────────────────────────────────────────────────
static int tool_stream_read(const char* args, char* buf, int max, uint32_t uid) {
    char name    [OBJECT_NAME_LEN]={0};
    char mb_str  [16]             ={0};
    at_json_str(args, "name",      name,   OBJECT_NAME_LEN);
    at_json_str(args, "max_bytes", mb_str, sizeof(mb_str));
    if (!name[0]) return at_err(buf, max, "name required");
    if (!catalog_check_access(uid, name, PERM_READ))     // (F)
        return at_err(buf, max, "access denied");

    int limit = 256;
    if (mb_str[0]) {
        limit = 0;
        for (int i=0; mb_str[i]>='0'&&mb_str[i]<='9'; i++)
            limit = limit*10 + (mb_str[i]-'0');
    }
    if (limit > 512) limit = 512;

    struct StreamEntry* se = stream_find(name);
    if (!se) return at_err(buf, max, "stream not found");

    int pos=0;
    at_app(buf,&pos,max,"{\"ok\":true,\"data\":\"");
    int bytes=0;
    for (uint32_t fi=0; fi<se->frames_used && bytes<limit; fi++) {
        uint8_t* frame = se->frames[fi];
        if (!frame) continue;
        uint32_t fsz = (fi+1 < se->frames_used) ? 4096u
                     : (se->size%4096u ? se->size%4096u : 4096u);
        for (uint32_t b=0; b<fsz && bytes<limit; b++, bytes++) {
            unsigned char c = frame[b];
            if      (c=='"')  { at_app(buf,&pos,max,"\\\""); }
            else if (c=='\\') { at_app(buf,&pos,max,"\\\\"); }
            else if (c=='\n') { at_app(buf,&pos,max,"\\n");  }
            else if (c<0x20||c>0x7E) { /* skip */ }
            else              { at_char(buf,&pos,max,(char)c); }
        }
    }
    at_app(buf,&pos,max,"\",\"bytes\":");
    at_uint(buf,&pos,max,(uint32_t)bytes);
    at_app(buf,&pos,max,"}");
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] stream_read %s → %d bytes\n", name, bytes);
    return 0;
}

// ─── Tool: stream_write ───────────────────────────────────────────────────────
static int tool_stream_write(const char* args, char* buf, int max, uint32_t uid) {
    char name[OBJECT_NAME_LEN]={0};
    static char data_buf[512];
    data_buf[0]='\0';
    at_json_str(args, "name", name,     OBJECT_NAME_LEN);
    at_json_str(args, "data", data_buf, sizeof(data_buf));
    if (!name[0]||!data_buf[0]) return at_err(buf, max, "name and data required");
    if (!catalog_check_access(uid, name, PERM_WRITE))    // (F)
        return at_err(buf, max, "access denied");

    uint32_t dlen=0; while(data_buf[dlen]) dlen++;
    int rc = stream_write_chunk(name, (const uint8_t*)data_buf, dlen, 0, 1);
    int pos=0;
    if (rc==0) {
        at_app (buf,&pos,max,"{\"ok\":true,\"bytes\":");
        at_uint (buf,&pos,max,dlen);
        at_char (buf,&pos,max,'}');
    } else {
        at_app(buf,&pos,max,"{\"ok\":false,\"error\":\"write failed\"}");
    }
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] stream_write %s %u bytes → %s\n",
                         name, dlen, rc==0?"ok":"fail");
    return rc;
}

// ─── Tool: agent_run (C) ──────────────────────────────────────────────────────
#define SCHEMA_AGENT_RUN \
    "{\"type\":\"function\",\"function\":{\"name\":\"agent_run\"," \
    "\"description\":\"Invoke another AI agent with a message and get its answer.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"target agent name\"}," \
    "\"message\":{\"type\":\"string\",\"description\":\"message to send\"}}" \
    ",\"required\":[\"name\",\"message\"]}}}"

static int tool_agent_run(const char* args, char* buf, int max, uint32_t uid) {
    char name[OBJECT_NAME_LEN]={0};
    static char msg_buf[AGENT_PROMPT_LEN];
    msg_buf[0]='\0';
    at_json_str(args, "name",    name,    OBJECT_NAME_LEN);
    at_json_str(args, "message", msg_buf, AGENT_PROMPT_LEN);
    if (!name[0]||!msg_buf[0]) return at_err(buf, max, "name and message required");
    if (!catalog_check_access(uid, name, PERM_EXECUTE))  // (F)
        return at_err(buf, max, "access denied");
    struct AgentRunRequest ar;
    at_copy(ar.name,    name,    OBJECT_NAME_LEN);
    at_copy(ar.message, msg_buf, AGENT_PROMPT_LEN);
    // Note: recursive agent_run (A calls B calls A) will deadlock; avoid cycles.
    uint64_t rc = sys_sls_agent_run(&ar);
    int pos=0;
    if (rc == 0) {
        const char* ans = "";
        for (int i=0; i<AGENT_MAX; i++) {
            if (agent_table[i].active && at_streq(agent_table[i].name, name)) {
                ans = agent_table[i].last_answer; break;
            }
        }
        at_app (buf,&pos,max,"{\"ok\":true,\"answer\":\"");
        at_json (buf,&pos,max,ans);
        at_app (buf,&pos,max,"\"}");
    } else {
        at_app(buf,&pos,max,"{\"ok\":false,\"error\":\"sub-agent run failed\"}");
    }
    buf[pos]='\0';
    kernel_serial_printf("[TOOL] agent_run %s → %s\n", name, rc==0?"ok":"fail");
    return (rc==0)?0:-1;
}

// ─── Tool registry ────────────────────────────────────────────────────────────
struct AtTool {
    const char* name;
    uint32_t    flag;
    const char* schema_json;
    int (*fn)(const char* args, char* result, int max, uint32_t uid);
};

static const struct AtTool tool_registry[] = {
    { "db_select",    AGENT_TOOL_DB_SELECT,    SCHEMA_DB_SELECT,    tool_db_select    },
    { "db_insert",    AGENT_TOOL_DB_INSERT,    SCHEMA_DB_INSERT,    tool_db_insert    },
    { "db_query",     AGENT_TOOL_DB_QUERY,     SCHEMA_DB_QUERY,     tool_db_query     },
    { "tier_promote", AGENT_TOOL_TIER_PROMOTE, SCHEMA_TIER_PROMOTE, tool_tier_promote },
    { "stream_read",  AGENT_TOOL_STREAM_READ,  SCHEMA_STREAM_READ,  tool_stream_read  },
    { "stream_write", AGENT_TOOL_STREAM_WRITE, SCHEMA_STREAM_WRITE, tool_stream_write },
    { "agent_run",    AGENT_TOOL_AGENT_RUN,    SCHEMA_AGENT_RUN,    tool_agent_run    },  // (C)
};
#define AT_TOOL_COUNT ((int)(sizeof(tool_registry)/sizeof(tool_registry[0])))

// ─── agent_tools_build_schema ─────────────────────────────────────────────────
int agent_tools_build_schema(uint32_t tool_mask, char* buf, int max) {
    int pos=0;
    at_char(buf,&pos,max,'[');
    int first=1;
    for (int i=0; i<AT_TOOL_COUNT; i++) {
        if (!(tool_mask & tool_registry[i].flag)) continue;
        if (!first) at_char(buf,&pos,max,',');
        first=0;
        at_app(buf,&pos,max,tool_registry[i].schema_json);
    }
    at_char(buf,&pos,max,']');
    buf[pos]='\0';
    return pos;
}

// ─── agent_tools_execute ──────────────────────────────────────────────────────
int agent_tools_execute(const char* tool_name, const char* args_json,
                        char* result_buf, int result_max,
                        uint32_t tool_mask, uint32_t caller_uid) {
    for (int i=0; i<AT_TOOL_COUNT; i++) {
        if (!at_streq(tool_name, tool_registry[i].name)) continue;
        if (!(tool_mask & tool_registry[i].flag)) {
            return at_err(result_buf, result_max, "tool not permitted");
        }
        return tool_registry[i].fn(args_json, result_buf, result_max, caller_uid);
    }
    return at_err(result_buf, result_max, "unknown tool");
}
