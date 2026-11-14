#ifndef LINUX_THREADS_H
#define LINUX_THREADS_H

/*
 * linux_threads.h — M4.1/M4.2: the kernel's Linux thread-group object and
 * the pure context construction the clone path uses.
 *
 * M4.1 establishes the object every thread-create path must use; M4.2 adds
 * the held-descriptor create in process.c and the context helper below. The
 * group object itself still owns no policy: it counts references, and the
 * sidecar decides when a clone is allowed.
 *
 *   - one group per Linux address space, with the leader's PID as TGID;
 *   - a reference count on the shared PML4, so the last thread (not the
 *     first exiting one) destroys the address space;
 *   - the per-thread identity fields on ProcessDescriptor: group id, TID,
 *     "is a non-leader thread", and the child-clear-tid address.
 *
 * The group table is static and bounded by the process table. A group is
 * reusable only after both counters reach zero; in M4.1 they start and move
 * together. `detach` refuses to act on a half-torn group (one counter zero
 * or the two disagreeing): it clears the descriptor and reports the group
 * handled, but returns no PML4, so a live member's address space is never
 * freed through the legacy path. That is the invariant the M4.2 clone path
 * will rely on, and the one tests/linux_thread_group_host_test.c drives.
 *
 * The API is intentionally small and policy-free: `create` starts a group,
 * `attach` is the M4.2 seam that joins a second execution context to it,
 * and `detach` returns what the caller must do with the address space. It
 * does not call user_destroy_page_table() itself because the capability
 * table must be torn down while the page table is still alive; the caller
 * performs both steps in the existing teardown order.
 */

#include <stdint.h>

#define LINUX_THREAD_GROUP_MAX 16u

struct ProcessDescriptor;

struct LinuxThreadGroup {
    uint64_t cr3;          /* the one PML4 every thread in the group uses */
    uint32_t tgid;         /* leader PID; stable until the group is freed  */
    uint16_t cr3_refs;     /* one reference per live thread                */
    uint16_t live_threads; /* group members not yet detached               */
    uint8_t  used;
    uint8_t  reserved[7];
};

/* Clear the whole table (boot init, and the host test's reset). */
void linux_thread_groups_reset(void);

/* Start the group for a freshly-created Linux leader. `pd->pid` becomes the
 * TGID and its TID; `cr3` becomes the group's shared address space. Returns
 * the 1-based group id on success (the value stored in pd->linux_tgid), or
 * 0 when the table is full or the arguments are invalid. */
uint32_t linux_thread_group_create(struct ProcessDescriptor *pd, uint64_t cr3);

/* M4.2: build the child's initial user context from the parent's post-syscall
 * park context. The child returns to the instruction after the parent's
 * syscall (parent->park_ctx.rcx), with rax = 0 — exactly Linux's clone return
 * convention — on `child_stack`, with the parent's callee-saved registers and
 * RFLAGS. `flags` is the already-validated clone word: CLONE_SETTLS selects
 * `tls` for the child's FS base, otherwise the parent's FS base is inherited;
 * CLONE_CHILD_CLEARTID records `ctid` on the child. The child is not attached
 * to a group here. Returns 0, or -1 for a non-canonical stack/TLS/RIP or a
 * missing parent park context. Pure: no allocations, no scheduler calls. */
int linux_thread_clone_context(struct ProcessDescriptor *child,
                               const struct ProcessDescriptor *parent,
                               uint64_t child_stack, uint64_t flags,
                               uint64_t tls, uint64_t ctid);

/* M4.2 seam: join `pd` (already carrying the shared `cr3`) to an existing
 * group. Sets the per-thread fields, increments both counters, and refuses
 * a group/cr3 mismatch. Returns the group id, or 0 on refusal. */
uint32_t linux_thread_group_attach(struct ProcessDescriptor *pd,
                                   uint32_t group_id, uint32_t tid);

/* Remove `pd` from its group. Returns 1 when the descriptor belonged to a
 * group (handled), 0 when it did not (legacy/non-Linux teardown).
 * `*cr3_to_destroy` is set to the shared PML4 only when this was the last
 * reference; otherwise it is 0. The caller must run cap_table_teardown()
 * before destroying the returned page table. The descriptor's group fields
 * are cleared either way, so a double detach is a no-op. A half-torn group
 * is left intact and returns no PML4 (leak in preference to use-after-free). */
int linux_thread_group_detach(struct ProcessDescriptor *pd,
                              uint64_t *cr3_to_destroy);

/* Queries for the teardown path, the future clone path, and host tests. */
uint32_t linux_thread_group_refs(uint32_t group_id);
uint32_t linux_thread_group_live(uint32_t group_id);
uint32_t linux_thread_group_tgid(uint32_t group_id);
uint64_t linux_thread_group_cr3(uint32_t group_id);
int      linux_thread_group_used(uint32_t group_id);

/* M4.5: resolve a leader PID to its group id (0 when no live group has that
 * TGID). The exit_group sweep accepts the Linux-visible tgid, not the
 * kernel's private group slot. */
uint32_t linux_thread_group_id_by_tgid(uint32_t tgid);

/* Host-test seam: when LINUX_THREAD_GROUP_TEST_SEAM is defined the module
 * supplies linux_user_copy_in() over one explicitly mapped word, so the
 * clear-child-tid exit rule can be driven without a boot or page tables. */
#ifdef LINUX_THREAD_GROUP_TEST_SEAM
void linux_thread_test_clear_tid_target(uint64_t va, uint32_t *host);
#endif

#ifdef LINUX_THREAD_GROUP_TEST_SEAM
/* Host-test only: force the two counters apart so detach's half-torn refusal
 * is reachable without corrupting the production API. Not compiled into the
 * kernel image. */
int linux_thread_group_test_force_counters(uint32_t group_id,
                                           uint16_t cr3_refs,
                                           uint16_t live_threads);
#endif

#endif /* LINUX_THREADS_H */
