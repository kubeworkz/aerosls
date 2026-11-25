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
#include "linux_abi.h"
#include "linux_futex.h"
#include "process.h"

/* Declared directly rather than through cap.h/user_paging.h: this file is
 * compiled into host tests that stub both and do not want the capability
 * layer's transitive includes. */
void cap_table_teardown(uint32_t pid);
void user_destroy_page_table(uint64_t pml4_phys);

struct LinuxThreadGroup linux_group_table[PROC_MAX];

/* ─── I2: clone planning and context synthesis ─────────────────────────────
 * Both are pure. The flag check is deliberately strict about the six shared
 * flags: this kernel's group has no private fd table, fs root or signal
 * handler table to hand a child that omitted one, so accepting the subset
 * would return success for a semantic the next syscall would violate. */
int linux_clone_plan(uint64_t flags, uint64_t child_stack, uint64_t tls,
                     uint64_t parent_fs_base, struct LinuxClonePlan* out) {
    if (!out) return -LINUX_EINVAL;
    out->flags = 0;
    out->child_stack = 0;
    out->tls = 0;
    out->fs_base = 0;

    /* Unknown/vfork/parent/namespace/ptrace/clear-TID bits all fail this
     * one mask check. CLONE_CHILD_CLEARTID is intentionally absent: Go's
     * measured path does not use it and v1 makes no clear-TID promise. */
    if (flags & ~LINUX_CLONE_ALLOWED) return -LINUX_EINVAL;
    if ((flags & LINUX_CLONE_GO_FLAGS) != LINUX_CLONE_GO_FLAGS)
        return -LINUX_EINVAL;

    /* Linux's own consistency rules, named separately so a future shape
     * change cannot accidentally accept CLONE_THREAD alone. */
    if (!(flags & LINUX_CLONE_THREAD)) return -LINUX_EINVAL;
    if (!(flags & LINUX_CLONE_VM) || !(flags & LINUX_CLONE_SIGHAND))
        return -LINUX_EINVAL;

    /* The kernel never allocates this stack — Go mapped it in the shared
     * heap before trapping — so zero/non-canonical means the child would
     * iretq into a non-address. */
    if (child_stack == 0 || !linux_fs_addr_is_user_canonical(child_stack))
        return -LINUX_EINVAL;

    out->flags = flags;
    out->child_stack = child_stack;
    out->tls = tls;
    if (flags & LINUX_CLONE_SETTLS) {
        if (!linux_fs_addr_is_user_canonical(tls)) return -LINUX_EINVAL;
        out->fs_base = tls;
    } else {
        out->fs_base = parent_fs_base;
    }
    return 0;
}

void linux_clone_build_context(struct TaskContext* ctx, uint64_t parent_rip,
                               uint64_t child_stack) {
    if (!ctx) return;
    uint64_t* w = (uint64_t*)ctx;
    for (int i = 0; i < 20; i++) w[i] = 0;
    /* The clone wrapper's child lands at the instruction after syscall with
     * rsi still holding its stack argument; Go's asm moves it to SP before
     * entering mstart. rax = 0 is the child's clone return. */
    ctx->rsi   = child_stack;
    ctx->rax   = 0;
    ctx->rip   = parent_rip;
    ctx->cs    = 0x23;   /* ring-3 code */
    ctx->rflags = 0x202; /* IF on */
    ctx->rsp   = child_stack;
    ctx->ss    = 0x1B;   /* ring-3 data */
}

