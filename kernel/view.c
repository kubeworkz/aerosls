/*
 * view.c — Query-Surface Roadmap Phase 5 implementation. See view.h for
 * the full design writeup.
 */
#include "view.h"
#include "object_catalog.h"
#include "kernel_io.h"
#include "persist.h"   // persist_views()

struct SLSViewDef views[VIEW_MAX];

// ─── String helpers (no libc — vw_* here, matching this codebase's
// established per-file convention: db_* in database.c, gp_* in
// group_profile.c, au_* in auth.c, etc.). ─────────────────────────────────
static int vw_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void vw_strncpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i + 1 < max && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static uint32_t vw_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static int find_view_idx(const char* name) {
    for (int i = 0; i < VIEW_MAX; i++) {
        if (views[i].active && vw_streq(views[i].name, name)) return i;
    }
    return -1;
}

// See view.h's own header comment for why this check exists and what it
// deliberately does NOT defend against (a later CREATE TABLE shadowing an
// earlier view). Local re-scan of object_catalog[], the same "row-set
// table" filter sql_exec.c's own find_table_catalog_index() uses, rather
// than exporting that static function across a file boundary just for
// this one check.
static int find_table_name_collision(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (vw_streq(object_catalog[i].name, name)) return 1;
    }
    return 0;
}

int view_find_index(const char* name) {
    return find_view_idx(name);
}

// ─── view_create ──────────────────────────────────────────────────────────
int view_create(uint32_t caller_uid, const char* name, const char* sql_text) {
    if (!name || !name[0]) {
        kernel_serial_print("[VIEW] ERROR: view name required.\n");
        return 1;
    }
    if (vw_strlen(name) >= OBJECT_NAME_LEN) {
        kernel_serial_print("[VIEW] ERROR: view name too long.\n");
        return 1;
    }
    if (!sql_text || !sql_text[0]) {
        kernel_serial_print("[VIEW] ERROR: view definition (AS <select...>) required.\n");
        return 1;
    }
    if (vw_strlen(sql_text) >= VIEW_SQL_TEXT_LEN) {
        kernel_serial_print("[VIEW] ERROR: view definition text too long.\n");
        return 1;
    }
    if (find_table_name_collision(name)) {
        kernel_serial_printf("[VIEW] ERROR: a table named '%s' already exists.\n", name);
        return 1;
    }
    if (find_view_idx(name) >= 0) {
        kernel_serial_printf("[VIEW] ERROR: view '%s' already exists.\n", name);
        return 1;
    }

    int free_slot = -1;
    for (int i = 0; i < VIEW_MAX; i++) {
        if (!views[i].active) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        kernel_serial_print("[VIEW] ERROR: view table full.\n");
        return 1;
    }

    struct SLSViewDef* v = &views[free_slot];
    vw_strncpy(v->name, name, OBJECT_NAME_LEN);
    vw_strncpy(v->sql_text, sql_text, VIEW_SQL_TEXT_LEN);
    v->owner_uid = caller_uid;
    v->active    = 1;

    kernel_serial_printf("[VIEW] Created view '%s' (owner_uid=%u).\n", v->name, v->owner_uid);
    persist_views();
    return 0;
}

// ─── view_drop ─────────────────────────────────────────────────────────────
int view_drop(uint32_t caller_uid, const char* name) {
    int idx = find_view_idx(name);
    if (idx < 0) {
        kernel_serial_printf("[VIEW] ERROR: view '%s' not found.\n", name);
        return 1;
    }
    struct SLSViewDef* v = &views[idx];

    // Same minimal owner-or-kernel gate database_drop() uses -- see
    // database.c's own comment on why this doesn't (yet) go through the
    // full role/group/authlist chain catalog_check_access() runs for
    // ordinary objects.
    if (v->owner_uid != caller_uid && catalog_get_role(caller_uid) != ROLE_SYSTEM_KERNEL) {
        kernel_serial_printf("[VIEW] ERROR: uid %u is not permitted to drop view '%s'.\n",
                             caller_uid, name);
        return 2;
    }

    v->active = 0;
    kernel_serial_printf("[VIEW] Dropped view '%s'.\n", name);
    persist_views();
    return 0;
}
