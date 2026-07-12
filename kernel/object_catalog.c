#include "object_catalog.h"
#include "journal.h"
#include "lock_mgr.h"
#include "../user/permissions.h"

// Forward declaration — avoids pulling the full tier_mgr.h include graph into this file
extern void tier_notify_access(uint64_t object_id);

// Forward declarations for transaction integration (Phase 3/6)
extern uint64_t tx_get_active(uint32_t thread_id);
extern uint64_t wal_stage(uint32_t thread_id, uint64_t object_id,
                           const char* key,
                           const char* old_value, const char* new_value);
extern uint32_t kernel_get_current_thread_id(void);

// ─── Globals ──────────────────────────────────────────────────────────────────
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;

// Next virtual address for named object allocation.
// Starts above the kernel-private range and below the raw SLS break at 0x700...
#define OBJ_VADDR_BASE   0x0001000000000000ULL
#define PAGE_SIZE_BYTES  4096ULL

static uint64_t next_obj_vaddr = OBJ_VADDR_BASE;

// ─── Internal String Helpers ──────────────────────────────────────────────────
static size_t cat_strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int cat_streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void cat_strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ─── FNV-1a hash (matches generate_unique_object_id in lockfree_map) ──────────
static uint64_t fnv1a(const char* s, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

// ─── Catalog Lookup ───────────────────────────────────────────────────────────
static int find_by_name(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && cat_streq(object_catalog[i].name, name))
            return (int)i;
    }
    return -1;
}

// ─── Role Helpers ─────────────────────────────────────────────────────────────
SLSRole catalog_get_role(uint32_t uid) {
    // UID 0 is always kernel
    if (uid == 0) return ROLE_SYSTEM_KERNEL;
    for (int i = 0; i < ROLE_TABLE_MAX; i++) {
        if (role_table[i].active && role_table[i].uid == uid)
            return role_table[i].role;
    }
    return ROLE_GUEST; // default role
}

// Returns 1 if uid is allowed the requested permission on the named object
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    SLSRole role = catalog_get_role(uid);

    // Kernel always passes
    if (role == ROLE_SYSTEM_KERNEL) return 1;

    int idx = find_by_name(obj_name);
    if (idx < 0) return 0;

    struct SLSObjectEntry* e = &object_catalog[idx];

    // Owner always has full access to their own object
    if (e->owner_uid == uid) return 1;

    // DB_ADMIN: full access to DB_TABLE and DB_INDEX
    if (role == ROLE_DB_ADMIN) {
        if (e->type == OBJ_TYPE_DB_TABLE || e->type == OBJ_TYPE_DB_INDEX)
            return 1;
    }

    // APP_USER: read-only on DB_TABLE
    if (role == ROLE_APP_USER) {
        if (e->type == OBJ_TYPE_DB_TABLE && (needed_perm & PERM_READ))
            return !(needed_perm & PERM_WRITE) && !(needed_perm & PERM_EXECUTE);
    }

    // GUEST: read-only on HEAP_BLOB only
    if (role == ROLE_GUEST) {
        if (e->type == OBJ_TYPE_HEAP_BLOB && needed_perm == PERM_READ)
            return 1;
        return 0;
    }

    // Fall back to stored per-object perm_mask
    return (e->perm_mask & needed_perm) == needed_perm;
}