void linux_groups_init(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct LinuxThreadGroup* g = &linux_group_table[i];
        g->used = 0;
        g->exiting = 0;
        g->live_threads = 0;
        g->fwd_busy = 0;
        g->barrier_reported = 0;
        g->group_id = 0;
        g->leader_pid = 0;
        g->cap_owner_pid = 0;
        g->fwd_owner_tid = 0;
        g->fwd_wait_mask = 0;
        g->next_thread_index = 0;
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
    if (!pd || pd->tid == 0) return 0;
    if (pd->group_index >= (uint32_t)PROC_MAX) return 0;
    struct LinuxThreadGroup* g = &linux_group_table[pd->group_index];
    if (!g->used) return 0;
    /* A descriptor's group_index is meaningful only for a live thread whose
     * group id is this group's: the control plane and boot parent are zeroed
     * descriptors, and a zeroed group_index must not alias group table slot 0
     * (which the first tenant happens to occupy). */
    if (pd->tgid != g->group_id) return 0;
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
        /* A stale, inactive slot is not a live member: its pid must not
         * redirect a later process's cap-table lookup. */
        if (!proc_table[i].active || proc_table[i].pid != pid) continue;
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
        g->next_thread_index = 1;   /* leader is index 0; the first clone is 1 */
        pd->tid = pd->pid;
        pd->tgid = pd->pid;
        pd->thread_index = 0;
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

int linux_group_join_restored_thread(struct LinuxThreadGroup* g,
                                     struct ProcessDescriptor* child,
                                     uint16_t thread_index) {
    if (!g || !child || !g->used || g->exiting) return -1;
    if (child->pid == 0) return -1;
    if (child->group_index != LINUX_GROUP_NONE) return -1;
    if (thread_index == 0 || thread_index >= LINUX_THREADS_MAX) return -1;
    if (g->live_threads >= LINUX_THREADS_MAX) return -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) continue;
        if (proc_table[i].group_index != (uint32_t)(g - linux_group_table))
            continue;
        if (proc_table[i].thread_index == thread_index) return -1;
    }
    if (linux_group_add_thread(g) != 0) return -1;
    child->cr3 = g->cr3;
    child->tid = child->pid;
    child->tgid = g->group_id;
    child->thread_index = thread_index;
    child->group_index = (uint32_t)(g - linux_group_table);
    child->is_group_leader = 0;
    child->sysret_result = 0;
    if ((uint32_t)thread_index + 1u > g->next_thread_index)
        g->next_thread_index = (uint32_t)thread_index + 1u;
    return 0;
}

uint32_t linux_group_futex_barrier_complete(struct LinuxThreadGroup* g,
                                            uint64_t uaddr) {
    if (!g || !g->used || g->exiting || uaddr == 0) return 0;
    uint32_t live = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active || linux_group_of(pd) != g) continue;
        live++;
        if (pd->state != PROC_BLOCKED) return 0;
        if (pd->park_syscall != SYS_LINUX_FUTEX_RESUME) return 0;
        struct LinuxFutexWaitInfo info;
        if (!linux_futex_waiter_info(pd->tid, &info)) return 0;
        if (info.group_id != g->group_id || info.uaddr != uaddr) return 0;
    }
    return (live == (uint32_t)g->live_threads) ? live : 0;
}

int linux_group_leave_thread(struct LinuxThreadGroup* g) {
    if (!g || !g->used || g->exiting) return 0;
    if (g->live_threads == 0) return 0;   /* already empty: no transition */
    g->live_threads--;
    return g->live_threads == 0;
}

int linux_group_join_thread(struct LinuxThreadGroup* g,
                            struct ProcessDescriptor* child) {
    if (!g || !child || !g->used || g->exiting) return -1;
    if (child->pid == 0) return -1;
    if (child->group_index != LINUX_GROUP_NONE) return -1;
    int gi = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (g == &linux_group_table[i]) { gi = i; break; }
    }
    if (gi < 0) return -1;
    if (linux_group_add_thread(g) != 0) return -1;
    /* A stable index belongs to a LIVE thread, not to a monotonic counter: a
     * Go group can join and leave threads many times over one group lifetime.
     * Reuse the lowest index no live member currently holds. The next-index
     * field is only a starting hint. */
    uint16_t ti = 1;
    for (; ti < LINUX_THREADS_MAX; ti++) {
        int used = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            if (proc_table[i].active &&
                proc_table[i].group_index == (uint32_t)gi &&
                proc_table[i].thread_index == ti) { used = 1; break; }
        }
        if (!used) break;
    }
    if (ti >= LINUX_THREADS_MAX) {
        (void)linux_group_leave_thread(g);
        return -1;
    }
    child->cr3 = g->cr3;   /* membership means the group's one root */
    child->tid = child->pid;
    child->tgid = g->group_id;
    child->thread_index = ti;
    g->next_thread_index = (uint32_t)ti + 1u;
    child->group_index = (uint32_t)gi;
    child->is_group_leader = 0;
    child->sysret_result = 0;
    return 0;
}

