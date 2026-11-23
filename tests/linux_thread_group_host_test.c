/*
 * linux_thread_group_host_test.c — E7 TG-1 increment I1: the group substrate.
 *
 * What this proves (no QEMU, no clone):
 *   - a LINUX descriptor becomes a group of one: group_id/leader/cap owner are
 *     its pid, cr3 is the group's page table, live_threads is 1, and the
 *     descriptor carries tid/tgid/group_index/leader;
 *   - every thread of the group resolves the group's cap owner, so
 *     cap_table_index() would find one capability table and one channel pair;
 *   - the shared span and cursors are the GROUP's: a write through one member
 *     is visible through another, and a group-less descriptor reads zero;
 *   - LINUX_THREADS_MAX is the admission cap (EAGAIN to I2's clone at 8);
 *   - teardown happens ONCE at the group: exactly one cap_table_teardown and
 *     one user_destroy_page_table, a second destroy is a no-op, and the
 *     descriptor identity is cleared only after the group is gone.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/linux_thread_group_host_test \
 *       tests/linux_thread_group_host_test.c kernel/linux_thread.c \
 *       kernel/linux_abi.c
 *   /tmp/linux_thread_group_host_test
 *
 * The capability table and page-table teardown are counted stubs here; the
 * point is the call COUNT and the ids they receive, not either subsystem.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/process.h"
#include "kernel/linux_thread.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* The descriptor table linux_thread.c resolves membership through. */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count;

/* ### The teardown counters: the group must call each exactly once. ### */
static int      g_cap_teardowns;
static uint32_t g_cap_last_pid;
static int      g_page_teardowns;
static uint64_t g_page_last_cr3;

void cap_table_teardown(uint32_t pid) {
    g_cap_teardowns++;
    g_cap_last_pid = pid;
}
void user_destroy_page_table(uint64_t pml4_phys) {
    g_page_teardowns++;
    g_page_last_cr3 = pml4_phys;
}

static void plant(struct ProcessDescriptor* pd, uint32_t pid, uint64_t cr3) {
    memset(pd, 0, sizeof(*pd));
    pd->pid = pid;
    pd->cr3 = cr3;
    pd->active = 1;
    pd->group_index = LINUX_GROUP_NONE;
}

