/*
 * workmgmt_phase4_host_test.c — Navigator-Parity Gap Roadmap Phase 4
 * verification: a standalone host-buildable test for job priority
 * (ProcessDescriptor.priority, consulted by pick_next_process_in_partition()),
 * job hold/release (process_hold()/process_release(), the new PROC_HELD
 * state), and message queues (kernel/msgqueue.c) — all executed as the REAL,
 * unmodified functions, not reimplementations.
 *
 * Same "#include kernel/process.c directly to reach its `static` scheduling
 * helpers, with stubs for the heavy dependencies (loader/user_paging/
 * object_catalog/kernel_enter_ring3) it never actually calls in this test"
 * approach tests/scheduler_fairness_host_test.c already established — see
 * that file's own header comment for the full rationale. This test's stub
 * list is identical to that one's. kernel/msgqueue.c has a much smaller
 * dependency footprint (just kernel_io.h's print functions and timer.h's
 * kernel_tick_counter) so it's linked as a real, separate object file rather
 * than needing any stubs of its own.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 -I user \
 *       -o /tmp/workmgmt_phase4_host_test \
 *       tests/workmgmt_phase4_host_test.c kernel/partition.c kernel/msgqueue.c
 *   /tmp/workmgmt_phase4_host_test
 */
#include "kernel/process.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/msgqueue.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ─── Stubs for process.c's dependencies this test never actually calls ──
 * Identical set to scheduler_fairness_host_test.c's own stubs — see that
 * file's header comment for why each one is safe to fake here. */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void* allocate_physical_ram_frame(void) { return (void*)0x1000; }
uint64_t loader_load_into_process(const char* object_name, uint64_t base_vaddr, uint64_t* pml4, uint32_t partition_id) {
    (void)object_name; (void)base_vaddr; (void)pml4; (void)partition_id; return 0;
}
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) { (void)partition_id; return (void*)0x1000; }
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm; return 1;
}
uint64_t user_clone_page_table(void) { return 0; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
void kernel_enter_ring3(uint64_t* rsp_save, uint64_t* cr3_save,
                         uint64_t cr3, uint64_t rip, uint64_t rsp) {
    (void)rsp_save; (void)cr3_save; (void)cr3; (void)rip; (void)rsp;
}
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
void persist_partitions(void) {}
static int      g_vfree_partition_calls = 0;
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; g_vfree_partition_calls++; return 0; }
static int      g_reset_usage_calls = 0;
int partition_reset_frame_usage(uint32_t partition_id) { (void)partition_id; g_reset_usage_calls++; return 0; }

/* msgqueue.c's own dependency (timer.h's kernel_tick_counter) — same
 * definition every other host test linking a security_audit.c/msgqueue.c
 * consumer already uses. */
volatile uint64_t kernel_tick_counter = 0;

/* Pull in process.c itself for its `static` scheduling helpers and REAL
 * proc_table[]/proc_count globals — see scheduler_fairness_host_test.c. */
#include "kernel/process.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static void reset_proc_table(void) {
    memset(proc_table, 0, sizeof(proc_table));
    proc_count = 0;
    g_last_partition_scheduled = PARTITION_SYSTEM;
    for (int i = 0; i < PARTITION_MAX; i++) g_last_index_in_partition[i] = -1;
}

static void add_runnable_prio(int idx, uint32_t pid, uint32_t partition_id, ProcPriority prio) {
    proc_table[idx].active       = 1;
    proc_table[idx].pid          = pid;
    proc_table[idx].partition_id = partition_id;
    proc_table[idx].state        = PROC_SUSPENDED;
    proc_table[idx].kernel_rsp   = 1;   /* nonzero sentinel — "actually entered" */
    proc_table[idx].priority     = prio;
}

