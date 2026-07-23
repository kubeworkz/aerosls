#include "group_profile.h"
#include "kernel_io.h"
#include "../user/permissions.h"
#include "partition.h"   // partition_get_for_uid() -- Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4
#include "tenant.h"       // tenant_caller_may_administer()

// ─── String helpers (same small local-copy convention as auth.c's au_*
// and object_catalog.c's cat_* -- this codebase doesn't share one string
// helper module across kernel files, each translation unit keeps its own) ──
static int gp_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void gp_strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ─── Globals ────────────────────────────────────────────────────────────────
struct SLSGroupEntry group_table[GROUP_TABLE_MAX];
uint32_t             group_table_count = 0;

static int find_group_idx(const char* name) {
    for (int i = 0; i < GROUP_TABLE_MAX; i++) {
        if (group_table[i].active && gp_streq(group_table[i].name, name)) return i;
    }
    return -1;
}

// ─── catalog_role_grants ───────────────────────────────────────────────────
// Factored verbatim out of object_catalog.c's catalog_check_access() -- see
// this header's own comment for why. Mirrors that function's exact rule
// order and semantics; only the owner-uid short-circuit, kernel-always-
// passes, partition-boundary check, and the final perm_mask fallback stay
// behind in catalog_check_access() itself (those aren't role-specific).
int catalog_role_grants(SLSRole role, struct SLSObjectEntry* e, uint32_t needed_perm) {
    if (role == ROLE_SYSTEM_KERNEL) return 1;

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

    // GUEST: read-only on HEAP_BLOB and STREAM only
    if (role == ROLE_GUEST) {
        if ((e->type == OBJ_TYPE_HEAP_BLOB || e->type == OBJ_TYPE_STREAM)
                && needed_perm == PERM_READ)
            return 1;
        return 0;
    }

    // APP_USER: read + execute on PROGRAM objects (can spawn, not modify)
    if (role == ROLE_APP_USER) {
        if (e->type == OBJ_TYPE_PROGRAM &&
                (needed_perm & (PERM_READ | PERM_EXECUTE)) &&
                !(needed_perm & PERM_WRITE))
            return 1;
    }

    return 0;
}

// ─── group_create ───────────────────────────────────────────────────────────
// Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4: group_create()
// used to take no caller_uid at all and had zero permission gate -- any uid
// could call it via do_syscall() and mint a group naming ANY role, with no
// identity check whatsoever. Now scoped to caller_uid's own partition
// (stamped via partition_get_for_uid()) and gated behind
// tenant_caller_may_administer(), so only a tenant's own recorded owner (or
// a global ROLE_SYSTEM_KERNEL/ROLE_DB_ADMIN caller) can create groups
// administering that tenant -- self-service administration without needing
// global authority, and no more silently-global group namespace.
int group_create(uint32_t caller_uid, const char* name, SLSRole role) {
    if (!name || !name[0]) return 0;
    uint32_t partition_id = partition_get_for_uid(caller_uid);
    if (!tenant_caller_may_administer(caller_uid, partition_id)) {
        kernel_serial_printf("[GROUP] ERROR: uid %u may not administer partition %u's RBAC.\n",
                             caller_uid, partition_id);
        return 0;
    }
    if (find_group_idx(name) >= 0) {
        kernel_serial_printf("[GROUP] ERROR: Group '%s' already exists.\n", name);
        return 0;
    }
    for (int i = 0; i < GROUP_TABLE_MAX; i++) {
        if (!group_table[i].active) {
            gp_strncpy(group_table[i].name, name, GROUP_NAME_LEN);
            group_table[i].group_role   = role;
            group_table[i].member_count = 0;
            group_table[i].active       = 1;
            group_table[i].partition_id = partition_id;
            if ((uint32_t)(i + 1) > group_table_count) group_table_count = (uint32_t)(i + 1);
            kernel_serial_printf("[GROUP] Created group '%s' with role %s (partition_id=%u).\n",
                                 name, role_name(role), partition_id);
            return 1;
        }
    }
    kernel_serial_print("[GROUP] ERROR: Group table full.\n");
    return 0;
}

