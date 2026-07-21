#include "msgqueue.h"
#include "kernel_io.h"
#include "timer.h"   // kernel_tick_counter

// ─── String helpers (same small local-copy convention as group_profile.c's
// gp_* and authlist.c's own copies -- this codebase doesn't share one string
// helper module across kernel files) ────────────────────────────────────────
static int mq_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void mq_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    if (src) { for (; i + 1 < n && src[i]; i++) dst[i] = src[i]; }
    dst[i] = '\0';
}

// ─── Globals ────────────────────────────────────────────────────────────────
struct SLSMsgQueueEntry mq_table[MQ_MAX];
uint32_t                mq_table_count = 0;

static int find_mq_idx(const char* name) {
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].active && mq_streq(mq_table[i].name, name)) return i;
    }
    return -1;
}

// ─── mq_create ───────────────────────────────────────────────────────────────
int mq_create(const char* name) {
    if (!name || !name[0]) return 0;
    if (find_mq_idx(name) >= 0) {
        kernel_serial_printf("[MQ] ERROR: Queue '%s' already exists.\n", name);
        return 0;
    }
    for (int i = 0; i < MQ_MAX; i++) {
        if (!mq_table[i].active) {
            mq_strncpy(mq_table[i].name, name, MQ_NAME_LEN);
            mq_table[i].active = 1;
            mq_table[i].head   = 0;
            mq_table[i].tail   = 0;
            mq_table[i].count  = 0;
            if ((uint32_t)(i + 1) > mq_table_count) mq_table_count = (uint32_t)(i + 1);
            kernel_serial_printf("[MQ] Created message queue '%s'.\n", name);
            return 1;
        }
    }
    kernel_serial_print("[MQ] ERROR: Message queue table full.\n");
    return 0;
}

// ─── mq_send ─────────────────────────────────────────────────────────────────
int mq_send(const char* name, uint32_t sender_uid, const char* text, uint64_t tick) {
    int idx = find_mq_idx(name);
    if (idx < 0) {
        kernel_serial_printf("[MQ] ERROR: Queue '%s' not found.\n", name);
        return -1;
    }
    struct SLSMsgQueueEntry* q = &mq_table[idx];
    if (q->count >= MQ_QUEUE_DEPTH) {
        kernel_serial_printf("[MQ] ERROR: Queue '%s' is full (%d messages).\n",
                             name, MQ_QUEUE_DEPTH);
        return -2;
    }
    struct SLSMsgQueueMessage* m = &q->msgs[q->tail];
    m->sender_uid = sender_uid;
    m->tick       = tick;
    mq_strncpy(m->text, text, MQ_MSG_TEXT_LEN);
    q->tail = (q->tail + 1) % MQ_QUEUE_DEPTH;
    q->count++;
    kernel_serial_printf("[MQ] uid %u -> '%s': %s\n", sender_uid, name, m->text);
    return 0;
}

// ─── mq_receive ──────────────────────────────────────────────────────────────
int mq_receive(const char* name, struct SLSMsgQueueMessage* out) {
    int idx = find_mq_idx(name);
    if (idx < 0) return 0;
    struct SLSMsgQueueEntry* q = &mq_table[idx];
    if (q->count == 0) return 0;
    *out = q->msgs[q->head];
    q->head = (q->head + 1) % MQ_QUEUE_DEPTH;
    q->count--;
    return 1;
}

// ─── mq_list ─────────────────────────────────────────────────────────────────
void mq_list(void) {
    kernel_serial_printf("\n[MQ] Message Queues\n %-32s %s\n", "Name", "Depth");
    int shown = 0;
    for (int i = 0; i < MQ_MAX; i++) {
        if (!mq_table[i].active) continue;
        kernel_serial_printf(" %-32s %u/%d\n",
                             mq_table[i].name, mq_table[i].count, MQ_QUEUE_DEPTH);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no message queues defined)\n");
    kernel_serial_printf(" %d queue(s).\n\n", shown);
}

// ─── Syscall wrappers ────────────────────────────────────────────────────────
uint64_t sys_sls_mq_create(struct SLSMQCreateRequest* req) {
    if (!req) return 1;
    return mq_create(req->name) ? 0 : 1;
}

uint64_t sys_sls_mq_send(struct SLSMQSendRequest* req) {
    if (!req) return 1;
    return mq_send(req->name, req->sender_uid, req->text, kernel_tick_counter) == 0 ? 0 : 1;
}

uint64_t sys_sls_mq_receive(struct SLSMQReceiveRequest* req) {
    if (!req) return 1;
    req->got = (uint8_t)mq_receive(req->name, &req->msg);
    return 0;
}