// ─── Phase 1: valloc ─────────────────────────────────────────────────────────
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) {
    if (!req || req->name[0] == '\0' || req->size_pages == 0)
        return 0;

    uint64_t obj_id = fnv1a(req->name, cat_strlen(req->name));

    // Reject duplicates
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && object_catalog[i].object_id == obj_id) {
            kernel_serial_printf("[CATALOG] ERROR: Object '%s' already exists.\n",
                                 req->name);
            return 0;
        }
    }

    if (object_catalog_count >= CATALOG_MAX_OBJECTS) {
        kernel_serial_print("[CATALOG] ERROR: Catalog capacity exceeded.\n");
        return 0;
    }

    uint32_t slot = object_catalog_count;

    // Assign the next available virtual address range
    uint64_t vaddr = next_obj_vaddr;
    next_obj_vaddr += (uint64_t)req->size_pages * PAGE_SIZE_BYTES;

    struct SLSObjectEntry* e = &object_catalog[slot];
    e->object_id    = obj_id;
    cat_strncpy(e->name, req->name, OBJECT_NAME_LEN);
    e->type         = req->type;
    e->base_vaddr   = vaddr;
    e->size_pages   = req->size_pages;
    e->owner_uid    = req->owner_uid;
    // Assign initial storage tier based on object type
    switch (req->type) {
        case OBJ_TYPE_SYSTEM_METADATA: e->storage_tier = STORAGE_TIER_L1_CACHE; break;
        case OBJ_TYPE_DB_TABLE:        e->storage_tier = STORAGE_TIER_L2_DRAM;  break;
        case OBJ_TYPE_DB_INDEX:        e->storage_tier = STORAGE_TIER_L2_DRAM;  break;
        default:                       e->storage_tier = STORAGE_TIER_L3_SSD;   break;
    }
    e->owner_role   = catalog_get_role(req->owner_uid);
    e->perm_mask    = req->perm_mask ? req->perm_mask
                                     : (PERM_READ | PERM_WRITE | PERM_OWNER);
    e->active       = 1;

    // Initialise an empty record store for this slot
    object_records[slot].object_id   = obj_id;
    object_records[slot].field_count = 0;

    object_catalog_count++;

    // Safe print: avoid mixed-type variadic printf which crashes on freestanding x86.
    kernel_serial_print("[CATALOG] valloc: '");
    kernel_serial_print(e->name);
    kernel_serial_print("' type=");
    kernel_serial_print(obj_type_name(e->type));
    kernel_serial_print(" tier=");
    kernel_serial_print(tier_name(e->storage_tier));
    kernel_serial_print("\n");

    return obj_id;
}

// ─── Phase 1: vfree ───────────────────────────────────────────────────────────
uint64_t sys_sls_vfree(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) {
        kernel_serial_printf("[CATALOG] ERROR: Object '%s' not found.\n", name);
        return 1;
    }
    object_catalog[idx].active = 0;
    object_records[idx].field_count = 0;
    kernel_serial_printf("[CATALOG] vfree: '%s' released from address space.\n",
                         name);
    return 0;
}

// ─── Phase 1: ls objects ──────────────────────────────────────────────────────
void sys_sls_obj_list(void) {
    kernel_serial_print(
        "\n[CATALOG] Object Directory\n"
        " %-20s %-18s %-10s %-8s %s\n"
        " %-20s %-18s %-10s %-8s %s\n",
        "Name", "Virtual Address", "Type", "Pages", "Tier",
        "--------------------", "------------------",
        "----------", "--------", "----------");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        struct SLSObjectEntry* e = &object_catalog[i];
        kernel_serial_printf(
            " %-20s 0x%016lx %-10s %-8u %s\n",
            e->name, e->base_vaddr,
            obj_type_name(e->type), e->size_pages,
            tier_name(e->storage_tier));
        shown++;
    }
    if (shown == 0)
        kernel_serial_print(" (no objects allocated)\n");
    kernel_serial_printf(" %u object(s) total.\n\n", shown);
}

