#ifndef MSGQUEUE_H
#define MSGQUEUE_H

#include <stdint.h>

/*
 * msgqueue.h — Navigator-Parity Gap Roadmap Phase 4: message queues.
 *
 * IBM i's *MSGQ concept: a named object that any authorized job can post a
 * short text message to, and that (usually one) receiver drains in FIFO
 * order — used for operator messages, job completion notices, and simple
 * inter-job signaling. This is a different shape than kernel/ipc.h's
 * existing bus: IPC ports are numeric (fixed 0x1001-0x1006 kernel-service
 * ports, or a small 0x2000-0x200F user-port range bound 1:1 to a single
 * owning pid) and messages are structured opcode+payload records meant for
 * service dispatch, not free-text operator communication. A named, FIFO,
 * multi-reader-capable queue that anyone can look up by name doesn't fit
 * that shape without distorting it, so this is a new, small, independent
 * fixed-size table -- "layered on top of the IPC bus" (per the roadmap
 * draft) turned out to mean "informed by its conventions" (fixed-size
 * table, bump-allocated, no-reclaim posture) rather than literally built out
 * of ipc_queues[]/ipc_user_queues[], once the actual shape of a named
 * multi-reader text queue was worked through -- documented here rather than
 * silently deviating from the original draft's wording.
 *
 * Sizing: MQ_MAX=8 named queues and MQ_QUEUE_DEPTH=16 messages/queue are
 * deliberately small, matching this codebase's "how many does a small
 * simulated deployment plausibly need" judgment call (same reasoning as
 * GROUP_TABLE_MAX/AUTHLIST_MAX in Phase 3). Unlike kernel/ipc.h's queues
 * (posted to from ISR context, hence the atomic __atomic_* ops there), mq_*
 * functions are only ever reached via syscall dispatch -- ordinary kernel
 * context, no interrupt-context producer -- so a plain non-atomic circular
 * buffer is the right level of mechanism, matching group_profile.c/
 * authlist.c's own plain-table convention rather than copying ipc.c's
 * atomic one where it isn't needed.
 */

#define MQ_NAME_LEN       32
#define MQ_MAX            8
#define MQ_QUEUE_DEPTH    16
#define MQ_MSG_TEXT_LEN   80

struct SLSMsgQueueMessage {
    uint32_t sender_uid;
    uint64_t tick;
    char     text[MQ_MSG_TEXT_LEN];
};

struct SLSMsgQueueEntry {
    char     name[MQ_NAME_LEN];
    uint8_t  active;
    struct SLSMsgQueueMessage msgs[MQ_QUEUE_DEPTH];
    uint32_t head;    // consumer index
    uint32_t tail;    // producer index
    uint32_t count;   // current depth
};

extern struct SLSMsgQueueEntry mq_table[MQ_MAX];
extern uint32_t                mq_table_count;   // high-water mark, mirrors group_table_count's own bump style

// Creates a new named queue. Returns 1 on success, 0 if the name is already
// taken or the table is full (bump-allocated, no reclaim -- same posture as
// group_table[]/auth_tokens[] elsewhere in this codebase).
int mq_create(const char* name);

// Appends a message to the named queue. Returns 0 on success, -1 if the
// queue doesn't exist, -2 if it's at MQ_QUEUE_DEPTH capacity (oldest message
// is NOT evicted -- a full queue silently rejects new sends, same "no
// reclaim, caller must drain first" posture as every other bounded
// structure in this codebase; no ring-buffer overwrite invented).
int mq_send(const char* name, uint32_t sender_uid, const char* text, uint64_t tick);

// Dequeues the oldest message from the named queue into *out. Returns 1 if a
// message was available, 0 if the queue doesn't exist or is empty.
int mq_receive(const char* name, struct SLSMsgQueueMessage* out);

// Prints the queue table (name + current depth) to the serial port, mirrors
// group_list()/authlist_list()'s own style.
void mq_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 248-251 are the next free numbers after this same phase's own process
// hold/release/priority additions (245-247) -- confirmed via grep across
// every header defining SYS_SLS_* before picking these.
#define SYS_SLS_MQ_CREATE   248
#define SYS_SLS_MQ_SEND     249
#define SYS_SLS_MQ_RECEIVE  250
#define SYS_SLS_MQ_LIST     251

struct SLSMQCreateRequest {
    char name[MQ_NAME_LEN];
};

struct SLSMQSendRequest {
    char     name[MQ_NAME_LEN];
    uint32_t sender_uid;
    char     text[MQ_MSG_TEXT_LEN];
};

struct SLSMQReceiveRequest {
    char                       name[MQ_NAME_LEN];  // [in]
    struct SLSMsgQueueMessage  msg;                // [out] valid only if got==1
    uint8_t                    got;                // [out]
};

uint64_t sys_sls_mq_create(struct SLSMQCreateRequest* req);
uint64_t sys_sls_mq_send(struct SLSMQSendRequest* req);
uint64_t sys_sls_mq_receive(struct SLSMQReceiveRequest* req);

#endif /* MSGQUEUE_H */
