#include "syscall_dispatch.h"
#include "object_catalog.h"
#include "transaction.h"
#include "microkernel.h"
#include "ipc.h"
#include "tier_mgr.h"
#include "query_engine.h"
#include "process.h"
#include "loader.h"
#include "agent.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"
#include "../kernel/secure_api.h"
#include "partition.h"
#include "frame_pool.h"
#include "sql_exec.h"
#include "vecstore.h"   // Vector Store Roadmap Phase 4 -- pulls in ../net/ollama_client.h transitively
#include "rowstore.h"   // Gap Remediation Phase B -- SYS_SLS_ROWSTORE_CREATE_TABLE
#include "vec_join.h"   // Gap Remediation Phase C -- SYS_SLS_VEC_JOIN
#include "vec_index.h"  // Gap Remediation Phase C -- SYS_SLS_VEC_INDEX_CREATE/SEARCH
#include "group_profile.h" // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_GROUP_*
#include "authlist.h"      // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_AUTHLIST_*
#include "security_audit.h" // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_AUDIT_LIST
#include "msgqueue.h"      // Navigator-Parity Gap Roadmap Phase 4 -- SYS_SLS_MQ_*
#include "../net/net.h"    // Navigator-Parity Gap Roadmap Phase 5c -- SYS_SLS_NET_STATUS
#include "database.h"      // Database Namespace & Access Roadmap Phase 4 -- SYS_SLS_DATABASE_*

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

    // ── Ring-3 User IPC (166–168) ─────────────────────────────────────────────
    case SYS_SLS_IPC_BIND: {
        /* arg = (void*)(uintptr_t)port  — bind calling process to user port */
        uint16_t port = (uint16_t)(uintptr_t)arg;
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)(uint32_t)ipc_user_bind(port, caller_pid);
    }
    case SYS_SLS_IPC_SEND: {
        /* arg = pointer to IPCUserSendReq */
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)(uint32_t)ipc_user_send(
            (const struct IPCUserSendReq*)arg, caller_pid);
    }
    case SYS_SLS_IPC_RECV: {
        /* arg = pointer to IPCUserRecvReq (port field set by caller) */
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)ipc_user_recv((struct IPCUserRecvReq*)arg, caller_pid);
    }

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

    // ── Phase B: Processes (160–164) ───────────────────────────────────────
    case SYS_SLS_PROC_CREATE:
        return process_create((struct ProcCreateRequest*)arg);
    case SYS_SLS_PROC_KILL:
        process_kill((uint32_t)(uintptr_t)arg); return 0;
    case SYS_SLS_PROC_LIST:
        sys_sls_proc_list(); return 0;
    case SYS_SLS_EXIT:
        process_exit((uint32_t)(uintptr_t)arg); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 4: hold/release/priority (245-247) ──
    case SYS_SLS_PROC_HOLD:
        return process_hold((uint32_t)(uintptr_t)arg) == 0 ? 0 : 1;
    case SYS_SLS_PROC_RELEASE:
        return process_release((uint32_t)(uintptr_t)arg) == 0 ? 0 : 1;
    case SYS_SLS_PROC_PRIORITY_SET:
        return sys_sls_proc_priority_set((struct SLSProcPrioritySetRequest*)arg);

    // ── Ring-3 debug output (165) ───────────────────────────────────────
    case 165: /* SYS_SLS_SERIAL_WRITE */
        if (arg) kernel_serial_print((const char*)arg);
        return 0;

    // ── Phase C: Loader (170–171) ───────────────────────────────────────
    case SYS_SLS_LOAD:
        return sys_sls_load((const char*)arg,
                            kernel_get_current_thread_id());
    case SYS_SLS_UPLOAD_BINARY:
        return sys_sls_upload_binary((struct SLSUploadRequest*)arg);
    case 172: /* loader_list */
        loader_list(); return 0;
    // Gap Remediation Phase G: struct-based, replacing the old raw-string,
    // no-output-to-caller shape (see loader.h's own comment).
    case SYS_SLS_SIMI_INFO:
        return sys_sls_simi_info((struct SLSSimiInfoRequest*)arg);
    case SYS_SLS_PROGRAM_SPAWN:
        return program_load((const char*)arg,
                            kernel_get_current_thread_id());

    // ── Phase D: Web App (180–182) ───────────────────────────────────────
    case SYS_SLS_WEBAPP_SET:
        return sys_sls_webapp_set((struct WebAppSetRequest*)arg);
    case SYS_SLS_WEBAPP_LIST:
        sys_sls_webapp_list(arg ? (const char*)arg : "*"); return 0;

    // ── Phase G: Token Auth (190–192) ─────────────────────────────────────
    case SYS_SLS_AUTH_CREATE:
        return auth_create_token((struct AuthCreateRequest*)arg, 0);
    case SYS_SLS_AUTH_LIST:
        sys_sls_auth_list(); return 0;
    case SYS_SLS_AUTH_REVOKE:
        return auth_revoke_by_email((const char*)arg);

    // ── Phase H: AI Agents (200–207) ──────────────────────────────────────────
    case SYS_SLS_AGENT_CREATE:
        return sys_sls_agent_create((struct AgentCreateRequest*)arg);
    case SYS_SLS_AGENT_RUN:
        return sys_sls_agent_run((struct AgentRunRequest*)arg);
    case SYS_SLS_AGENT_STATUS:
        sys_sls_agent_status((const char*)arg); return 0;
    case SYS_SLS_AGENT_KILL:
        return sys_sls_agent_kill((const char*)arg);
    case SYS_SLS_AGENT_LIST:
        sys_sls_agent_list(); return 0;
    case SYS_SLS_WORKFLOW_CREATE:
        return sys_sls_workflow_create((struct WorkflowCreateRequest*)arg);
    case SYS_SLS_WORKFLOW_RUN:
        return sys_sls_workflow_run((struct WorkflowRunRequest*)arg);
    case SYS_SLS_WORKFLOW_STATUS:
        sys_sls_workflow_status((const char*)arg); return 0;
    case SYS_SLS_AGENT_SCHEDULE:
        return sys_sls_agent_schedule((struct AgentScheduleRequest*)arg);

    // ── Phase 8: LPAR groundwork (210–212) ────────────────────────────────────
    case SYS_SLS_PARTITION_CREATE:
        return sys_sls_partition_create((struct SLSPartitionCreateRequest*)arg);
    case SYS_SLS_PARTITION_ASSIGN:
        return sys_sls_partition_assign((struct SLSPartitionAssignRequest*)arg);
    case SYS_SLS_PARTITION_LIST:
        sys_sls_partition_list(); return 0;

    // ── Phase 13: LPAR physical memory quotas (213) ─────────────────────────
    case SYS_SLS_PARTITION_QUOTA_SET:
        return sys_sls_partition_quota_set((struct SLSPartitionQuotaSetRequest*)arg);

    // ── Gap Remediation Phase F: sys_sls_partition_quota_list() was fully
    // implemented since Phase 13 but never got a syscall number or a
    // dispatcher case (217) -- fixed here.
    case SYS_SLS_PARTITION_QUOTA_LIST:
        sys_sls_partition_quota_list(); return 0;

    // ── Phase 14: LPAR partition lifecycle (214-216) ────────────────────────
    case SYS_SLS_PARTITION_DESTROY:
        return sys_sls_partition_destroy((uint32_t)(uintptr_t)arg);
    case SYS_SLS_PARTITION_PAUSE:
        return sys_sls_partition_pause((uint32_t)(uintptr_t)arg);
    case SYS_SLS_PARTITION_RESUME:
        return sys_sls_partition_resume((uint32_t)(uintptr_t)arg);

    // ── Multi-Node Partition Scaling Roadmap, Phase 6: cold migration (218) ──
    case SYS_SLS_PARTITION_MIGRATE:
        return sys_sls_partition_migrate((struct SLSPartitionMigrateRequest*)arg);

    // ── Phase 22: SQL engine, live at last (220) ────────────────────────────
    // The first dispatch-reachable entry point into Phases 19-22's SQL
    // engine -- sql_execute() was previously callable only from its own
    // host tests. Autocommit only (see sql_exec.h) -- caller_uid travels
    // inside the request struct, matching SLSVallocRequest's own
    // owner_uid-embedded-in-the-request convention, since do_syscall()
    // itself has no uid context of its own to supply.
    case SYS_SLS_SQL_EXECUTE:
        return sys_sls_sql_execute((struct SLSSqlRequest*)arg);

    // ── Vector Store Roadmap Phase 4: make it live (221-224) ────────────────
    // The first dispatch-reachable entry points into Phases 1-3's vector
    // store + Ollama embedding client -- vecstore_create_collection()/
    // vecstore_insert()/vecstore_search() and ollama_embed() were
    // previously callable only from their own host tests, exactly the gap
    // SYS_SLS_SQL_EXECUTE closed for the SQL engine above. caller_uid
    // travels inside each request struct (see vecstore.h's own comment),
    // same convention as SYS_SLS_SQL_EXECUTE.
    case SYS_SLS_VEC_CREATE:
        return sys_sls_vec_create((struct SLSVecCreateRequest*)arg);
    // ── VectorStore Gap Analysis §1.3 follow-on: opt-in external_id
    // uniqueness (268) -- see vecstore.h's own comment on this syscall. ────
    case SYS_SLS_VEC_SET_UNIQUE:
        return sys_sls_vec_set_unique((struct SLSVecSetUniqueRequest*)arg);
    case SYS_SLS_VEC_INSERT:
        return sys_sls_vec_insert((struct SLSVecInsertRequest*)arg);
    case SYS_SLS_VEC_EMBED_INSERT:
        return sys_sls_vec_embed_insert((struct SLSVecEmbedInsertRequest*)arg);
    case SYS_SLS_VEC_SEARCH:
        return sys_sls_vec_search((struct SLSVecSearchRequest*)arg);

    // ── Gap Remediation Phase B: the live path rowstore_create_table() ──────
    // never had (225) -- see rowstore.h's own comment on this syscall.
    case SYS_SLS_ROWSTORE_CREATE_TABLE:
        return sys_sls_rowstore_create_table((struct SLSRowstoreCreateTableRequest*)arg);

    // ── Gap Remediation Phase C: live surfaces vec_join_resolve() (226) and
    // the HNSW index (227-228) never had -- see vec_join.h's/vec_index.h's
    // own comments on these syscalls.
    case SYS_SLS_VEC_JOIN:
        return sys_sls_vec_join((struct SLSVecJoinRequest*)arg);
    case SYS_SLS_VEC_INDEX_CREATE:
        return sys_sls_vec_index_create((struct SLSVecIndexCreateRequest*)arg);
    case SYS_SLS_VEC_INDEX_SEARCH:
        return sys_sls_vec_index_search((struct SLSVecIndexSearchRequest*)arg);
    case SYS_SLS_VEC_LIST:
        sys_sls_vec_list();
        return 0;
    case SYS_SLS_VEC_INDEX_LIST:
        sys_sls_vec_index_list();
        return 0;

    // ── VectorStore Interface Roadmap Phase 1: deletion ─────────────────────
    case SYS_SLS_VEC_DELETE:
        return sys_sls_vec_delete((struct SLSVecDeleteRequest*)arg);
    case SYS_SLS_VEC_INDEX_DROP:
        return sys_sls_vec_index_drop((struct SLSVecIndexDropRequest*)arg);

    // ── VectorStore Interface Roadmap Phase 2: semantic (embed-then-search) ──
    case SYS_SLS_VEC_EMBED_SEARCH:
        return sys_sls_vec_embed_search((struct SLSVecEmbedSearchRequest*)arg);
    case SYS_SLS_VEC_INDEX_EMBED_SEARCH:
        return sys_sls_vec_index_embed_search((struct SLSVecIndexEmbedSearchRequest*)arg);

    // ── VectorStore Interface Roadmap Phase 3: rebuild/backfill ─────────────
    case SYS_SLS_VEC_INDEX_REBUILD:
        return sys_sls_vec_index_rebuild((struct SLSVecIndexRebuildRequest*)arg);

    // ── Navigator-Parity Gap Roadmap Phase 3: group profiles (237-239) ──────
    case SYS_SLS_GROUP_CREATE:
        return sys_sls_group_create((struct SLSGroupCreateRequest*)arg);
    case SYS_SLS_GROUP_ADD_MEMBER:
        return sys_sls_group_add_member((struct SLSGroupAddMemberRequest*)arg);
    case SYS_SLS_GROUP_LIST:
        group_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 3: authorization lists (240-242) ─
    case SYS_SLS_AUTHLIST_CREATE:
        return sys_sls_authlist_create((struct SLSAuthListCreateRequest*)arg);
    case SYS_SLS_AUTHLIST_GRANT:
        return sys_sls_authlist_grant((struct SLSAuthListGrantRequest*)arg);
    case SYS_SLS_AUTHLIST_CHECK:
        return sys_sls_authlist_check((struct SLSAuthListCheckRequest*)arg);
    case SYS_SLS_AUTHLIST_LIST:
        authlist_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 3: security audit log (243) ──────
    case SYS_SLS_AUDIT_LIST:
        sys_sls_audit_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 4: message queues (248-251) ──────
    case SYS_SLS_MQ_CREATE:
        return sys_sls_mq_create((struct SLSMQCreateRequest*)arg);
    case SYS_SLS_MQ_SEND:
        return sys_sls_mq_send((struct SLSMQSendRequest*)arg);
    case SYS_SLS_MQ_RECEIVE:
        return sys_sls_mq_receive((struct SLSMQReceiveRequest*)arg);
    case SYS_SLS_MQ_LIST:
        mq_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 5c: network/disk status (252-253) ──
    case SYS_SLS_NET_STATUS:
        sys_sls_net_status(); return 0;
    case SYS_SLS_DISK_STATUS:
        sys_sls_disk_status(); return 0;

    // ── SQL Feature-Parity Roadmap, Phase 8 follow-on: schema import/export
    // (254-255) -- see sql_exec.h's own comment on these two syscalls. ───────
    case SYS_SLS_SCHEMA_EXPORT:
        return sys_sls_schema_export((struct SLSSchemaExportRequest*)arg);
    case SYS_SLS_SCHEMA_IMPORT:
        return sys_sls_schema_import((struct SLSSchemaImportRequest*)arg);

    // ── VectorStore Interface Roadmap follow-on: collection/index
    // definition export/import (256-257) -- see vec_index.h's own comment
    // on these two syscalls. ─────────────────────────────────────────────
    case SYS_SLS_VEC_SCHEMA_EXPORT:
        return sys_sls_vec_schema_export((struct SLSVecSchemaExportRequest*)arg);
    case SYS_SLS_VEC_SCHEMA_IMPORT:
        return sys_sls_vec_schema_import((struct SLSVecSchemaImportRequest*)arg);

    // ─── VectorStore Interface Roadmap follow-on: bulk vector data
    // export/import (258-259) -- see vecstore.h's own comment on these two
    // syscalls. ─────────────────────────────────────────────────────────
    case SYS_SLS_VEC_DATA_EXPORT:
        return sys_sls_vec_data_export((struct SLSVecDataExportRequest*)arg);
    case SYS_SLS_VEC_DATA_IMPORT:
        return sys_sls_vec_data_import((struct SLSVecDataImportRequest*)arg);

    // ─── Database Namespace & Access Roadmap Phase 4 (260-265) ──────────────
    case SYS_SLS_DATABASE_CREATE:
        return sys_sls_database_create((struct SLSDatabaseCreateRequest*)arg);
    case SYS_SLS_DATABASE_DROP:
        return sys_sls_database_drop((struct SLSDatabaseDropRequest*)arg);
    case SYS_SLS_DATABASE_LIST:
        database_list(); return 0;
    case SYS_SLS_DATABASE_GRANT_UID:
        return sys_sls_database_grant_uid((struct SLSDatabaseGrantUidRequest*)arg);
    case SYS_SLS_DATABASE_GRANT_GROUP:
        return sys_sls_database_grant_group((struct SLSDatabaseGrantGroupRequest*)arg);
    // ── Database Gap Analysis §2.1: revoke (266-267) ─────────────────────
    case SYS_SLS_DATABASE_REVOKE_UID:
        return sys_sls_database_revoke_uid((struct SLSDatabaseRevokeUidRequest*)arg);
    case SYS_SLS_DATABASE_REVOKE_GROUP:
        return sys_sls_database_revoke_group((struct SLSDatabaseRevokeGroupRequest*)arg);
    case SYS_SLS_DATABASE_CHECK:
        return sys_sls_database_check((struct SLSDatabaseCheckRequest*)arg);

    default:
        return 0;
    }
}
