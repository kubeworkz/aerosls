#include "object_catalog.h"
#include "persist.h"
#include "journal.h"
#include "lock_mgr.h"
#include "index_mgr.h"
#include "constraint.h"
#include "mqt.h"
#include "partition.h"
#include "../user/permissions.h"
// Navigator-Parity Gap Roadmap Phase 3: group profiles, authorization
// lists, and the security audit log. All three are separate, independently
// testable modules (see each header's own comment); object_catalog.c only
// needs to call into them from catalog_check_access()/sys_sls_role_set()
// below, the same "own module, thin integration point" shape as
// partition.h's partition_get_for_uid() call in catalog_check_access()
// already established for Phase 8.
#include "group_profile.h"
#include "authlist.h"
#include "security_audit.h"

// Forward declaration — avoids pulling the full tier_mgr.h include graph into this file
extern void tier_notify_access(uint64_t object_id);

// Forward declarations for transaction integration (Phase 3/6)
extern uint64_t tx_get_active(uint32_t thread_id);
extern uint64_t wal_stage(uint32_t thread_id, uint64_t object_id,
                           const char* key,
                           const char* old_value, const char* new_value);
extern uint32_t kernel_get_current_thread_id(void);

// Forward declaration -- VectorStore Interface Roadmap Phase 1. Notifies
// vecstore.c (and, transitively, vec_index.c) that a catalog object is
// about to be freed, so any vector-collection state and HNSW indexes built
// over it are released too instead of orphaned -- confirmed via grep
// before this fix that neither sys_sls_vfree() nor
// catalog_vfree_partition() below ever touched vector_collections[]/
// vec_indexes[] at all, permanently leaking a collection's slot and pages
// every time vfree ran on one. A bare extern here (not a #include), same
// convention as tier_notify_access() just above -- avoids a circular
// header dependency, since vecstore.h already includes object_catalog.h
// the other way around. A no-op call if the object being freed was never a
// vector collection (see vecstore_notify_object_freed()'s own comment).
extern void vecstore_notify_object_freed(const char* collection_name);

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