// ─── Phase 1: stat ────────────────────────────────────────────────────────────
uint64_t sys_sls_obj_stat(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) {
        kernel_serial_printf("[CATALOG] stat: Object '%s' not found.\n", name);
        return 1;
    }
    struct SLSObjectEntry* e = &object_catalog[idx];
    kernel_serial_printf(
        "\n[STAT] %s\n"
        "  Object ID  : 0x%016lx\n"
        "  Type       : %s\n"
        "  Base VAddr : 0x%016lx\n"
        "  Size       : %u page(s) (%u KB)\n"
        "  Owner UID  : %u  (Role: %s)\n"
        "  Tier       : %s\n"
        "  Perm Mask  : 0x%02x\n"
        "  Records    : %u field(s)\n\n",
        e->name,
        e->object_id,
        obj_type_name(e->type),
        e->base_vaddr,
        e->size_pages, e->size_pages * 4,
        e->owner_uid, role_name(e->owner_role),
        tier_name(e->storage_tier),
        e->perm_mask,
        object_records[idx].field_count);
    return 0;
}

// ─── Phase 2: role set ────────────────────────────────────────────────────────
uint64_t sys_sls_role_set(struct SLSRoleRequest* req) {
    if (!req) return 1;

    // Find existing entry for uid
    for (int i = 0; i < ROLE_TABLE_MAX; i++) {
        if (role_table[i].active && role_table[i].uid == req->uid) {
            role_table[i].role = req->role;
            kernel_serial_printf("[SECURITY] UID %u role updated to %s.\n",
                                 req->uid, role_name(req->role));
            return 0;
        }
    }
    // Find a free slot
    for (int i = 0; i < ROLE_TABLE_MAX; i++) {
        if (!role_table[i].active) {
            role_table[i].uid    = req->uid;
            role_table[i].role   = req->role;
            role_table[i].active = 1;
            kernel_serial_printf("[SECURITY] UID %u assigned role %s.\n",
                                 req->uid, role_name(req->role));
            return 0;
        }
    }
    kernel_serial_print("[SECURITY] ERROR: Role table full.\n");
    return 1;
}

// ─── Phase 2: grant / revoke ──────────────────────────────────────────────────
uint64_t sys_sls_grant(struct SLSGrantRequest* req, int is_grant) {
    if (!req) return 1;
    int idx = find_by_name(req->object_name);
    if (idx < 0) {
        kernel_serial_printf("[SECURITY] Object '%s' not found.\n",
                             req->object_name);
        return 1;
    }
    if (is_grant)
        object_catalog[idx].perm_mask |= req->perm_delta;
    else
        object_catalog[idx].perm_mask &= ~req->perm_delta;

    kernel_serial_printf(
        "[SECURITY] %s uid=%u obj='%s' perms=0x%02x (mask now 0x%02x)\n",
        is_grant ? "GRANT" : "REVOKE",
        req->uid, req->object_name, req->perm_delta,
        object_catalog[idx].perm_mask);
    return 0;
}

// ─── Phase 1/6: select ────────────────────────────────────────────────────────
// If req->key is empty or "*", dump all fields (full record view).
uint64_t sys_sls_select(struct SLSRecordRequest* req) {
    int obj_idx = find_by_name(req->name);
    if (obj_idx < 0) {
        kernel_serial_printf("[DB] SELECT: Object '%s' not found.\n", req->name);
        return 1;
    }
    tier_notify_access(object_catalog[obj_idx].object_id);

    // Empty key or "*" → dump every active field
    if (req->key[0] == '\0' || (req->key[0] == '*' && req->key[1] == '\0')) {
        struct SLSObjectRecord* rec = &object_records[obj_idx];
        if (rec->field_count == 0) {
            kernel_serial_printf("[DB] %s: (no records)\n", req->name);
            return 0;
        }
        kernel_serial_printf("[DB] %s — %u field(s):\n", req->name, rec->field_count);
        for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
            if (!rec->fields[i].active) continue;
            // Resolve schema type for display
            const char* tname = "STRING";
            for (uint32_t j = 0; j < SCHEMA_MAX_FIELDS; j++) {
                if (object_schemas[obj_idx].fields[j].active &&
                    cat_streq(object_schemas[obj_idx].fields[j].key,
                              rec->fields[i].key)) {
                    tname = field_type_name(object_schemas[obj_idx].fields[j].type);
                    break;
                }
            }
            kernel_serial_printf("  %-32s [%-6s] = %s\n",
                                 rec->fields[i].key, tname, rec->fields[i].value);
        }
        return 0;
    }

    // Specific key lookup
    struct SLSObjectRecord* rec = &object_records[obj_idx];
    for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
        if (rec->fields[i].active && cat_streq(rec->fields[i].key, req->key)) {
            kernel_serial_printf("[DB] %s.%s = %s\n",
                                 req->name, req->key, rec->fields[i].value);
            return 0;
        }
    }
    kernel_serial_printf("[DB] SELECT: Key '%s' not found in '%s'.\n",
                         req->key, req->name);
    return 1;
}

