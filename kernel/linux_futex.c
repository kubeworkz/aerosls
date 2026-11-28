/*
 * linux_futex.c — E7 TG-1 increment I3: the private futex wait table.
 *
 * The contract is in linux_futex.h. Two properties are load-bearing here:
 *
 *   1. THE KEY IS (group_id, uaddr). A wake must reach the sibling that waits
 *      on the same word of the same process, and must never reach another
 *      process that happens to map a page at the same virtual address. The
 *      group id — not the tid — is the process half of the key; that is what
 *      the design's "per-thread key" tooth reddens.
 *
 *   2. CHECK, ENQUEUE AND BLOCK ARE ONE WINDOW. This module only does the
 *      enqueue; the caller (process.c) does it inside the same trap, with
 *      interrupts off, and only then switches away. So a wake either sees the
 *      record (and the waiter is already committed to parking) or runs before
 *      the enqueue (and the waiter's own value re-check makes it return
 *      EAGAIN). There is no lost-wakeup window for this module to close, and
 *      nothing here takes a lock.
 *
 * The table is a plain fixed array like proc_table and linux_group_table. No
 * allocation, no list links: a waiter record is one slot, and expiry/wake walk
 * the same sixteen slots the scheduler already walks.
 */

#include "linux_futex.h"
#include "linux_abi.h"   /* LINUX_EINVAL / LINUX_ENOSYS / LINUX_ENOMEM */

struct LinuxFutexWaiter linux_futex_table[LINUX_FUTEX_WAITERS];

void linux_futex_init(void) {
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        linux_futex_table[i].used = 0;
        linux_futex_table[i].group_id = 0;
        linux_futex_table[i].tid = 0;
        linux_futex_table[i].uaddr = 0;
        linux_futex_table[i].deadline = 0;
    }
}

int linux_futex_op_supported(uint32_t op) {
    switch (op) {
    case LINUX_FUTEX_OP_WAIT:
    case LINUX_FUTEX_OP_WAIT | LINUX_FUTEX_PRIVATE_FLAG:
    case LINUX_FUTEX_OP_WAKE:
    case LINUX_FUTEX_OP_WAKE | LINUX_FUTEX_PRIVATE_FLAG:
        return 0;
    default:
        /* Every other word: WAIT_BITSET/WAKE_BITSET (9/10), REQUEUE,
         * CMP_REQUEUE, WAKE_OP, LOCK_PI/UNLOCK_PI/TRYLOCK_PI, and any op with
         * CLOCK_REALTIME or an unknown flag bit. Refused by name rather than
         * degraded to WAIT/WAKE, because each one promises state this kernel
         * does not hold (a bitset, a second address, an owner). */
        return -LINUX_ENOSYS;
    }
}

int linux_futex_deadline(uint64_t sec, uint64_t nsec, uint64_t now,
                         uint64_t tick_ns, uint64_t* out) {
    uint64_t ns;
    uint64_t ticks;

    if (!out || tick_ns == 0) return -LINUX_EINVAL;
    if (nsec >= 1000000000ull) return -LINUX_EINVAL;   /* Linux's own EINVAL */
    if (sec > LINUX_FUTEX_MAX_SECONDS) sec = LINUX_FUTEX_MAX_SECONDS;

    /* sec is clamped first, so this cannot overflow: 86400 * 1e9 + 1e9 fits
     * comfortably in 64 bits. */
    ns = sec * 1000000000ull + nsec;
    /* Round UP: a wait must never end before the caller's deadline. */
    ticks = (ns + tick_ns - 1) / tick_ns;
    *out = now + ticks;
    return 0;
}

int linux_futex_wait_enqueue(uint32_t group_id, uint64_t uaddr, uint32_t tid,
                             uint64_t deadline) {
    int slot = -1;

    if (group_id == 0 || tid == 0) return -LINUX_EINVAL;   /* deadline 0 = forever */

    /* A tid that is already waiting updates ITS record: one descriptor has
     * one parked syscall, so two records for one tid would be state no resume
     * could resolve. */
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        if (linux_futex_table[i].used && linux_futex_table[i].tid == tid) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
            if (!linux_futex_table[i].used) {
                slot = (int)i;
                break;
            }
        }
    }
    if (slot < 0) return -LINUX_ENOMEM;

    linux_futex_table[slot].used = 1;
    linux_futex_table[slot].group_id = group_id;
    linux_futex_table[slot].tid = tid;
    linux_futex_table[slot].uaddr = uaddr;
    linux_futex_table[slot].deadline = deadline;
    return 0;
}