// ─── catalog_after_restore ───────────────────────────────────────────────────
// Called by persist.c after restoring object_catalog[] from NVMe.
// Recalculates next_obj_vaddr so future valloc() calls don't collide with
// virtual address ranges that were assigned before the reboot.
void catalog_after_restore(void) {
    uint64_t max_end = OBJ_VADDR_BASE;
    for (uint32_t i = 0; i < CATALOG_MAX_OBJECTS; i++) {
        if (!object_catalog[i].active) continue;
        uint64_t end = object_catalog[i].base_vaddr
                     + (uint64_t)object_catalog[i].size_pages * PAGE_SIZE_BYTES;
        if (end > max_end) max_end = end;
    }
    next_obj_vaddr = max_end;
}

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

    // Phase 8 (LPAR groundwork): partition boundary — the outermost check
    // below the kernel-always-passes rule above, ahead of even the owner
    // check. A uid outside an object's partition is denied regardless of
    // ownership/role — partition isolation is meant to model a hypervisor-
    // level boundary, stronger than any in-partition authority rule. Every
    // pre-Phase-8 object/uid defaults to partition 0 (PARTITION_SYSTEM ==
    // PARTITION_DEFAULT), so this is a no-op unless partitions are
    // explicitly created and used — see partition.h's top comment.
    if (e->partition_id != partition_get_for_uid(uid)) return 0;

    // Owner always has full access to their own object
    if (e->owner_uid == uid) return 1;

    // Phase 3 (Navigator-Parity Gap Roadmap): the DB_ADMIN/APP_USER/GUEST
    // rules that used to live inline here are now catalog_role_grants()
    // (group_profile.c) — reused verbatim below for group-derived access, so
    // there's exactly one copy of "what does this role allow" rather than a
    // hand-duplicated second copy for groups.
    if (catalog_role_grants(role, e, needed_perm)) return 1;

    // Phase 3: group-derived access. Additive, evaluated independently of
    // the caller's own role above — every active group uid is a member of
    // gets the exact same role-grants-access check a moment ago ran for
    // uid's individual role. A group can only ever add access, never
    // restrict what the individual role check already allowed (that check
    // already returned above if it passed).
    //
    // Deliberately runs BEFORE the GUEST hard-deny below, not after: an
    // important real bug this test suite's own first draft caught --
    // catalog_get_role() returns ROLE_GUEST for ANY uid with no role_table[]
    // entry at all (its own documented default), which is exactly the
    // common case for a uid that's only ever been granted access via group
    // or authlist membership, never given an individual role. Running the
    // GUEST hard-deny first would silently make group/authlist grants
    // unreachable for every such uid -- defeating this entire phase's
    // purpose. Groups/authlists must be checked before that hard-deny for
    // GUEST to mean anything for the uids Phase 3 actually cares about.
    for (int gi = 0; gi < GROUP_TABLE_MAX; gi++) {
        if (!group_table[gi].active) continue;
        if (!group_contains_uid(group_table[gi].name, uid)) continue;
        if (catalog_role_grants(group_table[gi].group_role, e, needed_perm)) return 1;
    }

    // Phase 3: authorization-list fallback. Same reasoning as the group
    // check just above -- must run before the GUEST hard-deny, for the same
    // "GUEST is also the default for every untouched uid" reason. The
    // entire point of an authlist is to grant access independent of what
    // the object's own perm_mask says, so it needs to be its own path.
    if (authlist_check_access(uid, obj_name, needed_perm)) return 1;

    // GUEST never falls through to the raw perm_mask fallback below —
    // preserves this function's pre-Phase-3 behavior exactly for that one
    // specific fallback (an unmatched GUEST request was always denied
    // perm_mask escalation). Every other role's "no rule matched" result
    // (including the one genuinely narrow edge case this refactor's own
    // audit turned up: an APP_USER requesting combined READ+WRITE on a
    // DB_TABLE, which catalog_role_grants() now answers with a plain 0
    // instead of the old inline code's separate hard-return-0 for that one
    // combination) is allowed to reach the perm_mask fallback below — no
    // real caller in this codebase actually requests combined perm bits in
    // one call (every rowstore.h/mvcc.h call site asks for PERM_READ or
    // PERM_WRITE separately), so this is a theoretical behavior difference,
    // not an observed one — named here rather than silently changed.
    if (role == ROLE_GUEST) {
        security_audit_log(uid, "ACCESS_DENIED", obj_name, 0);
        return 0;
    }

    // Fall back to stored per-object perm_mask
    if ((e->perm_mask & needed_perm) == needed_perm) return 1;

    // Phase 3: every path above failed — a real, final access denial.
    security_audit_log(uid, "ACCESS_DENIED", obj_name, 0);
    return 0;
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
        case OBJ_TYPE_PROGRAM:         e->storage_tier = STORAGE_TIER_L2_DRAM;  break;
        case OBJ_TYPE_STREAM:          e->storage_tier = STORAGE_TIER_L3_SSD;   break;
        default:                       e->storage_tier = STORAGE_TIER_L3_SSD;   break;
    }
    e->owner_role   = catalog_get_role(req->owner_uid);
    e->perm_mask    = req->perm_mask ? req->perm_mask
                                     : (PERM_READ | PERM_WRITE | PERM_OWNER);
    // Phase 8: if the caller didn't set partition_id explicitly (0 — the
    // same "0 means use a sensible default" idiom as perm_mask above),
    // default to the owner's own assigned partition rather than hardcoding
    // PARTITION_SYSTEM. This matters: catalog_check_access()'s partition
    // check runs before the owner check, so defaulting to a fixed 0 would
    // silently lock an owner in a non-system partition out of their own
    // freshly-created object. An explicit non-zero partition_id still
    // overrides (e.g. a provisioning process placing an object into a
    // partition other than its own).
    e->partition_id = req->partition_id ? req->partition_id
                                        : partition_get_for_uid(req->owner_uid);
    e->active       = 1;

    // Initialise an empty record store for this slot
    object_records[slot].object_id   = obj_id;
    object_records[slot].field_count = 0;

    object_catalog_count++;
    persist_catalog();

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
    // VectorStore Interface Roadmap Phase 1: must run BEFORE
    // object_catalog[idx].active is cleared below -- vecstore_notify_
    // object_freed() looks this object up by name through the same
    // object_catalog[].active gate every vecstore.c entry point uses, so
    // it would silently find nothing (and leak) if called after. A no-op
    // if this object was never a vector collection.
    vecstore_notify_object_freed(name);
    object_catalog[idx].active = 0;
    object_records[idx].field_count = 0;
    kernel_serial_printf("[CATALOG] vfree: '%s' released from address space.\n",
                         name);
    persist_catalog();
    return 0;
}

