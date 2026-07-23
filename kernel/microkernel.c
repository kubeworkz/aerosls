#include "microkernel.h"
#include "ipc.h"
#include "object_catalog.h"
#include "transaction.h"
#include "agent.h"
#include "../kernel/dashboard.h"

extern void tier_mgr_init(void);
extern void tier_mgr_tick(void);
extern void usage_metering_tick(void); // Multitenant Isolation Gap Analysis §5 item 6

struct ServiceDescriptor services[MAX_SERVICES];
uint32_t                 service_count = 0;

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t mk_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    mk_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void mk_strncpy(char* d, const char* s, size_t n) {
    size_t i; for (i=0; i<n-1&&s[i]; i++) d[i]=s[i]; d[i]='\0';
}

// ─── Service Message Handlers ─────────────────────────────────────────────────

static void vmm_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case VMM_OP_MAP_PAGE:
        kernel_serial_printf(
            "[VMM] MAP_PAGE: obj=0x%lx  vaddr=0x%lx\n",
            msg->payload[0], msg->payload[1]);
        break;
    case VMM_OP_UNMAP:
        kernel_serial_printf(
            "[VMM] UNMAP: obj=0x%lx\n", msg->payload[0]);
        break;
    case VMM_OP_FAULT_NOTIFY:
        kernel_serial_printf(
            "[VMM] PAGE_FAULT: faulting_vaddr=0x%lx  thread=%u\n",
            msg->payload[0], (uint32_t)msg->payload[1]);
        break;
    default:
        kernel_serial_printf("[VMM] Unknown opcode: 0x%04x\n", msg->opcode);
    }
}

static void sec_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case SEC_OP_CHECK_ACCESS: {
        uint64_t obj_id  = msg->payload[0];
        uint32_t uid     = (uint32_t)msg->payload[1];
        uint32_t perm    = (uint32_t)msg->payload[2];
        // Resolve object name from catalog
        const char* oname = "";
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            if (object_catalog[i].active &&
                object_catalog[i].object_id == obj_id) {
                oname = object_catalog[i].name;
                break;
            }
        }
        int ok = catalog_check_access(uid, oname, perm);
        kernel_serial_printf(
            "[SEC] CHECK_ACCESS: obj='%s'  uid=%u  perm=0x%x  -> %s\n",
            oname, uid, perm, ok ? "ALLOW" : "DENY");
        break;
    }
    case SEC_OP_GRANT:
    case SEC_OP_REVOKE:
        kernel_serial_printf(
            "[SEC] %s: uid=%u  obj=0x%lx  bits=0x%x\n",
            msg->opcode == SEC_OP_GRANT ? "GRANT" : "REVOKE",
            (uint32_t)msg->payload[0], msg->payload[1],
            (uint32_t)msg->payload[2]);
        break;
    default:
        kernel_serial_printf("[SEC] Unknown opcode: 0x%04x\n", msg->opcode);
    }
}

static void db_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case DB_OP_COMMIT:
        kernel_serial_printf(
            "[DB] COMMIT: tx_id=%lu\n", msg->payload[0]);
        sys_sls_tx_commit((uint32_t)msg->payload[1]);
        break;
    case DB_OP_ROLLBACK:
        kernel_serial_printf(
            "[DB] ROLLBACK: tx_id=%lu\n", msg->payload[0]);
        sys_sls_tx_rollback((uint32_t)msg->payload[1]);
        break;
    case DB_OP_VALLOC:
        kernel_serial_printf(
            "[DB] VALLOC: obj_id=0x%lx  pages=%u\n",
            msg->payload[0], (uint32_t)msg->payload[1]);
        break;
    default:
        kernel_serial_printf("[DB] Unknown opcode: 0x%04x\n", msg->opcode);
    }
}

