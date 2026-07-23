/*
 * scheduler_fairness_host_test.c — Phase 12 verification: a standalone
 * host-buildable test for kernel/process.c's new partition-fair
 * scheduling helpers, pick_next_partition() and
 * pick_next_process_in_partition(), executed as the REAL, unmodified
 * functions — not a reimplementation.
 *
 * schedule_ring3() itself can't be host-tested: its tail unconditionally
 * executes a privileged `mov cr3` instruction that would fault in Linux
 * user space. But the fairness ALGORITHM was deliberately factored out
 * into two pure, static helper functions that never touch CR3 or the
 * interrupt-stack frame — this test reaches them by compiling
 * kernel/process.c directly into this translation unit (`#include
 * "kernel/process.c"`, a legitimate C technique for exercising a file's
 * `static` internals without changing their linkage), after providing
 * stub definitions for process.c's heavier dependencies (loader,
 * user_paging, object_catalog, kernel_io) — none of which this test's
 * flow ever actually calls, since it drives the two helpers directly
 * rather than going through process_create()/program_spawn()/
 * schedule_ring3() itself.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 -I user \
 *       -o /tmp/scheduler_fairness_host_test \
 *       tests/scheduler_fairness_host_test.c kernel/partition.c
 *   /tmp/scheduler_fairness_host_test
 */
#include "kernel/process.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ─── Stubs for process.c's dependencies this test never actually calls ──
 * All of these back process_create()/program_spawn(), which this test
 * doesn't invoke — they exist purely so #include-ing process.c as source
 * compiles and links. Signatures must match the real headers exactly
 * (process.c includes those same headers, so a mismatch would be a
 * compile error, not a silent bug). */
void kernel_serial_print(const char* s) { (void)s; }
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) { (void)partition_id; (void)dest_node_id; return 0; }  /* Multi-Node Phase 6 addendum -- not exercised by this test, permissive "nothing to relocate" stub */
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
void persist_partitions(void) { /* Phase 10's persistence hook — irrelevant here */ }

/* Phase 14 (LPAR): partition_destroy() (kernel/partition.c, linked for real
 * below) calls into object_catalog.c and frame_pool.c, both of which have
 * dependency graphs far too heavy to link here (object_catalog.c alone
 * pulls in journal/lock_mgr/index_mgr/constraint/mqt — a dozen-plus extern
 * functions, the same category of "too heavy to host-test cheaply" this
 * project has accepted since Phase 9). These two stubs stand in for them,
 * but — unlike the pure "never called" stubs above — they record whether
 * and with what argument they were invoked, so this test can still verify
 * partition_destroy()'s REAL orchestration logic (does it call the right
 * things, with the right partition_id, in a flow that still ends in a
 * correctly-torn-down partition_table[]/partition_assign_table[]) even
 * though the two cross-subsystem calls themselves are faked. */
static int      g_vfree_partition_calls = 0;
static uint32_t g_vfree_partition_last_arg = 0xFFFFFFFFu;
uint32_t catalog_vfree_partition(uint32_t partition_id) {
    g_vfree_partition_calls++;
    g_vfree_partition_last_arg = partition_id;
    return 3;   /* pretend value, only used to confirm it's plumbed through if needed */
}
/* Multi-Node Partition Scaling Roadmap Phase 3: partition_destroy() now
 * calls partition_reclaim_all_frames() instead of partition_reset_frame_
 * usage() at this step -- same call-tracking-stub technique, just tracking
 * the new function's calls instead of the old one's. */
static int      g_reclaim_calls = 0;
static uint32_t g_reclaim_last_arg = 0xFFFFFFFFu;
uint32_t partition_reclaim_all_frames(uint32_t partition_id) {
    g_reclaim_calls++;
    g_reclaim_last_arg = partition_id;
    return 7;   /* pretend value, only used to confirm it's plumbed through into the log line if needed */
}
uint32_t cluster_local_node_id(void) { return 0; }   /* Multi-Node Partition Scaling Roadmap Phase 2 */
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }   /* Multi-Node Partition Scaling Roadmap Phase 6 -- not exercised by this test, safe "nothing to relinquish" stub */

/* Pull in process.c itself — this is what makes pick_next_partition()/
 * pick_next_process_in_partition() (both `static`) reachable from this
 * test's main(), and gives us the REAL proc_table[]/proc_count globals
 * process.c defines, rather than redefining our own. */
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