int main(void) {
    printf("=== linux_thread_group: E7 TG-1 I1 group substrate ===\n\n");

    /* ── 1. Init: every slot group-less ──────────────────────────────────── */
    memset(proc_table, 0, sizeof(proc_table));
    linux_groups_init();
    {
        int all_none = 1;
        for (int i = 0; i < PROC_MAX; i++)
            if (proc_table[i].group_index != LINUX_GROUP_NONE) all_none = 0;
        CHECK(all_none && linux_group_find(100) == 0,
              "init marks every descriptor group-less and binds no group");
    }

    /* ── 2. Create: one group of one, identity stamped ───────────────────── */
    plant(&proc_table[0], 100, 0xABCD1000ull);
    CHECK(linux_group_create(&proc_table[0]) == 0,
          "a LINUX descriptor creates its group");
    {
        struct LinuxThreadGroup* g = linux_group_of(&proc_table[0]);
        CHECK(g && g->used && g->live_threads == 1,
              "the group is live with exactly one thread");
        CHECK(g && g->group_id == 100 && g->leader_pid == 100 &&
              g->cap_owner_pid == 100,
              "group id, leader and cap owner are the creating pid");
        CHECK(g && g->cr3 == 0xABCD1000ull,
              "the group owns the descriptor's page-table root");
        CHECK(proc_table[0].tid == 100 && proc_table[0].tgid == 100 &&
              proc_table[0].is_group_leader == 1,
              "the leader's tid/tgid are its pid and it is marked leader");
        CHECK(linux_group_find(100) == g,
              "the group is findable by its own id");
    }

    /* ── 3. Shared span/cursors: stamped once, reached by accessors ──────── */
    linux_group_stamp_memory(&proc_table[0], 0x100000ull, 0x140000ull,
                             0x400000ull, 0x402000ull);
    CHECK(linux_group_heap_base(&proc_table[0]) == 0x100000ull &&
          linux_group_heap_limit(&proc_table[0]) == 0x140000ull &&
          linux_group_brk(&proc_table[0]) == 0x100000ull &&
          linux_group_mmap_top(&proc_table[0]) == 0x140000ull,
          "the stamp puts brk at the heap base and mmap_top at the limit");
    CHECK(linux_group_image_lo(&proc_table[0]) == 0x400000ull &&
          linux_group_image_hi(&proc_table[0]) == 0x402000ull &&
          linux_group_dir_pos(&proc_table[0]) == 0,
          "the image span and the getdents cursor start where the create set them");

    /* ── 4. A second member sees the group, not a private copy ───────────── */
    plant(&proc_table[1], 101, 0xABCD1000ull);   /* same address space */
    proc_table[1].group_index = proc_table[0].group_index;
    proc_table[1].tid = 101;
    proc_table[1].tgid = 100;
    proc_table[1].is_group_leader = 0;
    CHECK(linux_group_of(&proc_table[1]) == linux_group_of(&proc_table[0]) &&
          linux_group_brk(&proc_table[1]) == 0x100000ull,
          "a member resolves the same group and the same break");
    linux_group_set_brk(&proc_table[1], 0x102000ull);
    CHECK(linux_group_brk(&proc_table[0]) == 0x102000ull,
          "a break written through one member is the break the other sees");
    linux_group_set_mmap_top(&proc_table[1], 0x13F000ull);
    linux_group_add_dir_pos(&proc_table[1], 7);
    CHECK(linux_group_mmap_top(&proc_table[0]) == 0x13F000ull &&
          linux_group_dir_pos(&proc_table[0]) == 7,
          "the mmap cursor and the directory cursor are shared too");

    /* ── 5. Group-aware cap ownership ────────────────────────────────────── */
    CHECK(proc_group_cap_owner(101) == 100 &&
          proc_group_cap_owner(100) == 100,
          "a member and the leader both resolve the leader's cap table");
    plant(&proc_table[2], 200, 0x22222000ull);
    CHECK(proc_group_cap_owner(200) == 200,
          "a descriptor with no group keeps its own cap table");

    /* ── 6. Admission cap ────────────────────────────────────────────────── */
    {
        struct LinuxThreadGroup* g = linux_group_of(&proc_table[0]);
        int admitted = 0;
        while (linux_group_add_thread(g) == 0) admitted++;
        CHECK(g->live_threads == LINUX_THREADS_MAX && admitted == 7,
              "the group admits up to LINUX_THREADS_MAX (1 + 7 clones)");
        CHECK(linux_group_add_thread(g) == -1 && g->live_threads == LINUX_THREADS_MAX,
              "the next admission is refused without changing the count");
    }

    /* ── 7. Destroy: one cap teardown, one page teardown, one identity clear ─ */
    {
        struct LinuxThreadGroup* g = linux_group_of(&proc_table[0]);
        linux_group_destroy(g);
        CHECK(g_cap_teardowns == 1 && g_cap_last_pid == 100,
              "the group's destroy runs exactly one cap-table teardown, for the cap owner");
        CHECK(g_page_teardowns == 1 && g_page_last_cr3 == 0xABCD1000ull,
              "and exactly one page-table teardown, for the group's cr3");
        CHECK(!g->used && linux_group_of(&proc_table[0]) == 0,
              "the group slot is released and no descriptor resolves it");
        linux_group_destroy(g);
        CHECK(g_cap_teardowns == 1 && g_page_teardowns == 1,
              "a second destroy is a no-op (no double free)");
        linux_group_clear_identity(&proc_table[0]);
        CHECK(proc_table[0].tid == 0 && proc_table[0].tgid == 0 &&
              proc_table[0].group_index == LINUX_GROUP_NONE &&
              proc_table[0].is_group_leader == 0,
              "clearing the identity after destroy leaves no group index behind");
    }

    /* ── 8. A group-less descriptor reads zero through every accessor ─────── */
    CHECK(linux_group_heap_base(&proc_table[2]) == 0 &&
          linux_group_heap_limit(&proc_table[2]) == 0 &&
          linux_group_brk(&proc_table[2]) == 0 &&
          linux_group_mmap_top(&proc_table[2]) == 0 &&
          linux_group_image_lo(&proc_table[2]) == 0 &&
          linux_group_image_hi(&proc_table[2]) == 0 &&
          linux_group_dir_pos(&proc_table[2]) == 0,
          "an SLS/no-group descriptor reads zero from every shared accessor");
    linux_group_set_brk(&proc_table[2], 0x9999ull);
    CHECK(linux_group_brk(&proc_table[2]) == 0,
          "a setter on a group-less descriptor is a no-op");

    /* ── 9. Table exhaustion refuses rather than aliasing a live group ───── */
    {
        int made = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            plant(&proc_table[i], (uint32_t)(300 + i), 0x30000000ull + (uint64_t)i * 0x1000);
            if (linux_group_create(&proc_table[i]) == 0) made++;
        }
        CHECK(made == PROC_MAX,
              "the table holds PROC_MAX groups (one per process-table slot)");
        plant(&proc_table[0], 999, 0x99999000ull);
        CHECK(linux_group_create(&proc_table[0]) == -1,
              "a create with no free group slot fails instead of aliasing one");
        /* Destroy from the GROUP table: the failed create above repurposed a
         * descriptor, so membership is no longer a complete list. */
        for (int i = 0; i < PROC_MAX; i++)
            if (linux_group_table[i].used)
                linux_group_destroy(&linux_group_table[i]);
        CHECK(g_cap_teardowns == 1 + PROC_MAX && g_page_teardowns == 1 + PROC_MAX,
              "each created group tears down once, and only once");
    }

    printf("\nlinux_thread_group_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
