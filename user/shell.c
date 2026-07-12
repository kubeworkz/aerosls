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
#include "../kernel/process.h"
#include "../kernel/loader.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"

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
        "  write  <name> <payload>        direct heap write (no tx, legacy)\n"
        "  seal   <name>                  encrypt object with password\n"
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
void sls_shell_loop(void) {
    char input_buffer[256];
    kernel_serial_print("\n--- Multi-User SLS Secure Shell Active ---\n");
    kernel_serial_print("Type 'help' for available commands.\n\n");

    while (1) {
        if (current_tx_id)
            kernel_serial_printf("uid:%u[tx:%lu]> ", current_session_uid,
                                 current_tx_id);
        else
            kernel_serial_printf("uid:%u> ", current_session_uid);

        read_line(input_buffer);

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

        // ── seal <name> ───────────────────────────────────────────────────────
        else if (sh_starts(input_buffer, "seal ")) {
            kernel_serial_print("Enter encryption password: ");
            char pw[32];
            read_line(pw);
            uint64_t obj_id = 0;
            const char* name = input_buffer + 5;
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    sh_eq(object_catalog[i].name, name)) {
                    obj_id = object_catalog[i].object_id;
                    break;
                }
            }
            if (!obj_id) {
                kernel_serial_printf("seal: Object '%s' not found.\n", name);
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
        }
    }
}