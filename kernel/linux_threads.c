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
#include "linux_abi.h"

#ifdef LINUX_THREAD_GROUP_TEST_SEAM
#include <string.h>
static uint64_t g_test_ctid_va;
static uint32_t *g_test_ctid_host;

void linux_thread_test_clear_tid_target(uint64_t va, uint32_t *host) {
    g_test_ctid_va = va;
    g_test_ctid_host = host;
}

/* The seam's one mapped user word. Every other VA is a refused copy, which
 * is the same silent outcome a live unmapped ctid produces. */
int linux_user_copy_in(const uint8_t *src, uint64_t root, uint64_t va,
                       uint32_t n) {
    (void)root;
    if (!g_test_ctid_host || va != g_test_ctid_va || n != sizeof(uint32_t))
        return -1;
    memcpy(g_test_ctid_host, src, n);
    return 0;
}
#endif

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

/* M4.5: Linux's clear_child_tid exit rule, mechanism half. The word is
 * zeroed through the same present+user+writable walk as every other kernel
 * write, before the descriptor leaves the group and while the shared PML4 is
 * still live. A child that unmapped the word (or a clone whose ctid was
 * never mappable) gets no fabrication: the copy refusal is silent because
 * the exit itself must still complete. The wake half belongs to the sidecar,
 * which owns the futex queue and learns the address from the same clone. */
static void clear_child_tid_word(struct ProcessDescriptor *pd) {
    uint32_t zero = 0;
    if (!pd || pd->linux_clear_child_tid == 0 || pd->cr3 == 0) return;
    (void)linux_user_copy_in((const uint8_t *)&zero, pd->cr3,
                             pd->linux_clear_child_tid, sizeof(zero));
    pd->linux_clear_child_tid = 0;
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

/* M4.2's context construction. Kept in this file, beside the group object,
 * because it is the other half of "a thread is a descriptor plus a shared
 * address space": the descriptor's ring3_ctx is what the scheduler will
 * iret into, and this is the only place that writes a brand-new one from a
 * parent's parked syscall frame. No allocation and no scheduling decision
 * happens here, so the host test can drive it directly. */
int linux_thread_clone_context(struct ProcessDescriptor *child,
                               const struct ProcessDescriptor *parent,
                               uint64_t child_stack, uint64_t flags,
                               uint64_t tls, uint64_t ctid) {
    uint64_t rip, fs_base;
    if (!child || !parent) return -1;
    if (child_stack == 0 || child_stack > LINUX_USER_ADDR_MAX) return -1;
    rip = parent->park_ctx.rcx;
    if (rip == 0 || rip > LINUX_USER_ADDR_MAX) return -1;
    if (flags & LINUX_CLONE_SETTLS) {
        if (tls > LINUX_USER_ADDR_MAX) return -1;
        fs_base = tls;
    } else {
        fs_base = parent->fs_base;
        if (fs_base > LINUX_USER_ADDR_MAX) return -1;
    }

    child->cr3         = parent->cr3;
    child->fs_base     = fs_base;
    child->user_rip    = rip;
    child->user_rsp    = child_stack;
    child->parent_pid  = parent->pid;
    child->owner_uid   = parent->owner_uid;
    child->partition_id = parent->partition_id;
    child->personality = PROC_PERSONALITY_LINUX;
    /* The memory ledger is address-space state, not per-task state: a new
     * thread must start from the parent's current heap span, break and mmap
     * cursor or its first mmap is checked against a zero span (the std::
     * thread Rust runtime's alternate-signal-stack mmap, observed as E7's
     * "neither the grant nor the refusal" reply). */
    child->linux_heap_base  = parent->linux_heap_base;
    child->linux_heap_limit = parent->linux_heap_limit;
    child->linux_brk_cur    = parent->linux_brk_cur;
    child->linux_mmap_top   = parent->linux_mmap_top;
    child->linux_image_lo   = parent->linux_image_lo;
    child->linux_image_hi   = parent->linux_image_hi;
    child->linux_clear_child_tid =
        (flags & LINUX_CLONE_CHILD_CLEARTID) ? ctid : 0;
    child->linux_is_thread = 0;   /* attach() stamps the group membership */

    /* The child's first instruction is the one after the parent's clone. The
     * whole register set comes from the parent's SYSCALL-ENTRY capture, not
     * its ring3_ctx: musl's __clone passes the start routine in r9 and the
     * argument word on the child stack, and both must survive the clone.
     * (The earlier callee-saved-only copy left r9 as whatever the parent's
     * last resumed frame held — on a std::thread that was zero, so the child
     * reached `call *%r9` with RIP 0.) Linux clears rax for the child, leaves
     * the return RIP in rcx and RFLAGS in r11, and keeps every other GPR. */
    const struct CapParkCtx *p = &parent->park_ctx;
    child->ring3_ctx = parent->ring3_ctx;
    child->ring3_ctx.r15 = p->r15;
    child->ring3_ctx.r14 = p->r14;
    child->ring3_ctx.r13 = p->r13;
    child->ring3_ctx.r12 = p->r12;
    child->ring3_ctx.rbx = p->rbx;
    child->ring3_ctx.rbp = p->rbp;
    child->ring3_ctx.rdi = p->linux_args[0];
    child->ring3_ctx.rsi = p->linux_args[1];
    child->ring3_ctx.rdx = p->linux_args[2];
    child->ring3_ctx.r10 = p->linux_args[3];
    child->ring3_ctx.r8  = p->linux_args[4];
    child->ring3_ctx.r9  = p->linux_args[5];
    child->ring3_ctx.rcx = rip;       /* Linux leaves the return RIP in rcx */
    child->ring3_ctx.r11 = 0x202;     /* and the user RFLAGS in r11 */
    child->ring3_ctx.rax = 0;         /* clone's child return value */
    child->ring3_ctx.rip = rip;
    child->ring3_ctx.cs  = 0x23;
    child->ring3_ctx.rflags = 0x202;
    child->ring3_ctx.rsp = child_stack;
    child->ring3_ctx.ss  = 0x1B;
    child->has_ring3_ctx = 1;
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

    clear_child_tid_word(pd);

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

uint32_t linux_thread_group_id_by_tgid(uint32_t tgid) {
    if (tgid == 0) return 0;
    for (uint32_t i = 0; i < LINUX_THREAD_GROUP_MAX; i++) {
        if (g_groups[i].used && g_groups[i].tgid == tgid)
            return i + 1;
    }
    return 0;
}

uint64_t linux_thread_group_cr3(uint32_t group_id) {
    struct LinuxThreadGroup *g = group_get(group_id);
    return g ? g->cr3 : 0;
}

int linux_thread_group_used(uint32_t group_id) {
    return group_get(group_id) != 0;
}

#ifdef LINUX_THREAD_GROUP_TEST_SEAM
int linux_thread_group_test_force_counters(uint32_t group_id,
                                           uint16_t cr3_refs,
                                           uint16_t live_threads) {
    struct LinuxThreadGroup *g = group_get(group_id);
    if (!g) return 0;
    g->cr3_refs = cr3_refs;
    g->live_threads = live_threads;
    return 1;
}
#endif
