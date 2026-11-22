/*
 * linux_thread.c — E7 TG-1 increment I1: the Linux thread-group substrate.
 *
 * The design is in linux_thread.h. Two things here are load-bearing:
 *
 *   1. THE GROUP OWNS THE SHARED STATE. The create stamps the span and cursors
 *      onto the group once; every thread reaches them through the accessors,
 *      and the page-table root is the group's. No descriptor keeps a writable
 *      private copy, so a second thread cannot silently fork the break, the
 *      mmap cursor, the directory cursor or the address space by being
 *      scheduled. Descriptor fields are identity only: tid, tgid, group index,
 *      leader bit.
 *
 *   2. TEARDOWN HAPPENS ONCE, AT THE GROUP. I1 has no clone, but the single
 *      Linux task is already a group of one, and the three teardown sites
 *      (process_exit, process_kill, the deferred schedule path) route through
 *      linux_group_destroy() instead of calling cap_table_teardown() and
 *      user_destroy_page_table() per descriptor. The counter matters: I2 will
 *      let a sibling exit while the group lives, and a per-thread page-table
 *      free there is the double-teardown fault the plan's TG-1 teeth name.
 *
 * The group table is a fixed array sized PROC_MAX, like proc_table itself. The
 * one group per environment rule is enforced by the create (only a LINUX
 * descriptor gets a group) and by LINUX_THREADS_MAX (I2's clone admission).
 */

#include "linux_thread.h"
#include "process.h"

/* Declared directly rather than through cap.h/user_paging.h: this file is
 * compiled into host tests that stub both and do not want the capability
 * layer's transitive includes. */
void cap_table_teardown(uint32_t pid);
void user_destroy_page_table(uint64_t pml4_phys);

struct LinuxThreadGroup linux_group_table[PROC_MAX];

void linux_groups_init(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct LinuxThreadGroup* g = &linux_group_table[i];
        g->used = 0;
        g->exiting = 0;
        g->live_threads = 0;
        g->fwd_busy = 0;
        g->group_id = 0;
        g->leader_pid = 0;
        g->cap_owner_pid = 0;
        g->fwd_owner_tid = 0;
        g->fwd_wait_mask = 0;
        g->cr3 = 0;
        g->heap_base = 0;
        g->heap_limit = 0;
        g->brk_cur = 0;
        g->mmap_top = 0;
        g->image_lo = 0;
        g->image_hi = 0;
        g->dir_pos = 0;
        linux_group_clear_identity(&proc_table[i]);
    }
}

struct LinuxThreadGroup* linux_group_of(const struct ProcessDescriptor* pd) {
    if (!pd) return 0;
    if (pd->group_index >= (uint32_t)PROC_MAX) return 0;
    struct LinuxThreadGroup* g = &linux_group_table[pd->group_index];
    if (!g->used) return 0;
    return g;
}

struct LinuxThreadGroup* linux_group_find(uint32_t group_id) {
    if (group_id == 0) return 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (linux_group_table[i].used &&
            linux_group_table[i].group_id == group_id)
            return &linux_group_table[i];
    return 0;
}

uint32_t proc_group_cap_owner(uint32_t pid) {
    if (pid == 0) return 0;
    /* The descriptor is the membership record: a pid is in a group only while
     * its descriptor says so, so a dead member never resolves a live group's
     * table (and a reused pid starts group-less). */
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].pid != pid) continue;
        struct LinuxThreadGroup* g = linux_group_of(&proc_table[i]);
        if (g) return g->cap_owner_pid;
    }
    return pid;
}