// ─── Phase 14 (LPAR): catalog_vfree_partition ─────────────────────────────────
// Destroys every active catalog object whose partition_id matches, mirroring
// sys_sls_vfree()'s exact per-entry actions (active=0, field_count=0) but
// batched into one persist_catalog() call at the end instead of one per
// object. Called from partition_destroy() as one of its teardown steps.
uint32_t catalog_vfree_partition(uint32_t partition_id) {
    uint32_t freed = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (object_catalog[i].partition_id != partition_id) continue;
        kernel_serial_printf("[CATALOG] vfree (partition %u teardown): '%s'\n",
                             (unsigned)partition_id, object_catalog[i].name);
        // VectorStore Interface Roadmap Phase 1: same ordering requirement
        // as sys_sls_vfree() above -- must run before .active is cleared.
        vecstore_notify_object_freed(object_catalog[i].name);
        object_catalog[i].active = 0;
        object_records[i].field_count = 0;
        freed++;
    }
    if (freed) persist_catalog();
    return freed;
}

// ─── Phase 1: ls objects ──────────────────────────────────────────────────────
void sys_sls_obj_list(void) {
    // Gap Remediation Phase C fix: this was calling kernel_serial_print()
    // (kernel_io.h: `void kernel_serial_print(const char* s)` -- exactly
    // ONE argument, no format processing) with a format string plus 5
    // extra string arguments. Never caught because this file doesn't
    // include kernel_io.h itself (no prototype in scope to conflict with),
    // and this whole project has never been compiled end to end with the
    // real cross-compiler (docs/AeroSLS-Gap-Analysis-v0.1.md §1) --
    // discovered while writing an analogous sys_sls_vec_list() and nearly
    // copying the same bug. kernel_serial_printf() is the variadic one.
    kernel_serial_printf(
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
            security_audit_log(req->uid, "ROLE_CHANGE", role_name(req->role), 1);
            persist_catalog();
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
            security_audit_log(req->uid, "ROLE_CHANGE", role_name(req->role), 1);
            persist_catalog();
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
    // Phase 24: an object promoted to row-set storage (rowstore_create_table())
    // has its real data in table_headers[]/rowstore.c's pages, not here --
    // object_records[obj_idx] is stale/unmaintained for such an object (see
    // this codebase's own rowstore_create_table(), which never touches it).
    // Reading through the legacy field API here would silently return
    // whatever leftover/empty data happens to be in that stale slot instead
    // of an honest error -- reject instead, matching this whole roadmap's
    // "denial looks like absence" fix pattern rather than repeating it.
    if (object_catalog[obj_idx].uses_rowstore) {
        kernel_serial_printf(
            "[DB] SELECT: '%s' is a row-set table -- use the SQL/row-set path (sql_execute()), not the legacy record API.\n",
            req->name);
        return 4;
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
    // Phase 24: see sys_sls_select()'s own comment above -- same guard.
    if (object_catalog[obj_idx].uses_rowstore) {
        kernel_serial_printf(
            "[DB] UPDATE: '%s' is a row-set table -- use the SQL/row-set path (sql_execute()), not the legacy record API.\n",
            req->name);
        return 4;
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
                // Constraint check before acquiring lock or staging to WAL
                int cv = constraint_check_update(req->name, req->key,
                                                 rec->fields[i].value, req->value);
                if (cv) {
                    kernel_serial_printf("[DB] UPDATE blocked by constraint (code %d)\n", cv);
                    return 3;
                }
                // Acquire exclusive row lock before staging to WAL.
                if (lock_acquire(tx, object_catalog[obj_idx].object_id,
                                 req->key, LOCK_EXCLUSIVE) != 0) {
                    kernel_serial_print("[DB] UPDATE blocked: lock conflict\n");
                    return 2;
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
            {
                int cv = constraint_check_update(req->name, req->key,
                                                 rec->fields[i].value, req->value);
                if (cv) {
                    kernel_serial_printf("[DB] UPDATE blocked by constraint (code %d)\n", cv);
                    return 3;
                }
            }
            // Journal: write UB + UP immediately (auto-commit, no tx)
            journal_write(req->name, req->key,
                          rec->fields[i].value, req->value,
                          JENT_UB, 0);
            // Update any indexes on this field before changing the value
            index_on_update(req->name, req->key, req->key,
                            rec->fields[i].value, req->value);
            cat_strncpy(rec->fields[i].value, req->value, RECORD_VAL_LEN);
            journal_write(req->name, req->key,
                          "", req->value, JENT_UP, 0);
            kernel_serial_printf("[DB] UPDATE %s.%s = %s  [DIRECT]\n",
                                 req->name, req->key, req->value);
            mqt_refresh_for_table(req->name);   // auto-refresh MQTs
            persist_records();
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
    // Phase 24: see sys_sls_select()'s own comment above -- same guard.
    if (object_catalog[obj_idx].uses_rowstore) {
        kernel_serial_printf(
            "[DB] INSERT: '%s' is a row-set table -- use the SQL/row-set path (sql_execute()), not the legacy record API.\n",
            req->name);
        return 4;
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
        // Constraint check before acquiring lock + staging to WAL
        int cv = constraint_check_insert(req->name, req->key, req->value);
        if (cv) {
            kernel_serial_printf("[DB] INSERT blocked by constraint (code %d)\n", cv);
            return 3;
        }
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
            // Constraint check for direct (auto-commit) inserts
            int cv = constraint_check_insert(req->name, req->key, req->value);
            if (cv) {
                kernel_serial_printf("[DB] INSERT blocked by constraint (code %d)\n", cv);
                return 3;
            }
            cat_strncpy(rec->fields[i].key,   req->key,   RECORD_KEY_LEN);
            cat_strncpy(rec->fields[i].value, req->value, RECORD_VAL_LEN);
            rec->fields[i].active = 1;
            rec->field_count++;
            journal_write(req->name, req->key, "", req->value, JENT_PT, 0);
            index_on_insert(req->name, req->key, req->key, req->value);
            kernel_serial_printf("[DB] INSERT %s.%s = %s  [DIRECT]\n",
                                 req->name, req->key, req->value);
            mqt_refresh_for_table(req->name);   // auto-refresh MQTs
            persist_records();
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
    // Phase 24: see sys_sls_select()'s own comment above -- same guard.
    if (object_catalog[obj_idx].uses_rowstore) {
        kernel_serial_printf(
            "[DB] DELETE: '%s' is a row-set table -- use the SQL/row-set path (sql_execute()), not the legacy record API.\n",
            req->name);
        return 4;
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
        // Remove this key from all indexes on the table
        index_on_delete(req->name, req->key);
        kernel_serial_printf("[DB] DELETE %s.%s  [OK]\n", req->name, req->key);
        mqt_refresh_for_table(req->name);   // auto-refresh MQTs
        persist_records();
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
    // Phase 24: rowstore_create_table() computes a row-set table's
    // RowTableLayout ONCE from object_schemas[obj_idx] at promotion time
    // (rowstore.c's compute_layout()) -- it is never recomputed afterward.
    // Allowing a schema field to be added/retyped post-promotion would
    // silently desync table_headers[obj_idx].layout from object_schemas[]
    // (new/changed columns the row-set engine never allocated storage for
    // or never learns about), corrupting every future row read/write for
    // that table. Reject instead, same boundary as the record API guards
    // above -- schema changes for a live row-set table are out of scope
    // (ALTER TABLE-equivalent work, not attempted anywhere in this roadmap).
    if (object_catalog[obj_idx].uses_rowstore) {
        kernel_serial_printf(
            "[SCHEMA] '%s' is a row-set table -- its layout is fixed at rowstore_create_table() time and cannot be changed.\n",
            req->object_name);
        return 4;
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
        persist_schemas();
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
            persist_schemas();
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