int main(void) {
    /* ── Scenario 1: strict priority — a runnable HIGH process is always
     * chosen over runnable NORMAL/LOW ones in the same partition, repeatedly,
     * for as long as it stays runnable (this is the expected, documented
     * behavior of strict priority scheduling — a single always-runnable HIGH
     * job legitimately starves lower tiers, matching this phase's "smallest
     * real version first" 3-tier scope, not a full fair-share weighting
     * scheme). ────────────────────────────────────────────────────────────── */
    reset_proc_table();
    add_runnable_prio(0, 100, PARTITION_SYSTEM, PROC_PRIO_NORMAL);
    add_runnable_prio(1, 101, PARTITION_SYSTEM, PROC_PRIO_HIGH);
    add_runnable_prio(2, 102, PARTITION_SYSTEM, PROC_PRIO_LOW);

    int chosen1 = pick_next_process_in_partition(PARTITION_SYSTEM, -1);
    CHECK(chosen1 == 1, "s1: with all three tiers runnable, the HIGH-priority process (slot 1) is chosen first");
    int chosen1b = pick_next_process_in_partition(PARTITION_SYSTEM, -1);
    CHECK(chosen1b == 1, "s1: the same HIGH process is chosen again on the next tick -- strict priority, not just 'goes first once'");

    /* ── Scenario 2: excluding the current HIGH process (simulating it being
     * the one just preempted, per pick_next_process_in_partition()'s
     * exclude_idx contract) falls through to NORMAL, not LOW -- tier order is
     * respected even when the top tier's only member is unavailable this
     * round. ──────────────────────────────────────────────────────────────── */
    int chosen2 = pick_next_process_in_partition(PARTITION_SYSTEM, /*exclude_idx=*/1);
    CHECK(chosen2 == 0, "s2: excluding the HIGH process falls through to the NORMAL process (slot 0), not LOW");

    /* ── Scenario 3: with HIGH no longer runnable at all (not just excluded)
     * and NORMAL excluded, LOW is correctly reached as the last resort. ──── */
    proc_table[1].state = PROC_RUNNING;   /* HIGH process is no longer a SUSPENDED candidate */
    int chosen3 = pick_next_process_in_partition(PARTITION_SYSTEM, /*exclude_idx=*/0);
    CHECK(chosen3 == 2, "s3: with HIGH not runnable and NORMAL excluded, LOW (slot 2) is correctly reached");

    /* ── Scenario 4: backward compatibility — when every process is the
     * default PROC_PRIO_NORMAL (a deployment that never touches priority),
     * scheduling reduces to the exact pre-Phase-4 flat round robin: every
     * process gets a turn over N ticks, same as scheduler_fairness_host_
     * test.c's own scenario 2. ────────────────────────────────────────────── */
    reset_proc_table();
    for (int i = 0; i < 6; i++) add_runnable_prio(i, 300 + (uint32_t)i, PARTITION_SYSTEM, PROC_PRIO_NORMAL);
    int current_idx = 0;
    uint8_t seen[6] = {0};
    for (int tick = 0; tick < 6; tick++) {
        int next_idx = pick_next_process_in_partition(PARTITION_SYSTEM, current_idx);
        CHECK(next_idx >= 0, "s4: a candidate is always found among 6 all-NORMAL processes");
        seen[next_idx] = 1;
        current_idx = next_idx;
    }
    int all_seen4 = 1;
    for (int i = 0; i < 6; i++) if (!seen[i]) all_seen4 = 0;
    CHECK(all_seen4, "s4: all-NORMAL priorities behave exactly like the pre-Phase-4 flat round robin -- every process gets a turn");

    /* ── Scenario 5: process_hold() state-machine — success and every
     * documented failure code. ────────────────────────────────────────────── */
    reset_proc_table();
    add_runnable_prio(0, 500, PARTITION_SYSTEM, PROC_PRIO_NORMAL);   /* SUSPENDED */
    proc_table[1].active = 1; proc_table[1].pid = 501; proc_table[1].state = PROC_RUNNING;
    proc_table[2].active = 1; proc_table[2].pid = 502; proc_table[2].state = PROC_ZOMBIE;

    CHECK(process_hold(500) == 0, "s5: holding a SUSPENDED process succeeds");
    CHECK(proc_table[0].state == PROC_HELD, "s5: its state is now PROC_HELD (not PROC_SUSPENDED)");
    CHECK(process_hold(500) == -3, "s5: holding an already-held process fails with -3");
    CHECK(process_hold(501) == -2, "s5: holding a currently-RUNNING process fails with -2 (scoped out, see process.h's comment)");
    CHECK(process_hold(502) == -4, "s5: holding a ZOMBIE process fails with -4");
    CHECK(process_hold(99999) == -1, "s5: holding a nonexistent pid fails with -1");

    /* ── Scenario 6: a HELD process is invisible to the scheduler's own
     * candidate scan -- this is the entire point of PROC_HELD being distinct
     * from PROC_SUSPENDED (see process.h's extensive comment on this). ────── */
    g_last_index_in_partition[PARTITION_SYSTEM] = -1;
    int chosen6 = pick_next_process_in_partition(PARTITION_SYSTEM, -1);
    CHECK(chosen6 == -1, "s6: with the only process HELD, pick_next_process_in_partition finds nothing runnable");

    /* ── Scenario 7: process_release() returns the held process to
     * PROC_SUSPENDED, and it becomes schedulable again immediately. ──────── */
    CHECK(process_release(500) == 0, "s7: releasing a held process succeeds");
    CHECK(proc_table[0].state == PROC_SUSPENDED, "s7: its state is back to PROC_SUSPENDED (not directly PROC_RUNNING)");
    CHECK(process_release(500) == -2, "s7: releasing an already-non-held process fails with -2");
    CHECK(process_release(99999) == -1, "s7: releasing a nonexistent pid fails with -1");
    int chosen7 = pick_next_process_in_partition(PARTITION_SYSTEM, -1);
    CHECK(chosen7 == 0, "s7: after release, the process is schedulable again");

    /* ── Scenario 8: process_priority_set() validates range and pid. ──────── */
    CHECK(process_priority_set(500, PROC_PRIO_HIGH) == 0, "s8: setting a valid priority on an active pid succeeds");
    CHECK(proc_table[0].priority == PROC_PRIO_HIGH, "s8: the field was actually updated");
    CHECK(process_priority_set(500, (ProcPriority)99) == -2, "s8: an out-of-range priority value is rejected with -2");
    CHECK(process_priority_set(99999, PROC_PRIO_LOW) == -1, "s8: a nonexistent pid is rejected with -1");

    /* ── Scenario 9: message queues (kernel/msgqueue.c, linked for real). ─── */
    CHECK(mq_create("ops") == 1, "s9: mq_create succeeds for a new name");
    CHECK(mq_create("ops") == 0, "s9: creating a duplicate name fails");
    CHECK(mq_send("nonexistent", 7, "hi", 1) == -1, "s9: sending to a nonexistent queue fails with -1");
    CHECK(mq_send("ops", 7, "first message", 10) == 0, "s9: sending to 'ops' succeeds");
    CHECK(mq_send("ops", 8, "second message", 11) == 0, "s9: a second send to the same queue succeeds");

    struct SLSMsgQueueMessage m;
    CHECK(mq_receive("ops", &m) == 1, "s9: receiving from 'ops' returns a message");
    CHECK(m.sender_uid == 7 && strcmp(m.text, "first message") == 0,
          "s9: FIFO order -- the FIRST message sent is the first one received");
    CHECK(mq_receive("ops", &m) == 1, "s9: a second receive returns the second message");
    CHECK(m.sender_uid == 8 && strcmp(m.text, "second message") == 0,
          "s9: ...and it really is the second one sent, not a repeat of the first");
    CHECK(mq_receive("ops", &m) == 0, "s9: receiving from a now-empty queue returns 0");
    CHECK(mq_receive("nonexistent", &m) == 0, "s9: receiving from a nonexistent queue also returns 0 (looks like empty)");

    /* Fill 'ops' to capacity, then confirm the next send is rejected as full
     * (no silent overwrite -- bump-allocated, no-reclaim posture). */
    for (int i = 0; i < MQ_QUEUE_DEPTH; i++) {
        int rc = mq_send("ops", 1, "filler", (uint64_t)i);
        CHECK(rc == 0, "s9: filling 'ops' back up to MQ_QUEUE_DEPTH succeeds for every message");
    }
    CHECK(mq_send("ops", 1, "one too many", 999) == -2, "s9: sending one more once full fails with -2, not a silent overwrite");

    /* Table-full behavior: MQ_MAX total queues (1 already created: 'ops'). */
    char namebuf[MQ_NAME_LEN];
    int created = 0;
    for (int i = 0; i < MQ_MAX + 2; i++) {
        namebuf[0] = 'q'; namebuf[1] = (char)('0' + (i % 10)); namebuf[2] = (char)('a' + (i / 10)); namebuf[3] = '\0';
        if (mq_create(namebuf)) created++;
    }
    CHECK(created == MQ_MAX - 1,
          "s9: only MQ_MAX-1 more queues can be created after 'ops' -- the table is a real fixed size, not unbounded");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
