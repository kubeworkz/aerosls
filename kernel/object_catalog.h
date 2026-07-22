#ifndef OBJECT_CATALOG_H
#define OBJECT_CATALOG_H

#include <stdint.h>
#include <stddef.h>

// ─── Object Types ─────────────────────────────────────────────────────────────
typedef enum {
    OBJ_TYPE_SYSTEM_METADATA  = 0,
    OBJ_TYPE_DB_TABLE         = 1,
    OBJ_TYPE_DB_INDEX         = 2,
    OBJ_TYPE_HEAP_BLOB        = 3,
    OBJ_TYPE_SERVICE_PROCESS  = 4,  // Ring-3 executable + stack; scheduled by kernel
    OBJ_TYPE_WEB_APP          = 5,  // HTML/JS/CSS assets stored as key-value records
    OBJ_TYPE_JOURNAL          = 6,  // IBM i-style journal object
    OBJ_TYPE_PROGRAM          = 7,  // Executable program image: flat binary, ELF64,
                                     // or SIMI bytecode (see loader.h — SIMI objects
                                     // aren't spawnable yet, pending the Phase 3
                                     // native translator)
    OBJ_TYPE_STREAM           = 8,  // Raw byte-stream "file" (read-only by default)
    OBJ_TYPE_AGENT            = 9,  // AI agent (descriptor + memory table + history stream)
    OBJ_TYPE_WORKFLOW         = 10, // Ordered sequence of agent steps
} SLSObjectType;

// ─── Storage Tiers ────────────────────────────────────────────────────────────
typedef enum {
    STORAGE_TIER_L1_CACHE = 0,   // Hot: CPU SRAM / cache-resident
    STORAGE_TIER_L2_DRAM  = 1,   // Warm: DRAM-resident
    STORAGE_TIER_L3_SSD   = 2,   // Cold: NVMe persistent flash
} SLSStorageTier;

// ─── Role Definitions ─────────────────────────────────────────────────────────
typedef enum {
    ROLE_SYSTEM_KERNEL = 0,  // Ring-0: full unrestricted access
    ROLE_DB_ADMIN      = 1,  // R/W/X on all DB_TABLE objects
    ROLE_APP_USER      = 2,  // Read-only on DB_TABLE; no SYSTEM_METADATA
    ROLE_GUEST         = 3,  // Read-only on HEAP_BLOB only
} SLSRole;

static inline const char* role_name(SLSRole r) {
    switch (r) {
        case ROLE_SYSTEM_KERNEL: return "SYSTEM_KERNEL";
        case ROLE_DB_ADMIN:      return "DB_ADMIN";
        case ROLE_APP_USER:      return "APP_USER";
        case ROLE_GUEST:         return "GUEST";
        default:                 return "UNKNOWN";
    }
}

static inline const char* obj_type_name(SLSObjectType t) {
    switch (t) {
        case OBJ_TYPE_SYSTEM_METADATA: return "SYSTEM_METADATA";
        case OBJ_TYPE_DB_TABLE:        return "DB_TABLE";
        case OBJ_TYPE_DB_INDEX:        return "DB_INDEX";
        case OBJ_TYPE_HEAP_BLOB:       return "HEAP_BLOB";
        case OBJ_TYPE_SERVICE_PROCESS: return "SERVICE_PROCESS";
        case OBJ_TYPE_WEB_APP:         return "WEB_APP";
        case OBJ_TYPE_JOURNAL:         return "JOURNAL";
        case OBJ_TYPE_PROGRAM:         return "PROGRAM";
        case OBJ_TYPE_STREAM:          return "STREAM";
        case OBJ_TYPE_AGENT:           return "AGENT";
        case OBJ_TYPE_WORKFLOW:        return "WORKFLOW";
        default:                       return "UNKNOWN";
    }
}

