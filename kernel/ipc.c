#include "ipc.h"
#include "../kernel/dashboard.h"   // for read_tsc
#include "process.h"               // Phase 11 (LPAR): proc_table[] for pid->partition
#include "partition.h"             // Phase 11 (LPAR): PARTITION_DEFAULT fallback

struct IPCQueue ipc_queues[IPC_NUM_QUEUES];
struct IPCStats ipc_stats;

// Maps port number → queue array index
static int port_to_index(uint16_t port) {
    if (port < IPC_PORT_FIRST || port > IPC_PORT_LAST) return -1;
    return (int)(port - IPC_PORT_FIRST);
}

void ipc_init(void) {
    for (int i = 0; i < IPC_NUM_QUEUES; i++) {
        ipc_queues[i].head  = 0;
        ipc_queues[i].tail  = 0;
        ipc_queues[i].count = 0;
    }
    ipc_stats.total_posted     = 0;
    ipc_stats.total_dispatched = 0;
    ipc_stats.total_dropped    = 0;
    ipc_stats.avg_latency_ns   = 0;

    kernel_serial_print(
        "[IPC] Message Passing interface configured "
        "(Port bounds 0x1001 - 0x1006).\n");
}

// ─── Post a message into the destination port's queue ─────────────────────────
// Thread-safe via atomic count increment. Returns 0 on success.
int ipc_post(uint16_t port, const struct IPCMessage* msg) {
    int idx = port_to_index(port);
    if (idx < 0) return -1;

    struct IPCQueue* q = &ipc_queues[idx];

    if (__atomic_load_n(&q->count, __ATOMIC_SEQ_CST) >= IPC_QUEUE_DEPTH) {
        __atomic_fetch_add(&ipc_stats.total_dropped, 1, __ATOMIC_RELAXED);
        return -2;  // Queue full
    }

    uint32_t write_pos = __atomic_fetch_add(&q->tail, 1, __ATOMIC_SEQ_CST)
                         % IPC_QUEUE_DEPTH;
    q->msgs[write_pos]          = *msg;
    q->msgs[write_pos].consumed = 0;

    __atomic_fetch_add(&q->count, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&ipc_stats.total_posted, 1, __ATOMIC_RELAXED);
    return 0;
}

// ─── Receive the oldest message from a port's queue ───────────────────────────
// Returns 1 if a message was available, 0 if empty.
int ipc_recv(uint16_t port, struct IPCMessage* out) {
    int idx = port_to_index(port);
    if (idx < 0) return 0;

    struct IPCQueue* q = &ipc_queues[idx];

    if (__atomic_load_n(&q->count, __ATOMIC_SEQ_CST) == 0) return 0;

    uint64_t t0 = read_tsc();

    uint32_t read_pos = __atomic_fetch_add(&q->head, 1, __ATOMIC_SEQ_CST)
                        % IPC_QUEUE_DEPTH;
    *out = q->msgs[read_pos];

    __atomic_fetch_sub(&q->count, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&ipc_stats.total_dispatched, 1, __ATOMIC_RELAXED);

    // Rolling average: new_avg = (old_avg * 7 + sample) / 8
    uint64_t dt = read_tsc() - t0;
    ipc_stats.avg_latency_ns = (ipc_stats.avg_latency_ns * 7 + dt) / 8;

    return 1;
}

int ipc_queue_depth(uint16_t port) {
    int idx = port_to_index(port);
    if (idx < 0) return -1;
    return (int)__atomic_load_n(&ipc_queues[idx].count, __ATOMIC_RELAXED);
}

// ─── Syscall: SYS_SLS_IPC_POST ────────────────────────────────────────────────
uint64_t sys_sls_ipc_post(struct IPCPostRequest* req) {
    if (!req) return 1;
    return ipc_post(req->msg.dst_port, &req->msg) == 0 ? 0 : 1;
}

// ─── User-Port IPC State ──────────────────────────────────────────────────────
struct IPCQueue ipc_user_queues[IPC_USER_NUM_PORTS];
uint32_t        ipc_user_port_pid[IPC_USER_NUM_PORTS]; // 0 = unbound
uint32_t        ipc_user_port_partition[IPC_USER_NUM_PORTS]; // Phase 11 (LPAR)

// Initialised in ipc_init() — zero-fill via BSS is valid since PID 0 is unused.

// ─── pid_to_partition ─────────────────────────────────────────────────────────
// Phase 11 (LPAR): resolves a pid to the partition its process was spawned
// into (ProcessDescriptor.partition_id, set at spawn time by Phase 9). A
// pid with no active, matching entry in proc_table[] (already exited, or
// simply never existed) falls back to PARTITION_DEFAULT — the same
// conservative "unknown resolves to the default partition, not to an
// always-pass wildcard" choice partition_get_for_uid() makes for an
// unassigned uid. This scan mirrors the inline caller_pid resolution
// already duplicated three times in syscall_dispatch.c's IPC cases,
// applied here to the *target* end of a bind/send/recv instead.
static uint32_t pid_to_partition(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid)
            return proc_table[i].partition_id;
    }
    return PARTITION_DEFAULT;
}

