#ifndef OBJECT_CATALOG_H
#define OBJECT_CATALOG_H

#include <stdint.h>
#include <stddef.h>

// ─── Object Types ─────────────────────────────────────────────────────────────
typedef enum {
    OBJ_TYPE_SYSTEM_METADATA = 0,
    OBJ_TYPE_DB_TABLE        = 1,
    OBJ_TYPE_DB_INDEX        = 2,
    OBJ_TYPE_HEAP_BLOB       = 3,
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
#define CATALOG_MAX_OBJECTS  256
#define OBJECT_NAME_LEN       64

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
} SLSFieldType;

static inline const char* field_type_name(SLSFieldType t) {
    switch (t) {
        case FIELD_TYPE_STRING: return "STRING";
        case FIELD_TYPE_UINT64: return "UINT64";
        case FIELD_TYPE_FLOAT:  return "FLOAT";
        case FIELD_TYPE_BOOL:   return "BOOL";
        default:                return "?";
    }
}

#define SCHEMA_MAX_FIELDS  32   // same depth as RECORD_MAX_FIELDS

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

// ─── Typed Record Fields (key-value store inside a DB_TABLE object) ─────────────
#define RECORD_MAX_FIELDS  32
#define RECORD_KEY_LEN     48
#define RECORD_VAL_LEN     64

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

// ─── Public Catalog API ───────────────────────────────────────────────────────
extern struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
extern struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
extern struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
extern struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
extern uint32_t               object_catalog_count;

uint64_t sys_sls_valloc(struct SLSVallocRequest* req);
uint64_t sys_sls_vfree(const char* name);
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

#endif /* OBJECT_CATALOG_H */