static inline const char* tier_name(SLSStorageTier t) {
    switch (t) {
        case STORAGE_TIER_L1_CACHE: return "L1_CACHE";
        case STORAGE_TIER_L2_DRAM:  return "L2_DRAM";
        case STORAGE_TIER_L3_SSD:   return "L3_SSD";
        default:                    return "UNKNOWN";
    }
}

// ─── Catalog Entry ────────────────────────────────────────────────────────────
#define CATALOG_MAX_OBJECTS   128  // 128 objects — enough for ~100 CSV rows + system objects
#define OBJECT_NAME_LEN        64

struct SLSObjectEntry {
    uint64_t       object_id;               // FNV-1a hash of name
    char           name[OBJECT_NAME_LEN];
    SLSObjectType  type;
    uint64_t       base_vaddr;
    uint32_t       size_pages;
    uint32_t       owner_uid;
    SLSStorageTier storage_tier;
    SLSRole        owner_role;
    uint32_t       perm_mask;
    uint8_t        active;
    uint32_t       partition_id;            // Phase 8 (LPAR groundwork) — see partition.h;
                                             // 0 (PARTITION_SYSTEM/DEFAULT) for every object
                                             // predating Phase 8, by struct zero-init
    uint8_t        uses_rowstore;           // Phase 16 (relational layer) — see rowstore.h;
                                             // 0 for every object predating Phase 16 (struct
                                             // zero-init) and for every object that never opts
                                             // in: it keeps using the legacy single-record
                                             // object_records[] path exactly as before.
};

// ─── Role Assignment Table ────────────────────────────────────────────────────
#define ROLE_TABLE_MAX 64

struct SLSRoleEntry {
    uint32_t uid;
    SLSRole  role;
    uint8_t  active;
};

// ─── Typed Record Field Schema ───────────────────────────────────────────────
typedef enum {
    FIELD_TYPE_STRING = 0,
    FIELD_TYPE_UINT64 = 1,
    FIELD_TYPE_FLOAT  = 2,
    FIELD_TYPE_BOOL   = 3,
    // Phase 4 (SQL Feature-Parity Roadmap): BLOB is deliberately narrow in
    // this first cut -- opaque bytes stored/compared exactly like STRING
    // (same ROWSTORE_STRING_LEN=64-byte inline slot, same text-in/text-out
    // API boundary as every other column type in this codebase), not a
    // real large-object/TOAST-style overflow mechanism. No base64 or binary-
    // safe encoding is added at the SQL text layer either -- see rowstore.h's
    // Phase 4 note for the full scope writeup. A real BLOB column exists
    // purely so schemas can name the intent distinctly from STRING; it does
    // not yet unlock any capability STRING didn't already have.
    FIELD_TYPE_BLOB   = 4,
} SLSFieldType;

static inline const char* field_type_name(SLSFieldType t) {
    switch (t) {
        case FIELD_TYPE_STRING: return "STRING";
        case FIELD_TYPE_UINT64: return "UINT64";
        case FIELD_TYPE_FLOAT:  return "FLOAT";
        case FIELD_TYPE_BOOL:   return "BOOL";
        case FIELD_TYPE_BLOB:   return "BLOB";
        default:                return "?";
    }
}

#define SCHEMA_MAX_FIELDS  32   // same depth as RECORD_MAX_FIELDS

// ─── Typed Record Fields (key-value store inside a DB_TABLE object) ─────────────
#define RECORD_MAX_FIELDS  32
#define RECORD_KEY_LEN     64   // supports column names up to 63 chars
#define RECORD_VAL_LEN     256  // supports CSV values up to 255 chars

struct SLSSchemaField {
    char         key[RECORD_KEY_LEN];
    SLSFieldType type;
    uint8_t      active;
};

struct SLSObjectSchema {
    uint64_t              object_id;
    uint32_t              field_count;
    struct SLSSchemaField fields[SCHEMA_MAX_FIELDS];
};

struct SLSRecordField {
    char    key[RECORD_KEY_LEN];
    char    value[RECORD_VAL_LEN];
    uint8_t active;
};

