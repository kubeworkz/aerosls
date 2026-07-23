/*
 * ipc_partition_host_test.c — Phase 11 verification: a standalone
 * host-buildable test for kernel/ipc.c's new cross-partition boundary on
 * user IPC ports, linked against the REAL, unmodified kernel/ipc.c — not
 * a reimplementation.
 *
 * ipc.c's only two dependencies outside its own header are dashboard.h's
 * read_tsc() (a self-contained `static inline`, compiles as-is on the
 * host) and process.h's proc_table[]/PROC_MAX (a plain extern array this
 * test defines itself with two dummy processes in two different
 * partitions — no need to link the much heavier process.c). Small enough
 * dependency surface to actually execute, same shape as Phase 8's
 * partition_host_test.c and Phase 10's persist_partition_host_test.c.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/ipc_partition_host_test \
 *       tests/ipc_partition_host_test.c kernel/ipc.c kernel/partition.c
 *   /tmp/ipc_partition_host_test
 */
#include "kernel/process.h"
#include "kernel/ipc.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <string.h>

/* ─── Dummy process table: two pids in partition 1, one pid in partition 2 ──
 * process.c is NOT linked — this test defines proc_table[]/proc_count
 * itself (the real extern declared in process.h), populated directly
 * rather than via process_create()/program_spawn(), since this test only
 * needs "pid -> partition_id" to resolve, not a real spawn pipeline. */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t                 proc_count = 0;

static void add_fake_process(uint32_t pid, uint32_t partition_id) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active       = 1;
            proc_table[i].pid          = pid;
            proc_table[i].partition_id = partition_id;
            proc_table[i].state        = PROC_RUNNING;
            proc_count++;
            return;
        }
    }
}

void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { /* Phase 10's persistence hook — irrelevant here */ }

/* Phase 14 (LPAR): partition.c's partition_destroy() (added this phase)
 * calls into process.c/object_catalog.c/frame_pool.c, none of which this
 * IPC-focused test has any interest in linking — it never calls
 * partition_destroy() itself. See scheduler_fairness_host_test.c for the
 * real, call-tracked coverage of destroy/pause/resume. */
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }   /* Multi-Node Partition Scaling Roadmap Phase 3 -- replaces partition_reset_frame_usage() at partition_destroy()'s call site */
uint32_t cluster_local_node_id(void) { return 0; }   /* Multi-Node Partition Scaling Roadmap Phase 2 */
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }   /* Multi-Node Partition Scaling Roadmap Phase 6 -- not exercised by this test, safe "nothing to relinquish" stub */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* pid 10, pid 11 -> partition 1 ("tenant-a"-equivalent); pid 20 -> partition 2 */
    add_fake_process(10, 1);
    add_fake_process(11, 1);
    add_fake_process(20, 2);

    ipc_init();

    /* ── Bind port 0x2000 to pid 10 (partition 1) ─────────────────────── */
    CHECK(ipc_user_bind(0x2000, 10) == 0, "pid 10 binds port 0x2000");

    /* ── Same-partition send succeeds ─────────────────────────────────── */
    struct IPCUserSendReq req;
    memset(&req, 0, sizeof(req));
    req.dst_port = 0x2000;
    req.opcode   = 0xAAAA;
    req.payload[0] = 111;
    CHECK(ipc_user_send(&req, 10) == 0, "pid 10 (same partition as binder) sends to 0x2000: accepted");
    CHECK(ipc_queue_depth(0x2000) == -1, "ipc_queue_depth is for kernel-service ports only, -1 for a user port (sanity check on the API, not this phase's logic)");

    /* ── Cross-partition send is denied, queue untouched ──────────────── */
    struct IPCUserSendReq req2;
    memset(&req2, 0, sizeof(req2));
    req2.dst_port = 0x2000;
    req2.opcode   = 0xBBBB;
    req2.payload[0] = 222;
    CHECK(ipc_user_send(&req2, 20) == -1, "pid 20 (partition 2) sending to pid 10's port 0x2000 (partition 1): denied");

    /* ── Cross-partition recv denied — the same message must NOT be
     * drainable by an outsider, and the real recv from step above must
     * still be sitting in the queue afterward ─────────────────────────── */
    struct IPCUserRecvReq rreq;
    memset(&rreq, 0, sizeof(rreq));
    rreq.port = 0x2000;
    CHECK(ipc_user_recv(&rreq, 20) == 0, "pid 20 (partition 2) receiving from port 0x2000 (partition 1): denied, looks like empty");

    /* ── Same-partition recv by a DIFFERENT pid than the binder succeeds —
     * confirms this is partition-scoped, not "only the exact binder" ──── */
    memset(&rreq, 0, sizeof(rreq));
    rreq.port = 0x2000;
    int got = ipc_user_recv(&rreq, 11);
    CHECK(got == 1, "pid 11 (same partition as binder pid 10, but a different pid) receives from 0x2000: accepted");
    CHECK(rreq.msg.opcode == 0xAAAA && rreq.msg.payload[0] == 111,
          "the message received is pid 10's original send (0xAAAA/111), not the denied cross-partition one (0xBBBB/222)");

    /* ── Queue should now be empty — confirms the earlier denied cross-
     * partition send from pid 20 never actually got queued at all ──────── */
    memset(&rreq, 0, sizeof(rreq));
    rreq.port = 0x2000;
    CHECK(ipc_user_recv(&rreq, 10) == 0, "queue is empty after the one legitimate message was drained — the denied send left nothing behind");

    /* ── Unbound port: pre-Phase-11 behavior unchanged — anyone can send ── */
    struct IPCUserSendReq req3;
    memset(&req3, 0, sizeof(req3));
    req3.dst_port = 0x2005;   /* never bound */
    req3.opcode   = 0xCCCC;
    CHECK(ipc_user_send(&req3, 20) == 0, "sending to an unbound port (0x2005) still works regardless of partition — no regression");
    memset(&rreq, 0, sizeof(rreq));
    rreq.port = 0x2005;
    CHECK(ipc_user_recv(&rreq, 99) == 1, "receiving from an unbound port still works for any pid — no regression");

    /* ── Kernel-service ports are a completely separate array/path,
     * untouched by any of this — sanity check it still works normally ──── */
    struct IPCMessage svc_msg;
    memset(&svc_msg, 0, sizeof(svc_msg));
    svc_msg.dst_port = IPC_PORT_VMMGR;
    svc_msg.opcode   = VMM_OP_MAP_PAGE;
    CHECK(ipc_post(IPC_PORT_VMMGR, &svc_msg) == 0, "kernel-service port 0x1001 post still works, unaffected by partition logic");
    struct IPCMessage svc_out;
    CHECK(ipc_recv(IPC_PORT_VMMGR, &svc_out) == 1, "kernel-service port 0x1001 recv still works, unaffected by partition logic");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