// ─── group_add_member ───────────────────────────────────────────────────────
// Same §5 item 5 / §7 item 4 gate as group_create() above, checked against
// the GROUP's own recorded partition_id (not necessarily caller_uid's own --
// a tenant owner administers their tenant's groups regardless of which
// partition uid 0/kernel happens to be evaluating from). Additionally
// rejects adding a member whose own partition doesn't match the group's --
// cross-tenant group membership is refused outright rather than silently
// allowed and relying on catalog_check_access()'s object-level partition
// boundary to make it harmless; a customer's own group roster should only
// ever contain that customer's own users.
int group_add_member(uint32_t caller_uid, const char* name, uint32_t uid) {
    int idx = find_group_idx(name);
    if (idx < 0) {
        kernel_serial_printf("[GROUP] ERROR: Group '%s' not found.\n", name);
        return 0;
    }
    struct SLSGroupEntry* g = &group_table[idx];
    if (!tenant_caller_may_administer(caller_uid, g->partition_id)) {
        kernel_serial_printf("[GROUP] ERROR: uid %u may not administer group '%s' (partition %u).\n",
                             caller_uid, name, g->partition_id);
        return 0;
    }
    if (partition_get_for_uid(uid) != g->partition_id) {
        kernel_serial_printf("[GROUP] ERROR: uid %u is not in the partition (%u) group '%s' is scoped to -- cross-tenant membership refused.\n",
                             uid, g->partition_id, name);
        return 0;
    }
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->member_uids[i] == uid) {
            kernel_serial_printf("[GROUP] uid %u is already a member of '%s'.\n", uid, name);
            return 0;
        }
    }
    if (g->member_count >= GROUP_MAX_MEMBERS) {
        kernel_serial_printf("[GROUP] ERROR: Group '%s' member list full.\n", name);
        return 0;
    }
    g->member_uids[g->member_count++] = uid;
    kernel_serial_printf("[GROUP] uid %u added to group '%s'.\n", uid, name);
    return 1;
}

// ─── group_contains_uid ─────────────────────────────────────────────────────
int group_contains_uid(const char* name, uint32_t uid) {
    int idx = find_group_idx(name);
    if (idx < 0) return 0;
    struct SLSGroupEntry* g = &group_table[idx];
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->member_uids[i] == uid) return 1;
    }
    return 0;
}

// ─── group_get_partition_id ─────────────────────────────────────────────────
int group_get_partition_id(const char* name, uint32_t* out_partition_id) {
    int idx = find_group_idx(name);
    if (idx < 0) return 0;
    if (out_partition_id) *out_partition_id = group_table[idx].partition_id;
    return 1;
}

// ─── group_list ──────────────────────────────────────────────────────────────
void group_list(void) {
    kernel_serial_printf("\n[GROUP] Group Profiles\n %-32s %-14s %s\n",
                         "Name", "Role", "Members");
    int shown = 0;
    for (int i = 0; i < GROUP_TABLE_MAX; i++) {
        if (!group_table[i].active) continue;
        kernel_serial_printf(" %-32s %-14s %u member(s)\n",
                             group_table[i].name,
                             role_name(group_table[i].group_role),
                             group_table[i].member_count);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no groups defined)\n");
    kernel_serial_printf(" %d group(s).\n\n", shown);
}

// ─── Syscall wrappers ────────────────────────────────────────────────────────
uint64_t sys_sls_group_create(struct SLSGroupCreateRequest* req) {
    if (!req) return 1;
    return group_create(req->caller_uid, req->name, req->role) ? 0 : 1;
}

uint64_t sys_sls_group_add_member(struct SLSGroupAddMemberRequest* req) {
    if (!req) return 1;
    return group_add_member(req->caller_uid, req->name, req->uid) ? 0 : 1;
}
