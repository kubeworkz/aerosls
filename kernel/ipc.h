#ifndef IPC_H
#define IPC_H

#include <stdint.h>

// ─── IPC Port Assignments ─────────────────────────────────────────────────────
// Port space: 0x1000 – 0x1FFF (matches bus log output)
#define IPC_PORT_VMMGR    0x1001   // VirtualMemoryMgr
#define IPC_PORT_SECMGR   0x1002   // ObjectSecurityMgr
#define IPC_PORT_DBMGR    0x1003   // NativeDbStoreMgr
#define IPC_PORT_TIERMGR  0x1004   // StorageTierMgr
#define IPC_PORT_LOGMGR   0x1005   // RecoveryLogVerifier

#define IPC_PORT_FIRST    0x1001
#define IPC_PORT_LAST     0x1005
#define IPC_NUM_QUEUES    5

// ─── Opcodes per Service ──────────────────────────────────────────────────────
// VirtualMemoryMgr
#define VMM_OP_MAP_PAGE      0x0101
#define VMM_OP_UNMAP         0x0102
#define VMM_OP_FAULT_NOTIFY  0x0103

// ObjectSecurityMgr
#define SEC_OP_CHECK_ACCESS  0x0201
#define SEC_OP_GRANT         0x0202
#define SEC_OP_REVOKE        0x0203

// NativeDbStoreMgr
#define DB_OP_COMMIT         0x0301
#define DB_OP_ROLLBACK       0x0302
#define DB_OP_VALLOC         0x0303

// StorageTierMgr
#define TIER_OP_PROMOTE      0x0401
#define TIER_OP_DEMOTE       0x0402
#define TIER_OP_FLUSH        0x0403

// RecoveryLogVerifier
#define LOG_OP_APPEND        0x0501
#define LOG_OP_VERIFY        0x0502
#define LOG_OP_RECOVER       0x0503

// ─── Message Struct ───────────────────────────────────────────────────────────
// No-copy design: payload carries pointers / object IDs, not raw data bytes.
struct IPCMessage {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t opcode;
    uint64_t payload[4];   // opcode-specific args (object_id, size, flags, etc.)
    uint32_t reply_token;  // echoed in the reply for caller matching
    uint8_t  consumed;     // set to 1 by the service after processing
    uint8_t  _pad[3];
};

// ─── Per-Port Circular Queue ──────────────────────────────────────────────────
#define IPC_QUEUE_DEPTH 8   // reduced from 32 for BSS budget

struct IPCQueue {
    struct IPCMessage msgs[IPC_QUEUE_DEPTH];
    volatile uint32_t head;    // consumer index
    volatile uint32_t tail;    // producer index
    volatile uint32_t count;   // current depth
};

// ─── IPC Statistics ───────────────────────────────────────────────────────────
struct IPCStats {
    uint64_t total_posted;
    uint64_t total_dispatched;
    uint64_t total_dropped;     // queue-full discards
    uint64_t avg_latency_ns;    // rolling average dispatch latency
};

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_IPC_POST  134
#define SYS_SLS_IPC_STAT  133

// ─── IPC Post Request (syscall 134 argument) ──────────────────────────────────
struct IPCPostRequest {
    struct IPCMessage msg;
};

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct IPCQueue ipc_queues[IPC_NUM_QUEUES];
extern struct IPCStats ipc_stats;

void ipc_init(void);
int  ipc_post(uint16_t port, const struct IPCMessage* msg);
int  ipc_recv(uint16_t port, struct IPCMessage* out);
int  ipc_queue_depth(uint16_t port);

// Kernel syscall handler
uint64_t sys_sls_ipc_post(struct IPCPostRequest* req);

#endif /* IPC_H */
