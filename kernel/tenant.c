/*
 * tenant.c — Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2
 * implementation. See tenant.h for the full design writeup.
 */
#include "tenant.h"
#include "partition.h"
#include "database.h"
#include "kernel_io.h"
#include "persist.h"   // persist_tenants()

struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;   // 0 reserved for NONE — see tenant.h's own comment

// ─── String helpers (no libc — tn_* here, matching this codebase's
// established per-file convention: db_* in database.c, gp_* in
// group_profile.c, au_* in auth.c, etc.). ───────────────────────────────
static int tn_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void tn_strncpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i + 1 < max && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static uint32_t tn_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static int find_tenant_idx(const char* name) {
    for (int i = 0; i < TENANT_MAX; i++) {
        if (tenants[i].active && tn_streq(tenants[i].name, name)) return i;
    }
    return -1;
}

// ─── tenant_create ──────────────────────────────────────────────────────
int tenant_create(uint32_t caller_uid, const char* name, uint32_t* out_tenant_id) {
    if (!name || !name[0]) {
        kernel_serial_print("[TENANT] ERROR: tenant name required.\n");
        return 1;
    }
    if (tn_strlen(name) >= TENANT_NAME_LEN) {
        kernel_serial_print("[TENANT] ERROR: tenant name too long.\n");
        return 1;
    }
    if (find_tenant_idx(name) >= 0) {
        kernel_serial_printf("[TENANT] ERROR: tenant '%s' already exists.\n", name);
        return 1;
    }

    int free_slot = -1;
    for (int i = 0; i < TENANT_MAX; i++) {
        if (!tenants[i].active) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        kernel_serial_print("[TENANT] ERROR: tenant table full.\n");
        return 1;
    }

    // ── Step 1: partition_create(), first because it's the cheaper side to
    // roll back (partition_destroy() is real, well-exercised teardown from
    // LPAR Phase 14). ─────────────────────────────────────────────────────
    uint32_t partition_id = partition_create(name);
    if (partition_id == 0xFFFFFFFFu) {
        kernel_serial_printf("[TENANT] ERROR: partition_create() failed for tenant '%s' (partition table full).\n", name);
        return 2;
    }

    // ── Step 2: database_create(). On failure, roll back the partition
    // just created above so tenant_create() never leaves an orphan
    // partition with no matching database behind it. ───────────────────
    int db_rc = database_create(caller_uid, name);
    if (db_rc != 0) {
        kernel_serial_printf("[TENANT] ERROR: database_create() failed for tenant '%s' (rc=%d) -- rolling back partition %u.\n",
                             name, db_rc, partition_id);
        partition_destroy(partition_id);
        return 3;
    }
    uint32_t database_id = database_find_id(name);

    // ── Step 3: put the caller inside their own new partition, unless
    // caller_uid is 0 (kernel) -- uid 0 is permanently PARTITION_SYSTEM,
    // see tenant.h's own comment on why this is checked explicitly rather
    // than relying on partition_assign_uid()'s own failure return. ──────
    if (caller_uid != 0) {
        partition_assign_uid(caller_uid, partition_id);
    }

    // ── Step 4: record the tenant identity that ties both together. ────
    struct SLSTenantEntry* t = &tenants[free_slot];
    tn_strncpy(t->name, name, TENANT_NAME_LEN);
    t->tenant_id    = tenant_next_id++;
    t->partition_id = partition_id;
    t->database_id  = database_id;
    t->owner_uid    = caller_uid;
    t->active       = 1;

    persist_tenants();

    kernel_serial_printf("[TENANT] Created tenant '%s' (id=%u, partition_id=%u, database_id=%u, owner_uid=%u).\n",
                         name, t->tenant_id, partition_id, database_id, caller_uid);

    if (out_tenant_id) *out_tenant_id = t->tenant_id;
    return 0;
}

void tenant_list(void) {
    kernel_serial_print("[TENANT] Defined tenants:\n");
    for (int i = 0; i < TENANT_MAX; i++) {
        if (!tenants[i].active) continue;
        kernel_serial_printf("  id=%u name='%s' partition_id=%u database_id=%u owner_uid=%u\n",
                             tenants[i].tenant_id, tenants[i].name,
                             tenants[i].partition_id, tenants[i].database_id,
                             tenants[i].owner_uid);
    }
}

uint32_t tenant_find_id(const char* name) {
    if (!name) return 0;
    int idx = find_tenant_idx(name);
    return idx < 0 ? 0 : tenants[idx].tenant_id;
}

uint32_t tenant_find_by_partition(uint32_t partition_id) {
    for (int i = 0; i < TENANT_MAX; i++) {
        if (tenants[i].active && tenants[i].partition_id == partition_id) return tenants[i].tenant_id;
    }
    return 0;
}

uint32_t tenant_find_by_database(uint32_t database_id) {
    if (database_id == 0) return 0;   // 0 is NONE/unassigned on the database side too -- never a real match
    for (int i = 0; i < TENANT_MAX; i++) {
        if (tenants[i].active && tenants[i].database_id == database_id) return tenants[i].tenant_id;
    }
    return 0;
}

// ─── Syscall wrapper ────────────────────────────────────────────────────
uint64_t sys_sls_tenant_create(struct SLSTenantCreateRequest* req) {
    if (!req) return 0;
    uint32_t tenant_id = 0;
    int rc = tenant_create(req->caller_uid, req->name, &tenant_id);
    return rc == 0 ? (uint64_t)tenant_id : 0;
}
