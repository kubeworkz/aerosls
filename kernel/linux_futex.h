#ifndef LINUX_FUTEX_H
#define LINUX_FUTEX_H

/*
 * linux_futex.h — E7 TG-1 increment I3: private futex wait/wake.
 *
 * The design is docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md §4.4. A futex is
 * kernel-local because the sidecar owns no page table: the word the caller
 * waits on lives in the group's address space, and only the kernel can read
 * it without copying the page. The kernel also owns the scheduler, so the
 * waiter's block and the waker's runnable flip are its state transitions.
 *
 * ─── The table ────────────────────────────────────────────────────────────
 * One fixed array keyed by (group_id, user_vaddr). A record names the waiting
 * descriptor by its tid, the 32-bit word's virtual address, and an ABSOLUTE
 * tick deadline (0 = forever). Group id, not tid, is the key's group half:
 * two threads of one process share the word, and a private futex in process A
 * must never be visible to process B even at the same virtual address. That
 * property is the design's named tooth ("make futex use a per-thread key").
 *
 * The table is sized PROC_MAX (16): at most one descriptor per process-table
 * slot can be blocked, because a blocked thread is not running to call wait
 * again. linux_futex.c re-states the number rather than including process.h
 * so the module stays host-testable with no descriptor table at all.
 *
 * ─── What is pure, and why ────────────────────────────────────────────────
 * Everything in this header is table/word arithmetic: no page-table walk, no
 * descriptor, no scheduler. That is deliberate. tests/linux_futex_host_test.c
 * drives the lost-wake, group-key, count, deadline and expiry rules without a
 * boot; process.c owns the two privileged halves (reading the word through
 * the caller's cr3, and rebuilding the waiter's ring-3 frame), and
 * syscall_dispatch.c owns the ABI parsing.
 *
 * ─── v1's served set ──────────────────────────────────────────────────────
 * FUTEX_WAIT and FUTEX_WAKE, with the PRIVATE flag and nothing else. The
 * census's Go path uses exactly FUTEX_WAIT_PRIVATE (128) and
 * FUTEX_WAKE_PRIVATE (129), and both take a RELATIVE timeout. PI futexes,
 * requeue, robust-list ownership, process-shared futexes, WAIT_BITSET and
 * CLOCK_REALTIME are refused ENOSYS by name rather than silently treated as
 * their nearest relative.
 */

#include <stdint.h>

/* The syscall number (x86-64). */
#define LINUX_FUTEX           202u

/* futex_op's command word and the two v1 serves. */
#define LINUX_FUTEX_OP_WAIT   0u
#define LINUX_FUTEX_OP_WAKE   1u
#define LINUX_FUTEX_OP_MASK   0x7Fu
#define LINUX_FUTEX_PRIVATE_FLAG 128u
#define LINUX_FUTEX_CLOCK_REALTIME 256u

/* The waiter table. One blocked descriptor per process-table slot. */
#define LINUX_FUTEX_WAITERS   16u

/* A relative timeout beyond a day is clamped here, not refused: Linux accepts
 * any timespec, and a tenant that asks for a week is not making a malformed
 * request — this kernel's tick counter is the thing that would have to hold
 * it, so the wait simply ends at the bound. Named, so the clamp is not a
 * silent 64-bit overflow. */
#define LINUX_FUTEX_MAX_SECONDS 86400ull

/* The futex resume sentinel: park_syscall's value while a descriptor is
 * blocked in futex_wait. Its own value (not SYS_LINUX_TRAP_RESUME) because the
 * resume does NOT re-run the trap: the waiter already read the word, compared
 * it and enqueued, and the waker's answer is delivered straight into rax. */
#define SYS_LINUX_FUTEX_RESUME 0x4C465846u   /* "LFXF" */