struct SLSObjectRecord {
    uint64_t             object_id;
    uint32_t             field_count;
    struct SLSRecordField fields[RECORD_MAX_FIELDS];
};

// ─── Syscall Argument Structs ─────────────────────────────────────────────────
struct SLSVallocRequest {
    char          name[OBJECT_NAME_LEN];
    SLSObjectType type;
    uint32_t      size_pages;
    uint32_t      owner_uid;
    uint32_t      perm_mask;
    uint32_t      partition_id;   // Phase 8: which partition's SLS this object lives
                                   // in; 0 (PARTITION_SYSTEM/DEFAULT) if the caller
                                   // doesn't set it (zero-initialized request struct),
                                   // matching every pre-Phase-8 valloc call site exactly
};

struct SLSRecordRequest {
    char name[OBJECT_NAME_LEN];
    char key[RECORD_KEY_LEN];
    char value[RECORD_VAL_LEN];   // empty for select
};

struct SLSSchemaRequest {
    char         object_name[OBJECT_NAME_LEN];
    char         key[RECORD_KEY_LEN];
    SLSFieldType type;
};

struct SLSRoleRequest {
    uint32_t uid;
    SLSRole  role;
};

struct SLSGrantRequest {
    uint32_t uid;
    char     object_name[OBJECT_NAME_LEN];
    uint32_t perm_delta;           // bits to add (grant) or remove (revoke)
};

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_ALLOCATE    105
#define SYS_SLS_VALLOC      110
#define SYS_SLS_VFREE       111
#define SYS_SLS_OBJ_STAT    112
#define SYS_SLS_OBJ_LIST    113
#define SYS_SLS_ROLE_SET    114
#define SYS_SLS_GRANT       115
#define SYS_SLS_REVOKE      116
#define SYS_SLS_SELECT      117
#define SYS_SLS_UPDATE      118
#define SYS_SLS_INSERT      119
#define SYS_SLS_DELETE      143
#define SYS_SLS_SCHEMA_SET  144
#define SYS_SLS_SCHEMA_SHOW 145
#define SYS_SLS_PROC_CREATE     160
#define SYS_SLS_PROC_KILL       161
#define SYS_SLS_PROC_LIST       162
#define SYS_SLS_PROGRAM_SPAWN   163   // spawn an OBJ_TYPE_PROGRAM object as a process

// ─── Public Catalog API ───────────────────────────────────────────────────────
extern struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
extern struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
extern struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
extern struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
extern uint32_t               object_catalog_count;

uint64_t sys_sls_valloc(struct SLSVallocRequest* req);
uint64_t sys_sls_vfree(const char* name);
// Phase 14 (LPAR): destroys every catalog object belonging to partition_id
// in one pass (mirrors sys_sls_vfree()'s per-entry actions). Returns the
// number of objects freed. Called from partition_destroy().
uint32_t catalog_vfree_partition(uint32_t partition_id);
void     sys_sls_obj_list(void);
uint64_t sys_sls_obj_stat(const char* name);
uint64_t sys_sls_role_set(struct SLSRoleRequest* req);
uint64_t sys_sls_grant(struct SLSGrantRequest* req, int is_grant);
uint64_t sys_sls_select(struct SLSRecordRequest* req);
uint64_t sys_sls_update(struct SLSRecordRequest* req);
uint64_t sys_sls_insert(struct SLSRecordRequest* req);
uint64_t sys_sls_delete(struct SLSRecordRequest* req);
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req);
void     sys_sls_schema_show(const char* name);

// Role check called by other subsystems (transaction, secure_api)
int      catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm);
SLSRole  catalog_get_role(uint32_t uid);

// Called by persist.c after restoring the catalog from NVMe to recalculate
// the next_obj_vaddr watermark from the restored base_vaddr + size_pages values.
void     catalog_after_restore(void);

#endif /* OBJECT_CATALOG_H */
