#include "ipc.h"
#include "../kernel/dashboard.h"   // for read_tsc

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
        "(Port bounds 0x1001 - 0x1005).\n");
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
