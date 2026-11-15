/*
 * linux_clone.c — M4.2: the kernel's held thread-create primitive.
 *
 * The sidecar owns Linux clone(56)'s policy. This file is the mechanism it
 * asks the kernel for: one new execution context in the parent's existing
 * LinuxThreadGroup, never a new address space. The sequence is deliberately
 * ordered so a failure cannot leave a half-visible thread:
 *
 *   1. validate the flag word (linux_clone_flags_verdict) and every user
 *      address the request will touch, BEFORE allocating anything;
 *   2. create a PROC_HELD descriptor with a shared CR3 and a context copied
 *      from the parent's parked syscall frame;
 *   3. attach it to the parent's group (the reference that owns the PML4);
 *   4. mint the child's messenger pair from this sidecar to the child and
 *      register the child's name for the sidecar's exit path;
 *   5. write ptid/ctid, then release the child.
 *
 * Every post-create failure tears the child back down through process_kill,
 * which detaches the group reference. The parent's PML4 is never destroyed
 * while the parent itself is alive.
 */

#include "cap.h"
#include "process.h"
#include "linux_abi.h"
#include "linux_threads.h"
#include "kernel_io.h"

static uint64_t clone_fail(struct SLSLinuxCloneThreadRequest *req,
                           int32_t status) {
    if (req) req->status = status;
    kernel_serial_printf("[PROC] M4.2 clone refused status=%d parent=%u\n",
                         (int)status, req ? req->parent_pid : 0u);
    return (uint64_t)(int64_t)status;
}

static struct ProcessDescriptor *clone_find_pid(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid)
            return &proc_table[i];
    }
    return 0;
}

