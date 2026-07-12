#include "syscall_dispatch.h"
#include "object_catalog.h"
#include "transaction.h"
#include "microkernel.h"
#include "ipc.h"
#include "tier_mgr.h"
#include "query_engine.h"
#include "../kernel/secure_api.h"

// ─── sys_sls_allocate — legacy direct-address allocation (syscall 105) ────────
// Returns the base virtual address of the named object, or 0 if not found.
static uint64_t sls_legacy_allocate(void* arg) {
    if (!arg) return 0;
    struct {
        uint64_t object_id;
        uint64_t size_requested;
        uint32_t access_flags;
    }* req = arg;

    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active &&
            object_catalog[i].object_id == req->object_id) {
            return object_catalog[i].base_vaddr;
        }
    }
    return 0;
}

// ─── do_syscall ───────────────────────────────────────────────────────────────
uint64_t do_syscall(uint64_t num, void* arg) {
    switch (num) {

    // ── Legacy (105–109) ──────────────────────────────────────────────────────
    case 105: /* SYS_SLS_ALLOCATE */
        return sls_legacy_allocate(arg);

    case SYS_SLS_CHMOD: /* 107 — update perm mask via object name */
        if (arg) {
            uint64_t* a = (uint64_t*)arg;  // a[0]=object_id, a[1]=mask
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    object_catalog[i].object_id == a[0]) {
                    object_catalog[i].perm_mask = (uint32_t)a[1];
                    return 0;
                }
            }
        }
        return 1;

    case SYS_SLS_SET_USER: /* 108 — shell updates session vars itself; no-op here */
        return 0;

    case 109: /* SYS_SLS_SECURE_SEAL */
        return sys_sls_secure_seal((struct SLSSealRequest*)arg);

    // ── Phase 1: Object Catalog (110–119) ─────────────────────────────────────
    case SYS_SLS_VALLOC:
        return sys_sls_valloc((struct SLSVallocRequest*)arg);
    case SYS_SLS_VFREE:
        return sys_sls_vfree((const char*)arg);
    case SYS_SLS_OBJ_LIST:
        sys_sls_obj_list(); return 0;
    case SYS_SLS_OBJ_STAT:
        return sys_sls_obj_stat((const char*)arg);
    case SYS_SLS_ROLE_SET:
        return sys_sls_role_set((struct SLSRoleRequest*)arg);
    case SYS_SLS_GRANT: {
        /* shell passes uint64_t[2] = {req_ptr, is_grant} */
        uint64_t* a = (uint64_t*)arg;
        return sys_sls_grant((struct SLSGrantRequest*)(uintptr_t)a[0], (int)a[1]);
    }
    case SYS_SLS_REVOKE: {
        uint64_t* a = (uint64_t*)arg;
        return sys_sls_grant((struct SLSGrantRequest*)(uintptr_t)a[0], 0);
    }
    case SYS_SLS_SELECT:
        return sys_sls_select((struct SLSRecordRequest*)arg);
    case SYS_SLS_UPDATE:
        return sys_sls_update((struct SLSRecordRequest*)arg);
    case SYS_SLS_INSERT:
        return sys_sls_insert((struct SLSRecordRequest*)arg);

    // ── Phase 3: Transactions (120–123) ───────────────────────────────────────
    case SYS_SLS_TX_BEGIN:
        return sys_sls_tx_begin((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_COMMIT:
        return sys_sls_tx_commit((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_ROLLBACK:
        return sys_sls_tx_rollback((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_RECOVER:
        sys_sls_tx_recover(); return 0;

    // ── Phase 4: Microkernel (130–134) ────────────────────────────────────────
    case SYS_SLS_SVC_LIST:
        sys_sls_svc_list(); return 0;
    case SYS_SLS_SVC_CRASH:
        return sys_sls_svc_crash((const char*)arg);
    case SYS_SLS_SVC_RESTART:
        return sys_sls_svc_restart((const char*)arg);
    case SYS_SLS_IPC_STAT:
        sys_sls_svc_list(); return 0;   /* combined view */
    case SYS_SLS_IPC_POST:
        return sys_sls_ipc_post((struct IPCPostRequest*)arg);

    // ── Phase 5: Storage Tiers (140–142) ──────────────────────────────────────
    case SYS_SLS_TIER_LIST:
        sys_sls_tier_list(); return 0;
    case SYS_SLS_TIER_PROMOTE:
        return sys_sls_tier_promote((const char*)arg);
    case SYS_SLS_TIER_DEMOTE:
        return sys_sls_tier_demote((const char*)arg);

    // ── Phase 6: Schema & Delete (143–145) ────────────────────────────────────
    case SYS_SLS_DELETE:
        return sys_sls_delete((struct SLSRecordRequest*)arg);
    case SYS_SLS_SCHEMA_SET:
        return sys_sls_schema_set((struct SLSSchemaRequest*)arg);
    case SYS_SLS_SCHEMA_SHOW:
        sys_sls_schema_show((const char*)arg); return 0;

    // ── Phase 7: Query Engine (150–151) ───────────────────────────────────────
    case SYS_SLS_QUERY:
        sys_sls_query((const char*)arg); return 0;
    case SYS_SLS_QUERY_SCAN:
        sys_sls_query_scan(); return 0;

    default:
        return 0;
    }
}