// ─── ipc_user_bind ───────────────────────────────────────────────────────────
// Bind process pid to a user port.  A port may be re-bound by the same pid.
// Returns 0 on success, -1 bad port, -2 already owned by different pid.
//
// Phase 11 (LPAR): records the binding pid's partition alongside the pid
// itself — this is what ipc_user_send()/ipc_user_recv() check against
// once the port is bound. Kernel-service ports (0x1001-0x1006) are a
// completely separate array/code path (ipc_queues[], ipc_post()) never
// touched by this function or its partition tracking — they stay
// unconditionally reachable, the "permanently PARTITION_SYSTEM" treatment
// the roadmap calls for, achieved here simply by never being in scope.
int ipc_user_bind(uint16_t port, uint32_t pid) {
    if (port < IPC_USER_PORT_FIRST || port > IPC_USER_PORT_LAST) return -1;
    int idx = (int)(port - IPC_USER_PORT_FIRST);
    uint32_t owner = ipc_user_port_pid[idx];
    if (owner != 0 && owner != pid) return -2;
    ipc_user_port_pid[idx]       = pid;
    ipc_user_port_partition[idx] = pid_to_partition(pid);
    kernel_serial_printf("[IPC] PID %u bound to user port 0x%04x\n", pid, port);
    return 0;
}

// ─── ipc_user_send ───────────────────────────────────────────────────────────
// Post a message to any port: kernel service (0x1001-0x1006) or user (0x2000+).
// src_pid is used as the src_port field in the message header.
int ipc_user_send(const struct IPCUserSendReq* req, uint32_t src_pid) {
    if (!req) return -1;
    struct IPCMessage msg;
    msg.src_port    = (uint16_t)(src_pid & 0xFFFF); /* PID as "src port" */
    msg.dst_port    = req->dst_port;
    msg.opcode      = req->opcode;
    msg.reply_token = 0;
    msg.consumed    = 0;
    msg._pad[0] = msg._pad[1] = msg._pad[2] = 0;
    for (int i = 0; i < 4; i++) msg.payload[i] = req->payload[i];

    uint16_t dst = req->dst_port;

    // Kernel service port?
    if (dst >= IPC_PORT_FIRST && dst <= IPC_PORT_LAST)
        return ipc_post(dst, &msg);

    // User port?
    if (dst >= IPC_USER_PORT_FIRST && dst <= IPC_USER_PORT_LAST) {
        int idx = (int)(dst - IPC_USER_PORT_FIRST);

        // Phase 11 (LPAR): once a port has a bound owner, only a sender in
        // that owner's partition may deliver to it. An unbound port
        // (ipc_user_port_pid[idx] == 0) has no owner to protect yet, so
        // this is unchanged, pre-Phase-11 behavior — sending to a not-yet-
        // bound port already worked and still does. Denial returns -1,
        // the same code as "unknown port" (see the bottom of this
        // function) — mirrors catalog_check_access()'s "denial looks
        // like absence" precedent from Phase 7: a cross-partition send
        // shouldn't be distinguishable from a send to a port that simply
        // doesn't exist, or a process could use the send return code
        // alone to probe which ports are alive in other partitions.
        if (ipc_user_port_pid[idx] != 0 &&
            ipc_user_port_partition[idx] != pid_to_partition(src_pid)) {
            return -1;
        }

        struct IPCQueue* q = &ipc_user_queues[idx];
        if (__atomic_load_n(&q->count, __ATOMIC_SEQ_CST) >= IPC_QUEUE_DEPTH) {
            __atomic_fetch_add(&ipc_stats.total_dropped, 1, __ATOMIC_RELAXED);
            return -2; /* queue full */
        }
        uint32_t pos = __atomic_fetch_add(&q->tail, 1, __ATOMIC_SEQ_CST)
                       % IPC_QUEUE_DEPTH;
        q->msgs[pos]          = msg;
        q->msgs[pos].consumed = 0;
        __atomic_fetch_add(&q->count, 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&ipc_stats.total_posted, 1, __ATOMIC_RELAXED);
        return 0;
    }
    return -1; /* unknown port */
}

// ─── ipc_user_recv ───────────────────────────────────────────────────────────
// Non-blocking receive from the user port specified in req->port.
// Fills req->msg on success.  Returns 1 if message received, 0 if empty.
//
// Phase 11 (LPAR): caller_pid is who's actually asking (resolved
// server-side by syscall_dispatch.c, the same trusted way src_pid already
// is for ipc_user_send() — never taken from user-supplied req fields).
// Once a port has a bound owner, only a caller in that owner's partition
// may drain it — the receive-side half of "gated by sender/receiver
// partition"; without this, blocking cross-partition send alone would be
// hollow, since anyone could still read a same-partition port's mail by
// simply calling recv on its port number.
int ipc_user_recv(struct IPCUserRecvReq* req, uint32_t caller_pid) {
    if (!req) return 0;
    uint16_t port = req->port;
    if (port < IPC_USER_PORT_FIRST || port > IPC_USER_PORT_LAST) return 0;
    int idx = (int)(port - IPC_USER_PORT_FIRST);

    // Unbound port: no owner to protect yet, same "already worked, still
    // does" non-change as the send-side check above.
    if (ipc_user_port_pid[idx] != 0 &&
        ipc_user_port_partition[idx] != pid_to_partition(caller_pid)) {
        return 0;   // looks exactly like "queue empty" — denial-looks-like-absence again
    }

    struct IPCQueue* q = &ipc_user_queues[idx];
    if (__atomic_load_n(&q->count, __ATOMIC_SEQ_CST) == 0) return 0;
    uint32_t pos = __atomic_fetch_add(&q->head, 1, __ATOMIC_SEQ_CST)
                   % IPC_QUEUE_DEPTH;
    req->msg = q->msgs[pos];
    __atomic_fetch_sub(&q->count, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&ipc_stats.total_dispatched, 1, __ATOMIC_RELAXED);
    return 1;
}