/* ─── E7 TG-1 I4: the forward lease ───────────────────────────────────────
 * The state is the group's three fields (`fwd_busy`, `fwd_owner_tid`,
 * `fwd_wait_mask`); this is the whole transition table. The mask is one bit
 * per descriptor slot in the PROCESS TABLE: a group's members need not start
 * at slot 0 (sidecars occupy the low slots), so an 8-bit/LINUX_THREADS_MAX
 * mask lost every waiter above slot 7 — the Go gate's third thread sat at
 * slot 8 and its sigaltstack got a bare EAGAIN. PROC_MAX bits cover every
 * descriptor; LINUX_THREADS_MAX still bounds group admission. The BSP runs every transition with interrupts off
 * inside a syscall, so acquire/release are atomic against each other. */
int linux_group_fwd_acquire(struct LinuxThreadGroup* g, uint32_t tid) {
    if (!g || !g->used || g->exiting || tid == 0) return -1;
    if (!g->fwd_busy) {
        g->fwd_busy = 1;
        g->fwd_owner_tid = tid;
        return 0;
    }
    return (g->fwd_owner_tid == tid) ? 0 : 1;
}

int linux_group_fwd_wait(struct LinuxThreadGroup* g, uint32_t slot) {
    if (!g || !g->used || slot >= PROC_MAX) return -1;
    g->fwd_wait_mask |= (1u << slot);
    return 0;
}

int linux_group_fwd_cancel_wait(struct LinuxThreadGroup* g, uint32_t slot) {
    if (!g || !g->used || slot >= PROC_MAX) return -1;
    g->fwd_wait_mask &= ~(1u << slot);
    return 0;
}

int linux_group_fwd_release(struct LinuxThreadGroup* g, uint32_t tid) {
    if (!g || !g->used || tid == 0) return -1;
    if (!g->fwd_busy || g->fwd_owner_tid != tid) return -1;
    g->fwd_busy = 0;
    g->barrier_reported = 0;
    g->fwd_owner_tid = 0;
    return 0;
}

uint32_t linux_group_fwd_next_waiter(struct LinuxThreadGroup* g) {
    if (!g || !g->used) return PROC_MAX;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (!(g->fwd_wait_mask & (1u << i))) continue;
        g->fwd_wait_mask &= ~(1u << i);
        return i;
    }
    return PROC_MAX;
}

int linux_group_fwd_grant(struct LinuxThreadGroup* g, uint32_t tid) {
    if (!g || !g->used || g->exiting || tid == 0) return -1;
    if (g->fwd_busy) return -1;
    g->fwd_busy = 1;
    g->fwd_owner_tid = tid;
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
    g->next_thread_index = 0;
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
    pd->thread_index = 0;
    pd->group_index = LINUX_GROUP_NONE;
    pd->is_group_leader = 0;
    pd->sysret_result = 0;
}

uint32_t linux_group_release_restored(uint32_t group_id) {
    struct LinuxThreadGroup* g;
    uint32_t released = 0;

    if (group_id == 0) return 0;
    g = linux_group_find(group_id);
    if (!g) return 0;

    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active || !pd->restore_held) continue;
        if (linux_group_of(pd) != g) continue;
        pd->restore_held = 0;
        /* HELD is a create that has not run; a futex-blocked member is
         * waiting on the barrier and must stay BLOCKED until its wake. */
        if (pd->state == PROC_HELD) pd->state = PROC_SUSPENDED;
        released++;
    }
    return released;
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
