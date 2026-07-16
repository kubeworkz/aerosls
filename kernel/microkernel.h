#ifndef MICROKERNEL_H
#define MICROKERNEL_H

#include <stdint.h>
#include "ipc.h"
#include "agent.h"

// ─── Service State ────────────────────────────────────────────────────────────
typedef enum {
    SVC_STATE_ONLINE   = 0,
    SVC_STATE_DEGRADED = 1,
    SVC_STATE_CRASHED  = 2,
} SvcState;

static inline const char* svc_state_name(SvcState s) {
    switch (s) {
        case SVC_STATE_ONLINE:   return "ONLINE";
        case SVC_STATE_DEGRADED: return "DEGRADED";
        case SVC_STATE_CRASHED:  return "CRASHED";
        default:                 return "UNKNOWN";
    }
}

// ─── Service Descriptor ───────────────────────────────────────────────────────
#define SVC_NAME_LEN  32
#define MAX_SERVICES   8

struct ServiceDescriptor {
    char     name[SVC_NAME_LEN];
    uint32_t pid;               // e.g. 101–105
    uint16_t port;              // IPC port (0x1001–0x1005)
    uint64_t base_addr;         // kernel service segment address
    SvcState state;
    uint32_t reboot_count;
    uint32_t task_id;           // scheduler task slot ID
    // Latency stored as integer microseconds × 100 (e.g. 120 = 1.20ms)
    uint32_t latency_us_x100;
    uint64_t msgs_processed;
    uint8_t  active;
    void     (*handler)(struct IPCMessage*);  // message dispatch function
};

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_SVC_LIST    130
#define SYS_SLS_SVC_CRASH   131
#define SYS_SLS_SVC_RESTART 132

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct ServiceDescriptor services[MAX_SERVICES];
extern uint32_t                 service_count;

void     microkernel_init(void);
void     microkernel_service_poll(void);

// Syscall handlers
void     sys_sls_svc_list(void);
uint64_t sys_sls_svc_crash(const char* name);
uint64_t sys_sls_svc_restart(const char* name);

// Called by other subsystems to route work to the correct service
void     mk_post_db_commit(uint64_t tx_id);
void     mk_post_valloc(uint64_t obj_id, uint32_t size_pages);
void     mk_post_security_check(uint64_t obj_id, uint32_t uid, uint32_t perm);
void     mk_post_tier_flush(uint64_t obj_id);
void     mk_post_log_append(uint64_t entry_id, uint64_t tx_id);

// ─── Agent IPC wrappers ───────────────────────────────────────────────────────
// Pointer in payload[0] must remain valid until the AP service poll processes it.
// Since poll runs every ~10 ticks on Core 1, static/BSS-allocated structs are safe.
void     mk_post_agent_spawn(struct AgentCreateRequest* req);
void     mk_post_agent_step(struct AgentRunRequest* req);
void     mk_post_agent_kill(const char* name);

#endif /* MICROKERNEL_H */