// ─── Phase 1/6: update (WAL-aware) ───────────────────────────────────────────
uint64_t sys_sls_update(struct SLSRecordRequest* req) {
    int obj_idx = find_by_name(req->name);
    if (obj_idx < 0) {
        kernel_serial_printf("[DB] UPDATE: Object '%s' not found.\n", req->name);
        return 1;
    }
    tier_notify_access(object_catalog[obj_idx].object_id);

    struct SLSObjectRecord* rec = &object_records[obj_idx];
    for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
        if (rec->fields[i].active && cat_streq(rec->fields[i].key, req->key)) {

            // If an ACID transaction is open on the calling thread, stage to WAL.
            // The commit path closes ctx->active before calling us, so this won't
            // re-stage during tx commit replay.
            uint32_t tid = kernel_get_current_thread_id();
            uint64_t tx  = tx_get_active(tid);
            if (tx) {
                // Acquire exclusive row lock before staging to WAL.
                // Rejects the write if another transaction holds the key.
                if (lock_acquire(tx, object_catalog[obj_idx].object_id,
                                 req->key, LOCK_EXCLUSIVE) != 0) {
                    kernel_serial_print("[DB] UPDATE blocked: lock conflict\n");
                    return 2;  // conflict — caller should rollback
                }
                wal_stage(tid, object_catalog[obj_idx].object_id,
                          req->key, rec->fields[i].value, req->value);
                // Journal: UB (before-image) staged; UP committed on tx commit
                journal_write(req->name, req->key,
                              rec->fields[i].value, req->value,
                              JENT_UB, tx);
                kernel_serial_printf(
                    "[DB] UPDATE %s.%s staged -> tx=%lu  "
                    "(commit to apply)\n",
                    req->name, req->key, tx);
                return 0;
            }

            // Direct write — no open transaction
            // Journal: write UB + UP immediately (auto-commit, no tx)
            journal_write(req->name, req->key,
                          rec->fields[i].value, req->value,
                          JENT_UB, 0);
            cat_strncpy(rec->fields[i].value, req->value, RECORD_VAL_LEN);
            journal_write(req->name, req->key,
                          "", req->value, JENT_UP, 0);
            kernel_serial_printf("[DB] UPDATE %s.%s = %s  [DIRECT]\n",
                                 req->name, req->key, req->value);
            return 0;
        }
    }
    kernel_serial_printf("[DB] UPDATE: Key '%s' not found. Use 'insert' first.\n",
                         req->key);
    return 1;
}

