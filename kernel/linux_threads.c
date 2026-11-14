/*
 * linux_threads.c — M4.1: the Linux thread-group object and shared-CR3
 * reference accounting.
 *
 * There is still no clone path here. This file makes the lifetime rule
 * explicit before any second execution context exists:
 *
 *   - every Linux task owns exactly one group reference;
 *   - a leader's exit does not destroy the address space while another
 *     thread still holds a reference;
 *   - the last detach returns the PML4 for destruction, after the caller
 *     has torn down the capability table that still needs the table live;
 *   - a group entry is reused only after both counters reach zero.
 *
 * The `attach` function is the M4.2 seam. Its host test uses it to build a
 * multi-thread group and drive the exact last-reference transition without
 * a boot, a page table, or a second scheduler context.
 */

#include "linux_threads.h"
#include "process.h"

_Static_assert(LINUX_THREAD_GROUP_MAX >= PROC_MAX,
               "the group table must cover every process slot");

static struct LinuxThreadGroup g_groups[LINUX_THREAD_GROUP_MAX];

static void thread_fields_clear(struct ProcessDescriptor *pd) {
    if (!pd) return;
    pd->linux_tgid = 0;
    pd->linux_tid = 0;
    pd->linux_clear_child_tid = 0;
    pd->linux_is_thread = 0;
}

static struct LinuxThreadGroup *group_get(uint32_t group_id) {
    if (group_id == 0 || group_id > LINUX_THREAD_GROUP_MAX) return 0;
    struct LinuxThreadGroup *g = &g_groups[group_id - 1];
    return g->used ? g : 0;
}

void linux_thread_groups_reset(void) {
    for (uint32_t i = 0; i < LINUX_THREAD_GROUP_MAX; i++) {
        g_groups[i].cr3 = 0;
        g_groups[i].tgid = 0;
        g_groups[i].cr3_refs = 0;
        g_groups[i].live_threads = 0;
        g_groups[i].used = 0;
    }
}

uint32_t linux_thread_group_create(struct ProcessDescriptor *pd, uint64_t cr3) {
    if (!pd || cr3 == 0 || pd->linux_tgid != 0) return 0;
    for (uint32_t i = 0; i < LINUX_THREAD_GROUP_MAX; i++) {
        struct LinuxThreadGroup *g = &g_groups[i];
        if (g->used) continue;
        g->used = 1;
        g->cr3 = cr3;
        g->tgid = pd->pid;
        g->cr3_refs = 1;
        g->live_threads = 1;
        pd->linux_tgid = i + 1;
        pd->linux_tid = pd->pid;
        pd->linux_is_thread = 0;
        pd->linux_clear_child_tid = 0;
        return i + 1;
    }
    return 0;
}

uint32_t linux_thread_group_attach(struct ProcessDescriptor *pd,
                                   uint32_t group_id, uint32_t tid) {
    if (!pd || tid == 0 || pd->linux_tgid != 0 || pd->cr3 == 0) return 0;
    struct LinuxThreadGroup *g = group_get(group_id);
    if (!g || g->cr3 != pd->cr3) return 0;
    /* A group whose two counters disagree is half-torn: never extend it. */
    if (g->cr3_refs == 0 || g->cr3_refs != g->live_threads ||
        g->cr3_refs == 0xFFFFu) return 0;
    g->cr3_refs++;
    g->live_threads++;
    pd->linux_tgid = group_id;
    pd->linux_tid = tid;
    pd->linux_clear_child_tid = 0;
    pd->linux_is_thread = 1;
    return group_id;
}

int linux_thread_group_detach(struct ProcessDescriptor *pd,
                              uint64_t *cr3_to_destroy) {
    if (cr3_to_destroy) *cr3_to_destroy = 0;
    if (!pd || pd->linux_tgid == 0) return 0;

    uint32_t group_id = pd->linux_tgid;
    struct LinuxThreadGroup *g = group_get(group_id);
    if (!g) {
        /* A group id that does not resolve is stale state, not a shared
         * address space we may free: clear the descriptor fields and let
         * the caller take the legacy single-owner teardown path. */
        thread_fields_clear(pd);
        return 0;
    }

    /* M4.1 invariant: both counters start at one and move together. If they
     * disagree, the group is half-torn. Do not guess which one is right and
     * do not free a PML4 that a live member may still use: clear this
     * descriptor, report the group as handled so the caller skips the
     * legacy destroy, and leave the slot for explicit diagnosis. */
    if (g->cr3_refs == 0 || g->live_threads == 0 ||
        g->cr3_refs != g->live_threads) {
        thread_fields_clear(pd);
        return 1;
    }

    g->cr3_refs--;
    g->live_threads--;
    if (g->cr3_refs == 0 && g->live_threads == 0) {
        if (cr3_to_destroy) *cr3_to_destroy = g->cr3;
        g->cr3 = 0;
        g->tgid = 0;
        g->used = 0;
    }

    thread_fields_clear(pd);
    return 1;
}

uint32_t linux_thread_group_refs(uint32_t group_id) {
    struct LinuxThreadGroup *g = group_get(group_id);
    return g ? g->cr3_refs : 0;
}

uint32_t linux_thread_group_live(uint32_t group_id) {
    struct LinuxThreadGroup *g = group_get(group_id);
    return g ? g->live_threads : 0;
}

uint32_t linux_thread_group_tgid(uint32_t group_id) {
    struct LinuxThreadGroup *g = group_get(group_id);
    return g ? g->tgid : 0;
}

uint64_t linux_thread_group_cr3(uint32_t group_id) {
    struct LinuxThreadGroup *g = group_get(group_id);
    return g ? g->cr3 : 0;
}

int linux_thread_group_used(uint32_t group_id) {
    return group_get(group_id) != 0;
}