uint64_t sys_sls_linux_clone_thread(struct SLSLinuxCloneThreadRequest *req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;

    req->status = 0;
    req->child_tid = 0;
    req->child_r = CAP_NONE;
    req->child_w = CAP_NONE;
    req->sidecar_r = CAP_NONE;
    req->sidecar_w = CAP_NONE;
    req->tgid = 0;

    struct ProcessDescriptor *caller = process_find_current();
    if (!caller || !caller->active || !caller->sidecar_authority)
        return clone_fail(req, CAP_EPERM);

    /* The named parent must be the other end of the caller's own messenger
     * channel. Checking this here keeps a stale or guessed PID from cloning
     * a task the sidecar does not own. */
    uint32_t parent_pid = 0;
    if (!cap_linux_clone_parent(caller->pid, req->sidecar_ch_w, &parent_pid) ||
        parent_pid != req->parent_pid)
        return clone_fail(req, CAP_EPERM);

    struct ProcessDescriptor *parent = clone_find_pid(parent_pid);
    if (!parent || parent->personality != PROC_PERSONALITY_LINUX ||
        parent->linux_tgid == 0)
        return clone_fail(req, CAP_EINVAL);
    if (parent->state != PROC_RUNNING && parent->state != PROC_SUSPENDED &&
        parent->state != PROC_BLOCKED)
        return clone_fail(req, CAP_ERR_STATE);

    int verdict = linux_clone_flags_verdict(req->flags);
    if (verdict != 0)
        return clone_fail(req, cap_code_from_linux_errno(verdict));

    if (!(req->flags & LINUX_CLONE_VM) || !(req->flags & LINUX_CLONE_THREAD))
        return clone_fail(req, CAP_EINVAL);

    /* The child's first instruction is the parent's post-syscall RIP. The
     * context helper repeats the canonical checks; doing them here as well
     * means a bad request is refused before a descriptor is touched. */
    if (parent->park_ctx.rcx == 0 || parent->park_ctx.rcx > LINUX_USER_ADDR_MAX)
        return clone_fail(req, CAP_EFAULT);

    /* The stack pointer is one-past-the-top for normal downward growth, so
     * validate the byte the first push will use, not the pointer itself. */
    if (req->child_stack == 0)
        return clone_fail(req, CAP_EFAULT);
    if (linux_user_range_writable(parent->cr3, req->child_stack - 1, 1) != 0)
        return clone_fail(req, CAP_EFAULT);

    if (req->flags & LINUX_CLONE_SETTLS) {
        if (req->tls > LINUX_USER_ADDR_MAX)
            return clone_fail(req, CAP_EFAULT);
    } else if (parent->fs_base > LINUX_USER_ADDR_MAX) {
        return clone_fail(req, CAP_EFAULT);
    }

    if ((req->flags & LINUX_CLONE_PARENT_SETTID) && req->ptid != 0) {
        if (linux_user_range_writable(parent->cr3, req->ptid,
                                      (uint32_t)sizeof(uint32_t)) != 0)
            return clone_fail(req, CAP_EFAULT);
    }
    if ((req->flags & (LINUX_CLONE_CHILD_SETTID |
                       LINUX_CLONE_CHILD_CLEARTID)) && req->ctid != 0) {
        if (linux_user_range_writable(parent->cr3, req->ctid,
                                      (uint32_t)sizeof(uint32_t)) != 0)
            return clone_fail(req, CAP_EFAULT);
    }

    uint32_t child_pid = proc_create_linux_thread(parent, req->child_stack,
                                                  req->flags, req->tls,
                                                  req->ctid);
    if (child_pid == 0)
        return clone_fail(req, CAP_ENOMEM);
    struct ProcessDescriptor *child = clone_find_pid(child_pid);
    if (!child) {
        proc_discard_linux_thread(child_pid);
        return clone_fail(req, CAP_ERR_STATE);
    }

    /* Join the group before any channel or release: from here the child owns
     * one group reference, and process_kill's teardown will detach it rather
     * than destroy the parent's PML4. */
    if (linux_thread_group_attach(child, parent->linux_tgid, child_pid) == 0) {
        proc_discard_linux_thread(child_pid);
        return clone_fail(req, CAP_ERR_STATE);
    }
    /* attach() clears the per-thread fields; re-stamp the clear-tid address
     * the context helper recorded from CLONE_CHILD_CLEARTID. */
    if (req->flags & LINUX_CLONE_CHILD_CLEARTID)
        child->linux_clear_child_tid = req->ctid;

    uint16_t sidecar_r = CAP_NONE, sidecar_w = CAP_NONE;
    uint16_t child_r = CAP_NONE, child_w = CAP_NONE;
    int chan_rc = cap_chan_create(caller->pid, child_pid,
                                  &sidecar_r, &sidecar_w,
                                  &child_r, &child_w);
    if (chan_rc < 0) {
        process_kill(child_pid);
        return clone_fail(req, chan_rc);
    }

    if (sidecar_registry_register(child->name, child_pid,
                                  child->partition_id) < 0) {
        process_kill(child_pid);
        return clone_fail(req, CAP_ETABLEFULL);
    }

    if ((req->flags & LINUX_CLONE_PARENT_SETTID) && req->ptid != 0) {
        uint32_t tid = child_pid;
        if (linux_user_copy_in((const uint8_t *)&tid, parent->cr3,
                               req->ptid, sizeof(tid)) != 0) {
            sidecar_registry_remove_pid(child_pid);
            process_kill(child_pid);
            return clone_fail(req, CAP_EFAULT);
        }
    }
    if ((req->flags & LINUX_CLONE_CHILD_SETTID) && req->ctid != 0) {
        uint32_t tid = child_pid;
        if (linux_user_copy_in((const uint8_t *)&tid, parent->cr3,
                               req->ctid, sizeof(tid)) != 0) {
            sidecar_registry_remove_pid(child_pid);
            process_kill(child_pid);
            return clone_fail(req, CAP_EFAULT);
        }
    }

    if (process_release(child_pid) != 0) {
        sidecar_registry_remove_pid(child_pid);
        process_kill(child_pid);
        return clone_fail(req, CAP_ERR_STATE);
    }

    req->status = 0;
    req->child_tid = child_pid;
    req->child_r = child_r;
    req->child_w = child_w;
    req->sidecar_r = sidecar_r;
    req->sidecar_w = sidecar_w;
    req->tgid = parent->linux_tgid;
    kernel_serial_printf(
        "[PROC] M4.2 clone: parent %u -> child %u (tgid=%u) released.\n",
        parent->pid, child_pid, req->tgid);
    return 0;
}

/* M4.5: exit_group(231)'s group sweep. The sidecar owns the Linux policy
 * and calls this only after its own request is parked. The group id is
 * resolved from the Linux-visible TGID (the leader's PID), not accepted from
 * the sidecar as an internal slot.
 *
 * Death is a precondition of the report, not an assumption. process_kill()
 * DEFERS a RUNNING target (it only marks pending_teardown and leaves the
 * descriptor active until the next schedule); returning a killed count after
 * that would let the sidecar reap a task that can still execute. So:
 *
 *   1. preflight the whole snapshot. If any member is PROC_RUNNING, return
 *      CAP_EAGAIN before the first kill: the sidecar must yield and retry.
 *   2. snapshot the member PIDs (process_kill mutates proc_table and the
 *      last teardown may recycle a slot).
 *   3. revalidate each PID after every teardown. A pid no longer in THIS
 *      group is skipped, never killed as a stale entry.
 *   4. only report success after both the group's live-thread count and a
 *      fresh descriptor scan are zero. Anything still live (including a
 *      kill that deferred) returns CAP_EAGAIN and no success count.
 *
 * The caller is expected to retry on CAP_EAGAIN after sched_yield(). The
 * sweep is idempotent: a retry re-snapshots what remains. */