// ─── Phase 1/6: insert (WAL-aware) ───────────────────────────────────────────
uint64_t sys_sls_insert(struct SLSRecordRequest* req) {
    int obj_idx = find_by_name(req->name);
    if (obj_idx < 0) {
        kernel_serial_printf("[DB] INSERT: Object '%s' not found.\n", req->name);
        return 1;
    }
    tier_notify_access(object_catalog[obj_idx].object_id);

    struct SLSObjectRecord* rec = &object_records[obj_idx];
    if (rec->field_count >= RECORD_MAX_FIELDS) {
        kernel_serial_print("[DB] INSERT: Record capacity full.\n");
        return 1;
    }
    // Reject duplicate keys
    for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
        if (rec->fields[i].active && cat_streq(rec->fields[i].key, req->key)) {
            kernel_serial_printf(
                "[DB] INSERT: Key '%s' already exists. Use 'update'.\n",
                req->key);
            return 1;
        }
    }

    // If tx open, stage as an insert (old_value = "" signals a new field)
    uint32_t tid = kernel_get_current_thread_id();
    uint64_t tx  = tx_get_active(tid);
    if (tx) {
        // Acquire exclusive lock on the new key
        if (lock_acquire(tx, object_catalog[obj_idx].object_id,
                         req->key, LOCK_EXCLUSIVE) != 0) {
            kernel_serial_print("[DB] INSERT blocked: lock conflict\n");
            return 2;
        }
        wal_stage(tid, object_catalog[obj_idx].object_id,
                  req->key, "", req->value);
        journal_write(req->name, req->key, "", req->value, JENT_PT, tx);
        kernel_serial_printf(
            "[DB] INSERT %s.%s staged -> tx=%lu  (commit to apply)\n",
            req->name, req->key, tx);
        return 0;
    }

    // Direct insert
    for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
        if (!rec->fields[i].active) {
            cat_strncpy(rec->fields[i].key,   req->key,   RECORD_KEY_LEN);
            cat_strncpy(rec->fields[i].value, req->value, RECORD_VAL_LEN);
            rec->fields[i].active = 1;
            rec->field_count++;
            journal_write(req->name, req->key, "", req->value, JENT_PT, 0);
            kernel_serial_printf("[DB] INSERT %s.%s = %s  [DIRECT]\n",
                                 req->name, req->key, req->value);
            return 0;
        }
    }
    return 1;
}

// ─── Phase 6: delete ──────────────────────────────────────────────────────────
uint64_t sys_sls_delete(struct SLSRecordRequest* req) {
    int obj_idx = find_by_name(req->name);
    if (obj_idx < 0) {
        kernel_serial_printf("[DB] DELETE: Object '%s' not found.\n", req->name);
        return 1;
    }
    struct SLSObjectEntry* e = &object_catalog[obj_idx];

    // Honour the append-only flag — deletion is not permitted
    if (e->perm_mask & FLAG_APPEND_ONLY) {
        kernel_serial_printf(
            "[DB] DELETE denied: '%s' is marked FLAG_APPEND_ONLY.\n",
            req->name);
        return 1;
    }

    struct SLSObjectRecord* rec = &object_records[obj_idx];
    for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
        if (!rec->fields[i].active) continue;
        if (!cat_streq(rec->fields[i].key, req->key)) continue;

        // Stage to WAL if tx is open (old_value saved; new_value = "" marks deletion)
        uint32_t tid = kernel_get_current_thread_id();
        uint64_t tx  = tx_get_active(tid);
        if (tx) {
            if (lock_acquire(tx, e->object_id,
                             req->key, LOCK_EXCLUSIVE) != 0) {
                kernel_serial_print("[DB] DELETE blocked: lock conflict\n");
                return 2;
            }
            wal_stage(tid, e->object_id,
                      req->key, rec->fields[i].value, "");
            journal_write(req->name, req->key,
                          rec->fields[i].value, "", JENT_DL, tx);
            kernel_serial_printf(
                "[DB] DELETE %s.%s staged -> tx=%lu\n",
                req->name, req->key, tx);
            return 0;
        }

        rec->fields[i].active = 0;
        rec->field_count--;
        tier_notify_access(e->object_id);
        kernel_serial_printf("[DB] DELETE %s.%s  [OK]\n", req->name, req->key);
        return 0;
    }
    kernel_serial_printf("[DB] DELETE: Key '%s' not found in '%s'.\n",
                         req->key, req->name);
    return 1;
}