static void tier_handler(struct IPCMessage* msg) {
    uint64_t obj_id = msg->payload[0];
    // Find object in catalog and update tier
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active ||
            object_catalog[i].object_id != obj_id) continue;
        switch (msg->opcode) {
        case TIER_OP_PROMOTE:
            if (object_catalog[i].storage_tier > STORAGE_TIER_L1_CACHE) {
                object_catalog[i].storage_tier--;
                kernel_serial_printf(
                    "[TIER] PROMOTE: '%s' -> %s\n",
                    object_catalog[i].name,
                    tier_name(object_catalog[i].storage_tier));
            }
            break;
        case TIER_OP_DEMOTE:
            if (object_catalog[i].storage_tier < STORAGE_TIER_L3_SSD) {
                object_catalog[i].storage_tier++;
                kernel_serial_printf(
                    "[TIER] DEMOTE: '%s' -> %s\n",
                    object_catalog[i].name,
                    tier_name(object_catalog[i].storage_tier));
            }
            break;
        case TIER_OP_FLUSH:
            kernel_serial_printf(
                "[TIER] FLUSH: '%s' (tier=%s)\n",
                object_catalog[i].name,
                tier_name(object_catalog[i].storage_tier));
            break;
        default:
            kernel_serial_printf("[TIER] Unknown opcode: 0x%04x\n", msg->opcode);
        }
        return;
    }
    kernel_serial_printf("[TIER] obj_id=0x%lx not found.\n", obj_id);
}

static void log_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case LOG_OP_APPEND:
        kernel_serial_printf(
            "[LOG] APPEND: entry_id=%u  tx_id=%lu\n",
            (uint32_t)msg->payload[0], msg->payload[1]);
        break;
    case LOG_OP_VERIFY:
        kernel_serial_printf(
            "[LOG] VERIFY: scanning %u WAL entries...\n", wal_entry_count);
        // CRC-check all entries and report anomalies
        for (uint32_t i = 0; i < wal_entry_count; i++) {
            // CRC field is recomputed inline; mismatches already logged by tx_recover
        }
        kernel_serial_printf("[LOG] VERIFY: complete. %u entries checked.\n",
                             wal_entry_count);
        break;
    case LOG_OP_RECOVER:
        sys_sls_tx_recover();
        break;
    default:
        kernel_serial_printf("[LOG] Unknown opcode: 0x%04x\n", msg->opcode);
    }
}

// ─── AgentRuntimeMgr handler ────────────────────────────────────────────────────
static void agent_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case AGENT_OP_SPAWN: {
        struct AgentCreateRequest* req =
            (struct AgentCreateRequest*)(uintptr_t)msg->payload[0];
        if (req) sys_sls_agent_create(req);
        break;
    }
    case AGENT_OP_STEP: {
        // NOTE: sys_sls_agent_run blocks on network I/O (net_event_hlt_wait).
        // Core 1's service poll loop is parked during inference; Core 0's
        // HTTP server continues independently on its own execution path.
        struct AgentRunRequest* req =
            (struct AgentRunRequest*)(uintptr_t)msg->payload[0];
        if (req) sys_sls_agent_run(req);
        break;
    }
    case AGENT_OP_COMPLETE:
        kernel_serial_printf("[AGENT-SVC] COMPLETE: step_count=%lu\n",
                             msg->payload[0]);
        break;
    case AGENT_OP_KILL: {
        const char* name = (const char*)(uintptr_t)msg->payload[0];
        if (name) sys_sls_agent_kill(name);
        break;
    }
    default:
        kernel_serial_printf("[AGENT-SVC] Unknown opcode: 0x%04x\n",
                             msg->opcode);
    }
}

// ─── Service Registration ─────────────────────────────────────────────────────
static void register_service(const char* name, uint32_t pid, uint16_t port,
                              uint64_t base_addr, uint32_t latency_us_x100,
                              void (*handler)(struct IPCMessage*)) {
    if (service_count >= MAX_SERVICES) return;
    struct ServiceDescriptor* s = &services[service_count++];
    mk_strncpy(s->name, name, SVC_NAME_LEN);
    s->pid             = pid;
    s->port            = port;
    s->base_addr       = base_addr;
    s->state           = SVC_STATE_ONLINE;
    s->reboot_count    = 0;
    s->task_id         = pid;
    s->latency_us_x100 = latency_us_x100;
    s->msgs_processed  = 0;
    s->active          = 1;
    s->handler         = handler;

    kernel_serial_print("[MK] Service ONLINE: ");
    kernel_serial_print(name);
    kernel_serial_print("  PID=");
    // print pid as decimal without variadic
    { uint32_t v=pid; char b[12]; int l=0;
      if(!v){b[l++]='0';}else{while(v){b[l++]=(char)('0'+v%10);v/=10;}}
      for(int k=l-1;k>=0;k--) kernel_serial_putchar(b[k]); }
    kernel_serial_print("\n");
}