int linux_futex_wait_cancel(uint32_t tid) {
    if (tid == 0) return 0;
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].tid != tid) continue;
        linux_futex_table[i].used = 0;
        linux_futex_table[i].group_id = 0;
        linux_futex_table[i].tid = 0;
        linux_futex_table[i].uaddr = 0;
        linux_futex_table[i].deadline = 0;
        return 1;
    }
    return 0;
}

int linux_futex_wake_collect(uint32_t group_id, uint64_t uaddr, uint32_t count,
                             uint32_t* out_tids, uint32_t max) {
    uint32_t taken = 0;

    if (!out_tids || max == 0 || count == 0 || group_id == 0) return 0;

    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS && taken < count && taken < max; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].group_id != group_id) continue;
        if (linux_futex_table[i].uaddr != uaddr) continue;
        out_tids[taken++] = linux_futex_table[i].tid;
        linux_futex_table[i].used = 0;
        linux_futex_table[i].group_id = 0;
        linux_futex_table[i].tid = 0;
        linux_futex_table[i].uaddr = 0;
        linux_futex_table[i].deadline = 0;
    }
    return (int)taken;
}

int linux_futex_expire(uint64_t now, uint32_t* out_tids, uint32_t max) {
    uint32_t taken = 0;

    if (!out_tids || max == 0) return 0;

    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS && taken < max; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].deadline == 0) continue;   /* forever */
        if (now < linux_futex_table[i].deadline) {
            kernel_serial_printf(
                "[E7] futex expire: now=%lu deadline=%lu tid=%u (not yet)\n",
                (unsigned long)now,
                (unsigned long)linux_futex_table[i].deadline,
                linux_futex_table[i].tid);
            continue;
        }
        kernel_serial_printf(
            "[E7] futex expire: now=%lu deadline=%lu tid=%u EXPIRED\n",
            (unsigned long)now,
            (unsigned long)linux_futex_table[i].deadline,
            linux_futex_table[i].tid);
        out_tids[taken++] = linux_futex_table[i].tid;
        linux_futex_table[i].used = 0;
        linux_futex_table[i].group_id = 0;
        linux_futex_table[i].tid = 0;
        linux_futex_table[i].uaddr = 0;
        linux_futex_table[i].deadline = 0;
    }
    return (int)taken;
}

int linux_futex_forget_group(uint32_t group_id) {
    int removed = 0;

    if (group_id == 0) return 0;
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].group_id != group_id) continue;
        linux_futex_table[i].used = 0;
        linux_futex_table[i].group_id = 0;
        linux_futex_table[i].tid = 0;
        linux_futex_table[i].uaddr = 0;
        linux_futex_table[i].deadline = 0;
        removed++;
    }
    return removed;
}

int linux_futex_pending(uint32_t group_id, uint64_t uaddr) {
    int n = 0;

    if (group_id == 0) return 0;
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].group_id != group_id) continue;
        if (linux_futex_table[i].uaddr != uaddr) continue;
        n++;
    }
    return n;
}

int linux_futex_waiter_info(uint32_t tid, struct LinuxFutexWaitInfo* out) {
    if (!out || tid == 0) return 0;
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
        if (!linux_futex_table[i].used) continue;
        if (linux_futex_table[i].tid != tid) continue;
        out->group_id = linux_futex_table[i].group_id;
        out->tid      = linux_futex_table[i].tid;
        out->uaddr    = linux_futex_table[i].uaddr;
        out->deadline = linux_futex_table[i].deadline;
        return 1;
    }
    return 0;
}

uint32_t linux_futex_free_slots(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++)
        if (!linux_futex_table[i].used) n++;
    return n;
}