static int group_sweep_preflight(struct ProcessDescriptor *caller,
                                 uint32_t tgid, uint32_t group_id,
                                 uint32_t *pids, uint32_t *out_n) {
    uint32_t n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor *pd = &proc_table[i];
        if (!pd->active || pd->linux_tgid != group_id) continue;
        /* The group is confined to the caller's partition: a sidecar may
         * sweep its own Linux tasks, never another partition's. Validate
         * the whole snapshot before the first kill, so a refusal cannot
         * leave a half-swept group. */
        if (pd->partition_id != caller->partition_id)
            return CAP_EPERM;
        /* The sweep runs in the sidecar's syscall context. A member that is
         * PROC_RUNNING is therefore not this CPU's current task; it is a
         * target process_kill would defer. Refuse before killing anything
         * and let the sidecar yield/retry until it is no longer running. */
        if (pd->state == PROC_RUNNING) {
            kernel_serial_printf(
                "[PROC] M4.5 group sweep tgid=%u group=%u busy pid=%u (running)\n",
                tgid, group_id, pd->pid);
            return CAP_EAGAIN;
        }
        pids[n++] = pd->pid;
    }
    *out_n = n;
    return 0;
}

static uint32_t group_sweep_active_members(uint32_t group_id) {
    uint32_t live = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].linux_tgid == group_id)
            live++;
    }
    return live;
}

uint64_t sys_sls_linux_group_sweep(uint32_t tgid) {
    struct ProcessDescriptor *caller = process_find_current();
    if (!caller || !caller->active || !caller->sidecar_authority)
        return (uint64_t)(int64_t)CAP_EPERM;

    uint32_t group_id = linux_thread_group_id_by_tgid(tgid);
    if (group_id == 0)
        return (uint64_t)(int64_t)CAP_ERR_NOTFOUND;

    uint32_t pids[PROC_MAX];
    uint32_t n = 0;
    int verdict = group_sweep_preflight(caller, tgid, group_id, pids, &n);
    if (verdict != 0)
        return (uint64_t)(int64_t)verdict;

    uint32_t killed = 0;
    for (uint32_t k = 0; k < n; k++) {
        struct ProcessDescriptor *pd = clone_find_pid(pids[k]);
        if (pd && pd->active && pd->linux_tgid == group_id) {
            process_kill(pids[k]);
            /* process_kill returns after DEFERRING a RUNNING target. Do not
             * count it as killed: the liveness proof below is the report,
             * not this call's return. */
            struct ProcessDescriptor *after = clone_find_pid(pids[k]);
            if (after && after->active && after->linux_tgid == group_id) {
                kernel_serial_printf(
                    "[PROC] M4.5 group sweep tgid=%u group=%u deferred pid=%u\n",
                    tgid, group_id, pids[k]);
                return (uint64_t)(int64_t)CAP_EAGAIN;
            }
            killed++;
        }
    }

    uint32_t live = linux_thread_group_live(group_id);
    uint32_t active = group_sweep_active_members(group_id);
    if (live != 0 || active != 0) {
        kernel_serial_printf(
            "[PROC] M4.5 group sweep tgid=%u group=%u INCOMPLETE killed=%u live=%u active=%u\n",
            tgid, group_id, killed, live, active);
        return (uint64_t)(int64_t)CAP_EAGAIN;
    }

    kernel_serial_printf(
        "[PROC] M4.5 group sweep tgid=%u group=%u killed=%u live=0\n",
        tgid, group_id, killed);
    return killed;
}

/* M4.5: set_tid_address(218)'s kernel half. The sidecar answers the Linux
 * call and then names the task and word here; the descriptor owns the
 * pointer so every teardown path (exit, signal kill, fault, group sweep)
 * clears it through the one checked copy-in in linux_threads.c. */
uint64_t sys_sls_linux_set_clear_tid(struct SLSLinuxClearTidRequest *req) {
    struct ProcessDescriptor *caller = process_find_current();
    if (!caller || !caller->active || !caller->sidecar_authority)
        return (uint64_t)(int64_t)CAP_EPERM;
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;

    struct ProcessDescriptor *pd = clone_find_pid(req->pid);
    if (!pd || !pd->active || pd->personality != PROC_PERSONALITY_LINUX ||
        pd->linux_tgid == 0 || pd->partition_id != caller->partition_id)
        return (uint64_t)(int64_t)CAP_ERR_NOTFOUND;
    if (req->va != 0 &&
        linux_user_range_writable(pd->cr3, req->va,
                                  (uint32_t)sizeof(uint32_t)) != 0)
        return (uint64_t)(int64_t)CAP_EFAULT;
    pd->linux_clear_child_tid = req->va;
    return 0;
}