// ─── Phase 6: schema set ──────────────────────────────────────────────────────
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) {
    if (!req) return 1;
    int obj_idx = find_by_name(req->object_name);
    if (obj_idx < 0) {
        kernel_serial_printf("[SCHEMA] Object '%s' not found.\n",
                             req->object_name);
        return 1;
    }

    struct SLSObjectSchema* schema = &object_schemas[obj_idx];
    schema->object_id = object_catalog[obj_idx].object_id;

    // Update existing schema entry if the key already has a type defined
    for (uint32_t i = 0; i < SCHEMA_MAX_FIELDS; i++) {
        if (!schema->fields[i].active) continue;
        if (!cat_streq(schema->fields[i].key, req->key)) continue;
        schema->fields[i].type = req->type;
        kernel_serial_printf("[SCHEMA] Updated '%s.%s' -> %s\n",
                             req->object_name, req->key,
                             field_type_name(req->type));
        return 0;
    }
    // New field definition
    for (uint32_t i = 0; i < SCHEMA_MAX_FIELDS; i++) {
        if (!schema->fields[i].active) {
            cat_strncpy(schema->fields[i].key, req->key, RECORD_KEY_LEN);
            schema->fields[i].type   = req->type;
            schema->fields[i].active = 1;
            schema->field_count++;
            kernel_serial_printf("[SCHEMA] Defined '%s.%s' as %s\n",
                                 req->object_name, req->key,
                                 field_type_name(req->type));
            return 0;
        }
    }
    kernel_serial_print("[SCHEMA] Schema field capacity full.\n");
    return 1;
}

// ─── Phase 6: schema show ─────────────────────────────────────────────────────
// Prints the full schema with type definitions and live record values side by side.
void sys_sls_schema_show(const char* name) {
    int obj_idx = find_by_name(name);
    if (obj_idx < 0) {
        kernel_serial_printf("[SCHEMA] Object '%s' not found.\n", name);
        return;
    }
    struct SLSObjectEntry*  e      = &object_catalog[obj_idx];
    struct SLSObjectRecord* rec    = &object_records[obj_idx];
    struct SLSObjectSchema* schema = &object_schemas[obj_idx];

    kernel_serial_printf(
        "\n[SCHEMA] %s  @ 0x%016lx  [%s / %s]\n"
        "  %-32s  %-8s  %s\n"
        "  %-32s  %-8s  %s\n",
        e->name, e->base_vaddr,
        obj_type_name(e->type), tier_name(e->storage_tier),
        "Field Key", "Type", "Live Value",
        "--------------------------------", "--------",
        "------------------------------");

    if (rec->field_count == 0) {
        kernel_serial_print("  (no records)\n");
    } else {
        for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
            if (!rec->fields[i].active) continue;
            // Look up the schema type for this field key
            const char* tname = "STRING";
            for (uint32_t j = 0; j < SCHEMA_MAX_FIELDS; j++) {
                if (schema->fields[j].active &&
                    cat_streq(schema->fields[j].key, rec->fields[i].key)) {
                    tname = field_type_name(schema->fields[j].type);
                    break;
                }
            }
            kernel_serial_printf("  %-32s  %-8s  %s\n",
                                 rec->fields[i].key,
                                 tname,
                                 rec->fields[i].value);
        }
    }

    // Show any schema-defined fields not yet populated
    for (uint32_t j = 0; j < SCHEMA_MAX_FIELDS; j++) {
        if (!schema->fields[j].active) continue;
        // Check if this schema field has a live value
        int found = 0;
        for (uint32_t i = 0; i < RECORD_MAX_FIELDS; i++) {
            if (rec->fields[i].active &&
                cat_streq(rec->fields[i].key, schema->fields[j].key)) {
                found = 1; break;
            }
        }
        if (!found) {
            kernel_serial_printf("  %-32s  %-8s  (null)\n",
                                 schema->fields[j].key,
                                 field_type_name(schema->fields[j].type));
        }
    }
    kernel_serial_printf("  %u field(s)  |  %u schema definition(s)\n\n",
                         rec->field_count, schema->field_count);
}
