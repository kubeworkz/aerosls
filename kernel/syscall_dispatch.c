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
#include "service_registry.h"   // Orchestration Plan Phase 4
#include "workload.h"            // Orchestration Plan Phase 5
#include "frame_pool.h"
#include "storage_quota.h"   // Storage Isolation Roadmap Phase 1 -- SYS_SLS_PARTITION_STORAGE_QUOTA_SET/LIST
#include "../net/tcp_quota.h" // Network Fairness Phase 2 -- SYS_SLS_PARTITION_CONN_QUOTA_SET/LIST
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
#include "tenant.h"        // Multitenant Isolation Gap Analysis §5 item 1 -- SYS_SLS_TENANT_*
#include "usage_metering.h" // Multitenant Isolation Gap Analysis §5 item 6 -- SYS_SLS_USAGE_REPORT
#include "../net/consensus.h" // Multi-Node Partition Scaling Roadmap Phase 7 addendum -- SYS_SLS_CLUSTER_INIT/STATUS
#include "cap.h"            // Seed Kernel Phase 1 -- SYS_SLS_CAP_* (289-297)

// ─── spawn owner-uid resolution ──────────────────────────────────────────────
// The owner_uid threaded into program_spawn*()/program_load()/sys_sls_load()
// decides the child's catalog-authority check AND which partition it lands
// in (partition_get_for_uid()). These used kernel_get_current_thread_id(),
// which returns the microkernel task-table id — 0 for every ring-3 caller —
// so every ring-3-spawned child was silently owned by the kernel (uid 0:
// always-passing authority, PARTITION_SYSTEM placement), the same gap the
// HTTP path's req_uid threading already closed for /api/program/spawn. The
// real caller identity is the currently-executing process's owner_uid;
// kernel context (no ring-3 process) keeps the historical uid 0.
static uint32_t spawn_owner_uid(void) {
    struct ProcessDescriptor* cur = process_find_current();
    return cur ? cur->owner_uid : 0;
}

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
        return sys_sls_load((const char*)arg, spawn_owner_uid());
    case SYS_SLS_UPLOAD_BINARY:
        return sys_sls_upload_binary((struct SLSUploadRequest*)arg);
    case 172: /* loader_list */
        loader_list(); return 0;
    // Gap Remediation Phase G: struct-based, replacing the old raw-string,
    // no-output-to-caller shape (see loader.h's own comment).
    case SYS_SLS_SIMI_INFO:
        return sys_sls_simi_info((struct SLSSimiInfoRequest*)arg);
    case SYS_SLS_PROGRAM_SPAWN:
        return program_load((const char*)arg, spawn_owner_uid());
    // Seed Kernel Phase 1.5: NON-BLOCKING spawn — the child is queued with
    // a synthetic ring-3 context and the spawn returns immediately, so two
    // processes can be live at once (the blocking-cap_recv overlap test).
    case SYS_SLS_PROGRAM_SPAWN_NB:
        return program_spawn_nb((const char*)arg, spawn_owner_uid());
    // Phase 1.5 (gated async spawn): child starts PROC_HELD — the spawner
    // provisions its channel first, then releases it (SYS_SLS_PROC_RELEASE),
    // closing the spawn-then-provision timer race.
    case SYS_SLS_PROGRAM_SPAWN_NB_HELD:
        return program_spawn_nb_held((const char*)arg, spawn_owner_uid());

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

    // ── Multi-Node Partition Scaling Roadmap, Phase 7 addendum: operator-
    // driven node identity (279-280) -- closes the "cluster_init() is never
    // called from any real boot path" gap. Single-uint32_t ABI, same shape
    // as SYS_SLS_PARTITION_DESTROY/_PAUSE/_RESUME above. ─────────────────
    case SYS_SLS_CLUSTER_INIT:
        return sys_sls_cluster_init((uint32_t)(uintptr_t)arg);
    case SYS_SLS_CLUSTER_STATUS:
        sys_sls_cluster_status(); return 0;

    // ── Orchestration Plan Phase 4: service registry (281-284) ──────────
    // name -> partition/node/endpoint resolution. The node is NOT stored;
    // RESOLVE derives it from partition_owner_table[] each time, so a
    // lookup after partition_migrate() returns the new node with nothing
    // to invalidate (kernel/service_registry.h).
    case SYS_SLS_SERVICE_REGISTER:
        return sys_sls_service_register((struct SLSServiceRegisterRequest*)arg);
    case SYS_SLS_SERVICE_UNREGISTER:
        return sys_sls_service_unregister((struct SLSServiceRegisterRequest*)arg);
    case SYS_SLS_SERVICE_RESOLVE:
        return sys_sls_service_resolve((struct SLSServiceResolveRequest*)arg);
    case SYS_SLS_SERVICE_LIST:
        sys_sls_service_list(); return 0;

    // ── Orchestration Plan Phase 5: declarative workloads (285-288) ─────
    case SYS_SLS_WORKLOAD_DECLARE:
        return sys_sls_workload_declare((struct SLSWorkloadDeclareRequest*)arg);
    case SYS_SLS_WORKLOAD_DELETE:
        return sys_sls_workload_delete((struct SLSWorkloadDeclareRequest*)arg);
    case SYS_SLS_WORKLOAD_LIST:
        sys_sls_workload_list(); return 0;
    case SYS_SLS_RECONCILE_ENABLE:
        return sys_sls_reconcile_enable((uint32_t)(uintptr_t)arg);

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
    case SYS_SLS_OBJECT_SET_DATABASE:
        return sys_sls_object_set_database((struct SLSSetDatabaseRequest*)arg);
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

    // ─── Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2 (270-271) ──
    case SYS_SLS_TENANT_CREATE:
        return sys_sls_tenant_create((struct SLSTenantCreateRequest*)arg);
    case SYS_SLS_TENANT_LIST:
        tenant_list(); return 0;

    // ─── Multitenant Isolation Gap Analysis §5 item 6 / §7 item 6 (272) ──────
    case SYS_SLS_USAGE_REPORT:
        sys_sls_usage_report(); return 0;

    // ─── Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8 (273-274) ──
    case SYS_SLS_PARTITION_CPU_WEIGHT_SET:
        return sys_sls_partition_cpu_weight_set((struct SLSPartitionCpuWeightSetRequest*)arg);
    case SYS_SLS_PARTITION_CPU_WEIGHT_LIST:
        sys_sls_partition_cpu_weight_list(); return 0;

    // ─── Storage Isolation Roadmap Phase 1 (275-276) ─────────────────────────
    case SYS_SLS_PARTITION_STORAGE_QUOTA_SET:
        return sys_sls_partition_storage_quota_set((struct SLSPartitionStorageQuotaSetRequest*)arg);
    case SYS_SLS_PARTITION_STORAGE_QUOTA_LIST:
        sys_sls_partition_storage_quota_list(); return 0;

    // ─── Network Fairness Phase 2 (277-278) ──────────────────────────────────
    case SYS_SLS_PARTITION_CONN_QUOTA_SET:
        return sys_sls_partition_conn_quota_set((struct SLSPartitionConnQuotaSetRequest*)arg);
    case SYS_SLS_PARTITION_CONN_QUOTA_LIST:
        sys_sls_partition_conn_quota_list(); return 0;

    // ─── Seed Kernel Phase 1: capability layer (289-297) ─────────────────
    // docs/AeroSLS-Capability-SDK-Phase1-Seed-Kernel-Design-v0.1.md. The
    // caller's identity is resolved inside each wrapper via
    // cap_current_pid() (process.c's strong hook), never from a request
    // field -- same trusted-resolution discipline as the IPC cases above.
    case SYS_SLS_CAP_CREATE_MEM:
        return sys_sls_cap_create_mem((struct SLSCapCreateMemRequest*)arg);
    case SYS_SLS_CAP_ARENA_ALLOC:
        return sys_sls_cap_arena_alloc((struct SLSCapArenaAllocRequest*)arg);
    case SYS_SLS_CHAN_CREATE:
        return sys_sls_chan_create((struct SLSCapChanCreateRequest*)arg);
    case SYS_SLS_CAP_SEND:
        return sys_sls_cap_send((struct SLSCapSendRequest*)arg);
    case SYS_SLS_CAP_RECV:
        return sys_sls_cap_recv((struct SLSCapRecvRequest*)arg);
    case SYS_SLS_CAP_REVOKE:
        return sys_sls_cap_revoke((struct SLSCapRevokeRequest*)arg);
    case SYS_SLS_CAP_MAP:
        return sys_sls_cap_map((struct SLSCapMapRequest*)arg);
    case SYS_SLS_CAP_UNMAP:
        return sys_sls_cap_unmap((struct SLSCapUnmapRequest*)arg);
    case SYS_SLS_CAP_LIST:
        sys_sls_cap_list(); return 0;

    // ─── Polyglot Nexus Phase 3: message transport (302-304) ────────────
    // docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md §2.2-2.3. The
    // message syscalls carry an IDL payload plus up to CAP_MSG_MAX_CAPS
    // moved MEM caps; the framing/opcode header is the user library's
    // concern (the kernel envelope is payload-opaque). SYS_SLS_CAP_ARENA_FREE
    // drops a single MEM reference (arena frames return at refcount 0).
    case SYS_SLS_CAP_SEND_MSG:
        return sys_sls_cap_send_msg((struct SLSCapSendMsgRequest*)arg);
    case SYS_SLS_CAP_RECV_MSG:
        return sys_sls_cap_recv_msg((struct SLSCapRecvMsgRequest*)arg);
    case SYS_SLS_CAP_ARENA_FREE:
        return sys_sls_cap_arena_free((struct SLSCapArenaFreeRequest*)arg);

    // ─── Seed Kernel Phase 1, two-party verification (298) ───────────────
    // Child resolves its spawner's pid so it can mint a channel's far-end
    // caps directly into the parent's capability table.
    case SYS_SLS_GETPPID:
        return sys_sls_getppid();

    // ─── Seed Kernel Phase 1.5, immediate wake (300) ─────────────────────
    // Voluntary yield: the entry frame stays on the process's syscall stack
    // and it resumes at .syscall_return on its next schedule — the yield
    // looks to ring-3 like a syscall that took a while. Used to hand the
    // CPU back to an async child so it can exit cleanly.
    case SYS_SLS_YIELD:
        return sys_sls_yield();

    // ─── Phase 3 trampoline capabilities (305-306) ─────────────────────
    // Same-ring, zero-copy, hardware-enforced cross-sidecar calls via MPK.
    case SYS_SLS_TRAMPOLINE_CREATE:
        return sys_sls_trampoline_create((struct SLSTrampolineCreateRequest*)arg);
    case SYS_SLS_TRAMPOLINE_CALL:
        return sys_sls_trampoline_call((struct SLSTrampolineCallRequest*)arg);

    // ─── Phase 5: create_sidecar (310) ─────────────────────────────────
    // Self-hosted boot: accept a packed manifest blob, create a process,
    // map the image, build the initial cap table, write a BIB, and enter
    // ring-3 (async, HELD until parent provisions resources).
    case SYS_SLS_CREATE_SIDECAR:
        return sys_sls_create_sidecar((struct SLSCreateSidecarRequest*)arg);

    // ─── Phase 5 channel transport (311-315) ───────────────────────────
    // The kabi.rs k_chan_* contract over cap_send_msg/cap_recv_msg
    // (kernel/chan.c, docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md
    // §3-§5). These return the transport's POSITIVE CAP_ERR_* codes, not
    // the negative CAP_E* codes of the Phase-3 message syscalls.
    case SYS_SLS_CHAN_WAIT:
        return sys_sls_chan_wait((struct SLSChanWaitRequest*)arg);
    case SYS_SLS_CHAN_RECV:
        return sys_sls_chan_recv((struct SLSChanRecvRequest*)arg);
    case SYS_SLS_CHAN_SEND:
        return sys_sls_chan_send((struct SLSChanSendRequest*)arg);
    case SYS_SLS_CHAN_CLOSE:
        return sys_sls_chan_close((struct SLSChanCloseRequest*)arg);
    case SYS_SLS_CAP_INFO:
        return sys_sls_cap_info((struct SLSCapInfoRequest*)arg);
    case SYS_SLS_IO_IN:
        return sys_sls_io_in((struct SLSIoInRequest*)arg);
    case SYS_SLS_IO_OUT:
        return sys_sls_io_out((struct SLSIoOutRequest*)arg);
    case SYS_SLS_DEV_MMAP:
        return sys_sls_dev_mmap((struct SLSDevMmapRequest*)arg);
    case SYS_SLS_IRQ_BIND:
        return sys_sls_irq_bind((struct SLSIrqBindRequest*)arg);

    default:
        return 0;
    }
}