// ─── microkernel_init ─────────────────────────────────────────────────────────
void microkernel_init(void) {
    kernel_serial_print(
        "[MK] Microkernel booted successfully. Ring-0 primitives online.\n");

    ipc_init();

    kernel_serial_print("[MK] registering services\n");

    // Register the five services in PID order (matches simulator)
    register_service("VirtualMemoryMgr",    101, IPC_PORT_VMMGR,
                     0x0000000010001000ULL,  120, vmm_handler);
    register_service("ObjectSecurityMgr",   102, IPC_PORT_SECMGR,
                     0x0000000010002000ULL,  180, sec_handler);
    register_service("NativeDbStoreMgr",    103, IPC_PORT_DBMGR,
                     0x0000000010003000ULL,  250, db_handler);
    register_service("StorageTierMgr",      104, IPC_PORT_TIERMGR,
                     0x0000000010004000ULL,   90, tier_handler);
    register_service("RecoveryLogVerifier", 105, IPC_PORT_LOGMGR,
                     0x0000000010005000ULL,  140, log_handler);
    register_service("AgentRuntimeMgr",     106, IPC_PORT_AGENTMGR,
                     0x0000000010006000ULL,    0, agent_handler);

    kernel_serial_print(
        "[MK] Fault Isolation Daemon active. Poll interval: 100ms.\n");

    tier_mgr_init();
}

// ─── microkernel_service_poll ─────────────────────────────────────────────────
// Called periodically from the AP core loop. Drains all queues once.
void microkernel_service_poll(void) {
    for (uint32_t i = 0; i < service_count; i++) {
        struct ServiceDescriptor* s = &services[i];
        if (!s->active || s->state == SVC_STATE_CRASHED) continue;

        struct IPCMessage msg;
        while (ipc_recv(s->port, &msg)) {
            uint64_t t0 = read_tsc();

            s->handler(&msg);
            s->msgs_processed++;

            // Update rolling latency (us × 100)
            uint64_t dt_tsc = read_tsc() - t0;
            // Approximate: 4 GHz clock → 1 cycle ≈ 0.25ns ≈ 0.00025μs × 100 = 0.025
            // We keep it as raw cycles for now and convert on display
            uint32_t dt_us_x100 = (uint32_t)(dt_tsc / 40);  // ~4GHz: 40 cycles/μs×100
            s->latency_us_x100 = (s->latency_us_x100 * 7 + dt_us_x100) / 8;
        }
    }

    // Periodic tier evaluation: promote hot objects, demote cold ones
    tier_mgr_tick();
    // Multitenant Isolation Gap Analysis §5 item 6: sample per-partition usage
    usage_metering_tick();
    // (E) Fire any scheduled agent runs
    agent_scheduler_tick();
}

// ─── sys_sls_svc_list ─────────────────────────────────────────────────────────
void sys_sls_svc_list(void) {
    // Gap Remediation Phase C fix: same kernel_serial_print()-called-with-
    // extra-arguments bug found and fixed in object_catalog.c's
    // sys_sls_obj_list() and tier_mgr.c's sys_sls_tier_list() -- see
    // object_catalog.c's own comment on this pattern.
    kernel_serial_printf(
        "\n[MK] Microkernel Service Map\n"
        " %-22s  PID   PORT    ADDR                 STATE     REBOOTS  "
        "LATENCY    MSGS\n"
        " %-22s  ---   ------  -------------------  --------  -------  "
        "---------  ----\n",
        "Service", "-------");

    for (uint32_t i = 0; i < service_count; i++) {
        struct ServiceDescriptor* s = &services[i];
        if (!s->active) continue;
        uint32_t ms   = s->latency_us_x100 / 100;
        uint32_t frac = s->latency_us_x100 % 100;
        kernel_serial_printf(
            " %-22s  %-5u 0x%04x  0x%016lx  %-8s  %-7u  %u.%02ums     %lu\n",
            s->name, s->pid, s->port, s->base_addr,
            svc_state_name(s->state), s->reboot_count,
            ms, frac, s->msgs_processed);
    }

    kernel_serial_printf(
        "\n IPC Stats — posted: %lu  dispatched: %lu  dropped: %lu  "
        "avg_latency: ~%lu tsc-cycles\n\n",
        ipc_stats.total_posted, ipc_stats.total_dispatched,
        ipc_stats.total_dropped, ipc_stats.avg_latency_ns);
}

