#include "group_profile.h"
#include "kernel_io.h"
#include "../user/permissions.h"

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
int group_create(const char* name, SLSRole role) {
    if (!name || !name[0]) return 0;
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
            if ((uint32_t)(i + 1) > group_table_count) group_table_count = (uint32_t)(i + 1);
            kernel_serial_printf("[GROUP] Created group '%s' with role %s.\n",
                                 name, role_name(role));
            return 1;
        }
    }
    kernel_serial_print("[GROUP] ERROR: Group table full.\n");
    return 0;
}

// ─── group_add_member ───────────────────────────────────────────────────────
int group_add_member(const char* name, uint32_t uid) {
    int idx = find_group_idx(name);
    if (idx < 0) {
        kernel_serial_printf("[GROUP] ERROR: Group '%s' not found.\n", name);
        return 0;
    }
    struct SLSGroupEntry* g = &group_table[idx];
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
    return group_create(req->name, req->role) ? 0 : 1;
}

uint64_t sys_sls_group_add_member(struct SLSGroupAddMemberRequest* req) {
    if (!req) return 1;
    return group_add_member(req->name, req->uid) ? 0 : 1;
}