struct LinuxFutexWaiter {
    uint8_t  used;
    uint8_t  _pad[3];
    uint32_t group_id;
    uint32_t tid;
    uint32_t _pad2;
    uint64_t uaddr;
    uint64_t deadline;   /* absolute kernel ticks; 0 = wait forever */
};

extern struct LinuxFutexWaiter linux_futex_table[LINUX_FUTEX_WAITERS];

/* Zero every slot. process_init() calls it beside linux_groups_init(); the
 * host test calls it directly. */
void linux_futex_init(void);

/* 0 for the four words v1 serves (WAIT/WAKE, with or without PRIVATE), else
 * -LINUX_ENOSYS. Total over every uint32_t: an op with any other bit set —
 * including CLOCK_REALTIME, a bitset op, requeue or a PI command — is
 * refused by name, never mapped onto a nearby op. */
int linux_futex_op_supported(uint32_t op);

/* A relative timespec (sec, nsec) to an ABSOLUTE tick deadline. Pure, with
 * tick_ns passed in so the kernel's KERNEL_TICK_NS and the host test's value
 * are the same function with different inputs.
 *
 * Returns 0 with *out = now + ceil(seconds*1e9 + nsec / tick_ns), or
 * -LINUX_EINVAL for nsec >= 1e9, a NULL out, or tick_ns == 0. sec beyond
 * LINUX_FUTEX_MAX_SECONDS clamps (see above). A zero timespec yields
 * *out == now: the caller turns that into ETIMEDOUT before parking, exactly
 * as Linux does. */
int linux_futex_deadline(uint64_t sec, uint64_t nsec, uint64_t now,
                         uint64_t tick_ns, uint64_t* out);

/* Enqueue one waiter. A duplicate tid REPLACES its record (a descriptor can
 * have only one parked syscall; the update keeps the table a function of the
 * live state). Returns 0, -LINUX_EINVAL for a zero group or tid, or
 * -LINUX_ENOMEM when every slot is taken. A deadline of 0 means forever. */
int linux_futex_wait_enqueue(uint32_t group_id, uint64_t uaddr, uint32_t tid,
                             uint64_t deadline);

/* Remove one tid's record. Returns 1 when it was waiting, 0 otherwise. */
int linux_futex_wait_cancel(uint32_t tid);

/* Take up to `count` waiters of (group_id, uaddr), oldest slot first, into
 * out_tids (capacity max). Returns how many records were removed and written
 * — the waiter-side waker decides how many of those tids still resolve to a
 * live descriptor. */
int linux_futex_wake_collect(uint32_t group_id, uint64_t uaddr, uint32_t count,
                             uint32_t* out_tids, uint32_t max);

/* Take every waiter whose deadline has passed at `now` into out_tids.
 * Forever waiters (deadline 0) are never expired. Returns the count removed
 * and written. */
int linux_futex_expire(uint64_t now, uint32_t* out_tids, uint32_t max);

/* Drop every waiter of a group (the group's teardown owns this). Returns the
 * number removed. */
int linux_futex_forget_group(uint32_t group_id);

/* How many records wait on this exact key. The host test's group/address
 * isolation check and the teardown net read it. */
int linux_futex_pending(uint32_t group_id, uint64_t uaddr);

/* TG-2c: the wait a tid is parked in, if any. Copies the key and the
 * ABSOLUTE tick deadline (0 = forever) so the checkpoint can carry the
 * remaining timeout instead of a boot-local deadline. Returns 1 when found,
 * 0 otherwise. */
struct LinuxFutexWaitInfo {
    uint32_t group_id;
    uint32_t tid;
    uint64_t uaddr;
    uint64_t deadline;
};
int linux_futex_waiter_info(uint32_t tid, struct LinuxFutexWaitInfo* out);

/* Free waiter slots. The restore checks this before pouring a barrier: a
 * barrier that cannot fit its re-enqueued waits must refuse BEFORE any page
 * moves, not strand the first half in the table. */
uint32_t linux_futex_free_slots(void);

#endif /* LINUX_FUTEX_H */