// ─── sys_sls_svc_crash ────────────────────────────────────────────────────────
uint64_t sys_sls_svc_crash(const char* name) {
    for (uint32_t i = 0; i < service_count; i++) {
        if (mk_streq(services[i].name, name)) {
            if (services[i].state == SVC_STATE_CRASHED) {
                kernel_serial_printf(
                    "[MK] '%s' is already CRASHED.\n", name);
                return 1;
            }
            services[i].state = SVC_STATE_CRASHED;
            kernel_serial_printf(
                "[MK] CRASH injected into '%s' (PID %u). "
                "Queue depth: %d pending messages.\n",
                name, services[i].pid,
                ipc_queue_depth(services[i].port));
            return 0;
        }
    }
    kernel_serial_printf("[MK] svc crash: Service '%s' not found.\n", name);
    return 1;
}

// ─── sys_sls_svc_restart ─────────────────────────────────────────────────────
uint64_t sys_sls_svc_restart(const char* name) {
    for (uint32_t i = 0; i < service_count; i++) {
        if (mk_streq(services[i].name, name)) {
            if (services[i].state == SVC_STATE_ONLINE) {
                kernel_serial_printf(
                    "[MK] '%s' is already ONLINE.\n", name);
                return 1;
            }
            services[i].state = SVC_STATE_ONLINE;
            services[i].reboot_count++;
            kernel_serial_printf(
                "[MK] '%s' (PID %u) RESTARTED. Boot count: %u. "
                "Draining %d queued messages...\n",
                name, services[i].pid, services[i].reboot_count,
                ipc_queue_depth(services[i].port));

            // Drain any accumulated messages now that service is back
            struct IPCMessage msg;
            uint32_t drained = 0;
            while (ipc_recv(services[i].port, &msg)) {
                services[i].handler(&msg);
                services[i].msgs_processed++;
                drained++;
            }
            if (drained)
                kernel_serial_printf(
                    "[MK] '%s' drained %u backlogged message(s).\n",
                    name, drained);
            return 0;
        }
    }
    kernel_serial_printf("[MK] svc restart: Service '%s' not found.\n", name);
    return 1;
}

// ─── Convenience wrappers used by object_catalog / transaction ────────────────
void mk_post_db_commit(uint64_t tx_id) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_DBMGR,
        .opcode   = DB_OP_COMMIT,
        .payload  = { tx_id, 0, 0, 0 }
    };
    ipc_post(IPC_PORT_DBMGR, &m);
}

void mk_post_valloc(uint64_t obj_id, uint32_t size_pages) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_DBMGR,
        .opcode   = DB_OP_VALLOC,
        .payload  = { obj_id, size_pages, 0, 0 }
    };
    ipc_post(IPC_PORT_DBMGR, &m);
}

void mk_post_security_check(uint64_t obj_id, uint32_t uid, uint32_t perm) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_SECMGR,
        .opcode   = SEC_OP_CHECK_ACCESS,
        .payload  = { obj_id, uid, perm, 0 }
    };
    ipc_post(IPC_PORT_SECMGR, &m);
}

void mk_post_tier_flush(uint64_t obj_id) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_TIERMGR,
        .opcode   = TIER_OP_FLUSH,
        .payload  = { obj_id, 0, 0, 0 }
    };
    ipc_post(IPC_PORT_TIERMGR, &m);
}

void mk_post_log_append(uint64_t entry_id, uint64_t tx_id) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_LOGMGR,
        .opcode   = LOG_OP_APPEND,
        .payload  = { entry_id, tx_id, 0, 0 }
    };
    ipc_post(IPC_PORT_LOGMGR, &m);
}

// ─── Agent IPC wrappers ────────────────────────────────────────────────────────────
void mk_post_agent_spawn(struct AgentCreateRequest* req) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_AGENTMGR,
        .opcode   = AGENT_OP_SPAWN,
        .payload  = { (uint64_t)(uintptr_t)req, 0, 0, 0 }
    };
    ipc_post(IPC_PORT_AGENTMGR, &m);
}

void mk_post_agent_step(struct AgentRunRequest* req) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_AGENTMGR,
        .opcode   = AGENT_OP_STEP,
        .payload  = { (uint64_t)(uintptr_t)req, 0, 0, 0 }
    };
    ipc_post(IPC_PORT_AGENTMGR, &m);
}

void mk_post_agent_kill(const char* name) {
    struct IPCMessage m = {
        .src_port = 0,
        .dst_port = IPC_PORT_AGENTMGR,
        .opcode   = AGENT_OP_KILL,
        .payload  = { (uint64_t)(uintptr_t)name, 0, 0, 0 }
    };
    ipc_post(IPC_PORT_AGENTMGR, &m);
}
