#include <stdint.h>
#include "permissions.h"
#include "../kernel/syscall_dispatch.h"
#include "../kernel/object_catalog.h"
#include "../kernel/secure_api.h"
#include "../kernel/transaction.h"
#include "../kernel/microkernel.h"
#include "../kernel/ipc.h"
#include "../kernel/tier_mgr.h"
#include "../kernel/query_engine.h"
#include "../kernel/journal.h"
#include "../kernel/lock_mgr.h"
#include "../kernel/index_mgr.h"
#include "../kernel/constraint.h"
#include "../kernel/cursor.h"
#include "../kernel/aggregate.h"
#include "../kernel/mqt.h"
#include "../kernel/process.h"
#include "../kernel/loader.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"
#include "../kernel/agent.h"
#include "../kernel/sql_exec.h"
#include "../kernel/vecstore.h"   // Vector Store Roadmap Phase 4 -- pulls in ../net/ollama_client.h transitively
#include "../kernel/rowstore.h"   // Gap Remediation Phase B -- SYS_SLS_ROWSTORE_CREATE_TABLE
#include "../kernel/vec_join.h"   // Gap Remediation Phase C -- SYS_SLS_VEC_JOIN
#include "../kernel/vec_index.h"  // Gap Remediation Phase C -- SYS_SLS_VEC_INDEX_CREATE/SEARCH
#include "../kernel/partition.h"  // Gap Remediation Phase F -- SYS_SLS_PARTITION_CREATE/ASSIGN/LIST/DESTROY/PAUSE/RESUME
#include "../kernel/frame_pool.h" // Gap Remediation Phase F -- SYS_SLS_PARTITION_QUOTA_SET/QUOTA_LIST

// ─── Legacy allocation request (syscall 105) ─────────────────────────────────
struct SLSAllocationRequest {
    uint64_t system_object_id;
    uint64_t size_requested;
    uint32_t access_flags;
};

// ─── Session State ────────────────────────────────────────────────────────────
static uint32_t current_session_uid = 1000;
static uint32_t current_session_gid = 1000;
static uint64_t current_tx_id       = 0;    // 0 = no open transaction

// ─── String helpers (freestanding) ───────────────────────────────────────────
static int sh_starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