static void add_runnable(int idx, uint32_t pid, uint32_t partition_id) {
    proc_table[idx].active       = 1;
    proc_table[idx].pid          = pid;
    proc_table[idx].partition_id = partition_id;
    proc_table[idx].state        = PROC_SUSPENDED;
    proc_table[idx].kernel_rsp   = 1;   /* nonzero sentinel — "actually entered" */
}

int main(void) {
    /* ── Scenario 1: uneven partition sizes — 5 processes in PARTITION_
     * SYSTEM (slots 0-4), 1 process in partition 7 (slot 5). Simulate 20
     * consecutive scheduling ticks the same way schedule_ring3() chains
     * them (chosen partition/process becomes the next tick's "current"),
     * and confirm partition-level turns split roughly evenly — NOT
     * proportional to how many processes each partition has, which is
     * exactly the bug Phase 12 exists to fix. ─────────────────────────── */
    reset_proc_table();
    add_runnable(0, 100, PARTITION_SYSTEM);
    add_runnable(1, 101, PARTITION_SYSTEM);
    add_runnable(2, 102, PARTITION_SYSTEM);
    add_runnable(3, 103, PARTITION_SYSTEM);
    add_runnable(4, 104, PARTITION_SYSTEM);
    add_runnable(5, 200, 7);

    int partition_turns[2] = {0, 0};   /* [0]=PARTITION_SYSTEM, [1]=partition 7 */
    int distinct_system_slots_seen = 0;
    uint8_t seen_slot[PROC_MAX]; memset(seen_slot, 0, sizeof(seen_slot));
    int current_idx = 0;   /* tick 0 starts with slot 0 "current" */
    uint32_t last_partition = PARTITION_SYSTEM;

    for (int tick = 0; tick < 20; tick++) {
        uint32_t chosen_partition;
        int found = pick_next_partition(last_partition, &chosen_partition);
        CHECK(found, "scenario 1: some partition always has a runnable process (never starves entirely)");
        if (!found) break;

        int next_idx = pick_next_process_in_partition(chosen_partition, current_idx);
        CHECK(next_idx >= 0, "scenario 1: chosen partition always has a pickable process");
        if (next_idx < 0) break;

        if (chosen_partition == PARTITION_SYSTEM) {
            partition_turns[0]++;
            if (!seen_slot[next_idx]) { seen_slot[next_idx] = 1; distinct_system_slots_seen++; }
        } else {
            partition_turns[1]++;
        }

        current_idx    = next_idx;
        last_partition = chosen_partition;
    }

    printf("   (scenario 1 tallies: PARTITION_SYSTEM turns=%d, partition-7 turns=%d)\n",
           partition_turns[0], partition_turns[1]);
    int diff = partition_turns[0] - partition_turns[1];
    if (diff < 0) diff = -diff;
    CHECK(diff <= 1, "scenario 1: 5-process partition and 1-process partition get near-equal turns (fair), not 5:1 (proportional to process count)");
    CHECK(distinct_system_slots_seen >= 2, "scenario 1: PARTITION_SYSTEM's turns rotate across more than one of its 5 processes (intra-partition fairness too)");

    /* ── Scenario 2: single-partition system — confirm this reduces to
     * exactly the pre-Phase-12 flat round robin (backward compatible by
     * construction, not just by argument). ───────────────────────────── */
    reset_proc_table();
    for (int i = 0; i < 6; i++) add_runnable(i, 300 + i, PARTITION_SYSTEM);
    current_idx = 0;
    last_partition = PARTITION_SYSTEM;
    uint8_t single_seen[6] = {0};
    for (int tick = 0; tick < 6; tick++) {
        uint32_t chosen_partition;
        pick_next_partition(last_partition, &chosen_partition);
        int next_idx = pick_next_process_in_partition(chosen_partition, current_idx);
        CHECK(chosen_partition == PARTITION_SYSTEM, "scenario 2: only partition present is always the one chosen");
        single_seen[next_idx] = 1;
        current_idx = next_idx;
        last_partition = chosen_partition;
    }
    int all_seen = 1;
    for (int i = 0; i < 6; i++) if (!single_seen[i]) all_seen = 0;
    CHECK(all_seen, "scenario 2: over 6 ticks in a 6-process single partition, every process gets a turn (matches old flat round robin)");

    /* ── Scenario 3: the single-process-in-its-own-partition edge case —
     * pick_next_partition() finds the partition (it doesn't know about
     * "current"), but pick_next_process_in_partition() correctly excludes
     * current_idx's own slot and returns -1, so schedule_ring3()'s
     * existing "!next -> resume current immediately" fallback still
     * fires exactly as it did pre-Phase-12. ────────────────────────────── */
    reset_proc_table();
    add_runnable(0, 400, 3);   /* the only process, in partition 3, "current" */
    uint32_t chosen_partition;
    int found3 = pick_next_partition(PARTITION_SYSTEM, &chosen_partition);
    CHECK(found3 && chosen_partition == 3, "scenario 3: pick_next_partition finds the lone process's partition");
    int next_idx3 = pick_next_process_in_partition(chosen_partition, /*exclude_idx=*/0);
    CHECK(next_idx3 == -1, "scenario 3: pick_next_process_in_partition correctly excludes current_idx itself, returns -1 -> fallback path fires");

    /* ── Scenario 4: nothing runnable at all ──────────────────────────── */
    reset_proc_table();
    proc_table[0].active = 1; proc_table[0].state = PROC_RUNNING; proc_table[0].kernel_rsp = 1; /* RUNNING, not SUSPENDED */
    uint32_t dummy;
    CHECK(pick_next_partition(PARTITION_SYSTEM, &dummy) == 0, "scenario 4: no SUSPENDED process anywhere -> pick_next_partition reports nothing runnable");

    /* ── Scenario 5 (Phase 14): process_kill_partition() kills every active
     * process in one partition and leaves every other partition's processes
     * (and proc_count's own bookkeeping) completely untouched. ──────────── */
    reset_proc_table();
    add_runnable(0, 600, PARTITION_SYSTEM);
    add_runnable(1, 601, PARTITION_SYSTEM);
    add_runnable(2, 602, 9);
    proc_count = 3;   /* add_runnable() doesn't touch proc_count -- set it up honestly, matching what process_create() would have left behind */
    uint32_t killed5 = process_kill_partition(PARTITION_SYSTEM);
    CHECK(killed5 == 2, "scenario 5: process_kill_partition kills exactly the 2 matching PARTITION_SYSTEM processes");
    CHECK(!proc_table[0].active && proc_table[0].state == PROC_ZOMBIE, "scenario 5: slot 0 (PARTITION_SYSTEM) is now inactive/ZOMBIE");
    CHECK(!proc_table[1].active && proc_table[1].state == PROC_ZOMBIE, "scenario 5: slot 1 (PARTITION_SYSTEM) is now inactive/ZOMBIE");
    CHECK(proc_table[2].active && proc_table[2].partition_id == 9, "scenario 5: slot 2 (partition 9) is completely untouched by killing partition 0");
    CHECK(proc_count == 1, "scenario 5: proc_count decremented by exactly 2 (3 -> 1), matching process_kill()'s own bookkeeping exactly");
    uint32_t killed5b = process_kill_partition(PARTITION_SYSTEM);
    CHECK(killed5b == 0, "scenario 5: calling again on an already-empty partition kills 0 -- not an error, not a crash");

    /* ── Scenario 6 (Phase 14): a paused partition is skipped by
     * pick_next_partition() entirely, even with runnable processes;
     * resuming makes it selectable again. Real partition_pause()/
     * partition_resume()/partition_is_paused() from the linked, unmodified
     * kernel/partition.c -- not stubs. ────────────────────────────────────── */
    reset_proc_table();
    add_runnable(0, 700, PARTITION_SYSTEM);
    add_runnable(1, 701, 7);
    CHECK(partition_pause(PARTITION_SYSTEM) == 0, "scenario 6: pausing PARTITION_SYSTEM succeeds (it's always a valid id, no partition_create() needed)");
    CHECK(partition_is_paused(PARTITION_SYSTEM) == 1, "scenario 6: partition_is_paused reflects the pause");
    uint32_t chosen6;
    /* last_partition=7 -> search order hits candidate 0 (SYSTEM) well before
     * wrapping back to candidate 7 -- so this proves SYSTEM was genuinely
     * skipped, not just "never reached". */
    int found6 = pick_next_partition(7, &chosen6);
    CHECK(found6 && chosen6 == 7, "scenario 6: paused PARTITION_SYSTEM is skipped even though it has a runnable process -- partition 7 is chosen instead");
    CHECK(partition_resume(PARTITION_SYSTEM) == 0, "scenario 6: resuming PARTITION_SYSTEM succeeds");
    CHECK(partition_is_paused(PARTITION_SYSTEM) == 0, "scenario 6: partition_is_paused reflects the resume");
    uint32_t chosen6b;
    int found6b = pick_next_partition(7, &chosen6b);
    CHECK(found6b && chosen6b == 0, "scenario 6: after resuming, PARTITION_SYSTEM is selectable again and wins its normal search-order priority");

    /* ── Scenario 7 (Phase 14): partition_destroy()'s real orchestration --
     * real partition_init()/partition_create()/partition_assign_uid()/
     * partition_get_for_uid() (kernel/partition.c) and real
     * process_kill_partition() (kernel/process.c, both linked as actual
     * source, not stubs) driven through an actual destroy call. Only the
     * two cross-subsystem calls (catalog vfree, frame usage reset) are
     * faked, per the stub comment above. ─────────────────────────────────── */
    partition_init();
    uint32_t tid = partition_create("tenant-a");
    CHECK(tid != 0xFFFFFFFFu, "scenario 7: partition_create succeeds, hands out a fresh id");
    CHECK(partition_assign_uid(50, tid) == 0, "scenario 7: uid 50 assigned into the new partition");
    CHECK(partition_get_for_uid(50) == tid, "scenario 7: uid 50 now resolves to the new partition");

    reset_proc_table();
    add_runnable(0, 800, tid);
    add_runnable(1, 801, tid);
    add_runnable(2, 802, PARTITION_SYSTEM);   /* must survive untouched */
    proc_count = 3;

    g_vfree_partition_calls = 0;
    g_reclaim_calls = 0;
    int rc = partition_destroy(tid);
    CHECK(rc == 0, "scenario 7: partition_destroy succeeds for a real, active, non-system partition");
    CHECK(!proc_table[0].active && !proc_table[1].active, "scenario 7: both of the destroyed partition's processes are now inactive (real process_kill_partition(), not a stub)");
    CHECK(proc_table[2].active && proc_table[2].partition_id == PARTITION_SYSTEM, "scenario 7: the unrelated PARTITION_SYSTEM process survives destroy untouched");
    CHECK(proc_count == 1, "scenario 7: proc_count reflects exactly the 2 real kills");
    CHECK(g_vfree_partition_calls == 1 && g_vfree_partition_last_arg == tid, "scenario 7: catalog_vfree_partition was called exactly once, with the destroyed partition's id");
    CHECK(g_reclaim_calls == 1 && g_reclaim_last_arg == tid, "scenario 7: partition_reclaim_all_frames was called exactly once, with the destroyed partition's id (Multi-Node Partition Scaling Roadmap Phase 3 -- replaces the old partition_reset_frame_usage() call site)");
    CHECK(partition_get_for_uid(50) == PARTITION_DEFAULT, "scenario 7: uid 50's assignment was cleared -- resolves back to PARTITION_DEFAULT, exactly as if never assigned");
    CHECK(partition_is_paused(tid) == 0, "scenario 7: destroy also clears any stale pause flag on the freed slot");

    uint32_t tid2 = partition_create("tenant-b");
    CHECK(tid2 == tid, "scenario 7: the destroyed partition's slot is reused by the next partition_create() call");

    CHECK(partition_destroy(PARTITION_SYSTEM) == 1, "scenario 7: PARTITION_SYSTEM can never be destroyed");
    CHECK(partition_destroy(12345) == 1, "scenario 7: destroying an undefined/never-created partition id fails cleanly");
    /* tid2 == tid: the freed slot was reused by "tenant-b", so destroying
     * tid now legitimately destroys tenant-b (a real, active partition) --
     * it should succeed, not fail. */
    CHECK(partition_destroy(tid) == 0, "scenario 7: destroying the reused slot (now 'tenant-b') succeeds -- it's a real, active partition again");
    CHECK(partition_destroy(tid) == 1, "scenario 7: destroying it a SECOND time (now truly empty, nothing occupies the slot) fails cleanly -- not a double-free crash");

    /* ── Scenario 8 (Multitenant Isolation Gap Analysis §5/§7 item 8): weighted
     * CPU scheduling. Real partition_set_cpu_weight()/partition_get_cpu_weight()
     * and the modified pick_next_partition() (all linked as real source, not
     * stubs). Partition 2 gets weight=3, partition 5 stays at the implicit
     * default weight=1 -- confirm partition 2 receives exactly 3x partition
     * 5's consecutive turns, deterministically, over whole rounds. ────────── */
    for (int i = 0; i < PARTITION_MAX; i++) partition_cpu_weight[i] = 0;   /* clean slate -- don't inherit state from any earlier scenario */
    g_partition_turns_remaining = 0;

    CHECK(partition_get_cpu_weight(2) == 1, "scenario 8: an unconfigured partition reports the default weight of 1");
    CHECK(partition_set_cpu_weight(2, 3) == 0, "scenario 8: setting partition 2's weight to 3 succeeds");
    CHECK(partition_get_cpu_weight(2) == 3, "scenario 8: partition 2 now reports weight 3");
    CHECK(partition_set_cpu_weight(PARTITION_MAX, 5) == 1, "scenario 8: setting a weight on an out-of-range partition id fails cleanly");
    CHECK(partition_get_cpu_weight(PARTITION_MAX) == 1, "scenario 8: an out-of-range partition id reads back as the safe default (1), never a garbage/OOB value");

    reset_proc_table();
    add_runnable(0, 900, 2);   /* partition 2: weight 3 */
    add_runnable(1, 901, 5);   /* partition 5: weight 1 (default) */

    int turns_a = 0, turns_b = 0;
    int cur8 = 0;
    uint32_t lastp8 = 5;   /* start "as if" partition 5 just ran, so the first rotation search is unbiased toward partition 2 */
    for (int tick = 0; tick < 16; tick++) {
        uint32_t chosen8;
        int found8 = pick_next_partition(lastp8, &chosen8);
        CHECK(found8, "scenario 8: a runnable partition is always found across all 16 ticks");
        if (!found8) break;
        int idx8 = pick_next_process_in_partition(chosen8, cur8);
        if (idx8 < 0) idx8 = cur8;   /* single process per partition -- pick_next_process_in_partition legitimately returns -1 here; reuse the known slot */
        if (chosen8 == 2) turns_a++; else if (chosen8 == 5) turns_b++;
        cur8 = idx8;
        lastp8 = chosen8;
    }
    printf("   (scenario 8 tallies over 16 ticks: partition 2 (weight 3) turns=%d, partition 5 (weight 1) turns=%d)\n", turns_a, turns_b);
    CHECK(turns_a + turns_b == 16, "scenario 8: every tick landed on one of the two configured partitions");
    CHECK(turns_a == 3 * turns_b, "scenario 8: partition 2 (weight 3) gets exactly 3x partition 5's (weight 1) consecutive turns, deterministically");

    /* Reset back to weight=0 (default) confirms the "0 == reset to default" convention. */
    CHECK(partition_set_cpu_weight(2, 0) == 0, "scenario 8: setting weight to 0 succeeds (0 == reset to default)");
    CHECK(partition_get_cpu_weight(2) == 1, "scenario 8: after resetting to 0, partition 2 reads back as the default weight 1");
    for (int i = 0; i < PARTITION_MAX; i++) partition_cpu_weight[i] = 0;   /* leave clean for any future scenario */
    g_partition_turns_remaining = 0;

    /* ── Scenario 9: re-run Scenario 1's exact flow now that the weighted-
     * scheduling code path exists, proving the default-weight-everywhere case
     * is byte-for-byte unchanged (g_partition_turns_remaining never becomes
     * nonzero when every partition is at the implicit default weight 1). ─── */
    reset_proc_table();
    add_runnable(0, 100, PARTITION_SYSTEM);
    add_runnable(1, 101, PARTITION_SYSTEM);
    add_runnable(2, 102, PARTITION_SYSTEM);
    add_runnable(3, 103, PARTITION_SYSTEM);
    add_runnable(4, 104, PARTITION_SYSTEM);
    add_runnable(5, 200, 7);
    int partition_turns9[2] = {0, 0};
    int current_idx9 = 0;
    uint32_t last_partition9 = PARTITION_SYSTEM;
    for (int tick = 0; tick < 20; tick++) {
        uint32_t chosen9;
        int found9 = pick_next_partition(last_partition9, &chosen9);
        if (!found9) break;
        int next_idx9 = pick_next_process_in_partition(chosen9, current_idx9);
        if (next_idx9 < 0) break;
        if (chosen9 == PARTITION_SYSTEM) partition_turns9[0]++; else partition_turns9[1]++;
        current_idx9 = next_idx9;
        last_partition9 = chosen9;
    }
    int diff9 = partition_turns9[0] - partition_turns9[1];
    if (diff9 < 0) diff9 = -diff9;
    CHECK(diff9 <= 1, "scenario 9: with no weights configured anywhere, behavior reproduces scenario 1's fair (not proportional) split exactly");
    CHECK(g_partition_turns_remaining == 0, "scenario 9: g_partition_turns_remaining never becomes nonzero when every partition sits at the default weight 1 (backward-compatibility guarantee, verified directly)");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
