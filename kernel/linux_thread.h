#ifndef LINUX_THREAD_H
#define LINUX_THREAD_H

/*
 * linux_thread.h — E7 TG-1 increment I1: the Linux thread group.
 *
 * A Linux task's address space, capability table and memory cursors belong to
 * its GROUP, not to the descriptor that happened to create them. I1 makes that
 * ownership structural before any second thread exists (clone is I2):
 *
 *   - one `LinuxThreadGroup` per POSIX environment, created when the create
 *     stamps a LINUX child and destroyed exactly once at teardown;
 *   - the descriptor carries only the thread's identity (tid/tgid) and an
 *     index into the group table, so no thread keeps a writable private copy
 *     of a shared cursor;
 *   - `cap_table_index()` resolves a group member to the group's cap owner,
 *     which is what makes one channel pair and one capability table visible to
 *     every thread of the group (I2's clone relies on it);
 *   - the page table root belongs to the group: `linux_group_destroy()` is the
 *     ONE site that calls `cap_table_teardown()` and
 *     `user_destroy_page_table()` for a group, and the per-descriptor teardown
 *     paths route through it rather than tearing a member's shared resources
 *     down under its siblings.
 *
 * The shared span fields are the ones the create stamps from the manifest's
 * BUDGET/IMAGE records. They move here from `struct ProcessDescriptor`
 * (E7 increments 6/16/20); callers reach them through the accessors below so a
 * future clone inherits them by joining the group rather than by copying
 * anything.
 *
 * Host test: tests/linux_thread_group_host_test.c (the group table is a plain
 * fixed array; cap/page teardown are extern calls the test stubs and counts).
 */

#include <stdint.h>

struct ProcessDescriptor;

/* Go's measured startup cloned six threads for a two-thread fixture; a smaller
 * cap would fail the real runtime. The process table also holds two sidecars
 * and the control plane, so PROC_MAX (16) has room for a full group. */
#define LINUX_THREADS_MAX 8

/* `group_index` value for a descriptor that belongs to no Linux group. */
#define LINUX_GROUP_NONE  0xFFFFFFFFu

struct LinuxThreadGroup {
    uint8_t  used;             /* this slot holds a live group */
    uint8_t  exiting;          /* destroy is in progress (one shot) */
    uint8_t  live_threads;     /* leader + clones; the last-thread rule (I2) */
    uint8_t  fwd_busy;         /* one in-flight forward for the whole group */
    uint32_t group_id;         /* == leader_pid; getpid()'s value */
    uint32_t leader_pid;       /* the descriptor whose identity stays valid */
    uint32_t cap_owner_pid;    /* descriptor whose cap table the group uses */
    uint32_t fwd_owner_tid;    /* the thread holding the forward lease */
    uint8_t  fwd_wait_mask;    /* one bit per thread slot waiting for the lease */
    uint8_t  _pad[3];
    uint64_t cr3;              /* the one user page-table root (owner) */
    uint64_t heap_base;        /* the BUDGET span, stamped by the create */
    uint64_t heap_limit;
    uint64_t brk_cur;          /* brk growth, shared by every thread */
    uint64_t mmap_top;         /* anonymous-map cursor, shared */
    uint64_t image_lo;         /* a linked ET_EXEC's PT_LOAD extent */
    uint64_t image_hi;
    uint64_t dir_pos;          /* getdents64 cursor: one listing at a time */
};

/* Definition is sized PROC_MAX in linux_thread.c; the declaration stays
 * incomplete so the header does not have to duplicate that constant. */
extern struct LinuxThreadGroup linux_group_table[];

/* Zero the group table and mark every descriptor as group-less. Called from
 * process_init(); a host test calls it directly against its own proc_table. */
void linux_groups_init(void);

/* The group a descriptor belongs to, or NULL for SLS tasks, the control plane,
 * a reused slot, and a descriptor whose group has already been destroyed. */
struct LinuxThreadGroup* linux_group_of(const struct ProcessDescriptor* pd);

/* Lookup by the group's own id (the leader's pid). */
struct LinuxThreadGroup* linux_group_find(uint32_t group_id);

/* The group's cap-table owner for a pid: the leader's pid for a member, the
 * pid itself for everyone else. This is the hook cap_table_index() calls at
 * its top, so every thread of a group resolves the same capability table and
 * the same one sidecar channel pair. */
uint32_t proc_group_cap_owner(uint32_t pid);

/* Create the group of one that a freshly created LINUX descriptor becomes:
 * group id/leader/cap owner = pd->pid, cr3 = pd->cr3, live_threads = 1, and
 * pd->tid = pd->tgid = pd->pid, is_group_leader = 1. Returns 0, or -1 when
 * the group table has no free slot (the caller refuses the create rather than
 * release a task with no group). */
int linux_group_create(struct ProcessDescriptor* pd);

/* Stamp the create's shared span and cursors onto the group. brk starts at the
 * base, mmap_top at the limit, and the getdents cursor at zero — the same
 * values E7 increments 6/16 stamped on the descriptor before I1. No-op when
 * the descriptor has no group. */
void linux_group_stamp_memory(struct ProcessDescriptor* pd,
                              uint64_t heap_base, uint64_t heap_limit,
                              uint64_t image_lo, uint64_t image_hi);

/* Admission for I2's clone: 0 when the group has room, -1 at the cap. */
int linux_group_add_thread(struct LinuxThreadGroup* g);

/* The group's ONE resource teardown: cap table twice-refused, page table
 * freed once, group slot released. The caller finishes the descriptor's own
 * syscall-stack/identity cleanup (process_exit, process_kill and the deferred
 * schedule path do). A second call is a no-op, so a duplicated exit path
 * cannot double-free the address space. */
void linux_group_destroy(struct LinuxThreadGroup* g);

/* Clear the descriptor's group identity after the group is destroyed or when
 * a slot is (re)initialised. Never touches group resources. */
void linux_group_clear_identity(struct ProcessDescriptor* pd);

/* ─── Shared-memory accessors ──────────────────────────────────────────────
 * All return 0 / no-op for a descriptor with no group, which keeps every
 * non-Linux caller branch-free. */
uint64_t linux_group_heap_base(const struct ProcessDescriptor* pd);
uint64_t linux_group_heap_limit(const struct ProcessDescriptor* pd);
uint64_t linux_group_brk(const struct ProcessDescriptor* pd);
void     linux_group_set_brk(struct ProcessDescriptor* pd, uint64_t brk);
uint64_t linux_group_mmap_top(const struct ProcessDescriptor* pd);
void     linux_group_set_mmap_top(struct ProcessDescriptor* pd, uint64_t top);
uint64_t linux_group_image_lo(const struct ProcessDescriptor* pd);
uint64_t linux_group_image_hi(const struct ProcessDescriptor* pd);
uint64_t linux_group_dir_pos(const struct ProcessDescriptor* pd);
void     linux_group_set_dir_pos(struct ProcessDescriptor* pd, uint64_t pos);
void     linux_group_add_dir_pos(struct ProcessDescriptor* pd, uint64_t add);

#endif /* LINUX_THREAD_H */