static size_t sh_len(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static int sh_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void sh_copy(char* d, const char* s, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

// Advance past current token (space-separated)
static const char* sh_next(const char* s) {
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

static uint32_t sh_atoi(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// ── Vector Store Roadmap Phase 4: shell parsing helpers ────────────────────
// No prior shell command needed to tokenize an unbounded, space-separated
// list of values (every existing command has a small, fixed argument
// count) -- the "vec insert"/"vec search" commands do (one value per
// vector dimension), so this is genuinely new machinery, not an oversight
// that a helper like this didn't already exist.

// Copies the current whitespace-delimited token from s into out (bounded to
// outlen, always NUL-terminated), then returns a pointer to the start of
// the NEXT token, skipping any run of spaces -- or a pointer to the
// terminating NUL if no more tokens remain.
static const char* sh_token(const char* s, char* out, size_t outlen) {
    size_t i = 0;
    while (*s && *s != ' ' && i < outlen - 1) out[i++] = *s++;
    out[i] = '\0';
    while (*s == ' ') s++;
    return s;
}

// Minimal freestanding string->float parser (no atof/strtof exists
// anywhere in this kernel -- confirmed by grep before writing this).
// Deliberately narrower than net/ollama_client.c's oc_parse_json_number():
// optional leading '-' and an optional decimal point, no exponent
// notation. That's a real, sufficient grammar for hand-typed shell input
// (a person is not going to type "4.5e-2" at a prompt) -- it is NOT
// sufficient for genuinely external JSON, which is exactly why
// oc_parse_json_number() is a separate, more complete implementation
// rather than this function being reused there.
// Gap Remediation (post-roadmap x86 boot-build fix): out-parameter, not a
// by-value float return -- the real x86-64 cross-build (-mno-sse) has no
// ABI path for returning a float; see kernel/vecstore.c's own header
// comment on the same fix applied there. Internal math is unchanged.
static void sh_atof(float* out, const char* s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    float v = 0.0f;
    while (*s >= '0' && *s <= '9') { v = v * 10.0f + (float)(*s - '0'); s++; }
    if (*s == '.') {
        s++;
        float frac = 0.1f;
        while (*s >= '0' && *s <= '9') { v += (float)(*s - '0') * frac; frac *= 0.1f; s++; }
    }
    *out = neg ? -v : v;
}

// ── Phase 22: row-printing callback for "sql" SELECTs ──────────────────────
// cursor_fetch_rows() hands back the cursor's FULL materialized row (column
// projection is metadata-only, per sql_exec.h -- see the roadmap's own
// Phase 19 findings), so this just prints every column of every fetched row.
static void sh_sql_print_row(struct RowId id, const struct RowValues* v, void* ctx) {
    (void)id; (void)ctx;
    for (uint32_t i = 0; i < v->count; i++) {
        kernel_serial_printf("%s%s", i ? ", " : "  ", v->values[i]);
    }
    kernel_serial_print("\n");
}

// ─── Shell Helpers ────────────────────────────────────────────────────────────
static void print_help(void) {
    kernel_serial_print(
        "\nSLS Shell Commands:\n"
        "  -- Session --\n"
        "  login <uid> <gid>              change session credentials\n"
        "  -- Object Catalog (Phase 1) --\n"
        "  valloc <name> <type> <pages>   allocate named persistent object\n"
        "                                   type: 0=SYSTEM_META 1=DB_TABLE\n"
        "                                         2=DB_INDEX    3=HEAP_BLOB\n"
        "  vfree  <name>                  release a named object\n"
        "  ls objects                     list all catalog entries\n"
        "  stat   <name>                  show full details for an object\n"
        "  -- DB Records (Phase 1) --\n"
        "  insert <object> <key> <value>  add a new record field\n"
        "  update <object> <key> <value>  modify a field (stages to WAL if tx open)\n"
        "  select <object> [<key>]        read one field, or all if key omitted\n"
        "  delete <object> <key>          remove a field (blocked if append-only)\n"
        "  -- Schema (Phase 6) --\n"
        "  schema set <obj> <key> <type>  define field type (STRING|UINT64|FLOAT|BOOL)\n"
        "  schema show <object>           dump schema + live values\n"
        "  -- Security (Phase 2) --\n"
        "  role set <uid> <role>          assign role (SYSTEM_KERNEL|DB_ADMIN|\n"
        "                                              APP_USER|GUEST)\n"
        "  grant  <uid> <object> <perm>   add perms (r|w|x|rw|rwx)\n"
        "  revoke <uid> <object> <perm>   remove perms\n"
        "  chmod  <name> <mask_hex>       set raw perm bitmask (legacy)\n"
        "  -- Transactions (Phase 3) --\n"
        "  tx begin                       open ACID transaction\n"
        "  tx commit                      commit staged writes to WAL\n"
        "  tx rollback                    discard staged writes\n"
        "  wal dump                       print WAL entries\n"
        "  wal recover                    replay WAL after simulated crash\n"
        "  -- Journaling (DB1 / IBM i STRJRNPF) --\n"
        "  journal create <name>            create a journal object\n"
        "  journal attach <jrn> <table>     start journaling a table\n"
        "  journal detach <jrn> <table>     stop journaling a table\n"
        "  journal list                     list all journals and attachments\n"
        "  journal dump <name>              show all entries for a journal\n"
        "  journal dump <name> <seq>        show entries since sequence number\n"
        "  journal purge <name>             remove rolled-back entries\n"
        "  -- Token Auth (Phase G) --\n"
        "  auth create <email> <uid> <role>  create a bearer token\n"
        "  auth list                         show token registry\n"
        "  auth revoke <email>               revoke all tokens for email\n"
        "  -- Web App Assets (Phase D) --\n"
        "  webapp set <obj> <path> <html>  store an asset (e.g. /hello.html)\n"
        "  webapp append <obj> <path> <s>  append content to an existing asset\n"
        "  webapp list [<obj>]             list all assets (use * for all)\n"
        "  -- Service Loader (Phase C) --\n"
        "  demo <name>                   load built-in AeroSLS test binary\n"
        "  upload <name> <hex>           write hex-encoded bytes to binary store\n"
        "  load <name>                   spawn process from uploaded binary\n"
        "  loader list                   show all binaries in the store\n"
        "  simi info <name>               structured SIMI header/entries/names/\n"
        "                                   activation-cache dump (Gap Remediation Phase G)\n"
        "  -- Process Isolation (Phase B) --\n"
        "  proc list                     show all Ring-3 processes\n"
        "  proc spawn <object>           create a process from SERVICE_PROCESS object\n"
        "  proc kill <pid>               terminate a running process\n"
        "  query <natural language text>  cognitive direct object scan\n"
        "  query scan                     export full catalog as JSON manifest\n"
        "  tier list                     show each object's current storage tier\n"
        "  tier promote <name>           pull object up one tier (L3->L2->L1)\n"
        "  tier demote  <name>           push object down one tier (L1->L2->L3)\n"
        "  -- Microkernel Bus (Phase 4) --\n"
        "  svc list                       show all service PIDs and states\n"
        "  svc crash <name>               inject a fault into a named service\n"
        "  svc restart <name>             restart a crashed service\n"
        "  ipc stat                       IPC queue depths and latency\n"
        "  ipc post <svc> <opcode_hex>    post a raw IPC message for testing\n"
        "  -- Row Locks (DB2 / Read-Committed isolation) --\n"
        "  lock list                      show all active row locks\n"
        "  -- Indexes (DB3 / keyed access path) --\n"
        "  index create <idx> <tbl> <fld>  build a sorted index on a field\n"
        "  index list                       show all indexes\n"
        "  index rebuild <name>             rescan table and rebuild\n"
        "  index drop <name>                remove an index\n"
        "  index scan <name> [<value>]      lookup / range scan via index\n"
        "  -- Constraints (DB4 / data integrity) --\n"
        "  constraint add <tbl> <fld> UNIQUE              no duplicate values\n"
        "  constraint add <tbl> <fld> NOT_NULL            reject empty values\n"
        "  constraint add <tbl> <fld> RANGE <min> <max>   numeric range check\n"
        "  constraint add <tbl> <fld> REFERENCE <reftbl>  FK integrity\n"
        "  constraint list [<table>]                       show constraints\n"
        "  constraint remove <tbl> <fld> <type>           drop a constraint\n"
        "  -- Cursors (DB5 / server-side iteration) --\n"
        "  cursor open <tbl> [where <fld>=<val>] [order <idx>]  open a cursor\n"
        "  cursor fetch <id> [<n>]                              fetch next N rows\n"
        "  cursor close <id>                                    close cursor\n"
        "  cursor list                                          list open cursors\n"
        "  -- Aggregates (DB6 / analytics) --\n"
        "  aggregate <tbl> COUNT [field] [where <f>=<v>] [group <f>] [having <n>]\n"
        "  aggregate <tbl> SUM|AVG|MIN|MAX <field> [where <f>=<v>] [order ASC|DESC]\n"
        "  select <tbl> [where <f>=<v>] [order <f> ASC|DESC]     ORDER BY query\n"
        "  -- Materialized Query Tables (DB7) --\n"
        "  mqt create <name> <base> COUNT|SUM|AVG|MIN|MAX [field] [group <f>]\n"
        "  mqt list                   show all MQTs\n"
        "  mqt refresh <name>         re-run query and update results\n"
        "  mqt drop <name>            remove MQT and result table\n"
        "  mqt scan <name>            show current MQT results\n"
        "  -- AI Agents (Phase H) --\n"
        "  agent create <name> <endpoint> <model>   create an agent (no tools)\n"
        "  agent run    <name> <message...>          run ReAct loop with message\n"
        "  agent list                                list all agents\n"
        "  agent status <name>                       show agent descriptor\n"
        "  agent kill   <name>                       stop and remove agent\n"
        "  agent schedule <name> <ticks> <msg...>    run every N service-poll ticks\n"
        "  agent unschedule <name>                   disable scheduled run\n"
        "  workflow create <name> <shared_tbl> <n>  define an n-step workflow\n"
        "  workflow addstep <name> <agent> <in> <out> append a step\n"
        "  workflow run    <name> <input...>         execute all steps\n"
        "  workflow list                             list all workflows\n"
        "  workflow status <name>                    show workflow descriptor\n"
        "  -- Row-Set Tables + SQL (Phases 16-22, Gap Remediation Phase B) --\n"
        "  table create <name>            promote a valloc'd+schema'd object to a row-set\n"
        "                                   table (run valloc + schema set on it first)\n"
        "  sql <statement>                 run one autocommit SQL statement\n"
        "  -- Vector Store (Phase 4) --\n"
        "  vec create <name> <dim>        register a vector collection (needs valloc first)\n"
        "  -- Vector Store (Phases 5-6, Gap Remediation Phase C) --\n"
        "  vec list                        list all vector collections\n"
        "  vec index list                  list all HNSW indexes\n"
        "  vec index create <idx> <coll> <cosine|l2>       define an HNSW index\n"
        "  vec index search <idx> <k> <ef> <query...>       approximate top-K search\n"
        "  vec join <coll> <table> <id_col> <cosine|l2> <k> <query...>\n"
        "                                   search then resolve matches to real rows\n"
        "  vec insert <name> <ext_id> <v0> <v1> ...       insert a raw vector\n"
        "  vec embed-insert <name> <ext_id> <model> <text...>  embed via local Ollama, then store\n"
        "  vec search <name> cosine|l2 <k> <v0> <v1> ...  top-k nearest neighbors\n"
        "  -- Partitions / LPAR (Phases 8/13/14, Gap Remediation Phase F) --\n"
        "  partition create <name>                 define a new partition\n"
        "  partition list                          list all defined partitions\n"
        "  partition assign <uid> <partition_id>   assign a uid to a partition\n"
        "  partition destroy <partition_id>        tear down a partition (kills its\n"
        "                                            processes, vfrees its objects)\n"
        "  partition pause <partition_id>           stop scheduling this partition\n"
        "  partition resume <partition_id>          resume scheduling this partition\n"
        "  partition quota <partition_id> <frames>  set a frame quota (0=unlimited)\n"
        "  partition quotas                         list per-partition usage/quota\n"
        "  write  <name> <payload>        direct heap write (no tx, legacy)\n"
        "  seal   <name> <password>      derive+store a password-based key for an\n"
        "                                   object (does NOT encrypt its data -- see\n"
        "                                   kernel/secure_api.c's own header comment)\n"
        "  help                           show this message\n\n");
}

// Parse "r", "w", "x", "rw", "rx", "rw", "rwx" → bitmask
static uint32_t parse_perm_string(const char* s) {
    uint32_t m = 0;
    while (*s) {
        if (*s == 'r') m |= PERM_READ;
        if (*s == 'w') m |= PERM_WRITE;
        if (*s == 'x') m |= PERM_EXECUTE;
        s++;
    }
    return m ? m : PERM_READ;
}

// Parse hex string (0x... or plain digits)
static uint32_t parse_hex(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    while (*s) {
        uint8_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) | d;
        s++;
    }
    return v;
}

// ─── Main Shell Loop ──────────────────────────────────────────────────────────
// File-scope callback for the 'index scan' shell command
static int shell_index_scan_cb(const char* k, const char* v) {
    kernel_serial_print("  key="); kernel_serial_print(k);
    kernel_serial_print("  val="); kernel_serial_print(v);
    kernel_serial_print("\n"); return 1;
}

// ─── Kernel-Side Shell Refactor (docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10) ──
// Pure string-in/string-out entry point into the same ~90-command dispatch
// that used to live only inside sls_shell_loop()'s while(1) body. Output is
// captured via kernel_serial_capture_start()/_stop() (kernel/kernel_io.c) --
// the single choke point every kernel_serial_print()/printf() call in the
// whole kernel already funnels through, so none of the 602 existing print
// call sites across 38 files needed to change, only this one function
// needed to exist. current_session_uid/current_session_gid/current_tx_id
// stay as file-scope statics, shared between the serial console and every
// call here -- one simulated machine, one live session, matching this
// project's existing single-DEMO_TOKEN, no-per-request-session model
// everywhere else (see §10.3 for the full rationale, not a new risk this
// introduces).
//
// Returns 1 if input matched a known command (the printed text is whatever
// that command produced -- this signals "recognized and ran," not "the
// underlying operation succeeded"; most commands only communicate real
// success/failure through their own printed text, exactly as they always
// have for a human reading the serial console). Returns 0 for an
// unrecognized non-empty command (out_buf holds the "Unknown command: ..."
// message) or for empty/whitespace input (out_buf is empty, matching the
// loop's own long-standing silent no-op for a blank line).
int sls_shell_execute(const char* input_buffer, char* out_buf, size_t out_cap) {
    int recognized = 1;
    kernel_serial_capture_start(out_buf, out_cap);

        // ── help ──────────────────────────────────────────────────────────────
        if (sh_eq(input_buffer, "help")) {
            print_help();
        }

        // ── login <uid> <gid> ─────────────────────────────────────────────────
        else if (sh_starts(input_buffer, "login ")) {
            const char* p = input_buffer + 6;
            uint32_t target_uid = sh_atoi(p);
            p = sh_next(p);
            uint32_t target_gid = sh_atoi(p);
            do_syscall(SYS_SLS_SET_USER,
                       (void*)((uint64_t)target_uid << 32 | target_gid));
            current_session_uid = target_uid;
            current_session_gid = target_gid;
            kernel_serial_printf(
                "Session credentials updated: uid=%u gid=%u role=%s\n",
                current_session_uid, current_session_gid,
                role_name(catalog_get_role(current_session_uid)));
        }

        // ── Phase 1: valloc <name> <type> <pages> ─────────────────────────────
        else if (sh_starts(input_buffer, "valloc ")) {
            const char* p = input_buffer + 7;
            struct SLSVallocRequest req;
            // extract name
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.name, p, nlen + 1 < OBJECT_NAME_LEN
                                 ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            req.type       = (SLSObjectType)sh_atoi(p);
            p = sh_next(p);
            req.size_pages = sh_atoi(p) ? sh_atoi(p) : 1;
            req.owner_uid  = current_session_uid;
            req.perm_mask  = PERM_RWX;
            req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
            do_syscall(SYS_SLS_VALLOC, &req);
        }

        // ── Phase 1: vfree <name> ─────────────────────────────────────────────
        else if (sh_starts(input_buffer, "vfree ")) {
            do_syscall(SYS_SLS_VFREE, (void*)(input_buffer + 6));
        }

        // ── Phase 1: ls objects ───────────────────────────────────────────────
        else if (sh_eq(input_buffer, "ls objects") ||
                 sh_eq(input_buffer, "ls")) {
            do_syscall(SYS_SLS_OBJ_LIST, 0);
        }

        // ── Phase 1: stat <name> ──────────────────────────────────────────────
        else if (sh_starts(input_buffer, "stat ")) {
            do_syscall(SYS_SLS_OBJ_STAT, (void*)(input_buffer + 5));
        }

        // ── Phase 1: insert <object> <key> <value> ────────────────────────────
        else if (sh_starts(input_buffer, "insert ")) {
            const char* p = input_buffer + 7;
            struct SLSRecordRequest req;
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.name, p, nlen + 1 < OBJECT_NAME_LEN
                                 ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            size_t klen = 0;
            while (p[klen] && p[klen] != ' ') klen++;
            sh_copy(req.key, p, klen + 1 < RECORD_KEY_LEN
                                ? klen + 1 : RECORD_KEY_LEN);
            p = sh_next(p);
            sh_copy(req.value, p, RECORD_VAL_LEN);
            do_syscall(SYS_SLS_INSERT, &req);
        }

        // ── Phase 1: update <object> <key> <value>  (WAL-aware since Phase 6) ────
        else if (sh_starts(input_buffer, "update ")) {
            const char* p = input_buffer + 7;
            struct SLSRecordRequest req;
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.name, p, nlen + 1 < OBJECT_NAME_LEN
                                 ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            size_t klen = 0;
            while (p[klen] && p[klen] != ' ') klen++;
            sh_copy(req.key, p, klen + 1 < RECORD_KEY_LEN
                                ? klen + 1 : RECORD_KEY_LEN);
            p = sh_next(p);
            sh_copy(req.value, p, RECORD_VAL_LEN);
            // WAL staging is handled kernel-side via tx_get_active()
            do_syscall(SYS_SLS_UPDATE, &req);
        }

        // ── Phase 1: select <object> [<key>]  (* or empty key = all fields) ──────
        else if (sh_starts(input_buffer, "select ")) {
            const char* p = input_buffer + 7;
            struct SLSRecordRequest req;
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.name, p, nlen + 1 < OBJECT_NAME_LEN
                                 ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            // If there is a trailing key, use it; otherwise pass empty string for "all"
            sh_copy(req.key, p, RECORD_KEY_LEN);
            req.value[0] = '\0';
            do_syscall(SYS_SLS_SELECT, &req);
        }

        // ── Phase 6: delete <object> <key> ───────────────────────────────────────
        else if (sh_starts(input_buffer, "delete ")) {
            const char* p = input_buffer + 7;
            struct SLSRecordRequest req;
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.name, p, nlen + 1 < OBJECT_NAME_LEN
                                 ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            sh_copy(req.key, p, RECORD_KEY_LEN);
            req.value[0] = '\0';
            do_syscall(SYS_SLS_DELETE, &req);
        }

        // ── Phase 6: schema set <object> <key> <type> ──────────────────────────
        else if (sh_starts(input_buffer, "schema set ")) {
            const char* p = input_buffer + 11;
            struct SLSSchemaRequest req;
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.object_name, p, nlen + 1 < OBJECT_NAME_LEN
                                        ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            size_t klen = 0;
            while (p[klen] && p[klen] != ' ') klen++;
            sh_copy(req.key, p, klen + 1 < RECORD_KEY_LEN
                                ? klen + 1 : RECORD_KEY_LEN);
            p = sh_next(p);
            if      (sh_eq(p, "UINT64") || sh_eq(p, "INT"))
                req.type = FIELD_TYPE_UINT64;
            else if (sh_eq(p, "FLOAT"))
                req.type = FIELD_TYPE_FLOAT;
            else if (sh_eq(p, "BOOL"))
                req.type = FIELD_TYPE_BOOL;
            else
                req.type = FIELD_TYPE_STRING;
            do_syscall(SYS_SLS_SCHEMA_SET, &req);
        }

        // ── Phase 6: schema show <object> ────────────────────────────────────
        else if (sh_starts(input_buffer, "schema show ")) {
            do_syscall(SYS_SLS_SCHEMA_SHOW, (void*)(input_buffer + 12));
        }

        // ── Phase 2: role set <uid> <role> ────────────────────────────────────
        else if (sh_starts(input_buffer, "role set ")) {
            const char* p = input_buffer + 9;
            struct SLSRoleRequest req;
            req.uid = sh_atoi(p);
            p = sh_next(p);
            if      (sh_eq(p, "SYSTEM_KERNEL")) req.role = ROLE_SYSTEM_KERNEL;
            else if (sh_eq(p, "DB_ADMIN"))      req.role = ROLE_DB_ADMIN;
            else if (sh_eq(p, "APP_USER"))      req.role = ROLE_APP_USER;
            else                                req.role = ROLE_GUEST;
            do_syscall(SYS_SLS_ROLE_SET, &req);
        }

        // ── Phase 2: grant <uid> <object> <perm> ─────────────────────────────
        else if (sh_starts(input_buffer, "grant ")) {
            const char* p = input_buffer + 6;
            struct SLSGrantRequest req;
            req.uid = sh_atoi(p);
            p = sh_next(p);
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.object_name, p,
                    nlen + 1 < OBJECT_NAME_LEN ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            req.perm_delta = parse_perm_string(p);
            // is_grant = 1
            uint64_t args[2] = { (uint64_t)(uintptr_t)&req, 1 };
            do_syscall(SYS_SLS_GRANT, args);
        }

        // ── Phase 2: revoke <uid> <object> <perm> ────────────────────────────
        else if (sh_starts(input_buffer, "revoke ")) {
            const char* p = input_buffer + 7;
            struct SLSGrantRequest req;
            req.uid = sh_atoi(p);
            p = sh_next(p);
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.object_name, p,
                    nlen + 1 < OBJECT_NAME_LEN ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            req.perm_delta = parse_perm_string(p);
            // is_grant = 0
            uint64_t args[2] = { (uint64_t)(uintptr_t)&req, 0 };
            do_syscall(SYS_SLS_GRANT, args);
        }

        // ── Phase 2: chmod <name> <mask_hex> (legacy) ────────────────────────
        else if (sh_starts(input_buffer, "chmod ")) {
            const char* p = input_buffer + 6;
            char name[OBJECT_NAME_LEN];
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(name, p, nlen + 1 < OBJECT_NAME_LEN
                             ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            uint64_t obj_id = 0;
            // find object_id for this name (inline lookup)
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    sh_eq(object_catalog[i].name, name)) {
                    obj_id = object_catalog[i].object_id;
                    break;
                }
            }
            uint32_t target_mask = parse_hex(p);
            uint64_t args[2] = { obj_id, target_mask };
            uint64_t status = do_syscall(SYS_SLS_CHMOD, args);
            if (status == 0) kernel_serial_print("Permissions matrix updated.\n");
            else kernel_serial_print(
                "Security Violation: Only the object owner can alter permissions.\n");
        }

        // ── Phase 3: tx begin ─────────────────────────────────────────────────
        else if (sh_eq(input_buffer, "tx begin")) {
            if (current_tx_id) {
                kernel_serial_printf(
                    "[TX] Transaction %lu already open.\n", current_tx_id);
            } else {
                current_tx_id = (uint64_t)do_syscall(
                    SYS_SLS_TX_BEGIN,
                    (void*)(uintptr_t)current_session_uid);
            }
        }

        // ── Phase 3: tx commit ────────────────────────────────────────────────
        else if (sh_eq(input_buffer, "tx commit")) {
            if (!current_tx_id) {
                kernel_serial_print("[TX] No open transaction.\n");
            } else {
                do_syscall(SYS_SLS_TX_COMMIT,
                           (void*)(uintptr_t)current_session_uid);
                current_tx_id = 0;
            }
        }

        // ── Phase 3: tx rollback ──────────────────────────────────────────────
        else if (sh_eq(input_buffer, "tx rollback")) {
            if (!current_tx_id) {
                kernel_serial_print("[TX] No open transaction.\n");
            } else {
                do_syscall(SYS_SLS_TX_ROLLBACK,
                           (void*)(uintptr_t)current_session_uid);
                current_tx_id = 0;
            }
        }

        // ── Phase 3: wal dump ─────────────────────────────────────────────────
        else if (sh_eq(input_buffer, "wal dump")) {
            wal_dump();
        }

        // ── Phase 22: sql <statement> ────────────────────────────────────────
        // The first real, syscall-reachable entry point into the Phases
        // 19-22 SQL engine (see docs/AeroSLS-RDBMS-Roadmap-v0.1.md §9) --
        // routes through the real SYS_SLS_SQL_EXECUTE syscall via
        // do_syscall(), not a direct function call, so this command
        // genuinely exercises the dispatch path this phase added, not just
        // the engine itself. Runs as one autocommit statement under
        // current_session_uid (matching every other command's uid source
        // in this file).
        else if (sh_starts(input_buffer, "sql ")) {
            struct SLSSqlRequest req;
            req.caller_uid = current_session_uid;
            sh_copy(req.sql_text, input_buffer + 4, sizeof(req.sql_text));
            uint64_t rc = do_syscall(SYS_SLS_SQL_EXECUTE, &req);
            if (rc != 0) {
                kernel_serial_printf("[SQL] error %d: %s\n",
                                     (int)req.result.error, req.result.error_msg);
            } else if (req.result.kind == SQL_STMT_SELECT) {
                kernel_serial_printf("[SQL] %u row(s)%s\n", req.result.row_count,
                                     req.result.truncated ? " (truncated)" : "");
                cursor_fetch_rows(req.result.cursor_id, req.result.row_count,
                                  sh_sql_print_row, 0);
                cursor_close(req.result.cursor_id);
            } else {
                kernel_serial_printf("[SQL] OK, %u row(s) affected\n", req.result.affected_rows);
            }
        }

        // ── Gap Remediation Phase B: table create <name> ─────────────────────
        // The live path rowstore_create_table() never had -- before this,
        // the only way to promote a valloc'd + schema'd object into a real
        // row-set table was a host test calling rowstore_create_table()
        // directly. Matches the "sql "/"vec create " blocks' own shape:
        // routes through the real SYS_SLS_ROWSTORE_CREATE_TABLE syscall via
        // do_syscall(), under current_session_uid. Does not valloc or
        // schema-set for you -- run "valloc <name> 1 <pages>" and one or
        // more "schema set <name> <col> <type>" first (see rowstore.h's own
        // comment on this syscall for why table creation isn't folded into
        // one combined command).
        else if (sh_starts(input_buffer, "table create ")) {
            struct SLSRowstoreCreateTableRequest req;
            req.caller_uid = current_session_uid;
            sh_copy(req.table_name, input_buffer + 13, sizeof(req.table_name));
            uint64_t rc = do_syscall(SYS_SLS_ROWSTORE_CREATE_TABLE, &req);
            kernel_serial_printf("[TABLE] create '%s' -> %s (status %d)\n",
                                 req.table_name, rc == 0 ? "OK" : "FAILED", req.status);
        }

        // ── Gap Remediation Phase F: partition create/list/assign/destroy/
        // pause/resume/quota — every one of these syscalls was already
        // correctly wired at the dispatch layer since Phase 8/13/14; this is
        // the first shell surface for any of them (mirrors "table create "'s
        // own shape immediately above). Prefix lengths below were computed
        // programmatically (Python len()), not hand-counted, after finding a
        // pre-existing hand-counting off-by-one in this same file's own
        // "vec index create "/"vec index search " blocks (offset 18 where
        // strlen() is actually 17) during this phase's own investigation --
        // named here rather than silently fixed elsewhere, since it's a
        // separate, pre-existing bug outside Phase F's own scope. ──────────
        else if (sh_starts(input_buffer, "partition create ")) {
            struct SLSPartitionCreateRequest req;
            sh_copy(req.name, input_buffer + 17, sizeof(req.name));
            uint64_t id = do_syscall(SYS_SLS_PARTITION_CREATE, &req);
            kernel_serial_printf("[PARTITION] create '%s' -> id=%llu%s\n",
                                 req.name, (unsigned long long)id,
                                 id == 0xFFFFFFFFu ? " (FAILED)" : "");
        }
        else if (sh_eq(input_buffer, "partition list")) {
            do_syscall(SYS_SLS_PARTITION_LIST, 0);
        }
        else if (sh_starts(input_buffer, "partition assign ")) {
            struct SLSPartitionAssignRequest req;
            const char* p = input_buffer + 17;
            char uidtok[16], pidtok[16];
            p = sh_token(p, uidtok, sizeof(uidtok));
            sh_token(p, pidtok, sizeof(pidtok));
            req.uid = sh_atoi(uidtok);
            req.partition_id = sh_atoi(pidtok);
            uint64_t rc = do_syscall(SYS_SLS_PARTITION_ASSIGN, &req);
            kernel_serial_printf("[PARTITION] assign uid=%u -> partition=%u -> %s\n",
                                 req.uid, req.partition_id, rc == 0 ? "OK" : "FAILED");
        }
        else if (sh_starts(input_buffer, "partition destroy ")) {
            uint32_t pid = sh_atoi(input_buffer + 18);
            uint64_t rc = do_syscall(SYS_SLS_PARTITION_DESTROY, (void*)(uintptr_t)pid);
            kernel_serial_printf("[PARTITION] destroy %u -> %s\n", pid, rc == 0 ? "OK" : "FAILED");
        }
        else if (sh_starts(input_buffer, "partition pause ")) {
            uint32_t pid = sh_atoi(input_buffer + 16);
            uint64_t rc = do_syscall(SYS_SLS_PARTITION_PAUSE, (void*)(uintptr_t)pid);
            kernel_serial_printf("[PARTITION] pause %u -> %s\n", pid, rc == 0 ? "OK" : "FAILED");
        }
        else if (sh_starts(input_buffer, "partition resume ")) {
            uint32_t pid = sh_atoi(input_buffer + 17);
            uint64_t rc = do_syscall(SYS_SLS_PARTITION_RESUME, (void*)(uintptr_t)pid);
            kernel_serial_printf("[PARTITION] resume %u -> %s\n", pid, rc == 0 ? "OK" : "FAILED");
        }
        else if (sh_starts(input_buffer, "partition quota ")) {
            struct SLSPartitionQuotaSetRequest req;
            const char* p = input_buffer + 16;
            char pidtok[16], qtok[24];
            p = sh_token(p, pidtok, sizeof(pidtok));
            sh_token(p, qtok, sizeof(qtok));
            req.partition_id = sh_atoi(pidtok);
            req.frame_quota  = (uint64_t)sh_atoi(qtok);
            uint64_t rc = do_syscall(SYS_SLS_PARTITION_QUOTA_SET, &req);
            kernel_serial_printf("[PARTITION] quota partition=%u frames=%llu -> %s\n",
                                 req.partition_id, (unsigned long long)req.frame_quota,
                                 rc == 0 ? "OK" : "FAILED");
        }
        else if (sh_eq(input_buffer, "partition quotas")) {
            do_syscall(SYS_SLS_PARTITION_QUOTA_LIST, 0);
        }

        // ── Vector Store Roadmap Phase 4: vec create/insert/embed-insert/
        // search ───────────────────────────────────────────────────────────
        // The first real, syscall-reachable entry points into the vector
        // store + Ollama embedding client (Phases 1-3), matching the "sql "
        // block immediately above -- each routes through a real
        // SYS_SLS_VEC_* syscall via do_syscall(), not a direct function
        // call, under current_session_uid (same uid source as every other
        // command in this file). "vec create" is the syscall this phase's
        // own roadmap scope bullet didn't originally name -- see vecstore.h's
        // header comment on why it was added anyway (without it, insert/
        // search are dispatch-reachable but never actually reachable, since
        // nothing else in this codebase can create a vector collection).
        //
        // embed-insert always targets a local Ollama instance
        // (127.0.0.1:11434) rather than taking an endpoint per call -- a
        // real, named first-cut simplification: this shell-demo surface has
        // no caller needing more than one Ollama instance yet, and adding
        // an endpoint argument to every "vec embed-insert" invocation would
        // be friction with no real use today.
        else if (sh_starts(input_buffer, "vec create ")) {
            struct SLSVecCreateRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 11;
            p = sh_token(p, req.collection_name, sizeof(req.collection_name));
            req.dimension = sh_atoi(p);
            uint64_t rc = do_syscall(SYS_SLS_VEC_CREATE, &req);
            kernel_serial_printf("[VEC] create '%s' dim=%u -> %s (status %d)\n",
                                 req.collection_name, req.dimension,
                                 rc == 0 ? "OK" : "FAILED", req.status);
        }
        else if (sh_starts(input_buffer, "vec insert ")) {
            struct SLSVecInsertRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 11;
            p = sh_token(p, req.collection_name, sizeof(req.collection_name));
            char idtok[32];
            p = sh_token(p, idtok, sizeof(idtok));
            req.external_id = (uint64_t)sh_atoi(idtok);
            uint32_t n = 0;
            while (*p && n < VECSTORE_MAX_DIMENSION) {
                char vtok[32];
                p = sh_token(p, vtok, sizeof(vtok));
                if (!vtok[0]) break;
                sh_atof(&req.values.values[n++], vtok);
            }
            req.values.count = n;
            uint64_t rc = do_syscall(SYS_SLS_VEC_INSERT, &req);
            if (rc == 0) {
                kernel_serial_printf("[VEC] inserted %u-dim vector into '%s', external_id=%llu -> page=%u slot=%u\n",
                                     n, req.collection_name, (unsigned long long)req.external_id,
                                     req.out_id.page_id, req.out_id.slot_index);
            } else {
                kernel_serial_printf("[VEC] insert failed, status %d\n", req.status);
            }
        }
        else if (sh_starts(input_buffer, "vec embed-insert ")) {
            struct SLSVecEmbedInsertRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 18;
            p = sh_token(p, req.collection_name, sizeof(req.collection_name));
            char idtok[32];
            p = sh_token(p, idtok, sizeof(idtok));
            req.external_id = (uint64_t)sh_atoi(idtok);
            p = sh_token(p, req.ollama_req.model, sizeof(req.ollama_req.model));
            sh_copy(req.ollama_req.endpoint_ip, "127.0.0.1", sizeof(req.ollama_req.endpoint_ip));
            req.ollama_req.port = 11434;
            sh_copy(req.ollama_req.prompt, p, sizeof(req.ollama_req.prompt));
            uint64_t rc = do_syscall(SYS_SLS_VEC_EMBED_INSERT, &req);
            if (rc == 0) {
                kernel_serial_printf("[VEC] embedded+inserted into '%s' via model '%s', external_id=%llu -> page=%u slot=%u\n",
                                     req.collection_name, req.ollama_req.model,
                                     (unsigned long long)req.external_id,
                                     req.out_id.page_id, req.out_id.slot_index);
            } else if (req.ollama_status != 0) {
                kernel_serial_printf("[VEC] embed-insert failed: Ollama call failed (status %d) -- is Ollama running at %s:%u?\n",
                                     req.ollama_status, req.ollama_req.endpoint_ip, req.ollama_req.port);
            } else {
                kernel_serial_printf("[VEC] embed-insert failed: embedding succeeded but insert failed, status %d\n",
                                     req.insert_status);
            }
        }
        else if (sh_starts(input_buffer, "vec search ")) {
            struct SLSVecSearchRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 11;
            p = sh_token(p, req.collection_name, sizeof(req.collection_name));
            char metrictok[16];
            p = sh_token(p, metrictok, sizeof(metrictok));
            req.metric = sh_eq(metrictok, "l2") ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
            char ktok[16];
            p = sh_token(p, ktok, sizeof(ktok));
            req.k = sh_atoi(ktok);
            uint32_t n = 0;
            while (*p && n < VECSTORE_MAX_DIMENSION) {
                char vtok[32];
                p = sh_token(p, vtok, sizeof(vtok));
                if (!vtok[0]) break;
                sh_atof(&req.query.values[n++], vtok);
            }
            req.query.count = n;
            do_syscall(SYS_SLS_VEC_SEARCH, &req);
            kernel_serial_printf("[VEC] search '%s' (%s, k=%u, query dim=%u) -> %u match(es)%s\n",
                                 req.collection_name, metrictok, req.k, n, req.match_count,
                                 req.truncated ? " (k truncated)" : "");
            for (uint32_t i = 0; i < req.match_count; i++) {
                int whole = (int)req.matches[i].distance;
                int frac  = (int)((req.matches[i].distance - (float)whole) * 1000.0f);
                if (frac < 0) frac = -frac;
                kernel_serial_printf("  #%u external_id=%llu distance=%d.%03d\n", i,
                                     (unsigned long long)req.matches[i].external_id, whole, frac);
            }
        }

        // ── Gap Remediation Phase C: vec list, vec index list ─────────────────
        // Live surfaces vector-collection/index enumeration never had at any
        // level -- a caller had to already know a name (docs/AeroSLS-Gap-
        // Analysis-v0.1.md §7). Mirrors "ls objects"'s own do_syscall(...,
        // 0) shape (no request struct needed for a pure list operation).
        else if (sh_eq(input_buffer, "vec list")) {
            do_syscall(SYS_SLS_VEC_LIST, 0);
        }
        else if (sh_eq(input_buffer, "vec index list")) {
            do_syscall(SYS_SLS_VEC_INDEX_LIST, 0);
        }

        // ── Gap Remediation Phase C: vec index create/search, vec join ───────
        // Live surfaces vec_index_create()/vec_index_search() (HNSW,
        // Vector Store Phase 6) and vec_join_resolve() (Phase 5) never had
        // -- both were plain host-testable kernel functions with zero
        // syscall/shell/HTTP reachability before this (see vec_index.h's
        // own point 6 and vec_join.h's own header comment, both of which
        // named this as a deliberate "revisit only when a real caller
        // needs it" cut). All three route through real syscalls via
        // do_syscall(), matching every other "vec "-prefixed command above.
        else if (sh_starts(input_buffer, "vec index create ")) {
            struct SLSVecIndexCreateRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 18;
            p = sh_token(p, req.index_name, sizeof(req.index_name));
            p = sh_token(p, req.collection_name, sizeof(req.collection_name));
            char metrictok[16];
            sh_token(p, metrictok, sizeof(metrictok));
            req.metric = sh_eq(metrictok, "l2") ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
            uint64_t rc = do_syscall(SYS_SLS_VEC_INDEX_CREATE, &req);
            kernel_serial_printf("[VEC] index create '%s' on '%s' (%s) -> %s (status %d)\n",
                                 req.index_name, req.collection_name, metrictok,
                                 rc == 0 ? "OK" : "FAILED", req.status);
        }
        else if (sh_starts(input_buffer, "vec index search ")) {
            struct SLSVecIndexSearchRequest req;
            req.caller_uid = current_session_uid;
            const char* p = input_buffer + 18;
            p = sh_token(p, req.index_name, sizeof(req.index_name));
            char ktok[16], eftok[16];
            p = sh_token(p, ktok, sizeof(ktok));
            req.k = sh_atoi(ktok);
            p = sh_token(p, eftok, sizeof(eftok));
            req.ef = sh_atoi(eftok);
            uint32_t n = 0;
            while (*p && n < VECSTORE_MAX_DIMENSION) {
                char vtok[32];
                p = sh_token(p, vtok, sizeof(vtok));
                if (!vtok[0]) break;
                sh_atof(&req.query.values[n++], vtok);
            }
            req.query.count = n;
            do_syscall(SYS_SLS_VEC_INDEX_SEARCH, &req);
            kernel_serial_printf("[VEC] index search '%s' (k=%u, ef=%u, query dim=%u) -> %u match(es)%s\n",
                                 req.index_name, req.k, req.ef, n, req.match_count,
                                 req.truncated ? " (k truncated)" : "");
            for (uint32_t i = 0; i < req.match_count; i++) {
                int whole = (int)req.matches[i].distance;
                int frac  = (int)((req.matches[i].distance - (float)whole) * 1000.0f);
                if (frac < 0) frac = -frac;
                kernel_serial_printf("  #%u external_id=%llu distance=%d.%03d\n", i,
                                     (unsigned long long)req.matches[i].external_id, whole, frac);
            }
        }
        else if (sh_starts(input_buffer, "vec join ")) {
            // Convenience: search, then join the results, in one command --
            // two real syscalls under the hood (SYS_SLS_VEC_SEARCH then
            // SYS_SLS_VEC_JOIN), not a shortcut around either. The syscall
            // itself (sys_sls_vec_join()) stays a pure join-already-computed-
            // matches primitive, matching vec_join_resolve()'s own real
            // contract -- this command is the ergonomic layer on top, the
            // same relationship "sql "/"vec search " have to their own
            // underlying engines.
            struct SLSVecSearchRequest sreq;
            sreq.caller_uid = current_session_uid;
            const char* p = input_buffer + 9;
            p = sh_token(p, sreq.collection_name, sizeof(sreq.collection_name));
            char table_name[OBJECT_NAME_LEN];
            p = sh_token(p, table_name, sizeof(table_name));
            char id_column[RECORD_KEY_LEN];
            p = sh_token(p, id_column, sizeof(id_column));
            char metrictok[16];
            p = sh_token(p, metrictok, sizeof(metrictok));
            sreq.metric = sh_eq(metrictok, "l2") ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
            char ktok[16];
            p = sh_token(p, ktok, sizeof(ktok));
            sreq.k = sh_atoi(ktok);
            uint32_t n = 0;
            while (*p && n < VECSTORE_MAX_DIMENSION) {
                char vtok[32];
                p = sh_token(p, vtok, sizeof(vtok));
                if (!vtok[0]) break;
                sh_atof(&sreq.query.values[n++], vtok);
            }
            sreq.query.count = n;
            do_syscall(SYS_SLS_VEC_SEARCH, &sreq);

            struct SLSVecJoinRequest jreq;
            jreq.caller_uid = current_session_uid;
            sh_copy(jreq.table_name, table_name, sizeof(jreq.table_name));
            sh_copy(jreq.id_column, id_column, sizeof(jreq.id_column));
            jreq.match_count = sreq.match_count < VEC_SEARCH_MAX_K ? sreq.match_count : VEC_SEARCH_MAX_K;
            for (uint32_t i = 0; i < jreq.match_count; i++) jreq.matches[i] = sreq.matches[i];
            do_syscall(SYS_SLS_VEC_JOIN, &jreq);

            kernel_serial_printf("[VEC] join '%s' -> '%s'.%s: %u search match(es) -> %u resolved row(s)%s\n",
                                 sreq.collection_name, table_name, id_column,
                                 sreq.match_count, jreq.result_count,
                                 jreq.truncated ? " (truncated)" : "");
            uint32_t nshown = jreq.result_count < VEC_JOIN_MAX_RESULTS ? jreq.result_count : VEC_JOIN_MAX_RESULTS;
            for (uint32_t i = 0; i < nshown; i++) {
                kernel_serial_printf("  external_id=%llu ->", (unsigned long long)jreq.results[i].match.external_id);
                for (uint32_t c = 0; c < jreq.results[i].row.count; c++)
                    kernel_serial_printf("%s%s", c ? ", " : " ", jreq.results[i].row.values[c]);
                kernel_serial_print("\n");
            }
        }

        // ── Phase 3: wal recover ──────────────────────────────────────────────
        else if (sh_eq(input_buffer, "wal recover")) {
            sys_sls_tx_recover();
        }
        // ── DB1: journal commands (IBM i-style journaling) ─────────────────────
        else if (sh_starts(input_buffer, "journal create ")) {
            const char* name = input_buffer + 15;
            struct SLSVallocRequest req;
            for (int i = 0; i < OBJECT_NAME_LEN; i++) req.name[i] = 0;
            for (int i = 0; name[i] && i < OBJECT_NAME_LEN - 1; i++) req.name[i] = name[i];
            req.type       = OBJ_TYPE_JOURNAL;
            req.size_pages = 1;
            req.owner_uid  = current_session_uid;
            req.perm_mask  = 0;
            req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
            sys_sls_valloc(&req);
            kernel_serial_printf("[JOURNAL] Created journal '%s'.\n", name);
        }
        else if (sh_starts(input_buffer, "journal attach ")) {
            // journal attach <journal_name> <table_name>
            const char* p = input_buffer + 15;
            char jname[64] = {0}, tname[64] = {0};
            int ji = 0;
            while (*p && *p != ' ' && ji < 63) jname[ji++] = *p++;
            while (*p == ' ') p++;
            int ti = 0;
            while (*p && ti < 63) tname[ti++] = *p++;
            journal_attach(jname, tname);
        }
        else if (sh_starts(input_buffer, "journal detach ")) {
            const char* p = input_buffer + 15;
            char jname[64] = {0}, tname[64] = {0};
            int ji = 0;
            while (*p && *p != ' ' && ji < 63) jname[ji++] = *p++;
            while (*p == ' ') p++;
            int ti = 0;
            while (*p && ti < 63) tname[ti++] = *p++;
            journal_detach(jname, tname);
        }
        else if (sh_eq(input_buffer, "journal list")) {
            kernel_serial_print("[JOURNAL] Attachments:\n");
            int found = 0;
            for (uint32_t i = 0; i < journal_attachment_count; i++) {
                if (journal_attachments[i].active) {
                    kernel_serial_printf("  %-24s  →  %s\n",
                        journal_attachments[i].journal_name,
                        journal_attachments[i].object_name);
                    found = 1;
                }
            }
            if (!found) kernel_serial_print("  (no journals active)\n");
        }
        else if (sh_starts(input_buffer, "journal dump ")) {
            const char* p = input_buffer + 13;
            char jname[64] = {0};
            int ji = 0;
            while (*p && *p != ' ' && ji < 63) jname[ji++] = *p++;
            while (*p == ' ') p++;
            uint64_t since = 0;
            while (*p >= '0' && *p <= '9') { since = since * 10 + (uint64_t)(*p - '0'); p++; }
            journal_dump(jname, since);
        }
        else if (sh_starts(input_buffer, "journal purge ")) {
            journal_purge(input_buffer + 14);
        }
        // ── Phase G: auth create <email> <uid> <role> ──────────────────────────
        else if (sh_starts(input_buffer, "auth create ")) {
            const char* p = input_buffer + 12;
            struct AuthCreateRequest req;
            size_t elen = 0;
            while (p[elen] && p[elen] != ' ') elen++;
            sh_copy(req.email, p, elen+1 < AUTH_EMAIL_LEN ? (int)(elen+1) : AUTH_EMAIL_LEN);
            p = sh_next(p);
            req.uid = sh_atoi(p);
            p = sh_next(p);
            if      (sh_eq(p, "SYSTEM_KERNEL")) req.role = ROLE_SYSTEM_KERNEL;
            else if (sh_eq(p, "DB_ADMIN"))      req.role = ROLE_DB_ADMIN;
            else if (sh_eq(p, "APP_USER"))      req.role = ROLE_APP_USER;
            else                                req.role = ROLE_GUEST;
            char tok[AUTH_TOKEN_LEN + 1];
            if (auth_create_token(&req, tok))
                kernel_serial_printf("[AUTH] Token: %s\n", tok);
        }

        // ── Phase G: auth list ───────────────────────────────────────────
        else if (sh_eq(input_buffer, "auth list")) {
            do_syscall(SYS_SLS_AUTH_LIST, 0);
        }

        // ── Phase G: auth revoke <email> ───────────────────────────────────
        else if (sh_starts(input_buffer, "auth revoke ")) {
            do_syscall(SYS_SLS_AUTH_REVOKE, (void*)(input_buffer + 12));
        }

        // ── Phase D: webapp set / append ──────────────────────────────────────
        else if (sh_starts(input_buffer, "webapp set ") ||
                 sh_starts(input_buffer, "webapp append ")) {
            int append = sh_starts(input_buffer, "webapp append ");
            const char* p = input_buffer + (append ? 14 : 11);
            struct WebAppSetRequest req;
            req.append = (uint8_t)append;
            // parse obj name
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.obj_name, p,
                    nlen+1 < OBJECT_NAME_LEN ? (int)(nlen+1) : OBJECT_NAME_LEN);
            p = sh_next(p);
            // parse path (URL)
            size_t plen = 0;
            while (p[plen] && p[plen] != ' ') plen++;
            sh_copy(req.path, p,
                    plen+1 < WEBAPP_PATH_LEN ? (int)(plen+1) : WEBAPP_PATH_LEN);
            p = sh_next(p);
            // rest of line = content
            size_t clen = 0;
            while (p[clen]) clen++;
            if (clen >= WEBAPP_CONTENT_LEN) clen = WEBAPP_CONTENT_LEN - 1;
            sh_copy(req.content, p, (int)(clen + 1));
            req.content_len = (uint32_t)clen;
            do_syscall(SYS_SLS_WEBAPP_SET, &req);
        }

        // ── Phase D: webapp list [<obj>] ───────────────────────────────────
        else if (sh_starts(input_buffer, "webapp list")) {
            const char* arg = sh_eq(input_buffer, "webapp list")
                              ? "*" : input_buffer + 12;
            do_syscall(SYS_SLS_WEBAPP_LIST, (void*)arg);
        }

        // ── Phase C: demo <name> ─────────────────────────────────────────────
        else if (sh_starts(input_buffer, "demo ")) {
            const char* name = input_buffer + 5;
            // Write the built-in demo binary to the store, then spawn
            struct SLSUploadRequest req;
            sh_copy(req.object_name, name, PROC_NAME_LEN);
            req.byte_offset = 0;
            req.chunk_len   = aerosls_demo_bin_size < UPLOAD_CHUNK_MAX
                              ? aerosls_demo_bin_size : UPLOAD_CHUNK_MAX;
            for (uint32_t i = 0; i < req.chunk_len; i++)
                req.chunk[i] = aerosls_demo_bin[i];
            req.is_last     = 1;
            do_syscall(SYS_SLS_UPLOAD_BINARY, &req);
            do_syscall(SYS_SLS_LOAD, (void*)name);
        }

        // ── Phase C: upload <name> <hex> ─────────────────────────────────────
        else if (sh_starts(input_buffer, "upload ")) {
            const char* p = input_buffer + 7;
            struct SLSUploadRequest req;
            // parse name
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(req.object_name, p,
                    nlen + 1 < PROC_NAME_LEN ? nlen + 1 : PROC_NAME_LEN);
            p = sh_next(p);
            // decode hex string into chunk[]
            req.byte_offset = 0;
            req.chunk_len   = 0;
            while (p[0] && p[1] && req.chunk_len < UPLOAD_CHUNK_MAX) {
                // each byte = two hex chars
                uint8_t hi = (uint8_t)(p[0] >= 'a' ? p[0]-'a'+10 :
                                       p[0] >= 'A' ? p[0]-'A'+10 : p[0]-'0');
                uint8_t lo = (uint8_t)(p[1] >= 'a' ? p[1]-'a'+10 :
                                       p[1] >= 'A' ? p[1]-'A'+10 : p[1]-'0');
                req.chunk[req.chunk_len++] = (uint8_t)((hi << 4) | lo);
                p += 2;
            }
            req.is_last = 1;
            do_syscall(SYS_SLS_UPLOAD_BINARY, &req);
        }

        // ── Phase C: load <name> ──────────────────────────────────────────────
        else if (sh_starts(input_buffer, "load ")) {
            do_syscall(SYS_SLS_LOAD, (void*)(input_buffer + 5));
        }

        // ── Phase C: loader list ───────────────────────────────────────────
        else if (sh_eq(input_buffer, "loader list")) {
            do_syscall(172, 0);
        }

        // ── Gap Remediation Phase G: simi info <name> ─────────────────────────
        // First shell surface for SYS_SLS_SIMI_INFO (173) -- previously
        // reachable only via loader_simi_info()'s own console dump, called
        // nowhere. Now struct-based (see loader.h): the syscall fills
        // req.result in place (single address space, no marshalling) and
        // this command formats it. Prefix offset via Python len(), not
        // hand-counted, per Phase F's own established practice.
        else if (sh_starts(input_buffer, "simi info ")) {
            struct SLSSimiInfoRequest req;
            sh_copy(req.object_name, input_buffer + 10, sizeof(req.object_name));
            do_syscall(SYS_SLS_SIMI_INFO, &req);
            struct SimiInfoResult* r = &req.result;
            if (r->status == SIMI_INFO_STATUS_NOT_FOUND) {
                kernel_serial_printf("[SIMI] '%s' not found\n", req.object_name);
            } else if (r->status == SIMI_INFO_STATUS_NOT_SIMI) {
                kernel_serial_printf("[SIMI] '%s' is not a SIMI object (format: %s)\n",
                                     req.object_name, r->format_name);
            } else if (r->status == SIMI_INFO_STATUS_CORRUPT) {
                kernel_serial_printf("[SIMI] '%s' has a corrupt SIMI header\n", req.object_name);
            } else {
                kernel_serial_printf("[SIMI] '%s': %u instr, %u literals, %u entries, %u names\n",
                                     req.object_name, r->num_instr, r->num_literals,
                                     r->num_entries, r->num_names);
                for (uint32_t i = 0; i < r->entries_returned; i++) {
                    kernel_serial_printf("  entry: %s @ %u\n",
                                         r->entries[i].name, r->entries[i].offset);
                }
                if (r->entries_truncated) {
                    kernel_serial_printf("  (%u more entries, truncated)\n",
                                         r->num_entries - r->entries_returned);
                }
                for (uint32_t i = 0; i < r->names_returned; i++) {
                    kernel_serial_printf("  name: %s\n", r->names[i].name);
                }
                if (r->names_truncated) {
                    kernel_serial_printf("  (%u more names, truncated)\n",
                                         r->num_names - r->names_returned);
                }
                if (r->activation.cached) {
                    kernel_serial_printf("  activation: cached, %u code pages, entry+%u, hash=%08x\n",
                                         r->activation.code_pages, r->activation.entry_offset,
                                         r->activation.content_hash);
                } else {
                    kernel_serial_printf("  activation: not cached\n");
                }
            }
        }

        // ── Phase B: proc list ───────────────────────────────────────────────
        else if (sh_eq(input_buffer, "proc list")) {
            do_syscall(SYS_SLS_PROC_LIST, 0);
        }

        // ── Phase B: proc spawn <object_name> ────────────────────────────────
        else if (sh_starts(input_buffer, "proc spawn ")) {
            struct ProcCreateRequest req;
            sh_copy(req.object_name, input_buffer + 11, PROC_NAME_LEN);
            req.owner_uid = current_session_uid;
            do_syscall(SYS_SLS_PROC_CREATE, &req);
        }

        // ── Phase B: proc kill <pid> ─────────────────────────────────────────
        else if (sh_starts(input_buffer, "proc kill ")) {
            uint32_t pid = sh_atoi(input_buffer + 10);
            do_syscall(SYS_SLS_PROC_KILL, (void*)(uintptr_t)pid);
        }

        // ── Phase 7: query scan (structured JSON manifest) ────────────────────
        else if (sh_eq(input_buffer, "query scan")) {
            do_syscall(SYS_SLS_QUERY_SCAN, 0);
        }

        // ── Phase 7: query <text> (natural language object scan) ──────────────
        else if (sh_starts(input_buffer, "query ")) {
            do_syscall(SYS_SLS_QUERY, (void*)(input_buffer + 6));
        }

        // ── Phase 5: tier list ──────────────────────────────────────────────
        else if (sh_eq(input_buffer, "tier list")) {
            do_syscall(SYS_SLS_TIER_LIST, 0);
        }

        // ── Phase 5: tier promote <name> ───────────────────────────────────────
        else if (sh_starts(input_buffer, "tier promote ")) {
            do_syscall(SYS_SLS_TIER_PROMOTE, (void*)(input_buffer + 13));
        }

        // ── Phase 5: tier demote <name> ───────────────────────────────────────
        else if (sh_starts(input_buffer, "tier demote ")) {
            do_syscall(SYS_SLS_TIER_DEMOTE, (void*)(input_buffer + 12));
        }

        // ── Phase 4: svc list ─────────────────────────────────────────────────
        else if (sh_eq(input_buffer, "svc list")) {
            do_syscall(SYS_SLS_SVC_LIST, 0);
        }

        // ── Phase 4: svc crash <name> ─────────────────────────────────────────
        else if (sh_starts(input_buffer, "svc crash ")) {
            do_syscall(SYS_SLS_SVC_CRASH, (void*)(input_buffer + 10));
        }

        // ── Phase 4: svc restart <name> ───────────────────────────────────────
        else if (sh_starts(input_buffer, "svc restart ")) {
            do_syscall(SYS_SLS_SVC_RESTART, (void*)(input_buffer + 12));
        }

        // ── Phase 4: ipc stat ─────────────────────────────────────────────────
        else if (sh_eq(input_buffer, "lock list")) {
            int found = 0;
            kernel_serial_print("[LOCK] Active row locks:\n");
            for (uint32_t i = 0; i < LOCK_MAX_ENTRIES; i++) {
                if (!lock_table[i].active) continue;
                kernel_serial_print("  tx=");
                kernel_serial_print_hex64(lock_table[i].tx_id);
                kernel_serial_print(" type=");
                kernel_serial_print(lock_table[i].type == (uint8_t)LOCK_EXCLUSIVE ? "X" : "S");
                kernel_serial_print(" key=");
                kernel_serial_print(lock_table[i].key[0] ? lock_table[i].key : "(obj)");
                kernel_serial_print("\n");
                found = 1;
            }
            if (!found) kernel_serial_print("  (no locks held)\n");
        }

        // ── DB3: index commands ───────────────────────────────────────────────
        else if (sh_starts(input_buffer, "index create ")) {
            // index create <idx_name> <table> <field>
            const char* p = input_buffer + 13;
            char iname[64], tname[64], fname[64];
            iname[0] = tname[0] = fname[0] = '\0';
            int ii = 0; while(*p && *p != ' ' && ii < 63) iname[ii++] = *p++;
            while(*p == ' ') p++;
            int ti = 0; while(*p && *p != ' ' && ti < 63) tname[ti++] = *p++;
            while(*p == ' ') p++;
            int fi = 0; while(*p && fi < 63) fname[fi++] = *p++;
            index_create(iname, tname, fname);
        }
        else if (sh_eq(input_buffer, "index list")) {
            int found = 0;
            for (int i = 0; i < INDEX_MAX; i++) {
                if (!index_store[i].active) continue;
                kernel_serial_print("  ");
                kernel_serial_print(index_store[i].index_name);
                kernel_serial_print("  on ");
                kernel_serial_print(index_store[i].table_name);
                kernel_serial_print(".");
                kernel_serial_print(index_store[i].field_name);
                kernel_serial_print("\n");
                found = 1;
            }
            if (!found) kernel_serial_print("  (no indexes)\n");
        }
        else if (sh_starts(input_buffer, "index rebuild ")) {
            index_rebuild(input_buffer + 14);
        }
        else if (sh_starts(input_buffer, "index drop ")) {
            index_drop(input_buffer + 11);
        }
        else if (sh_starts(input_buffer, "index scan ")) {
            // index scan <name> [<start_value>]
            const char* p = input_buffer + 11;
            char iname[64];
            iname[0] = '\0';
            int ii = 0; while(*p && *p != ' ' && ii < 63) iname[ii++] = *p++;
            while(*p == ' ') p++;
            kernel_serial_print("[INDEX] scan results:\n");
            index_range_scan(iname, *p ? p : "", shell_index_scan_cb);
        }

        // ── DB4: constraint commands ──────────────────────────────────────────
        else if (sh_starts(input_buffer, "constraint add ")) {
            const char* p = input_buffer + 15;
            char tbl[64], fld[64], typ[16];
            tbl[0] = fld[0] = typ[0] = '\0';
            int ti=0; while(*p&&*p!=' '&&ti<63) tbl[ti++]=*p++; while(*p==' ')p++;
            int fi=0; while(*p&&*p!=' '&&fi<63) fld[fi++]=*p++; while(*p==' ')p++;
            int yi=0; while(*p&&*p!=' '&&yi<15) typ[yi++]=*p++; while(*p==' ')p++;
            if      (!strcmp(typ, "UNIQUE"))    constraint_add_unique(tbl, fld);
            else if (!strcmp(typ, "NOT_NULL"))  constraint_add_not_null(tbl, fld);
            else if (!strcmp(typ, "REFERENCE")) constraint_add_reference(tbl, fld, p);
            else if (!strcmp(typ, "RANGE")) {
                char smin[20], smax[20]; smin[0]=smax[0]='\0';
                int mi=0; while(*p&&*p!=' '&&mi<19) smin[mi++]=*p++; while(*p==' ')p++;
                int xi=0; while(*p&&xi<19) smax[xi++]=*p++;
                int64_t mn=0,mx=0;
                const char*q=smin; int neg=(*q=='-'?q++,1:0);
                while(*q>='0'&&*q<='9'){mn=mn*10+(*q-'0');q++;} if(neg)mn=-mn;
                q=smax; neg=(*q=='-'?q++,1:0);
                while(*q>='0'&&*q<='9'){mx=mx*10+(*q-'0');q++;} if(neg)mx=-mx;
                constraint_add_range(tbl, fld, mn, mx);
            }
        }
        else if (sh_starts(input_buffer, "constraint list")) {
            const char* tbl = sh_eq(input_buffer, "constraint list")
                            ? "" : input_buffer + 16;
            int found = 0;
            static const char* const ctnames[] = {"UNIQUE","NOT_NULL","RANGE","REFERENCE"};
            for (int i = 0; i < CONSTRAINT_MAX; i++) {
                if (!constraint_table[i].active) continue;
                if (tbl[0] && !sh_eq(constraint_table[i].table_name, tbl)) continue;
                kernel_serial_print("  ");
                kernel_serial_print(constraint_table[i].table_name);
                kernel_serial_print(".");
                kernel_serial_print(constraint_table[i].field_name);
                kernel_serial_print(" ");
                kernel_serial_print(constraint_table[i].type <= 3
                                    ? ctnames[constraint_table[i].type] : "?");
                kernel_serial_print("\n");
                found = 1;
            }
            if (!found) kernel_serial_print("  (no constraints)\n");
        }
        else if (sh_starts(input_buffer, "constraint remove ")) {
            const char* p = input_buffer + 18;
            char tbl[64], fld[64], typ[16];
            tbl[0]=fld[0]=typ[0]='\0';
            int ti=0; while(*p&&*p!=' '&&ti<63) tbl[ti++]=*p++; while(*p==' ')p++;
            int fi=0; while(*p&&*p!=' '&&fi<63) fld[fi++]=*p++; while(*p==' ')p++;
            int yi=0; while(*p&&yi<15) typ[yi++]=*p++;
            int t=-1;
            if (!strcmp(typ,"UNIQUE"))    t=0;
            else if (!strcmp(typ,"NOT_NULL"))  t=1;
            else if (!strcmp(typ,"RANGE"))     t=2;
            else if (!strcmp(typ,"REFERENCE")) t=3;
            constraint_remove(tbl, fld, t);
        }

        // ── DB5: cursor commands ──────────────────────────────────────────────
        else if (sh_starts(input_buffer, "cursor open ")) {
            // cursor open <table> [where <field>=<value>] [order <index>]
            const char* p = input_buffer + 12;
            char tbl[64], wfld[64], wval[64], oidx[64];
            tbl[0] = wfld[0] = wval[0] = oidx[0] = '\0';
            int ti = 0; while (*p && *p != ' ' && ti < 63) tbl[ti++] = *p++;
            while (*p == ' ') p++;
            // optional: where <field>=<value>
            if (*p == 'w' && *(p+1)=='h' && *(p+2)=='e' && *(p+3)=='r' && *(p+4)=='e' && *(p+5)==' ') {
                p += 6;
                int fi = 0; while (*p && *p != '=' && fi < 63) wfld[fi++] = *p++;
                if (*p == '=') { p++; int vi = 0; while (*p && *p != ' ' && vi < 63) wval[vi++] = *p++; }
                while (*p == ' ') p++;
            }
            // optional: order <index_name>
            if (*p == 'o' && *(p+1)=='r' && *(p+2)=='d' && *(p+3)=='e' && *(p+4)=='r' && *(p+5)==' ') {
                p += 6;
                int oi = 0; while (*p && oi < 63) oidx[oi++] = *p++;
            }
            uint32_t cid = cursor_open(tbl, wfld, wval, oidx);
            if (cid) {
                kernel_serial_print("[CURSOR] id=");
                kernel_serial_print_hex64(cid);
                kernel_serial_print("\n");
            }
        }
        else if (sh_starts(input_buffer, "cursor fetch ")) {
            const char* p = input_buffer + 13;
            uint32_t cid = 0; while (*p >= '0' && *p <= '9') { cid = cid*10+(*p-'0'); p++; }
            while (*p == ' ') p++;
            uint32_t nrows = 5; if (*p) { nrows = 0; while (*p >= '0' && *p <= '9') { nrows = nrows*10+(*p-'0'); p++; } }
            if (!nrows) nrows = 5;
            static char cursor_fetch_buf[4096];
            cursor_fetch(cid, nrows, cursor_fetch_buf, (int)sizeof(cursor_fetch_buf));
            kernel_serial_print(cursor_fetch_buf);
            kernel_serial_print("\n");
        }
        else if (sh_starts(input_buffer, "cursor close ")) {
            uint32_t cid = 0;
            const char* p = input_buffer + 13;
            while (*p >= '0' && *p <= '9') { cid = cid*10+(*p-'0'); p++; }
            cursor_close(cid);
        }
        else if (sh_eq(input_buffer, "cursor list")) {
            int found = 0;
            for (int i = 0; i < CURSOR_MAX; i++) {
                if (!cursor_table[i].active) continue;
                kernel_serial_print("  #");
                kernel_serial_print_hex64(cursor_table[i].cursor_id);
                kernel_serial_print(" ");
                kernel_serial_print(cursor_table[i].table_name);
                if (cursor_table[i].where_field[0]) {
                    kernel_serial_print(" where ");
                    kernel_serial_print(cursor_table[i].where_field);
                    if (cursor_table[i].where_value[0]) {
                        kernel_serial_print("=");
                        kernel_serial_print(cursor_table[i].where_value);
                    }
                }
                kernel_serial_print(cursor_table[i].done ? " DONE\n" : "\n");
                found = 1;
            }
            if (!found) kernel_serial_print("  (no open cursors)\n");
        }

        // ── DB6: aggregate + ORDER BY commands ───────────────────────────────
        else if (sh_starts(input_buffer, "aggregate ") ||
                 sh_starts(input_buffer, "select ")) {
            // aggregate <tbl> COUNT|SUM|AVG|MIN|MAX [field] [where <f>=<v>] [group <f>] [having <n>] [order ASC|DESC]
            // select    <tbl> [where <f>=<v>] [order <f> ASC|DESC]
            int is_select = sh_starts(input_buffer, "select ");
            const char* p = input_buffer + (is_select ? 7 : 10);
            struct AggQuery q;
            q.table[0]=q.agg_field[0]=q.where_field[0]=q.where_eq[0]='\0';
            q.group_field[0]=q.order_field[0]='\0';
            q.having_min_count=0; q.order_desc=0;
            q.fn = (uint8_t)AGG_NONE;

            // parse table
            int ti=0; while(*p&&*p!=' '&&ti<OBJECT_NAME_LEN-1) q.table[ti++]=*p++; q.table[ti]='\0';
            while(*p==' ')p++;

            if (!is_select) {
                // parse fn
                char fn_s[8]; int fi=0; while(*p&&*p!=' '&&fi<7) fn_s[fi++]=*p++; fn_s[fi]='\0';
                while(*p==' ')p++;
                if      (!strcmp(fn_s,"COUNT")) q.fn=(uint8_t)AGG_COUNT;
                else if (!strcmp(fn_s,"SUM"))   q.fn=(uint8_t)AGG_SUM;
                else if (!strcmp(fn_s,"AVG"))   q.fn=(uint8_t)AGG_AVG;
                else if (!strcmp(fn_s,"MIN"))   q.fn=(uint8_t)AGG_MIN;
                else if (!strcmp(fn_s,"MAX"))   q.fn=(uint8_t)AGG_MAX;
                // parse optional field
                if (*p && *p!='w' && *p!='g' && *p!='h' && *p!='o') {
                    int ai=0; while(*p&&*p!=' '&&ai<RECORD_KEY_LEN-1) q.agg_field[ai++]=*p++; q.agg_field[ai]='\0';
                    while(*p==' ')p++;
                }
            }

            // parse optional clauses: where/group/having/order
            while (*p) {
                if (*p=='w'&&*(p+1)=='h'&&*(p+2)=='e'&&*(p+3)=='r'&&*(p+4)=='e'&&*(p+5)==' ') {
                    p+=6; int wi=0; while(*p&&*p!='='&&wi<RECORD_KEY_LEN-1) q.where_field[wi++]=*p++; q.where_field[wi]='\0';
                    if(*p=='='){p++;int vi=0;while(*p&&*p!=' '&&vi<RECORD_VAL_LEN-1) q.where_eq[vi++]=*p++; q.where_eq[vi]='\0';}
                    while(*p==' ')p++;
                } else if (*p=='g'&&*(p+1)=='r'&&*(p+2)=='o'&&*(p+3)=='u'&&*(p+4)=='p'&&*(p+5)==' ') {
                    p+=6; int gi=0; while(*p&&*p!=' '&&gi<RECORD_KEY_LEN-1) q.group_field[gi++]=*p++; q.group_field[gi]='\0';
                    while(*p==' ')p++;
                } else if (*p=='h'&&*(p+1)=='a'&&*(p+2)=='v'&&*(p+3)=='i'&&*(p+4)=='n'&&*(p+5)=='g'&&*(p+6)==' ') {
                    p+=7; int64_t hv=0; while(*p>='0'&&*p<='9'){hv=hv*10+(*p-'0');p++;}
                    q.having_min_count=hv; while(*p==' ')p++;
                } else if (*p=='o'&&*(p+1)=='r'&&*(p+2)=='d'&&*(p+3)=='e'&&*(p+4)=='r'&&*(p+5)==' ') {
                    p+=6;
                    // for 'select': order <field> ASC|DESC
                    if (is_select || q.fn==(uint8_t)AGG_NONE) {
                        int oi=0; while(*p&&*p!=' '&&oi<RECORD_KEY_LEN-1) q.order_field[oi++]=*p++; q.order_field[oi]='\0';
                        while(*p==' ')p++;
                    }
                    if (*p=='D'||*p=='d') q.order_desc=1;
                    while(*p&&*p!=' ')p++; while(*p==' ')p++;
                } else { while(*p&&*p!=' ')p++; while(*p==' ')p++; }
            }

            static char agg_result[8192];
            aggregate_exec(&q, agg_result, (int)sizeof(agg_result));
            kernel_serial_print(agg_result);
            kernel_serial_print("\n");
        }

        // ── DB7: MQT commands ─────────────────────────────────────────────────
        else if (sh_starts(input_buffer, "mqt create ")) {
            // mqt create <name> <base> COUNT|SUM|AVG|MIN|MAX [field] [group <f>] [where <f>=<v>]
            const char* p = input_buffer + 11;
            char mname[64], btable[64], fn_s[16], afld[64], gfld[64], wfld[64], weq[64];
            mname[0]=btable[0]=fn_s[0]=afld[0]=gfld[0]=wfld[0]=weq[0]='\0';
            int n=0; while(*p&&*p!=' '&&n<63) mname[n++]=*p++;  while(*p==' ')p++;
            n=0;     while(*p&&*p!=' '&&n<63) btable[n++]=*p++; while(*p==' ')p++;
            n=0;     while(*p&&*p!=' '&&n<15) fn_s[n++]=*p++;   while(*p==' ')p++;
            // optional: field then group/where
            while (*p) {
                if (*p=='g'&&*(p+1)=='r'&&*(p+2)=='o'&&*(p+3)=='u'&&*(p+4)=='p'&&*(p+5)==' ') {
                    p+=6; int gi=0; while(*p&&*p!=' '&&gi<63) gfld[gi++]=*p++; while(*p==' ')p++;
                } else if (*p=='w'&&*(p+1)=='h'&&*(p+2)=='e'&&*(p+3)=='r'&&*(p+4)=='e'&&*(p+5)==' ') {
                    p+=6; int wi=0; while(*p&&*p!='='&&wi<63) wfld[wi++]=*p++;
                    if(*p=='='){p++;int vi=0;while(*p&&*p!=' '&&vi<63) weq[vi++]=*p++;}
                    while(*p==' ')p++;
                } else {
                    int ai=0; while(*p&&*p!=' '&&ai<63) afld[ai++]=*p++; while(*p==' ')p++;
                }
            }
            uint8_t fn=(uint8_t)AGG_COUNT;
            if(!strcmp(fn_s,"SUM"))fn=(uint8_t)AGG_SUM; else if(!strcmp(fn_s,"AVG"))fn=(uint8_t)AGG_AVG;
            else if(!strcmp(fn_s,"MIN"))fn=(uint8_t)AGG_MIN; else if(!strcmp(fn_s,"MAX"))fn=(uint8_t)AGG_MAX;
            mqt_create(mname, btable, fn, afld, wfld, weq, gfld);
        }
        else if (sh_eq(input_buffer, "mqt list")) {
            int found = 0;
            static const char* fn_names[] = {"COUNT","SUM","AVG","MIN","MAX"};
            for (int i = 0; i < MQT_MAX; i++) {
                if (!mqt_table[i].active) continue;
                kernel_serial_print("  "); kernel_serial_print(mqt_table[i].mqt_name);
                kernel_serial_print(" <- "); kernel_serial_print(mqt_table[i].base_table);
                kernel_serial_print(" "); kernel_serial_print(mqt_table[i].fn<=4?fn_names[mqt_table[i].fn]:"?");
                kernel_serial_print("\n"); found = 1;
            }
            if (!found) kernel_serial_print("  (no MQTs)\n");
        }
        else if (sh_starts(input_buffer, "mqt refresh ")) {
            mqt_refresh(input_buffer + 12);
        }
        else if (sh_starts(input_buffer, "mqt drop ")) {
            mqt_drop(input_buffer + 9);
        }
        else if (sh_starts(input_buffer, "mqt scan ")) {
            // Use select to show MQT result table
            const char* mname = input_buffer + 9;
            struct SLSRecordRequest req;
            for (int i = 0; i < OBJECT_NAME_LEN; i++) req.name[i] = 0;
            for (int i = 0; mname[i] && i < OBJECT_NAME_LEN-1; i++) req.name[i] = mname[i];
            req.key[0] = '\0';
            sys_sls_obj_stat(req.name);
        }

        else if (sh_eq(input_buffer, "ipc stat")) {
            do_syscall(SYS_SLS_IPC_STAT, 0);
        }

        // ── Phase 4: ipc post <svc_name> <opcode_hex> ────────────────────────
        else if (sh_starts(input_buffer, "ipc post ")) {
            const char* p = input_buffer + 9;
            // Find target service by name to resolve its port
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            char svc_name[SVC_NAME_LEN];
            sh_copy(svc_name, p, nlen + 1 < SVC_NAME_LEN
                                 ? nlen + 1 : SVC_NAME_LEN);
            p = sh_next(p);
            uint32_t opcode = parse_hex(p);

            uint16_t target_port = 0;
            for (uint32_t i = 0; i < service_count; i++) {
                if (sh_eq(services[i].name, svc_name)) {
                    target_port = services[i].port;
                    break;
                }
            }
            if (!target_port) {
                kernel_serial_printf(
                    "ipc post: Service '%s' not found.\n", svc_name);
            } else {
                struct IPCPostRequest req;
                req.msg.src_port    = 0x0000;
                req.msg.dst_port    = target_port;
                req.msg.opcode      = opcode;
                req.msg.payload[0]  = 0;
                req.msg.payload[1]  = 0;
                req.msg.payload[2]  = 0;
                req.msg.payload[3]  = 0;
                req.msg.reply_token = 0;
                do_syscall(SYS_SLS_IPC_POST, &req);
                kernel_serial_printf(
                    "ipc post: opcode=0x%04x -> %s (port 0x%04x)\n",
                    opcode, svc_name, target_port);
            }
        }

        // ── Phase H: AI Agents ────────────────────────────────────────────────
        else if (sh_starts(input_buffer, "agent create ")) {
            const char* p = input_buffer + 13;
            struct AgentCreateRequest req;
            req.name[0]=req.inference_endpoint[0]=req.model[0]=req.system_prompt[0]='\0';
            req.tool_mask=0; req.owner_uid=current_session_uid;
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(req.name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            size_t elen=0; while(p[elen]&&p[elen]!=' ') elen++;
            sh_copy(req.inference_endpoint, p, elen+1<AGENT_ENDPOINT_LEN?elen+1:AGENT_ENDPOINT_LEN);
            p=sh_next(p);
            sh_copy(req.model, p, AGENT_MODEL_LEN);
            do_syscall(SYS_SLS_AGENT_CREATE, &req);
        }
        else if (sh_starts(input_buffer, "agent run ")) {
            const char* p = input_buffer + 10;
            struct AgentRunRequest req; req.name[0]=req.message[0]='\0';
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(req.name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            sh_copy(req.message, p, AGENT_PROMPT_LEN);
            do_syscall(SYS_SLS_AGENT_RUN, &req);
        }
        else if (sh_eq(input_buffer, "agent list")) {
            do_syscall(SYS_SLS_AGENT_LIST, 0);
        }
        else if (sh_starts(input_buffer, "agent status ")) {
            do_syscall(SYS_SLS_AGENT_STATUS, (void*)(input_buffer + 13));
        }
        else if (sh_starts(input_buffer, "agent kill ")) {
            do_syscall(SYS_SLS_AGENT_KILL, (void*)(input_buffer + 11));
        }
        else if (sh_starts(input_buffer, "agent schedule ")) {
            // agent schedule <name> <ticks> <message...>
            const char* p = input_buffer + 15;
            struct AgentScheduleRequest sr;
            sr.name[0]=sr.message[0]='\0'; sr.ticks=0;
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(sr.name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            sr.ticks=(uint32_t)sh_atoi(p);
            p=sh_next(p);
            sh_copy(sr.message, p, sizeof(sr.message));
            do_syscall(SYS_SLS_AGENT_SCHEDULE, &sr);
        }
        else if (sh_starts(input_buffer, "agent unschedule ")) {
            struct AgentScheduleRequest sr;
            sh_copy(sr.name, input_buffer+17, OBJECT_NAME_LEN);
            sr.message[0]='\0'; sr.ticks=0;
            do_syscall(SYS_SLS_AGENT_SCHEDULE, &sr);
        }

        // ── Phase H: Workflows ────────────────────────────────────────────────
        else if (sh_starts(input_buffer, "workflow create ")) {
            // workflow create <name> <shared_table> <step_count>
            const char* p = input_buffer + 16;
            struct WorkflowCreateRequest req;
            req.name[0]=req.shared_state_table[0]='\0';
            req.step_count=0; req.owner_uid=current_session_uid;
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(req.name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            size_t tlen=0; while(p[tlen]&&p[tlen]!=' ') tlen++;
            sh_copy(req.shared_state_table, p, tlen+1<OBJECT_NAME_LEN?tlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            req.step_count=(uint8_t)(sh_atoi(p)<WORKFLOW_MAX_STEPS?sh_atoi(p):WORKFLOW_MAX_STEPS);
            do_syscall(SYS_SLS_WORKFLOW_CREATE, &req);
        }
        else if (sh_starts(input_buffer, "workflow addstep ")) {
            // workflow addstep <wf_name> <agent_name> <input_key> <output_key>
            const char* p = input_buffer + 17;
            char wf_name[OBJECT_NAME_LEN]={0};
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(wf_name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            // Find the workflow and append a step
            for (int wi=0; wi<WORKFLOW_MAX; wi++) {
                if (!workflow_table[wi].active) continue;
                int m=1;
                for (int k=0; workflow_table[wi].name[k]||wf_name[k]; k++)
                    if (workflow_table[wi].name[k]!=wf_name[k]){m=0;break;}
                if (!m) continue;
                uint8_t s=workflow_table[wi].step_count;
                if (s>=WORKFLOW_MAX_STEPS) { kernel_serial_printf("[WF] step table full\n"); break; }
                size_t alen=0; while(p[alen]&&p[alen]!=' ') alen++;
                sh_copy(workflow_table[wi].steps[s].agent_name, p,
                        alen+1<OBJECT_NAME_LEN?alen+1:OBJECT_NAME_LEN);
                p=sh_next(p);
                size_t ilen=0; while(p[ilen]&&p[ilen]!=' ') ilen++;
                sh_copy(workflow_table[wi].steps[s].input_key, p,
                        ilen+1<RECORD_KEY_LEN?ilen+1:RECORD_KEY_LEN);
                p=sh_next(p);
                sh_copy(workflow_table[wi].steps[s].output_key, p, RECORD_KEY_LEN);
                workflow_table[wi].step_count++;
                kernel_serial_printf("[WF] '%s' step %u: agent=%s in=%s out=%s\n",
                    wf_name, s,
                    workflow_table[wi].steps[s].agent_name,
                    workflow_table[wi].steps[s].input_key,
                    workflow_table[wi].steps[s].output_key);
                break;
            }
        }
        else if (sh_starts(input_buffer, "workflow run ")) {
            const char* p = input_buffer + 13;
            struct WorkflowRunRequest req; req.name[0]=req.input[0]='\0';
            size_t nlen=0; while(p[nlen]&&p[nlen]!=' ') nlen++;
            sh_copy(req.name, p, nlen+1<OBJECT_NAME_LEN?nlen+1:OBJECT_NAME_LEN);
            p=sh_next(p);
            sh_copy(req.input, p, AGENT_PROMPT_LEN);
            do_syscall(SYS_SLS_WORKFLOW_RUN, &req);
        }
        else if (sh_eq(input_buffer, "workflow list")) {
            sys_sls_workflow_list();
        }
        else if (sh_starts(input_buffer, "workflow status ")) {
            do_syscall(SYS_SLS_WORKFLOW_STATUS, (void*)(input_buffer + 16));
        }

        // ── Legacy: write <name> <payload> ───────────────────────────────────
        else if (sh_starts(input_buffer, "write ")) {
            const char* p = input_buffer + 6;
            char name[OBJECT_NAME_LEN];
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(name, p, nlen + 1 < OBJECT_NAME_LEN
                             ? nlen + 1 : OBJECT_NAME_LEN);
            uint64_t obj_id = 0;
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    sh_eq(object_catalog[i].name, name)) {
                    obj_id = object_catalog[i].object_id;
                    break;
                }
            }
            struct SLSAllocationRequest req = {
                .system_object_id = obj_id,
                .size_requested   = 4096,
                .access_flags     = PERM_WRITE
            };
            char* ptr = (char*)do_syscall(SYS_SLS_ALLOCATE, &req);
            if (ptr) {
                const char* payload = sh_next(p);
                size_t i = 0;
                while (payload[i] && i < 4095) { ptr[i] = payload[i]; i++; }
                ptr[i] = '\0';
                kernel_serial_print("Direct memory mutation verified.\n");
            } else {
                kernel_serial_print("Access Denied: UID/GID lacks clearance.\n");
            }
        }

        // ── seal <name> <password> ───────────────────────────────────────────
        // Kernel-Side Shell Refactor (docs/AeroSLS-Web-Terminal-Plan-v0.1.md
        // §10.1): was an interactive two-line "Enter encryption password: "
        // prompt (a second read_line() call) -- the only command in this
        // entire file that ever read more than the one line it was given,
        // which is incompatible with sls_shell_execute()'s single-line
        // string-in/string-out contract below. The password is now the
        // second token on the same line, for both the serial console and
        // the web terminal (one shared function now services both, so the
        // two entry points can't diverge). Costs nothing security-wise:
        // kernel/secure_api.c's own header comment already states this
        // command does NOT encrypt the object's data.
        else if (sh_starts(input_buffer, "seal ")) {
            const char* p = input_buffer + 5;
            char name[OBJECT_NAME_LEN];
            size_t nlen = 0;
            while (p[nlen] && p[nlen] != ' ') nlen++;
            sh_copy(name, p, nlen + 1 < OBJECT_NAME_LEN
                             ? nlen + 1 : OBJECT_NAME_LEN);
            p = sh_next(p);
            char pw[32];
            sh_copy(pw, p, sizeof(pw));
            uint64_t obj_id = 0;
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    sh_eq(object_catalog[i].name, name)) {
                    obj_id = object_catalog[i].object_id;
                    break;
                }
            }
            if (!obj_id) {
                kernel_serial_printf("seal: Object '%s' not found.\n", name);
            } else if (!pw[0]) {
                kernel_serial_print("seal: password required (usage: seal <name> <password>).\n");
            } else {
                struct SLSSealRequest sreq;
                sreq.system_object_id = obj_id;
                size_t pwlen = sh_len(pw);
                for (size_t i = 0; i < 32 && i < pwlen; i++)
                    sreq.user_password[i] = pw[i];
                sreq.password_len = (uint32_t)(pwlen < 32 ? pwlen : 31);
                sreq.encryption_algorithm_flags = 1;
                do_syscall(SYS_SLS_SECURE_SEAL, &sreq);
            }
        }

        else if (input_buffer[0] != '\0') {
            kernel_serial_printf(
                "Unknown command: '%s'. Type 'help' for usage.\n",
                input_buffer);
            recognized = 0;
        }
        else {
            recognized = 0;
        }

    kernel_serial_capture_stop();
    return recognized;
}

// SHELL_EXEC_OUT_CAP must match the constant of the same name in
// net/http.c's api_shell_exec_post() -- no shared header exists for this
// one value (matching sls_shell_loop()'s own long-standing "just an extern
// declaration at the call site" convention, not a new header for one
// function), so both copies carry this cross-reference instead.
#define SHELL_EXEC_OUT_CAP 8192

void sls_shell_loop(void) {
    char input_buffer[256];
    static char output_buffer[SHELL_EXEC_OUT_CAP];
    kernel_serial_print("\n--- Multi-User SLS Secure Shell Active ---\n");
    kernel_serial_print("Type 'help' for available commands.\n\n");

    while (1) {
        if (current_tx_id)
            kernel_serial_printf("uid:%u[tx:%lu]> ", current_session_uid,
                                 current_tx_id);
        else
            kernel_serial_printf("uid:%u> ", current_session_uid);

        read_line(input_buffer);
        sls_shell_execute(input_buffer, output_buffer, sizeof(output_buffer));
        kernel_serial_print(output_buffer);
    }
}