int linux_group_create(struct ProcessDescriptor* pd) {
    if (!pd || pd->group_index != LINUX_GROUP_NONE) return -1;
    for (int i = 0; i < PROC_MAX; i++) {
        struct LinuxThreadGroup* g = &linux_group_table[i];
        if (g->used) continue;
        for (int j = 0; j < (int)sizeof(*g); j++) ((uint8_t*)g)[j] = 0;
        g->used = 1;
        g->group_id = pd->pid;
        g->leader_pid = pd->pid;
        g->cap_owner_pid = pd->pid;
        g->cr3 = pd->cr3;
        g->live_threads = 1;
        pd->tid = pd->pid;
        pd->tgid = pd->pid;
        pd->group_index = (uint32_t)i;
        pd->is_group_leader = 1;
        pd->sysret_result = 0;
        return 0;
    }
    return -1;
}

void linux_group_stamp_memory(struct ProcessDescriptor* pd,
                              uint64_t heap_base, uint64_t heap_limit,
                              uint64_t image_lo, uint64_t image_hi) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    if (!g) return;
    g->heap_base = heap_base;
    g->heap_limit = heap_limit;
    g->brk_cur = heap_base;
    g->mmap_top = heap_limit;
    g->image_lo = image_lo;
    g->image_hi = image_hi;
    g->dir_pos = 0;
}

int linux_group_add_thread(struct LinuxThreadGroup* g) {
    if (!g || !g->used || g->exiting) return -1;
    if (g->live_threads >= LINUX_THREADS_MAX) return -1;
    g->live_threads++;
    return 0;
}

void linux_group_destroy(struct LinuxThreadGroup* g) {
    if (!g || !g->used) return;
    if (g->exiting) return;
    g->exiting = 1;
    /* The ONE cap-table teardown for the group. It unmaps the group's
     * cap-derived PTEs through cap_proc_cr3(cap_owner_pid), which is why the
     * owner descriptor must still be active here — the callers destroy the
     * group before clearing the descriptor. */
    cap_table_teardown(g->cap_owner_pid);
    /* The ONE address-space teardown for the group. */
    if (g->cr3) user_destroy_page_table(g->cr3);
    g->used = 0;
    g->exiting = 0;
    g->live_threads = 0;
    g->fwd_busy = 0;
    g->group_id = 0;
    g->leader_pid = 0;
    g->cap_owner_pid = 0;
    g->fwd_owner_tid = 0;
    g->fwd_wait_mask = 0;
    g->cr3 = 0;
    g->heap_base = 0;
    g->heap_limit = 0;
    g->brk_cur = 0;
    g->mmap_top = 0;
    g->image_lo = 0;
    g->image_hi = 0;
    g->dir_pos = 0;
}

void linux_group_clear_identity(struct ProcessDescriptor* pd) {
    if (!pd) return;
    pd->tid = 0;
    pd->tgid = 0;
    pd->group_index = LINUX_GROUP_NONE;
    pd->is_group_leader = 0;
    pd->sysret_result = 0;
}

/* ─── Shared-memory accessors ───────────────────────────────────────────── */

uint64_t linux_group_heap_base(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->heap_base : 0;
}

uint64_t linux_group_heap_limit(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->heap_limit : 0;
}

uint64_t linux_group_brk(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->brk_cur : 0;
}

void linux_group_set_brk(struct ProcessDescriptor* pd, uint64_t brk) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    if (g) g->brk_cur = brk;
}

uint64_t linux_group_mmap_top(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->mmap_top : 0;
}

void linux_group_set_mmap_top(struct ProcessDescriptor* pd, uint64_t top) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    if (g) g->mmap_top = top;
}

uint64_t linux_group_image_lo(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->image_lo : 0;
}

uint64_t linux_group_image_hi(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->image_hi : 0;
}

uint64_t linux_group_dir_pos(const struct ProcessDescriptor* pd) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    return g ? g->dir_pos : 0;
}

void linux_group_set_dir_pos(struct ProcessDescriptor* pd, uint64_t pos) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    if (g) g->dir_pos = pos;
}

void linux_group_add_dir_pos(struct ProcessDescriptor* pd, uint64_t add) {
    struct LinuxThreadGroup* g = linux_group_of(pd);
    if (g) g->dir_pos += add;
}
