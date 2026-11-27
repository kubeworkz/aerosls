#include "syscall_dispatch.h"
#include "object_catalog.h"
#include "transaction.h"
#include "microkernel.h"
#include "ipc.h"
#include "tier_mgr.h"
#include "query_engine.h"
#include "process.h"
#include "linux_thread.h"
#include "linux_futex.h"
#include "loader.h"
#include "agent.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"
#include "../kernel/secure_api.h"
#include "partition.h"
#include "service_registry.h"   // Orchestration Plan Phase 4
#include "workload.h"            // Orchestration Plan Phase 5
#include "frame_pool.h"
#include "storage_quota.h"   // Storage Isolation Roadmap Phase 1 -- SYS_SLS_PARTITION_STORAGE_QUOTA_SET/LIST
#include "env_payload.h"     // E7 inc. 22: the gate teeth's codes (E7G_TOOTH compiles the arms out)
#include "env_storage.h"     // POSIX-Environments Roadmap v0.2 Phase P1b -- durable environment stores
#include "../net/tcp_quota.h" // Network Fairness Phase 2 -- SYS_SLS_PARTITION_CONN_QUOTA_SET/LIST
#include "sql_exec.h"
#include "vecstore.h"   // Vector Store Roadmap Phase 4 -- pulls in ../net/ollama_client.h transitively
#include "rowstore.h"   // Gap Remediation Phase B -- SYS_SLS_ROWSTORE_CREATE_TABLE
#include "vec_join.h"   // Gap Remediation Phase C -- SYS_SLS_VEC_JOIN
#include "vec_index.h"  // Gap Remediation Phase C -- SYS_SLS_VEC_INDEX_CREATE/SEARCH
#include "group_profile.h" // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_GROUP_*
#include "authlist.h"      // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_AUTHLIST_*
#include "security_audit.h" // Navigator-Parity Gap Roadmap Phase 3 -- SYS_SLS_AUDIT_LIST
#include "msgqueue.h"      // Navigator-Parity Gap Roadmap Phase 4 -- SYS_SLS_MQ_*
#include "../net/net.h"    // Navigator-Parity Gap Roadmap Phase 5c -- SYS_SLS_NET_STATUS
#include "database.h"      // Database Namespace & Access Roadmap Phase 4 -- SYS_SLS_DATABASE_*
#include "tenant.h"        // Multitenant Isolation Gap Analysis §5 item 1 -- SYS_SLS_TENANT_*
#include "usage_metering.h" // Multitenant Isolation Gap Analysis §5 item 6 -- SYS_SLS_USAGE_REPORT
#include "../net/consensus.h" // Multi-Node Partition Scaling Roadmap Phase 7 addendum -- SYS_SLS_CLUSTER_INIT/STATUS
#include "cap.h"            // Seed Kernel Phase 1 -- SYS_SLS_CAP_* (289-297)
#include "kernel_io.h"      // kernel_serial_print/printf -- the control-plane refusal lines
#include "linux_abi.h"      // E7 increment 2 -- the Linux-personality branch (§3.3)
#include "env_ckpt.h"       // E7 TG-2a -- the clone's group-thread record stamp
#include "timer.h"         // E7 increment 24 -- SYS_SLS_NOW_NS
#include "../arch/x86/user_paging.h"  // E7 §5.3 (the plan's inc 4) -- user_write_fs_base,
                                      // the MSR write of a granted ARCH_SET_FS

// ─── spawn owner-uid resolution ──────────────────────────────────────────────
// The owner_uid threaded into program_spawn*()/program_load()/sys_sls_load()
// decides the child's catalog-authority check AND which partition it lands
// in (partition_get_for_uid()). These used kernel_get_current_thread_id(),
// which returns the microkernel task-table id — 0 for every ring-3 caller —
// so every ring-3-spawned child was silently owned by the kernel (uid 0:
// always-passing authority, PARTITION_SYSTEM placement), the same gap the
// HTTP path's req_uid threading already closed for /api/program/spawn. The
// real caller identity is the currently-executing process's owner_uid;
// kernel context (no ring-3 process) keeps the historical uid 0.
static uint32_t spawn_owner_uid(void) {
    struct ProcessDescriptor* cur = process_find_current();
    return cur ? cur->owner_uid : 0;
}

/* ─── Partition control-plane authority (POSIX-Environments v0.2 §11 Q4) ───
 * ONE rule for every arm here that acts on a partition as a whole rather than
 * on the caller's own work: the budget setters — frame quota (213), CPU
 * weight (273), storage quota (275) and connection quota (277) — the
 * lifecycle operations — create (210), assign (211), destroy (214), pause
 * (215) and resume (216) — and cold migration (218). Assign and create are
 * here because they decide which partition a caller's NEXT process lands in
 * (see their own comments); the rest act on a partition's existence, schedule
 * or resources. A caller in any partition but PARTITION_SYSTEM is refused BY
 * NAME before the action runs, so a tenant can neither raise its own ceiling,
 * lower a neighbour's, mint or move a partition, nor freeze, resume or
 * destroy one. Kernel context (no current process) is the control plane's own
 * path — the HTTP routes reach these operations directly — and those routes
 * carry DB_ADMIN instead, the gate their sibling partition routes already
 * had.
 *
 * The rule arrived one finding at a time: 277 shipped with no caller check at
 * all, which made §11 Q4's new default decorative; 273/275 had the same
 * absence; 213 and 214-216 had it again; 210 and 218 were the last two, and
 * closing them is why this comment can now say "every arm" rather than
 * listing what is still open. Three names for one rule is how copies drift
 * apart — an eleventh partition-wide operation has one obvious place to join.
 *
 * PARTITION_SYSTEM and PARTITION_DEFAULT are both 0, so this reads "partition
 * 0 may", not "uid 0 may": a process whose uid was never assigned resolves to
 * partition 0 (partition_get_for_uid()) and passes. That is the kernel's own
 * resolution rather than a hole in it — a tenant's processes are placed by
 * the env manager's assignment, so an unassigned uid is the system
 * partition's default path, not a tenant that lost its row.
 * Returns 1 when the caller may proceed. */
static int partition_control_may_act(const char* tag, const char* what) {
    struct ProcessDescriptor* caller = process_find_current();
    if (!caller) return 1;
    if (caller->partition_id != PARTITION_SYSTEM) {
        kernel_serial_printf(
            "[%s] refused: pid=%u (partition %u) may not %s — only "
            "PARTITION_SYSTEM may\n",
            tag, (unsigned)caller->pid, (unsigned)caller->partition_id, what);
        return 0;
    }
    return 1;
}

// ─── sys_sls_allocate — legacy direct-address allocation (syscall 105) ────────
// Returns the base virtual address of the named object, or 0 if not found.
static uint64_t sls_legacy_allocate(void* arg) {
    if (!arg) return 0;
    struct {
        uint64_t object_id;
        uint64_t size_requested;
        uint32_t access_flags;
    }* req = arg;

    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active &&
            object_catalog[i].object_id == req->object_id) {
            return object_catalog[i].base_vaddr;
        }
    }
    return 0;
}

// ─── do_syscall ───────────────────────────────────────────────────────────────
uint64_t do_syscall(uint64_t num, void* arg) {
    /* ── E7 (increment 3): the Linux-personality branch (D1, §3.3) ────────
     * The FIRST thing the dispatcher does, before the driver-SDK switch and
     * before the legacy chain, because the personality decides what `num`
     * MEANS (§2.2): for a Linux task, rax=105 is setuid and must never be
     * read as SYS_SLS_ALLOCATE. A non-Linux caller takes exactly the path it
     * always took — one predictable branch, no behavior change.
     *
     * Increment 3 completes §3.3's steps 5-7 with TWO arms:
     *
     *  · the RESUME arm (num == SYS_LINUX_TRAP_RESUME): the park's re-run,
     *    and the ONLY place a sidecar reply is ever read — recv it, apply
     *    it through the §5.7 errno map (size and echo enforced by the pure
     *    linux_syscall_apply_reply), and the resulting rax rides the
     *    existing cap_recv_resume → syscall_return_path back to ring-3 as
     *    if the syscall had simply taken a while. A sentinel trap with NO
     *    forward in flight (park_syscall not armed) is a stray, refused BY
     *    NAME — the increment-2 discipline, applied to the resume number.
     *
     *  · the FIRST trap: capture → describe → the §3.5 channel check →
     *    arm → post → park. Every refusal on this side is -LINUX_ENOSYS,
     *    a LINUX errno, never a CAP code — this task's binary was compiled
     *    against Linux's numbering, and a CAP-family negative would be a
     *    value no errno can produce in its error path (§5.7). The branch
     *    still INTERPRETS nothing: it describes argument positions and
     *    either hands the description on or refuses, so rax=105 remains
     *    setuid from trap to sidecar (§3.6 — structural, not a filter). */
    {
        struct ProcessDescriptor* cur = process_find_current();
        if (cur && linux_personality_intercepts(cur->personality)) {
            uint16_t ch_w, ch_r;

            if (num == SYS_LINUX_TRAP_RESUME) {
                /* Stray vs. in-flight: the arm is set before the post and
                 * cleared when the reply lands, so an unarmed sentinel can
                 * only be a raw ring-3 trap wearing the resume number —
                 * there is no reply owed and nothing to park for. */
                if (!linux_forward_resume_is_live(cur->personality,
                                                  cur->park_syscall)) {
                    kernel_serial_printf(
                        "[E7] resume sentinel 0x%x with no forward in "
                        "flight — stray trap refused by name\n",
                        (unsigned)SYS_LINUX_TRAP_RESUME);
                    return (uint64_t)(int64_t)(-LINUX_ENOSYS);
                }
                if (cap_linux_find_channels(cur->pid, &ch_w, &ch_r) != 0) {
                    /* Woken with nowhere to deliver: the sidecar (or its
                     * end of the channel) died while we parked. Close the
                     * in-flight arm and answer with the map's own truth —
                     * "the holder went away under us" is -EIO, never a
                     * raw CAP code. */
                    cur->park_syscall = 0;
                    linux_forward_lease_release(cur);
                    return (uint64_t)(int64_t)linux_errno_from_cap(
                        CAP_ERR_REVOKED);
                }
                {
                    uint64_t rax = k_linux_deliver(cur->pid, ch_r,
                                                   (uint64_t)(uintptr_t)arg);
                    cur->park_syscall = 0;  /* in-flight closes HERE — the
                                            * next stray refuses again */
                    /* I4: the reply is applied, so the group's one forward
                     * slot is free again. The release may grant the lease to
                     * a parked sibling and restart its syscall; that frame is
                     * built but not entered (no switch here). */
                    linux_forward_lease_release(cur);
                    /* E7 increment 21: the gate workload's stop, at the
                     * sysret that closes a read(fd 3). `rax` is the value
                     * the sysret will deliver and park_ctx is the read's own
                     * entry frame, so this is the instant B9 names — after
                     * the reply is applied, before the next trap. The count
                     * is the bytes the REPLIES declared, not a chunk shape:
                     * the sidecar's 1 KiB granularity and the libc's 64 KiB
                     * request reach the same offset. */
                    if (cur->gate_read_pending) {
                        cur->gate_read_pending = 0;
                        if ((int64_t)rax > 0)
                            cur->gate_bytes += (uint32_t)rax;
                        if (cur->gate_bytes >= PROC_GATE_SRC_OFF)
                            (void)proc_gate_stop_at_sysret(rax);
                    }
                    /* E7 §5.3 apply half: the granted ARCH_SET_FS lands
                     * HERE — at the reply, inside this task's own trap,
                     * before the return below reaches the sysret into the
                     * new base. The GRANT is the sidecar's (its 158 arm);
                     * the address is re-checked against
                     * linux_fs_addr_is_user_canonical anyway, because the
                     * wrmsr is the dangerous half — a denial (rax != 0)
                     * leaves the old base untouched and returns the mapped
                     * errno exactly as any other failed syscall would. */
                    if (cur->fs_stage_valid) {
                        cur->fs_stage_valid = 0;
                        if (rax == 0) {
                            if (!linux_fs_addr_is_user_canonical(
                                    cur->fs_base_stage)) {
                                rax = (uint64_t)(int64_t)(-LINUX_EINVAL);
                            } else {
                                cur->fs_base = cur->fs_base_stage;
                                user_write_fs_base(cur->fs_base);
                            }
                        }
                        cur->fs_base_stage = 0;
                    }
                    /* E7 increment 6 apply half (§5.4): a staged brk is
                     * resolved HERE, at the reply, inside this task's own
                     * trap. The GRANT is the sidecar's (its 12 arm's
                     * ledger decides whether the break moves); the value
                     * is re-checked against the span THIS create stamped
                     * and the request the FIRST trap staged, because the
                     * page-table write is the dangerous half — the §5.3
                     * rule one level up, where the wrmsr was replaced by
                     * a map. A denial echo needs no frames (the break is
                     * already where the reply says it is). A grant maps
                     * fresh partition-charged frames, ZERO-FILLED before
                     * the task can read them: the pool recycles frames
                     * across tasks and partitions, so an unzeroed frame
                     * would carry its last tenant's bytes into this
                     * task's heap — anonymous memory must not, and the
                     * probe asserts the zero (MARKER 396's absence).
                     *
                     * x86-64 never caches a NOT-PRESENT translation (SDM
                     * Vol. 3A §4.10.4.1), so a fresh map needs no INVLPG
                     * for the task about to resume on this same cr3 —
                     * the same posture cap_arch_map_page's runtime maps
                     * already rely on.
                     *
                     * Frame exhaustion mid-batch UNWINDS the batch: the
                     * frames already mapped are read back through
                     * linux_user_page_frame (the copy's own boundaries —
                     * it mapped them, so it can read them), unmapped, and
                     * returned to the partition; the break does not move
                     * and the task sees Linux's own brk failure shape —
                     * the unchanged current break — never a half-mapped
                     * grant. */
                    if (cur->mem_stage_valid) {
                        uint64_t snum = cur->mem_stage_num;
                        uint64_t sa0  = cur->mem_stage_a0;
                        uint64_t sa1  = cur->mem_stage_a1;
                        uint64_t sa2  = cur->mem_stage_a2;
                        uint64_t sa3  = cur->mem_stage_a3;
                        cur->mem_stage_valid = 0;
                        cur->mem_stage_num   = 0;
                        cur->mem_stage_a0    = 0;
                        cur->mem_stage_a1    = 0;
                        cur->mem_stage_a2    = 0;
                        cur->mem_stage_a3    = 0;
                        if (snum == LINUX_BRK) {
                            struct LinuxMemBrkVerdict v =
                                linux_mem_brk_verdict(
                                    linux_group_heap_base(cur),
                                    linux_group_heap_limit(cur),
                                    linux_group_mmap_top(cur),
                                    linux_group_brk(cur), sa0, rax);
                            if (v.malformed) {
                                kernel_serial_printf(
                                    "[E7] brk reply 0x%lx for request "
                                    "0x%lx (span 0x%lx..0x%lx, cur "
                                    "0x%lx) is neither the grant nor "
                                    "the refusal - break unmoved\n",
                                    (unsigned long)rax,
                                    (unsigned long)sa0,
                                    (unsigned long)linux_group_heap_base(cur),
                                    (unsigned long)linux_group_heap_limit(cur),
                                    (unsigned long)linux_group_brk(cur));
                                rax = linux_group_brk(cur);
                            } else if (v.map_pages) {
                                uint64_t *pml4 =
                                    (uint64_t *)(uintptr_t)cur->cr3;
                                uint64_t va = v.map_va;
                                uint32_t done = 0;
                                while (done < v.map_pages) {
                                    void *frame = allocate_physical_ram_frame_for_partition(cur->partition_id);
                                    if (!frame) break;   /* quota or pool:
                                                         * the batch winds
                                                         * back below */
                                    {
                                        uint64_t *w = (uint64_t *)frame;
                                        int zi;
                                        for (zi = 0; zi < 512; zi++)
                                            w[zi] = 0;
                                    }
                                    user_map_page(pml4, va,
                                        (uint64_t)(uintptr_t)frame,
                                        USER_PTE_PRESENT | USER_PTE_USER |
                                        USER_PTE_WRITE |
                                        USER_PTE_NOEXEC);
                                    va += 4096;
                                    done++;
                                }
                                if (done == v.map_pages) {
                                    linux_group_set_brk(cur, v.new_cur);
                                } else {
                                    while (done) {
                                        uint64_t f;
                                        va -= 4096;
                                        f = linux_user_page_frame(
                                            cur->cr3, va);
                                        user_unmap_page(pml4, va);
                                        if (f) free_physical_ram_frame_for_partition(
                                            (void *)(uintptr_t)f,
                                            cur->partition_id);
                                        done--;
                                    }
                                    kernel_serial_printf(
                                        "[E7] brk grant 0x%lx: frame "
                                        "exhaustion mid-batch - batch "
                                        "unwound, break unmoved\n",
                                        (unsigned long)v.new_cur);
                                    rax = linux_group_brk(cur);
                                }
                            }
                        } else if (snum == LINUX_MMAP) {
                                /* E7 increment 16 apply half (§5.4): the
                                 * staged anonymous map is resolved HERE,
                                 * at the reply, inside this task's own
                                 * trap — the brk verdict's rule over
                                 * four staged words. The GRANT is the
                                 * sidecar's (its 9 arm's ledger — a
                                 * cursor descending from the span's top,
                                 * or the request's own FIXED address);
                                 * the verdict re-checks it against THIS
                                 * create's stamped span and the kernel's
                                 * OWN two cursors, because the
                                 * page-table write is the dangerous
                                 * half.
                                 *
                                 * MAP_FIXED REPLACES (Linux's own
                                 * semantics): every present page in the
                                 * range has its frame returned to the
                                 * partition before the new protection is
                                 * installed — and this is the ONE path
                                 * in this tree that edits a translation
                                 * ring-3 HAS run on (the allocator's
                                 * guard page lands on the brk heap's
                                 * first page), so each touched page gets
                                 * an `invlpg` (user_invlpg). PROT_NONE
                                 * backs no frames: a reservation whose
                                 * every access faults — PROT_NONE's
                                 * whole observable contract, and the
                                 * measured allocator's guard shape.
                                 *
                                 * Frame exhaustion mid-batch UNWINDS the
                                 * pages THIS grant backed (returned to
                                 * the partition) and answers -ENOMEM —
                                 * while the pages it already REPLACED
                                 * stay replaced: a hole where the old
                                 * mapping was is Linux's own failure
                                 * shape, and this kernel keeps no
                                 * shadow of the mappings it destroyed. */
                                struct LinuxMmapReq mreq;
                                struct LinuxMemMmapVerdict v;
                                mreq.addr  = sa0;
                                mreq.len   = sa1;
                                mreq.prot  = sa2;
                                mreq.flags = sa3;
                                v = linux_mem_mmap_verdict(
                                    linux_group_heap_base(cur),
                                    linux_group_heap_limit(cur),
                                    linux_group_brk(cur),
                                    linux_group_mmap_top(cur), &mreq, rax);
                                if (v.malformed) {
                                    kernel_serial_printf(
                                        "[E7] mmap reply 0x%lx for "
                                        "request addr=0x%lx len=%lu "
                                        "(span 0x%lx..0x%lx, cur 0x%lx, "
                                        "top 0x%lx) is neither the "
                                        "grant nor the refusal - "
                                        "nothing mapped\n",
                                        (unsigned long)rax,
                                        (unsigned long)sa0,
                                        (unsigned long)sa1,
                                        (unsigned long)linux_group_heap_base(cur),
                                        (unsigned long)linux_group_heap_limit(cur),
                                        (unsigned long)linux_group_brk(cur),
                                        (unsigned long)linux_group_mmap_top(cur));
                                    rax = (uint64_t)(int64_t)
                                          (-LINUX_ENOMEM);
                                } else {
                                    uint64_t *pml4 =
                                        (uint64_t *)(uintptr_t)cur->cr3;
                                    uint64_t va = v.map_va;
                                    uint32_t idx = 0, backed = 0;
                                    int failed = 0;
                                    while (idx < v.range_pages) {
                                        void *frame = 0;
                                        if (idx < v.map_pages) {
                                            frame = allocate_physical_ram_frame_for_partition(cur->partition_id);
                                            if (!frame) {
                                                failed = 1; break;
                                            }
                                            {
                                                uint64_t *w =
                                                    (uint64_t *)frame;
                                                int zi;
                                                for (zi = 0; zi < 512;
                                                     zi++)
                                                    w[zi] = 0;
                                            }
                                        }
                                        if (v.replace) {
                                            uint64_t old =
                                                linux_user_page_frame(
                                                    cur->cr3, va);
                                            user_unmap_page(pml4, va);
                                            if (old) free_physical_ram_frame_for_partition(
                                                (void *)(uintptr_t)old,
                                                cur->partition_id);
                                        }
                                        if (frame) {
                                            user_map_page(pml4, va,
                                                (uint64_t)(uintptr_t)frame,
                                                USER_PTE_PRESENT |
                                                USER_PTE_USER |
                                                USER_PTE_NOEXEC |
                                                (v.prot_write
                                                     ? USER_PTE_WRITE : 0));
                                            backed++;
                                        }
                                        if (v.replace)
                                            user_invlpg(va);
                                        va += 4096;
                                        idx++;
                                    }
                                    if (failed) {
                                        while (backed) {
                                            uint64_t f;
                                            va -= 4096;
                                            f = linux_user_page_frame(
                                                cur->cr3, va);
                                            user_unmap_page(pml4, va);
                                            if (f) free_physical_ram_frame_for_partition(
                                                (void *)(uintptr_t)f,
                                                cur->partition_id);
                                            if (v.replace)
                                                user_invlpg(va);
                                            backed--;
                                        }
                                        kernel_serial_printf(
                                            "[E7] mmap grant addr=0x%lx "
                                            "len=%lu: frame exhaustion "
                                            "mid-batch - backed pages "
                                            "unwound, -ENOMEM\n",
                                            (unsigned long)sa0,
                                            (unsigned long)sa1);
                                        rax = (uint64_t)(int64_t)
                                              (-LINUX_ENOMEM);
                                    } else {
                                        linux_group_set_mmap_top(cur,
                                            v.new_mmap_top);
                                    }
                                }
                            } else if (snum == LINUX_MUNMAP) {
                                /* E7 increment 25 apply half (§5.4): the
                                 * staged munmap is resolved HERE, at the
                                 * reply, inside this task's own trap.
                                 * The sidecar's 11 arm decided WHETHER
                                 * (and its verdict re-runs against this
                                 * create's span and break); the kernel's
                                 * page tables are the truth about which
                                 * pages are PRESENT, so the walk frees
                                 * exactly the USER 4 KiB leaves it finds
                                 * and treats an absent page as already
                                 * unmapped (POSIX: a double unmap is
                                 * success). Every freed frame returns to
                                 * the partition, and every touched
                                 * translation — ring-3 may have run on
                                 * the arena — is shot down with invlpg.
                                 *
                                 * The two cursors do NOT move: brk_cur is
                                 * the break's, and mmap_top keeps
                                 * descending, so v1 never re-hands a
                                 * freed virtual range to a later mmap
                                 * (frames return; the range waits for
                                 * teardown). A malformed success reply
                                 * answers -EINVAL with nothing unmapped.
                                 */
                                struct LinuxMunmapReq mreq;
                                struct LinuxMemMunmapVerdict v;
                                mreq.addr = sa0;
                                mreq.len  = sa1;
                                v = linux_mem_munmap_verdict(
                                    linux_group_heap_base(cur),
                                    linux_group_heap_limit(cur),
                                    linux_group_brk(cur), &mreq, rax);
                                if (v.malformed) {
                                    kernel_serial_printf(
                                        "[E7] munmap reply 0x%lx for "
                                        "request addr=0x%lx len=%lu "
                                        "(span 0x%lx..0x%lx, brk 0x%lx) "
                                        "is not success - nothing "
                                        "unmapped\n",
                                        (unsigned long)rax,
                                        (unsigned long)sa0,
                                        (unsigned long)sa1,
                                        (unsigned long)linux_group_heap_base(cur),
                                        (unsigned long)linux_group_heap_limit(cur),
                                        (unsigned long)linux_group_brk(cur));
                                    rax = (uint64_t)(int64_t)
                                          (-LINUX_EINVAL);
                                } else if (v.unmap_pages) {
                                    uint64_t *pml4 =
                                        (uint64_t *)(uintptr_t)cur->cr3;
                                    uint64_t va = v.unmap_va;
                                    uint64_t idx = 0, freed = 0;
                                    while (idx < v.unmap_pages) {
                                        uint64_t f =
                                            linux_user_page_frame(
                                                cur->cr3, va);
                                        if (f) {
                                            user_unmap_page(pml4, va);
                                            user_invlpg(va);
                                            free_physical_ram_frame_for_partition(
                                                (void *)(uintptr_t)f,
                                                cur->partition_id);
                                            freed++;
                                        }
                                        va += 4096;
                                        idx++;
                                    }
                                    kernel_serial_printf(
                                        "[E7] munmap addr=0x%lx len=%lu: "
                                        "%lu page(s) freed, %lu already "
                                        "absent\n",
                                        (unsigned long)sa0,
                                        (unsigned long)sa1,
                                        (unsigned long)freed,
                                        (unsigned long)(v.unmap_pages -
                                                        freed));
                                }
                            } else if (snum == LINUX_MPROTECT) {
                                /* E7 §10.15 apply half: the sidecar's
                                 * grant is re-checked over the staged
                                 * words and the task's own windows, then
                                 * EVERY page is prewalked before the first
                                 * PTE changes. No frame is allocated or
                                 * freed: a present leaf keeps its frame and
                                 * only its protection bits move. A missing
                                 * page — including the no-frame reservation
                                 * created by mmap(PROT_NONE) — answers
                                 * -ENOMEM with nothing changed. */
                                struct LinuxMprotectReq mreq;
                                struct LinuxMemMprotectVerdict v;
                                mreq.addr = sa0;
                                mreq.len  = sa1;
                                mreq.prot = sa2;
                                v = linux_mem_mprotect_verdict(
                                    linux_group_image_lo(cur),
                                    linux_group_image_hi(cur),
                                    cur->linux_stack_base,
                                    cur->linux_stack_base +
                                        (uint64_t)cur->linux_stack_pages * 4096ull,
                                    linux_group_heap_base(cur),
                                    linux_group_heap_limit(cur), &mreq, rax);
                                if (v.malformed) {
                                    kernel_serial_printf(
                                        "[E7] mprotect reply 0x%lx for "
                                        "request addr=0x%lx len=%lu "
                                        "prot=0x%lx is not success - "
                                        "nothing changed\n",
                                        (unsigned long)rax,
                                        (unsigned long)sa0,
                                        (unsigned long)sa1,
                                        (unsigned long)sa2);
                                    rax = (uint64_t)(int64_t)
                                          (-LINUX_EINVAL);
                                } else if (v.pages) {
                                    uint64_t *pml4 =
                                        (uint64_t *)(uintptr_t)cur->cr3;
                                    uint64_t va = v.va;
                                    uint64_t idx;
                                    int absent = 0;

                                    for (idx = 0; idx < v.pages; idx++) {
                                        if (!linux_user_page_frame(
                                                cur->cr3, va)) {
                                            absent = 1;
                                            break;
                                        }
                                        va += 4096;
                                    }
                                    if (absent) {
                                        kernel_serial_printf(
                                            "[E7] mprotect addr=0x%lx "
                                            "len=%lu: page 0x%lx is "
                                            "absent - nothing changed\n",
                                            (unsigned long)sa0,
                                            (unsigned long)sa1,
                                            (unsigned long)va);
                                        rax = (uint64_t)(int64_t)
                                              (-LINUX_ENOMEM);
                                    } else {
                                        uint64_t flags = USER_PTE_PRESENT;
                                        if (mreq.prot == LINUX_PROT_NONE) {
                                            flags |= USER_PTE_SOFT_NONE |
                                                     USER_PTE_NOEXEC;
                                        } else {
                                            flags |= USER_PTE_USER;
                                            if (mreq.prot & LINUX_PROT_WRITE)
                                                flags |= USER_PTE_WRITE;
                                            if (!(mreq.prot & LINUX_PROT_EXEC))
                                                flags |= USER_PTE_NOEXEC;
                                        }
                                        va = v.va;
                                        for (idx = 0; idx < v.pages; idx++) {
                                            user_protect_page(pml4, va,
                                                             flags);
                                            user_invlpg(va);
                                            va += 4096;
                                        }
                                        kernel_serial_printf(
                                            "[E7] mprotect addr=0x%lx "
                                            "len=%lu prot=0x%lx: %lu "
                                            "page(s) %s\n",
                                            (unsigned long)sa0,
                                            (unsigned long)sa1,
                                            (unsigned long)sa2,
                                            (unsigned long)v.pages,
                                            mreq.prot == LINUX_PROT_NONE
                                                ? "soft-protected (P=1 U=0)"
                                                : "re-protected");
                                    }
                                }
                            }
                        }
                    return rax;
                }
            }

            /* First trap: read the capture FIRST (before any call — the §3.4
             * rule on the C side too), describe, resolve the channel, arm,
             * post, park. k_linux_forward does not return once it parks;
             * its non-zero return is a refusal before any park. */
            {
                uint64_t largs[6];
                struct SLSLinuxSyscallRequest req;
                struct LinuxFdWritePlan plan;
                struct LinuxFdReplyPlan rplan;
                /* E7 increment 5 (the fd class): the message that crosses
                 * is the 72-byte description plus, for a write, its bytes
                 * — 8-aligned so k_linux_forward can read the header as a
                 * struct without a copy of its own. */
                uint64_t fwd_words[(sizeof(struct SLSLinuxSyscallRequest)
                                    + LINUX_FWD_ATTACH_CAP + 7u) / 8u];
                uint8_t *fwd = (uint8_t *)fwd_words;
                uint32_t fwd_len = (uint32_t)sizeof(struct SLSLinuxSyscallRequest);
                int status;
                proc_read_linux_args(largs);
                /* E7 TG-1 I2: the task class — identity and thread exit are
                 * kernel-local. getpid is the GROUP's id (the leader's pid),
                 * gettid this descriptor's own pid, and exit(60) releases ONE
                 * thread: its syscall stack and identity go, the group's cr3,
                 * cap table and sidecar row stay while siblings live. The
                 * last thread destroys the group exactly once. */
                if (num == LINUX_GETPID)
                    return (uint64_t)cur->tgid;
                if (num == LINUX_GETTID)
                    return (uint64_t)cur->tid;
                if (num == LINUX_EXIT) {
                    struct LinuxThreadGroup* eg = linux_group_of(cur);
                    if (eg && !cur->is_group_leader)
                        (void)env_ckpt_linux_group_thread_drop(
                            cur->partition_id, eg->group_id,
                            cur->thread_index);
                    kernel_serial_printf(
                        "[E7] exit(%lu) from TID %u group %u: thread-local "
                        "teardown (no sidecar request)\n",
                        (unsigned long)largs[0], cur->tid, cur->tgid);
                    linux_thread_exit(cur, (uint32_t)largs[0]);
                }
                /* E7 TG-1 I5: set_tid_address (218) is still the sidecar's to
                 * answer — that arm is where the tid tkill matches against is
                 * recorded — but the WORD it names is the kernel's, because
                 * the kernel is what writes it at exit. Capturing it here, at
                 * the first trap, keeps one authority per half: the sidecar
                 * owns the tid, the kernel owns the clear. A word that is not
                 * a canonical user address is stored as 0, so the glue never
                 * has an address to try. */
                if (num == LINUX_SET_TID_ADDRESS) {
                    cur->linux_clear_tid =
                        linux_ctid_ok(largs[0]) ? largs[0] : 0;
                }
                /* E7 increment 31 (P1-C's fault arm): rt_sigaction is also the
                 * fault path's only chance to learn a handler. A ring-3 #PF
                 * has no syscall behind it and cannot be parked, so the
                 * kernel keeps its OWN copy of the four fault signals' acts,
                 * read here at the first trap — the same 32 bytes the forward
                 * carries, through the same validated walker, and before any
                 * post. A read that fails records nothing: the fault path then
                 * keeps its attributed kill, which is the honest answer to a
                 * handler this kernel could not see. */
                if (num == LINUX_RT_SIGACTION && largs[1] != 0) {
                    uint8_t actb[LINUX_SIGACT_BYTES];
                    if (linux_user_copy_out(actb, cur->cr3, largs[1],
                                            LINUX_SIGACT_BYTES) == 0)
                        (void)linux_fault_act_record(cur->linux_fault_act,
                                                     (uint32_t)largs[0],
                                                     actb);
                }
                /* E7 TG-1 I5: set_robust_list (273) is kernel-local for the
                 * same reason futex is: the list lives in the group's page
                 * table and its walk ends in this scheduler's wakes. The
                 * length must be the head this shim understands, so a caller
                 * with a different struct is refused by name rather than
                 * walked with the wrong strides. */
                if (num == LINUX_SET_ROBUST_LIST) {
                    if (!linux_robust_list_ok(largs[0], largs[1])) {
                        kernel_serial_printf(
                            "[E7] set_robust_list head=0x%lx len=%lu "
                            "refused: not a canonical 8-aligned user head "
                            "of %u bytes\n",
                            (unsigned long)largs[0], (unsigned long)largs[1],
                            (unsigned)LINUX_ROBUST_LIST_HEAD_BYTES);
                        return (uint64_t)(int64_t)(-LINUX_EINVAL);
                    }
                    cur->linux_robust_list = largs[0];
                    kernel_serial_printf(
                        "[E7] set_robust_list: TID %u head=0x%lx len=%lu "
                        "recorded (walked at this thread's exit)\n",
                        cur->tid, (unsigned long)largs[0],
                        (unsigned long)largs[1]);
                    return 0;
                }
                /* E7 TG-1 I3: futex (202) is kernel-local. The sidecar owns
                 * no page table and is not the scheduler, so the kernel reads
                 * the waited-on word through the caller's own root, compares
                 * it, and parks the caller in the group's wait table; a
                 * sibling's FUTEX_WAKE resumes it with rax=0, the deadline
                 * tick with -ETIMEDOUT. The check and the enqueue are inside
                 * this ONE trap (interrupts off), so a wake cannot slip
                 * between them: it either finds the record or the waiter's
                 * own value check answers EAGAIN. */
                if (num == LINUX_FUTEX) {
                    uint32_t fop  = (uint32_t)largs[1];
                    uint32_t fcmd = fop & LINUX_FUTEX_OP_MASK;
                    struct LinuxThreadGroup* fg = linux_group_of(cur);
                    if (linux_futex_op_supported(fop) != 0) {
                        kernel_serial_printf(
                            "[E7] futex op=0x%lx refused: v1 serves only "
                            "WAIT(0)/WAKE(1), private, relative timeout\n",
                            (unsigned long)largs[1]);
                        return (uint64_t)(int64_t)(-LINUX_ENOSYS);
                    }
                    if (!fg) {
                        kernel_serial_printf(
                            "[E7] futex refused: TID %u is in no thread "
                            "group\n", cur->tid);
                        return (uint64_t)(int64_t)(-LINUX_EINVAL);
                    }
                    if (fcmd == LINUX_FUTEX_OP_WAKE) {
                        /* Linux does not dereference the word on a wake;
                         * the key match alone is the operation. */
                        return (uint64_t)linux_futex_wake_threads(
                            fg->group_id, largs[0], (uint32_t)largs[2]);
                    }
                    /* FUTEX_WAIT: read the word, compare, park. */
                    {
                        uint8_t  wb[4];
                        uint32_t word;
                        uint64_t deadline = 0;
                        int frc;

                        if (!linux_fs_addr_is_user_canonical(largs[0]) ||
                            linux_user_copy_out(wb, cur->cr3, largs[0],
                                                4) != 0) {
                            kernel_serial_printf(
                                "[E7] futex wait at 0x%lx refused: bad user "
                                "word (EFAULT)\n",
                                (unsigned long)largs[0]);
                            return (uint64_t)(int64_t)(-LINUX_EFAULT);
                        }
                        word = (uint32_t)wb[0] |
                               ((uint32_t)wb[1] << 8) |
                               ((uint32_t)wb[2] << 16) |
                               ((uint32_t)wb[3] << 24);
                        if (word != (uint32_t)largs[2])
                            return (uint64_t)(int64_t)(-LINUX_EAGAIN);

                        if (largs[3] != 0) {
                            uint64_t tv[2];
                            if (linux_user_copy_out((uint8_t*)tv, cur->cr3,
                                                    largs[3],
                                                    LINUX_TIMESPEC_BYTES) != 0)
                                return (uint64_t)(int64_t)(-LINUX_EFAULT);
                            if ((int64_t)tv[0] < 0 || (int64_t)tv[1] < 0)
                                return (uint64_t)(int64_t)(-LINUX_EINVAL);
                            /* A zero timespec is Linux's "do not block":
                             * the value matched, so the answer is the
                             * timeout itself, with no park. */
                            if (tv[0] == 0 && tv[1] == 0)
                                return (uint64_t)(int64_t)(-LINUX_ETIMEDOUT);
                            frc = linux_futex_deadline(tv[0], tv[1],
                                                       kernel_tick_counter,
                                                       KERNEL_TICK_NS,
                                                       &deadline);
                            if (frc != 0) return (uint64_t)(int64_t)frc;
                        }
                        return (uint64_t)(int64_t)linux_futex_block(
                            cur, fg->group_id, largs[0], deadline);
                    }
                }
                /* E7 TG-1 I2: clone is the kernel-local thread class. It
                 * never describes, posts or parks: the group already owns
                 * the address space, cap table and sidecar row, so the only
                 * work is a new descriptor in the same group. The child
                 * starts at this trap's return RIP with rax=0. */
                if (num == LINUX_CLONE) {
                    struct LinuxClonePlan cplan;
                    uint64_t parent_rip = 0;
                    uint32_t child_tid = 0;
                    int crc;
                    if (!proc_read_linux_return_rip(&parent_rip)) {
                        kernel_serial_printf(
                            "[E7] clone refused: no syscall entry capture "
                            "(kernel context)\n");
                        return (uint64_t)(int64_t)(-LINUX_EINVAL);
                    }
                    crc = linux_clone_plan(largs[0], largs[1], largs[4],
                                           largs[3], cur->fs_base, &cplan);
                    if (crc != 0) {
                        kernel_serial_printf(
                            "[E7] clone flags=0x%lx stack=0x%lx tls=0x%lx "
                            "refused before any table change: %s\n",
                            (unsigned long)largs[0],
                            (unsigned long)largs[1],
                            (unsigned long)largs[4],
                            crc == -LINUX_EAGAIN
                                ? "group/process table full"
                                : "not the Go thread shape or bad user "
                                  "stack/TLS");
                        return (uint64_t)(int64_t)crc;
                    }
                    crc = linux_clone_thread(cur, &cplan, parent_rip,
                                             &child_tid);
                    if (crc != 0) {
                        kernel_serial_printf(
                            "[E7] clone refused: %s\n",
                            crc == -LINUX_EAGAIN
                                ? "group cap or resources exhausted"
                                : "invalid parent/group identity");
                        return (uint64_t)(int64_t)crc;
                    }
                    /* TG-2a: the child is the one descriptor whose tid is the
                     * clone result. Its group/index were assigned by
                     * linux_group_join_thread(); stamp that identity into the
                     * environment's record so a later capture can name the
                     * thread. A record-less (boot census) group is a no-op. */
                    for (int ci = 0; ci < PROC_MAX; ci++) {
                        if (!proc_table[ci].active ||
                            proc_table[ci].tid != child_tid) continue;
                        (void)env_ckpt_linux_group_thread_stamp(
                            cur->partition_id, cur->tgid,
                            proc_table[ci].thread_index,
                            proc_table[ci].name,
                            proc_table[ci].user_rip);
                        break;
                    }
                    return (uint64_t)child_tid;
                }
#if E7G_TOOTH
                /* E7 increment 22 (tests/e7_gate_check.sh --teeth): the two
                 * forward-path teeth, armed on the descriptor the pour
                 * adopted and selected only by a teeth build's restore body.
                 *
                 * Both aim at the FIRST read after the console's stop — the
                 * continuation — and both break something the shipped path
                 * already handles by name, so the failure is one of the
                 * gate's own refusals rather than a kernel fault:
                 *
                 *   no-fd    the descriptor the read names is replaced by one
                 *            the sidecar's table does not carry, so the REAL
                 *            lookup misses and answers -EBADF. The read never
                 *            reaches the VFS: no offset line, no Done.
                 *   no-heap  the reply's destination page — the child's own
                 *            copy buffer — is unmapped just before the
                 *            forward, so the sidecar's bytes arrive and the
                 *            deliver's checked copy refuses them -EFAULT (the
                 *            kernel does not fault). The read WAS issued at
                 *            the captured offset, which is the signature that
                 *            differs from no-fd. The tenant's own unwind then
                 *            frees that 64 KiB buffer, and the allocator's
                 *            header lives in the page's last bytes, so ITS
                 *            #PF lands inside the withheld page — the address
                 *            the check harness matches against this line. */
                if (cur->tooth && num == LINUX_READ) {
                    if (cur->tooth == ENV_PAYLOAD_TOOTH_NO_FD) {
                        kernel_serial_printf(
                            "[E7-GATE] tooth no-fd: the continuation's read"
                            "(fd %lu) is forwarded as fd %u, which the sidecar's "
                            "table does not carry -- the real lookup answers "
                            "EBADF\n",
                            (unsigned long)largs[0],
                            (unsigned)ENV_PAYLOAD_TOOTH_DEAD_FD);
                        largs[0] = ENV_PAYLOAD_TOOTH_DEAD_FD;
                    } else if (cur->tooth == ENV_PAYLOAD_TOOTH_NO_HEAP) {
                        uint64_t pv = largs[1] & ~0xFFFULL;
                        user_unmap_page(cur->cr3, pv);
                        kernel_serial_printf(
                            "[E7-GATE] tooth no-heap: the reply destination "
                            "page 0x%llx (the child's copy buffer) is unmapped "
                            "before the read is forwarded -- the deliver's "
                            "checked copy answers EFAULT while this kernel "
                            "does not fault; the tenant's own free then takes "
                            "the #PF in this page\n",
                            (unsigned long long)pv);
                    }
                }
#endif
                /* E7 increment 12 (§10.13's step 3): the reply payload's
                 * staged destination clears FIRST — before any plan's
                 * refusal can return below — so every refusal path leaves
                 * the stage empty (the four-place lifecycle: top of the
                 * first trap, consume at the deliver, refusal, slot
                 * reuse). A destination staged by an earlier forward can
                 * therefore never survive into the next trap's window. */
                cur->reply_stage_num   = 0;
                cur->reply_stage_va    = 0;
                cur->reply_stage_len   = 0;
                cur->reply_stage_kind  = 0;
                cur->reply_stage_aux   = 0;
                cur->reply_stage_va2   = 0;
                cur->reply_stage_len2  = 0;
                cur->reply_stage_va3   = 0;
                cur->reply_stage_valid = 0;
                /* E7 increment 34: a reveal probe's stage lives for exactly
                 * one forward, so it is cleared here with the rest — a
                 * stage from a forward that never reached a reply must not
                 * commit its mask at a later 14. */
                cur->linux_sig_mask_stage_valid = 0;
                cur->linux_sig_mask_stage_probe = 0;
                cur->linux_sig_mask_stage_old   = 0;
                cur->linux_sig_mask_stage_new   = 0;
                cur->linux_sig_mask_stage_dst   = 0;
                /* E7 increment 5, plan half: BEFORE the describe (the
                 * clamp must be IN the description) and before the
                 * channel lookup (a refusal the first trap can produce
                 * alone, §3.3 — no channel needed to say -EFAULT). A
                 * write's bytes and a poll's pollfd array are planned
                 * here; every other number's plan is empty and the
                 * forward below stays the plain 72-byte description it
                 * has always been. */
                plan = linux_fd_write_plan(num, largs);
                if (plan.refuse) {
                    kernel_serial_printf(
                        "[E7] linux write %lu bytes at 0x%lx refused "
                        "before post: bad user span (plan)\n",
                        (unsigned long)largs[2],
                        (unsigned long)largs[1]);
                    return (uint64_t)(int64_t)plan.refuse;
                }
                /* E7 increment 24: the clamp rides IN the description for
                 * WRITE only — args[2] is that number's count. poll's
                 * args[2] is the TIMEOUT, so overwriting it with the array
                 * length would answer a 5 s poll on the array's byte count
                 * (24 ms for the std probe's three fds), not on its timeout.
                 * The array's length is derived by both ends from args[1],
                 * so poll needs no clamp word. */
                if (num == LINUX_WRITE && plan.len) largs[2] = plan.len;
                /* M1: sendto's count is the same one-truth clamp — the
                 * kernel's attach length rides in args[2] so header,
                 * payload and the count the task sees cannot disagree. */
                if (num == LINUX_SENDTO && plan.len) largs[2] = plan.len;
                /* M1 fidelity: sendto's destination is a second user span.
                 * The write plan cannot attach two disjoint buffers, so
                 * copy the 16-byte sockaddr_in here, decode its Host-order
                 * ip/port, and forward them packed in args[4] with the
                 * marker in args[5]. A NULL destination is unaffected; a
                 * non-16 addrlen was already refused by the plan. */
                if (num == LINUX_SENDTO && largs[4] != 0) {
                    uint8_t dest_addr[LINUX_SOCKADDR_IN_BYTES];
                    uint32_t dest_ip = 0;
                    uint16_t dest_port = 0;
                    int dest_ret;
                    if (linux_user_copy_out(dest_addr, cur->cr3, largs[4],
                                            LINUX_SOCKADDR_IN_BYTES) != 0) {
                        kernel_serial_printf(
                            "[M1] sendto destination 0x%lx refused before "
                            "post: unmapped or non-user span\n",
                            (unsigned long)largs[4]);
                        return (uint64_t)(int64_t)(-LINUX_EFAULT);
                    }
                    dest_ret = linux_sendto_dest_decode(dest_addr, &dest_ip,
                                                        &dest_port);
                    if (dest_ret) {
                        kernel_serial_printf(
                            "[M1] sendto destination refused before post: "
                            "non-AF_INET family\n");
                        return (uint64_t)(int64_t)dest_ret;
                    }
                    largs[4] = ((uint64_t)dest_ip << 16) | dest_port;
                    largs[5] = LINUX_SENDTO_DEST_MARKER;
                }
                /* E7 increment 10 (the signal seam): the act/set byte
                 * crossing — the write plan's twin for the sig class,
                 * computed after it (one number is one plan) and BEFORE
                 * the describe, so its refusals are first-trap refusals
                 * too: a foreign sigsetsize, an oldact/oldset this kernel
                 * cannot write back (§10.10's copy-IN gap), an act/set
                 * span ring 3 could not read. The folded src/len then
                 * rides the SAME copy block below — one walker, one arm
                 * ordering, one EFAULT posture — and the sidecar derives
                 * the attach length from num and args[1] != 0 the same
                 * way (its payload check is the twin). */
                {
                    struct LinuxSigCopyPlan splan =
                        linux_sig_copy_plan(num, largs);
                    if (splan.refuse) {
                        kernel_serial_printf(
                            "[E7] linux sig syscall %lu refused before "
                            "post: %s\n",
                            (unsigned long)num,
                            splan.refuse == -LINUX_EINVAL
                                ? "sigsetsize != 8 (kernel ABI sigset "
                                  "is 8 bytes)"
                                : "act/set span not user-readable "
                                  "(plan)");
                        return (uint64_t)(int64_t)splan.refuse;
                    }
                    if (!plan.len && splan.len) {
                        plan.src = splan.src;
                        plan.len = splan.len;
                    }
                }
                /* E7 increment 12 (the reply payload, §10.13's step 3):
                 * the OUT-POINTER's destination span — the write plan's
                 * twin over the IN direction, computed after the sig
                 * plan (one number is one plan; a foreign sigsetsize
                 * still answers EINVAL first) and BEFORE the describe,
                 * so a destination ring 3 could not write is a first-trap
                 * -EFAULT with no post. A NULL out-pointer plans nothing
                 * (nothing is owed back). The (dst, len) below is
                 * STAGED — beside fs_base_stage, set immediately before
                 * the arm — and the deliver copies only within it: the
                 * copy-in aims where the FIRST trap said, never where
                 * the reply says. */
                {
                    struct LinuxFdReplyPlan rp =
                        linux_fd_reply_plan(num, largs);
                    if (rp.refuse) {
                        kernel_serial_printf(
                            "[E7] linux reply dst 0x%lx (%u bytes) "
                            "refused before post: bad user span "
                            "(reply plan)\n",
                            (unsigned long)rp.dst,
                            (unsigned)rp.len);
                        return (uint64_t)(int64_t)rp.refuse;
                    }
                    rplan = rp;
                }
                /* E7 increment 33: rt_sigprocmask INSIDE a trap-delivered
                 * handler. The #PF delivery moved this kernel's mask to the
                 * handler's set, but the sidecar entered nothing and was
                 * told nothing (a trap has no round trip to ride), so its
                 * copy still holds the pre-delivery one: forwarding would
                 * report the narrower set on a query and compute BLOCK /
                 * UNBLOCK on the wrong base. For exactly this window the
                 * kernel is the mask's authority — it answers 14 here, from
                 * pd->linux_sig_mask, with the sidecar's own arithmetic, and
                 * posts nothing (nothing is half-answered). It sits AFTER
                 * every first-trap refusal above (sigsetsize, the set's
                 * span, the oldset's span), so those keep their answers.
                 *
                 * The tkill path is untouched: reply_apply entered that
                 * handler AND moved the sidecar's mask before answering, so
                 * it clears linux_sig_fault_active — which is what
                 * linux_sig_mask_fault_owned asks beside sig_active. */
                if (num == LINUX_RT_SIGPROCMASK) {
                    /* E7 increment 35: a frame the handler LEFT instead of
                     * returning from. Increments 32–34's window is founded on
                     * the handler still being IN the frame its delivery built:
                     * the delivery enters with rsp == the frame's address and
                     * a handler only grows DOWN, so a handler still in its
                     * frame makes this call with rsp <= frame_rsp. A 15
                     * restores from that frame and closes the window
                     * (linux_sig_return_apply). A handler that leaves by
                     * longjmp never sends that 15 — siglongjmp restores the
                     * mask through THIS number first (so a 14 does arrive) and
                     * then pops rsp back to the setjmp caller, above the frame
                     * — and without this check the kernel would go on
                     * answering every 14 of that task's life out of a copy the
                     * sidecar never saw: increment 33's split, made permanent
                     * and unclosable.
                     *
                     * The mask both ends still agree on is the one the frame
                     * RECORDS. uc_sigmask is the PRE-delivery set (the builder
                     * copies spec->blocked), the sidecar's copy never moved
                     * during the window, and every 14 outside it stages what it
                     * moved to — so the frame's word IS the sidecar's.
                     * Restoring it is a no-op for the sanctioned exit
                     * (siglongjmp already restored that same set through this
                     * very number) and the only honest answer for an
                     * unsanctioned one: a handler's mask edits are dropped
                     * rather than left where no other end can see them (§10.30
                     * names that). The read is the validated walker, so a frame
                     * the task unmapped leaves the copy alone — the next
                     * consented 14's reply commit realigns it — and every path
                     * still closes the window, so no later 15 can restore a
                     * frame that is no longer the task's. */
                    {
                        uint64_t now = proc_read_user_rsp();
                        if (cur->linux_sig_fault_active &&
                            linux_sig_frame_abandoned(
                                cur->linux_sig_frame_rsp, now)) {
                            uint64_t at = cur->linux_sig_frame_rsp;
                            uint64_t agreed = cur->linux_sig_mask;
                            int read = linux_user_copy_out(
                                           (uint8_t *)&agreed, cur->cr3,
                                           at + LINUX_SIGFRAME_SIGMASK,
                                           LINUX_SIGSET_BYTES) == 0;
                            cur->linux_sig_mask         = agreed;
                            cur->linux_sig_active       = 0;
                            cur->linux_sig_fault_active = 0;
                            cur->linux_sig_frame_rsp    = 0;
                            kernel_serial_printf(
                                "[E7] fault handler abandoned the frame at "
                                "%lx (rsp %lx): mask window closed at %lx%s\n",
                                (unsigned long)at, (unsigned long)now,
                                (unsigned long)agreed,
                                read ? "" : ", frame unreadable");
                        }
                    }
                    /* E7 increment 34: whose mask answers, whether the call
                     * owes a REVEAL, and — the prerequisite both rest on —
                     * the kernel's own copy of the thread's blocked set,
                     * which this forward is about to change. Until now that
                     * copy moved only at DELIVERIES, so a guest that blocked
                     * a signal (or unblocked one) between two signals was
                     * invisible to the fault path: its frame would carry the
                     * wrong mask, and a trap handler could not free a bit the
                     * kernel never knew was blocked. Every 14 therefore
                     * stages what it moved to, and the reply hook commits it
                     * only on a consent — the sidecar still answers every
                     * unblock it answered before, unchanged. */
                    int owned = linux_sig_mask_fault_owned(
                                    cur->linux_sig_active,
                                    cur->linux_sig_fault_active);
                    uint64_t old = cur->linux_sig_mask;
                    uint64_t new_mask = old;
                    int probe = 0;
                    int planned = 0;

                    if (largs[1] != 0) {
                        uint64_t set = 0;
                        if (linux_user_copy_out((uint8_t *)&set, cur->cr3,
                                                largs[1],
                                                LINUX_SIGSET_BYTES) != 0)
                            return (uint64_t)(int64_t)(-LINUX_EFAULT);
                        planned = linux_sig_mask_fault_plan(old, largs[0], set,
                                                           &new_mask, &probe);
                        if (!planned) {
                            /* A `how` Linux does not have. The trap window
                             * answers for itself (increment 33's EINVAL);
                             * outside it the sidecar's own arm answers,
                             * exactly as before — and nothing is staged,
                             * because a refusal moves nothing. */
                            if (owned) {
                                kernel_serial_printf(
                                    "[E7] rt_sigprocmask inside a fault "
                                    "handler refused: how=%lu is not "
                                    "BLOCK/UNBLOCK/SETMASK\n",
                                    (unsigned long)largs[0]);
                                return (uint64_t)(int64_t)(-LINUX_EINVAL);
                            }
                        }
                    }
                    if (owned && !probe) {
                        /* E7 increment 33's answer, unchanged: nothing went
                         * from BLOCKED to UNBLOCKED, so nothing can be
                         * revealed and the arithmetic IS the whole call. The
                         * oldset is written through the validated copy and
                         * the mask committed only after it lands, so a bad
                         * pointer is -EFAULT with nothing moved. */
                        if (largs[2] != 0 &&
                            linux_user_copy_in((const uint8_t *)&old, cur->cr3,
                                               largs[2],
                                               LINUX_SIGSET_BYTES) != 0)
                            return (uint64_t)(int64_t)(-LINUX_EFAULT);
                        cur->linux_sig_mask = new_mask;
                        kernel_serial_printf(
                            "[E7] rt_sigprocmask in a fault handler: oldset "
                            "%lx -> %lx (the kernel is the mask's authority "
                            "here)\n",
                            (unsigned long)old, (unsigned long)new_mask);
                        return 0;
                    }
                    /* E7 increment 34: this call frees a blocked bit, so it
                     * may REVEAL a pending signal — and `pending` and the
                     * dispositions that decide what a reveal means are the
                     * sidecar's, not the kernel's. The kernel still owns the
                     * mask for the window, so the forward is a tagged
                     * CONTROL request rather than the child's call: SETMASK
                     * (a shape the 14 arm already parses), no set and no
                     * oldset pointers, no payload, the mask it computed in
                     * args[4] and the tag in args[5]. The sidecar runs its
                     * own reveal ladder and leaves ITS mask alone; a consent
                     * commits ours at the reply (kernel/chan.c) and a refusal
                     * moves nothing at all — so the two copies still agree
                     * after the return, and a tkill-delivered handler's
                     * unblock is untouched (the whole window is gated on the
                     * trap flag above). */
                    if (!planned) {
                        /* A query moves nothing: no stage, and the forward
                         * below is the child's own call. */
                    } else {
                    cur->linux_sig_mask_stage_valid = 1;
                    cur->linux_sig_mask_stage_probe = owned ? 1 : 0;
                    cur->linux_sig_mask_stage_old   = old;
                    cur->linux_sig_mask_stage_new   = new_mask;
                    cur->linux_sig_mask_stage_dst   = owned ? largs[2] : 0;
                    if (!owned) {
                        /* The sidecar answers this one — a tkill-delivered
                         * handler's unblock, or an ordinary task's call.
                         * Nothing about that changed: the stage only lets the
                         * reply hook keep the KERNEL's copy in step, which
                         * the fault window and the fault delivery's blocked
                         * check both read. */
                        kernel_serial_printf(
                            "[E7] rt_sigprocmask: mask %lx staged for the reply\n",
                            (unsigned long)new_mask);
                    } else {
                    plan.src  = 0;
                    plan.len  = 0;
                    rplan.dst = 0;
                    rplan.len = 0;
                    rplan.aux = 0;
                    rplan.kind = LINUX_STAGE_NONE;
                    largs[0] = 2;   /* the arm's SETMASK shape          */
                    largs[1] = 0;   /* no set, no oldset, no payload    */
                    largs[2] = 0;
                    largs[4] = new_mask;
                    largs[5] = LINUX_SIG_REVEAL_TAG;
                    kernel_serial_printf(
                        "[E7] fault handler unmask: forwarding the reveal, mask %lx\n",
                        (unsigned long)new_mask);
                    }
                    }
                }
                /* E7 increment 6 (the mem class, §5.4), stage half: a
                 * brk's requested break is captured HERE — before the
                 * describe and before the post — because the reply can
                 * only mean "the break moved to X" against the break the
                 * FIRST trap saw, and the first trap's frame is abandoned
                 * by the park (the increment-4 lesson: the stage lives
                 * on the descriptor, not on the stack). Every other
                 * forward clears the stage, so an old request can never
                 * wait for a grant it was not asked for.
                 *
                 * The W|X refusal is a first-trap refusal too, beside
                 * the write plan's own: simultaneous write+execute is
                 * refused by the shim itself (§5.4 — the census's
                 * mprotect row carries LSC_F_REFUSES_WX), and it is a
                 * property of the REQUEST (a prot word, no object, no
                 * policy) — so no channel is needed to say -EPERM and
                 * the refusal lands before any arm. The sidecar carries
                 * the same refusal as its own 10 arm; both ends exist so
                 * neither alone is the load-bearing refusal (guard N3). */
                cur->mem_stage_valid = 0;
                cur->mem_stage_num   = 0;
                cur->mem_stage_a0    = 0;
                cur->mem_stage_a1    = 0;
                cur->mem_stage_a2    = 0;
                cur->mem_stage_a3    = 0;
                if (num == LINUX_BRK) {
                    cur->mem_stage_valid = 1;
                    cur->mem_stage_num   = num;
                    cur->mem_stage_a0    = largs[0];
                } else if (num == LINUX_MMAP) {
                    /* E7 increment 16 (§5.4): the anonymous-map ladder.
                     * Everything a mmap can be REFUSED for, before any
                     * arm — request words only (a prot word, a flags
                     * word, a FIXED address against this create's own
                     * stamped span), so no channel is posted for a
                     * shape that cannot be answered. The sidecar's 9
                     * arm repeats the same ladder as its own twin
                     * (guard clause V): neither end is load-bearing
                     * alone. A pass stages the WHOLE ask — addr, len,
                     * prot, flags — because the verdict at the reply
                     * re-checks the grant against what the FIRST trap
                     * saw: the brk rule over four words instead of
                     * one. */
                    struct LinuxMmapReq mreq;
                    int mref;
                    mreq.addr  = largs[0];
                    mreq.len   = largs[1];
                    mreq.prot  = largs[2];
                    mreq.flags = largs[3];
                    mref = linux_mem_mmap_refusal(&mreq,
                            linux_group_heap_base(cur),
                            linux_group_heap_limit(cur));
                    if (mref) {
                        kernel_serial_printf(
                            "[E7] linux mmap addr=0x%lx len=%lu prot=%lu "
                            "flags=0x%lx refused before post: %s\n",
                            (unsigned long)largs[0],
                            (unsigned long)largs[1],
                            (unsigned long)largs[2],
                            (unsigned long)largs[3],
                            mref == -LINUX_EPERM
                                ? "write+execute is refused (§5.4)"
                                : mref == -LINUX_EINVAL
                                ? "malformed request word"
                                : mref == -LINUX_ENOMEM
                                ? "outside the stamped span"
                                : "shape not served (by name)");
                        return (uint64_t)(int64_t)mref;
                    }
                    cur->mem_stage_valid = 1;
                    cur->mem_stage_num   = num;
                    cur->mem_stage_a0    = largs[0];
                    cur->mem_stage_a1    = largs[1];
                    cur->mem_stage_a2    = largs[2];
                    cur->mem_stage_a3    = largs[3];
                } else if (num == LINUX_MUNMAP) {
                    /* E7 increment 25 (§5.4): munmap's first trap is
                     * the same shape rule as its brk/mmap siblings — a
                     * malformed length, an unaligned address, a range
                     * outside THIS create's stamped span, or one that
                     * starts below the current break is refused before
                     * any post, with the words on the line. A pass
                     * stages the two words the reply's verdict re-checks;
                     * the sidecar's 11 arm repeats the rule as its twin
                     * (guard clause X), so neither end is load-bearing
                     * alone. */
                    struct LinuxMunmapReq mreq;
                    int mref;
                    mreq.addr = largs[0];
                    mreq.len  = largs[1];
                    mref = linux_mem_munmap_refusal(&mreq,
                            linux_group_heap_base(cur),
                            linux_group_heap_limit(cur),
                            linux_group_brk(cur));
                    if (mref) {
                        kernel_serial_printf(
                            "[E7] linux munmap addr=0x%lx len=%lu "
                            "refused before post: %s\n",
                            (unsigned long)largs[0],
                            (unsigned long)largs[1],
                            mref == -LINUX_EINVAL
                                ? "malformed, outside the stamped span, "
                                  "or below the current break"
                                : "refused");
                        return (uint64_t)(int64_t)mref;
                    }
                    cur->mem_stage_valid = 1;
                    cur->mem_stage_num   = num;
                    cur->mem_stage_a0    = largs[0];
                    cur->mem_stage_a1    = largs[1];
                } else if (num == LINUX_MPROTECT) {
                    /* E7 §10.15: the first trap of a real permission
                     * change. The words are validated against this
                     * task's OWN image, stack and heap windows; the
                     * sidecar's 10 arm answers the non-W|X policy, and
                     * the reply apply walks the page table because a
                     * window can contain a hole. A refusal here posts
                     * nothing, exactly like the mmap ladder. */
                    struct LinuxMprotectReq mreq;
                    uint64_t stack_lo = cur->linux_stack_base;
                    uint64_t stack_hi = stack_lo +
                        (uint64_t)cur->linux_stack_pages * 4096ull;
                    int mref;

                    mreq.addr = largs[0];
                    mreq.len  = largs[1];
                    mreq.prot = largs[2];
                    mref = linux_mem_mprotect_refusal(&mreq,
                            linux_group_image_lo(cur),
                            linux_group_image_hi(cur),
                            stack_lo, stack_hi,
                            linux_group_heap_base(cur),
                            linux_group_heap_limit(cur));
                    if (mref) {
                        kernel_serial_printf(
                            "[E7] linux mprotect addr=0x%lx len=%lu "
                            "prot=0x%lx refused before post: %s\n",
                            (unsigned long)largs[0],
                            (unsigned long)largs[1],
                            (unsigned long)largs[2],
                            mref == -LINUX_EPERM
                                ? "write+execute is refused (§5.4)"
                                : mref == -LINUX_EINVAL
                                ? "malformed request word"
                                : "outside the image/stack/heap window");
                        return (uint64_t)(int64_t)mref;
                    }
                    cur->mem_stage_valid = 1;
                    cur->mem_stage_num   = num;
                    cur->mem_stage_a0    = largs[0];
                    cur->mem_stage_a1    = largs[1];
                    cur->mem_stage_a2    = largs[2];
                    cur->mem_stage_a3    = 0;
                }
                /* E7 step 4 (the file class): the getdents64 cursor
                 * rides IN the description — args[5], a word the child's
                 * own 3-argument call never uses, is where the sidecar's
                 * skip lives (the sidecar is stateless over it: it
                 * slices its listing from the index this word names).
                 * Any open resets the cursor: one listing at a time is
                 * the named v1 boundary (two interleaved directory fds
                 * would share this cursor), and a reused slot resets it
                 * beside every other piece of per-task state. */
                if (num == LINUX_OPEN)
                    linux_group_set_dir_pos(cur, 0);
                if (num == LINUX_GETDENTS64)
                    largs[5] = linux_group_dir_pos(cur);
                /* E7 increment 21: remember that the forward about to be
                 * armed is the gate workload's read(fd 3), so the resume
                 * sentinel can count the bytes its reply actually returns and
                 * stop at the sysret that crosses src_off. No limit is acted
                 * on here — stopping BEFORE the forward would leave the read
                 * unissued and the fd short of the offset. */
                if (cur->gate_profile && num == LINUX_READ && largs[0] == 3u)
                    cur->gate_read_pending = 1;
                linux_syscall_describe(&req, num, largs);
                if (cap_linux_find_channels(cur->pid, &ch_w, &ch_r) != 0) {
                    kernel_serial_printf(
                        "[E7] linux syscall %lu (class=%s) refused: no "
                        "sidecar channel — §3.5 wants exactly one\n",
                        (unsigned long)num,
                        linux_syscall_class_name(linux_syscall_class(num)));
                    return (uint64_t)(int64_t)(-LINUX_ENOSYS);
                }
                /* E7 §5.3 (the plan's increment 4), stage half: an
                 * ARCH_SET_FS is captured HERE — before the post — and is
                 * applied only if THIS forward's reply grants it (the
                 * resume arm below, same predicate on both ends of the
                 * seam). Every other forward clears the stage, so an old
                 * TLS value can never wait for a grant it was not asked
                 * for. */
                cur->fs_base_stage = 0;
                cur->fs_stage_valid = 0;
                if (linux_arch_prctl_is_set_fs(num, largs[0])) {
                    cur->fs_base_stage = largs[1];
                    cur->fs_stage_valid = 1;
                }
                /* E7 increment 5, copy half: attach the bytes, if any,
                 * AFTER the channel is known (a refused copy must not
                 * leave a half-built message anywhere) and BEFORE the arm
                 * — so a copy refusal is a plain first-trap refusal with
                 * no arm to un-arm. linux_user_copy_out reads only this
                 * task's own user pages (present, user, 4 KiB) or refuses
                 * -LINUX_EFAULT; it never faults over a tenant pointer. */
                if (plan.len) {
                    if (linux_user_copy_out(fwd + sizeof(struct SLSLinuxSyscallRequest),
                                            cur->cr3, plan.src,
                                            plan.len) != 0) {
                        kernel_serial_printf(
                            "[E7] linux write %lu bytes at 0x%lx refused "
                            "before post: unmapped or non-user span "
                            "(copy)\n",
                            (unsigned long)plan.len,
                            (unsigned long)plan.src);
                        return (uint64_t)(int64_t)(-LINUX_EFAULT);
                    }
                    fwd_len += plan.len;
                } else {
                    /* E7 step 4 (the file class): the path numbers' own
                     * attach — open/stat/lstat/unlink/mkdir carry one
                     * NUL-terminated string behind the header, rename
                     * TWO back to back (the sidecar splits at the first
                     * NUL and validates the second's, its twin of these
                     * bounds). Measured and copied HERE, where the cr3
                     * is in scope, still before the post: a span ring 3
                     * could not read is -EFAULT, a string with no NUL
                     * within LINUX_PATH_CAP is -ENAMETOOLONG by name —
                     * never a truncated path. A number with no path
                     * plans nothing (the classifier's total function)
                     * and this arm adds no bytes, as ever. */
                    struct LinuxFdPathPlan pplan =
                        linux_fd_path_plan(num, largs);
                    if (pplan.src) {
                        uint32_t plen1 = 0, plen2 = 0;
                        int m = linux_path_measure(cur->cr3, pplan.src,
                                                   LINUX_PATH_CAP, &plen1);
                        if (m != 0) {
                            kernel_serial_printf(
                                "[E7] linux path (syscall %lu) refused "
                                "before post: %s\n",
                                (unsigned long)num,
                                m == -LINUX_ENAMETOOLONG
                                    ? "path over LINUX_PATH_CAP (no NUL)"
                                    : "path span not user-readable (measure)");
                            return (uint64_t)(int64_t)m;
                        }
                        if (linux_user_copy_out(
                                fwd + sizeof(struct SLSLinuxSyscallRequest),
                                cur->cr3, pplan.src, plen1) != 0) {
                            kernel_serial_printf(
                                "[E7] linux path (syscall %lu) refused "
                                "before post: unmapped span (copy)\n",
                                (unsigned long)num);
                            return (uint64_t)(int64_t)(-LINUX_EFAULT);
                        }
                        fwd_len += plen1;
                        if (pplan.two) {
                            m = linux_path_measure(cur->cr3, pplan.src2,
                                                   LINUX_PATH_CAP, &plen2);
                            if (m != 0) {
                                kernel_serial_printf(
                                    "[E7] linux rename dst path refused "
                                    "before post: %s\n",
                                    m == -LINUX_ENAMETOOLONG
                                        ? "path over LINUX_PATH_CAP (no NUL)"
                                        : "path span not user-readable");
                                return (uint64_t)(int64_t)m;
                            }
                            if (linux_user_copy_out(
                                    fwd + sizeof(struct SLSLinuxSyscallRequest)
                                        + plen1,
                                    cur->cr3, pplan.src2, plen2) != 0) {
                                return (uint64_t)(int64_t)(-LINUX_EFAULT);
                            }
                            fwd_len += plen2;
                        }
                    }
                }
                {
                    struct SLSLinuxSyscallRequest *fwd_req =
                        (struct SLSLinuxSyscallRequest *)(void *)fwd;
                    *fwd_req = req;
                }
                /* E7 TG-1 I4: the group's forward lease. The sidecar's
                 * channel pair is per GROUP (every thread resolves to the
                 * leader's table), so two forwards in flight at once would
                 * race for one reply. Acquire HERE — after every first-trap
                 * refusal, before the stage or post — and never park owning
                 * it: a busy sibling records its slot and parks without
                 * posting, and the owner's release restarts it at its
                 * `syscall` (rip -= 2), so this block runs again with the
                 * same registers and now owns the lease. A refusal below or
                 * the delivered reply releases it. */
                {
                    struct LinuxThreadGroup* lg = linux_group_of(cur);
                    int lrc;
                    if (!lg) return (uint64_t)(int64_t)(-LINUX_EINVAL);
                    lrc = linux_group_fwd_acquire(lg, cur->tid);
                    if (lrc < 0) return (uint64_t)(int64_t)(-LINUX_EINVAL);
                    if (lrc > 0)
                        return (uint64_t)(int64_t)linux_forward_lease_wait(
                            cur, (uint32_t)num);
                    if (cur->tid != lg->leader_pid)
                        kernel_serial_printf(
                            "[E7] forward lease: TID %u group %u acquired for "
                            "syscall %lu\n",
                            cur->tid, lg->group_id, (unsigned long)num);
                }
                /* E7 increment 12: stage the reply payload's destination
                 * HERE — after every first-trap refusal has had its say
                 * (no channel, bad attach, W|X, bad poll span — each
                 * returns with the stage still clear) and immediately
                 * before the arm, so the window between staging and the
                 * deliver is exactly the forward's in-flight life. */
                if (rplan.len) {
                    cur->reply_stage_num   = num;
                    cur->reply_stage_va    = rplan.dst;
                    cur->reply_stage_len   = rplan.len;
                    cur->reply_stage_kind  = rplan.kind;
                    cur->reply_stage_aux   = rplan.aux;
                    cur->reply_stage_va2   = rplan.dst2;
                    cur->reply_stage_len2  = rplan.len2;
                    cur->reply_stage_va3   = rplan.dst3;
                    cur->reply_stage_valid = 1;
                }
                /* Arm BEFORE the post: cap_maybe_handoff (process.c) reads
                 * this state and skips the post's send-handoff — a sysret
                 * handoff would return to ring-3 MID-forward with its
                 * hardcoded rax=0, losing the reply (the sidecar's wake of
                 * this task is expected: it is parked on our channel). The
                 * park re-stamps the same value via proc_park_capture. */
                cur->park_syscall = SYS_LINUX_TRAP_RESUME;
                status = k_linux_forward(cur->pid, ch_w, ch_r, fwd, fwd_len);
                /* Reached ONLY when no park happened (post refused, or no
                 * park possible) — no reply was read on this path, by
                 * construction. Un-arm, then map the refusal into the
                 * units this task speaks (both CAP families, one table). */
                cur->park_syscall = 0;
                linux_forward_lease_release(cur);   /* post refused: free */
                cur->gate_read_pending = 0;   /* no park: no reply is owed */
                kernel_serial_printf(
                    "[E7] linux syscall %lu (class=%s) not delivered: "
                    "cap status %d\n",
                    (unsigned long)num,
                    linux_syscall_class_name(linux_syscall_class(num)),
                    status);
                return (uint64_t)(int64_t)linux_errno_from_cap(status);
            }
        }
    }

    /* Driver SDK block (307-309, 316-318): routed EXPLICITLY, before the
     * legacy switch. GCC builds one jump table for the legacy dense case
     * range (up to ~213) and the sparse high cases were unreachable
     * (observed: SYS_IRQ_MASK returned 0 from `default` on the target
     * while 311-315 dispatched fine — the table's range gate skipped the
     * last cases). The sidecar bootstrap depends on these, so they get a
     * dedicated path that cannot be miscompiled. */
    switch (num) {
    case SYS_SLS_IO_IN:
        return sys_sls_io_in((struct SLSIoInRequest*)arg);
    case SYS_SLS_IO_OUT:
        return sys_sls_io_out((struct SLSIoOutRequest*)arg);
    case SYS_SLS_DEV_MMAP:
        return sys_sls_dev_mmap((struct SLSDevMmapRequest*)arg);
    case SYS_SLS_IRQ_BIND:
        return sys_sls_irq_bind((struct SLSIrqBindRequest*)arg);
    case SYS_SLS_IRQ_UNBIND:
        return sys_sls_irq_unbind((struct SLSIrqUnbindRequest*)arg);
    case SYS_SLS_IRQ_MASK:
        return sys_sls_irq_mask((struct SLSIrqMaskRequest*)arg);
    case SYS_SLS_BOOT_GEN:
        return sys_sls_boot_gen();
    default:
        break;
    }

    switch (num) { 

    // ── Legacy (105–109) ──────────────────────────────────────────────────────
    case 105: /* SYS_SLS_ALLOCATE */
        return sls_legacy_allocate(arg);

    case SYS_SLS_CHMOD: /* 107 — update perm mask via object name */
        if (arg) {
            uint64_t* a = (uint64_t*)arg;  // a[0]=object_id, a[1]=mask
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active &&
                    object_catalog[i].object_id == a[0]) {
                    object_catalog[i].perm_mask = (uint32_t)a[1];
                    return 0;
                }
            }
        }
        return 1;

    case SYS_SLS_SET_USER: /* 108 — shell updates session vars itself; no-op here */
        return 0;

    case 109: /* SYS_SLS_SECURE_SEAL */
        return sys_sls_secure_seal((struct SLSSealRequest*)arg);

    // ── Phase 1: Object Catalog (110–119) ─────────────────────────────────────
    case SYS_SLS_VALLOC:
        return sys_sls_valloc((struct SLSVallocRequest*)arg);
    case SYS_SLS_VFREE:
        return sys_sls_vfree((const char*)arg);
    case SYS_SLS_OBJ_LIST:
        sys_sls_obj_list(); return 0;
    case SYS_SLS_OBJ_STAT:
        return sys_sls_obj_stat((const char*)arg);
    case SYS_SLS_ROLE_SET:
        return sys_sls_role_set((struct SLSRoleRequest*)arg);
    case SYS_SLS_GRANT: {
        /* shell passes uint64_t[2] = {req_ptr, is_grant} */
        uint64_t* a = (uint64_t*)arg;
        return sys_sls_grant((struct SLSGrantRequest*)(uintptr_t)a[0], (int)a[1]);
    }
    case SYS_SLS_REVOKE: {
        uint64_t* a = (uint64_t*)arg;
        return sys_sls_grant((struct SLSGrantRequest*)(uintptr_t)a[0], 0);
    }
    case SYS_SLS_SELECT:
        return sys_sls_select((struct SLSRecordRequest*)arg);
    case SYS_SLS_UPDATE:
        return sys_sls_update((struct SLSRecordRequest*)arg);
    case SYS_SLS_INSERT:
        return sys_sls_insert((struct SLSRecordRequest*)arg);

    // ── Phase 3: Transactions (120–123) ───────────────────────────────────────
    case SYS_SLS_TX_BEGIN:
        return sys_sls_tx_begin((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_COMMIT:
        return sys_sls_tx_commit((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_ROLLBACK:
        return sys_sls_tx_rollback((uint32_t)(uintptr_t)arg);
    case SYS_SLS_TX_RECOVER:
        sys_sls_tx_recover(); return 0;

    // ── Phase 4: Microkernel (130–134) ────────────────────────────────────────
    case SYS_SLS_SVC_LIST:
        sys_sls_svc_list(); return 0;
    case SYS_SLS_SVC_CRASH:
        return sys_sls_svc_crash((const char*)arg);
    case SYS_SLS_SVC_RESTART:
        return sys_sls_svc_restart((const char*)arg);
    case SYS_SLS_IPC_STAT:
        sys_sls_svc_list(); return 0;   /* combined view */
    case SYS_SLS_IPC_POST:
        return sys_sls_ipc_post((struct IPCPostRequest*)arg);

    // ── Ring-3 User IPC (166–168) ─────────────────────────────────────────────
    case SYS_SLS_IPC_BIND: {
        /* arg = (void*)(uintptr_t)port  — bind calling process to user port */
        uint16_t port = (uint16_t)(uintptr_t)arg;
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)(uint32_t)ipc_user_bind(port, caller_pid);
    }
    case SYS_SLS_IPC_SEND: {
        /* arg = pointer to IPCUserSendReq */
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)(uint32_t)ipc_user_send(
            (const struct IPCUserSendReq*)arg, caller_pid);
    }
    case SYS_SLS_IPC_RECV: {
        /* arg = pointer to IPCUserRecvReq (port field set by caller) */
        uint32_t caller_pid = 0;
        for (int _i = 0; _i < PROC_MAX; _i++) {
            if (proc_table[_i].active && proc_table[_i].state == PROC_RUNNING) {
                caller_pid = proc_table[_i].pid; break;
            }
        }
        return (uint64_t)ipc_user_recv((struct IPCUserRecvReq*)arg, caller_pid);
    }

    // ── Phase 5: Storage Tiers (140–142) ──────────────────────────────────────
    case SYS_SLS_TIER_LIST:
        sys_sls_tier_list(); return 0;
    case SYS_SLS_TIER_PROMOTE:
        return sys_sls_tier_promote((const char*)arg);
    case SYS_SLS_TIER_DEMOTE:
        return sys_sls_tier_demote((const char*)arg);

    // ── Phase 6: Schema & Delete (143–145) ────────────────────────────────────
    case SYS_SLS_DELETE:
        return sys_sls_delete((struct SLSRecordRequest*)arg);
    case SYS_SLS_SCHEMA_SET:
        return sys_sls_schema_set((struct SLSSchemaRequest*)arg);
    case SYS_SLS_SCHEMA_SHOW:
        sys_sls_schema_show((const char*)arg); return 0;

    // ── Phase 7: Query Engine (150–151) ───────────────────────────────────────
    case SYS_SLS_QUERY:
        sys_sls_query((const char*)arg); return 0;
    case SYS_SLS_QUERY_SCAN:
        sys_sls_query_scan(); return 0;

    // ── Phase B: Processes (160–164) ───────────────────────────────────────
    case SYS_SLS_PROC_CREATE:
        return process_create((struct ProcCreateRequest*)arg);
    case SYS_SLS_PROC_KILL:
        process_kill((uint32_t)(uintptr_t)arg); return 0;
    case SYS_SLS_PROC_LIST:
        sys_sls_proc_list(); return 0;
    case SYS_SLS_EXIT:
        process_exit((uint32_t)(uintptr_t)arg); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 4: hold/release/priority (245-247) ──
    case SYS_SLS_PROC_HOLD:
        return process_hold((uint32_t)(uintptr_t)arg) == 0 ? 0 : 1;
    case SYS_SLS_PROC_RELEASE:
        return process_release((uint32_t)(uintptr_t)arg) == 0 ? 0 : 1;
    case SYS_SLS_PROC_PRIORITY_SET:
        return sys_sls_proc_priority_set((struct SLSProcPrioritySetRequest*)arg);

    // ── Ring-3 debug output (165) ───────────────────────────────────────
    case 165: /* SYS_SLS_SERIAL_WRITE */
        if (arg) kernel_serial_print((const char*)arg);
        return 0;

    // ── Phase C: Loader (170–171) ───────────────────────────────────────
    case SYS_SLS_LOAD:
        return sys_sls_load((const char*)arg, spawn_owner_uid());
    case SYS_SLS_UPLOAD_BINARY:
        return sys_sls_upload_binary((struct SLSUploadRequest*)arg);
    case 172: /* loader_list */
        loader_list(); return 0;
    // Gap Remediation Phase G: struct-based, replacing the old raw-string,
    // no-output-to-caller shape (see loader.h's own comment).
    case SYS_SLS_SIMI_INFO:
        return sys_sls_simi_info((struct SLSSimiInfoRequest*)arg);
    case SYS_SLS_PROGRAM_SPAWN:
        return program_load((const char*)arg, spawn_owner_uid());
    // Seed Kernel Phase 1.5: NON-BLOCKING spawn — the child is queued with
    // a synthetic ring-3 context and the spawn returns immediately, so two
    // processes can be live at once (the blocking-cap_recv overlap test).
    case SYS_SLS_PROGRAM_SPAWN_NB:
        return program_spawn_nb((const char*)arg, spawn_owner_uid());
    // Phase 1.5 (gated async spawn): child starts PROC_HELD — the spawner
    // provisions its channel first, then releases it (SYS_SLS_PROC_RELEASE),
    // closing the spawn-then-provision timer race.
    case SYS_SLS_PROGRAM_SPAWN_NB_HELD:
        return program_spawn_nb_held((const char*)arg, spawn_owner_uid());

    // ── Phase D: Web App (180–182) ───────────────────────────────────────
    case SYS_SLS_WEBAPP_SET:
        return sys_sls_webapp_set((struct WebAppSetRequest*)arg);
    case SYS_SLS_WEBAPP_LIST:
        sys_sls_webapp_list(arg ? (const char*)arg : "*"); return 0;

    // ── Phase G: Token Auth (190–192) ─────────────────────────────────────
    case SYS_SLS_AUTH_CREATE:
        return auth_create_token((struct AuthCreateRequest*)arg, 0);
    case SYS_SLS_AUTH_LIST:
        sys_sls_auth_list(); return 0;
    case SYS_SLS_AUTH_REVOKE:
        return auth_revoke_by_email((const char*)arg);

    // ── Phase H: AI Agents (200–207) ──────────────────────────────────────────
    case SYS_SLS_AGENT_CREATE:
        return sys_sls_agent_create((struct AgentCreateRequest*)arg);
    case SYS_SLS_AGENT_RUN:
        return sys_sls_agent_run((struct AgentRunRequest*)arg);
    case SYS_SLS_AGENT_STATUS:
        sys_sls_agent_status((const char*)arg); return 0;
    case SYS_SLS_AGENT_KILL:
        return sys_sls_agent_kill((const char*)arg);
    case SYS_SLS_AGENT_LIST:
        sys_sls_agent_list(); return 0;
    case SYS_SLS_WORKFLOW_CREATE:
        return sys_sls_workflow_create((struct WorkflowCreateRequest*)arg);
    case SYS_SLS_WORKFLOW_RUN:
        return sys_sls_workflow_run((struct WorkflowRunRequest*)arg);
    case SYS_SLS_WORKFLOW_STATUS:
        sys_sls_workflow_status((const char*)arg); return 0;
    case SYS_SLS_AGENT_SCHEDULE:
        return sys_sls_agent_schedule((struct AgentScheduleRequest*)arg);

    // ── Phase 8: LPAR groundwork (210–212) ────────────────────────────────────
    case SYS_SLS_PARTITION_CREATE:
        // The last arm of this group to be gated, and the one a tenant had the
        // most direct use for: minting a partition is claiming a share of the
        // frame pool and a slot in the table every later operation looks up.
        // Refused with the arm's OWN failure sentinel (0xFFFFFFFF), not EPERM:
        // this syscall returns a partition id, and create's callers already
        // branch on that sentinel — a refusal has to look like the failure the
        // call site knows how to report. The line above it carries the reason.
        if (!partition_control_may_act("PARTITION", "create a partition"))
            return 0xFFFFFFFFu;
        return sys_sls_partition_create((struct SLSPartitionCreateRequest*)arg);
    case SYS_SLS_PARTITION_ASSIGN:
        // Not in the lifecycle group below, but the same rule and the reason
        // the four gates beside it hold: partition_assign_uid() accepts any
        // non-zero uid for any valid partition, and partition 0 IS
        // PARTITION_SYSTEM, so an ungated assign is a way into the gate —
        // assign a uid you control to partition 0, spawn, and the caller
        // check on the other arms reads "system". Gating the operations
        // while leaving this arm open would be a gate with a door in it.
        if (!partition_control_may_act("PARTITION", "assign a uid to a partition"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_assign((struct SLSPartitionAssignRequest*)arg);
    case SYS_SLS_PARTITION_LIST:
        sys_sls_partition_list(); return 0;

    // ── Phase 13: LPAR physical memory quotas (213) ─────────────────────────
    // §11 Q4's authority rule (partition_control_may_act() above): the frame
    // quota is this partition's share of RAM, so a tenant raising its own is
    // the same starvation the connection quota's default exists to prevent.
    case SYS_SLS_PARTITION_QUOTA_SET:
        if (!partition_control_may_act("FRAME-QUOTA", "change a frame quota"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_quota_set((struct SLSPartitionQuotaSetRequest*)arg);

    // ── Gap Remediation Phase F: sys_sls_partition_quota_list() was fully
    // implemented since Phase 13 but never got a syscall number or a
    // dispatcher case (217) -- fixed here.
    case SYS_SLS_PARTITION_QUOTA_LIST:
        sys_sls_partition_quota_list(); return 0;

    // ── Phase 14: LPAR partition lifecycle (214-216) ────────────────────────
    // The same rule on the operations that act on a partition's existence and
    // schedule: a tenant that could destroy, pause or resume a neighbour
    // would own it without ever being granted its resources. Refused before
    // the operation, so a denied caller never reaches the process kill the
    // destroy path starts with. partition_destroy() still refuses
    // PARTITION_SYSTEM on its own; that is a different guard (the target), not
    // this one (the caller).
    case SYS_SLS_PARTITION_DESTROY:
        if (!partition_control_may_act("PARTITION", "destroy a partition"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_destroy((uint32_t)(uintptr_t)arg);
    case SYS_SLS_PARTITION_PAUSE:
        if (!partition_control_may_act("PARTITION", "pause a partition"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_pause((uint32_t)(uintptr_t)arg);
    case SYS_SLS_PARTITION_RESUME:
        if (!partition_control_may_act("PARTITION", "resume a partition"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_resume((uint32_t)(uintptr_t)arg);

    // ── Multi-Node Partition Scaling Roadmap, Phase 6: cold migration (218) ──
    // Same rule: moving a partition to another node is an operator's decision
    // about where a tenant lives, and partition_migrate()'s own guard covers
    // the TARGET (PARTITION_SYSTEM never migrates) — this one covers the
    // CALLER. The kernel's failover and environment-migration paths call
    // partition_migrate() directly and are unaffected by construction.
    case SYS_SLS_PARTITION_MIGRATE:
        if (!partition_control_may_act("PARTITION", "migrate a partition"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_migrate((struct SLSPartitionMigrateRequest*)arg);

    // ── Multi-Node Partition Scaling Roadmap, Phase 7 addendum: operator-
    // driven node identity (279-280) -- closes the "cluster_init() is never
    // called from any real boot path" gap. Single-uint32_t ABI, same shape
    // as SYS_SLS_PARTITION_DESTROY/_PAUSE/_RESUME above. ─────────────────
    case SYS_SLS_CLUSTER_INIT:
        return sys_sls_cluster_init((uint32_t)(uintptr_t)arg);
    case SYS_SLS_CLUSTER_STATUS:
        sys_sls_cluster_status(); return 0;

    // ── Orchestration Plan Phase 4: service registry (281-284) ──────────
    // name -> partition/node/endpoint resolution. The node is NOT stored;
    // RESOLVE derives it from partition_owner_table[] each time, so a
    // lookup after partition_migrate() returns the new node with nothing
    // to invalidate (kernel/service_registry.h).
    case SYS_SLS_SERVICE_REGISTER:
        return sys_sls_service_register((struct SLSServiceRegisterRequest*)arg);
    case SYS_SLS_SERVICE_UNREGISTER:
        return sys_sls_service_unregister((struct SLSServiceRegisterRequest*)arg);
    case SYS_SLS_SERVICE_RESOLVE:
        return sys_sls_service_resolve((struct SLSServiceResolveRequest*)arg);
    case SYS_SLS_SERVICE_LIST:
        sys_sls_service_list(); return 0;

    // ── Orchestration Plan Phase 5: declarative workloads (285-288) ─────
    case SYS_SLS_WORKLOAD_DECLARE:
        return sys_sls_workload_declare((struct SLSWorkloadDeclareRequest*)arg);
    case SYS_SLS_WORKLOAD_DELETE:
        return sys_sls_workload_delete((struct SLSWorkloadDeclareRequest*)arg);
    case SYS_SLS_WORKLOAD_LIST:
        sys_sls_workload_list(); return 0;
    case SYS_SLS_RECONCILE_ENABLE:
        return sys_sls_reconcile_enable((uint32_t)(uintptr_t)arg);

    // ── Phase 22: SQL engine, live at last (220) ────────────────────────────
    // The first dispatch-reachable entry point into Phases 19-22's SQL
    // engine -- sql_execute() was previously callable only from its own
    // host tests. Autocommit only (see sql_exec.h) -- caller_uid travels
    // inside the request struct, matching SLSVallocRequest's own
    // owner_uid-embedded-in-the-request convention, since do_syscall()
    // itself has no uid context of its own to supply.
    case SYS_SLS_SQL_EXECUTE:
        return sys_sls_sql_execute((struct SLSSqlRequest*)arg);

    // ── Vector Store Roadmap Phase 4: make it live (221-224) ────────────────
    // The first dispatch-reachable entry points into Phases 1-3's vector
    // store + Ollama embedding client -- vecstore_create_collection()/
    // vecstore_insert()/vecstore_search() and ollama_embed() were
    // previously callable only from their own host tests, exactly the gap
    // SYS_SLS_SQL_EXECUTE closed for the SQL engine above. caller_uid
    // travels inside each request struct (see vecstore.h's own comment),
    // same convention as SYS_SLS_SQL_EXECUTE.
    case SYS_SLS_VEC_CREATE:
        return sys_sls_vec_create((struct SLSVecCreateRequest*)arg);
    // ── VectorStore Gap Analysis §1.3 follow-on: opt-in external_id
    // uniqueness (268) -- see vecstore.h's own comment on this syscall. ────
    case SYS_SLS_VEC_SET_UNIQUE:
        return sys_sls_vec_set_unique((struct SLSVecSetUniqueRequest*)arg);
    case SYS_SLS_OBJECT_SET_DATABASE:
        return sys_sls_object_set_database((struct SLSSetDatabaseRequest*)arg);
    case SYS_SLS_VEC_INSERT:
        return sys_sls_vec_insert((struct SLSVecInsertRequest*)arg);
    case SYS_SLS_VEC_EMBED_INSERT:
        return sys_sls_vec_embed_insert((struct SLSVecEmbedInsertRequest*)arg);
    case SYS_SLS_VEC_SEARCH:
        return sys_sls_vec_search((struct SLSVecSearchRequest*)arg);

    // ── Gap Remediation Phase B: the live path rowstore_create_table() ──────
    // never had (225) -- see rowstore.h's own comment on this syscall.
    case SYS_SLS_ROWSTORE_CREATE_TABLE:
        return sys_sls_rowstore_create_table((struct SLSRowstoreCreateTableRequest*)arg);

    // ── Gap Remediation Phase C: live surfaces vec_join_resolve() (226) and
    // the HNSW index (227-228) never had -- see vec_join.h's/vec_index.h's
    // own comments on these syscalls.
    case SYS_SLS_VEC_JOIN:
        return sys_sls_vec_join((struct SLSVecJoinRequest*)arg);
    case SYS_SLS_VEC_INDEX_CREATE:
        return sys_sls_vec_index_create((struct SLSVecIndexCreateRequest*)arg);
    case SYS_SLS_VEC_INDEX_SEARCH:
        return sys_sls_vec_index_search((struct SLSVecIndexSearchRequest*)arg);
    case SYS_SLS_VEC_LIST:
        sys_sls_vec_list();
        return 0;
    case SYS_SLS_VEC_INDEX_LIST:
        sys_sls_vec_index_list();
        return 0;

    // ── VectorStore Interface Roadmap Phase 1: deletion ─────────────────────
    case SYS_SLS_VEC_DELETE:
        return sys_sls_vec_delete((struct SLSVecDeleteRequest*)arg);
    case SYS_SLS_VEC_INDEX_DROP:
        return sys_sls_vec_index_drop((struct SLSVecIndexDropRequest*)arg);

    // ── VectorStore Interface Roadmap Phase 2: semantic (embed-then-search) ──
    case SYS_SLS_VEC_EMBED_SEARCH:
        return sys_sls_vec_embed_search((struct SLSVecEmbedSearchRequest*)arg);
    case SYS_SLS_VEC_INDEX_EMBED_SEARCH:
        return sys_sls_vec_index_embed_search((struct SLSVecIndexEmbedSearchRequest*)arg);

    // ── VectorStore Interface Roadmap Phase 3: rebuild/backfill ─────────────
    case SYS_SLS_VEC_INDEX_REBUILD:
        return sys_sls_vec_index_rebuild((struct SLSVecIndexRebuildRequest*)arg);

    // ── Navigator-Parity Gap Roadmap Phase 3: group profiles (237-239) ──────
    case SYS_SLS_GROUP_CREATE:
        return sys_sls_group_create((struct SLSGroupCreateRequest*)arg);
    case SYS_SLS_GROUP_ADD_MEMBER:
        return sys_sls_group_add_member((struct SLSGroupAddMemberRequest*)arg);
    case SYS_SLS_GROUP_LIST:
        group_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 3: authorization lists (240-242) ─
    case SYS_SLS_AUTHLIST_CREATE:
        return sys_sls_authlist_create((struct SLSAuthListCreateRequest*)arg);
    case SYS_SLS_AUTHLIST_GRANT:
        return sys_sls_authlist_grant((struct SLSAuthListGrantRequest*)arg);
    case SYS_SLS_AUTHLIST_CHECK:
        return sys_sls_authlist_check((struct SLSAuthListCheckRequest*)arg);
    case SYS_SLS_AUTHLIST_LIST:
        authlist_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 3: security audit log (243) ──────
    case SYS_SLS_AUDIT_LIST:
        sys_sls_audit_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 4: message queues (248-251) ──────
    case SYS_SLS_MQ_CREATE:
        return sys_sls_mq_create((struct SLSMQCreateRequest*)arg);
    case SYS_SLS_MQ_SEND:
        return sys_sls_mq_send((struct SLSMQSendRequest*)arg);
    case SYS_SLS_MQ_RECEIVE:
        return sys_sls_mq_receive((struct SLSMQReceiveRequest*)arg);
    case SYS_SLS_MQ_LIST:
        mq_list(); return 0;

    // ── Navigator-Parity Gap Roadmap Phase 5c: network/disk status (252-253) ──
    case SYS_SLS_NET_STATUS:
        sys_sls_net_status(); return 0;
    case SYS_SLS_DISK_STATUS:
        sys_sls_disk_status(); return 0;

    // ── SQL Feature-Parity Roadmap, Phase 8 follow-on: schema import/export
    // (254-255) -- see sql_exec.h's own comment on these two syscalls. ───────
    case SYS_SLS_SCHEMA_EXPORT:
        return sys_sls_schema_export((struct SLSSchemaExportRequest*)arg);
    case SYS_SLS_SCHEMA_IMPORT:
        return sys_sls_schema_import((struct SLSSchemaImportRequest*)arg);

    // ── VectorStore Interface Roadmap follow-on: collection/index
    // definition export/import (256-257) -- see vec_index.h's own comment
    // on these two syscalls. ─────────────────────────────────────────────
    case SYS_SLS_VEC_SCHEMA_EXPORT:
        return sys_sls_vec_schema_export((struct SLSVecSchemaExportRequest*)arg);
    case SYS_SLS_VEC_SCHEMA_IMPORT:
        return sys_sls_vec_schema_import((struct SLSVecSchemaImportRequest*)arg);

    // ─── VectorStore Interface Roadmap follow-on: bulk vector data
    // export/import (258-259) -- see vecstore.h's own comment on these two
    // syscalls. ─────────────────────────────────────────────────────────
    case SYS_SLS_VEC_DATA_EXPORT:
        return sys_sls_vec_data_export((struct SLSVecDataExportRequest*)arg);
    case SYS_SLS_VEC_DATA_IMPORT:
        return sys_sls_vec_data_import((struct SLSVecDataImportRequest*)arg);

    // ─── Database Namespace & Access Roadmap Phase 4 (260-265) ──────────────
    case SYS_SLS_DATABASE_CREATE:
        return sys_sls_database_create((struct SLSDatabaseCreateRequest*)arg);
    case SYS_SLS_DATABASE_DROP:
        return sys_sls_database_drop((struct SLSDatabaseDropRequest*)arg);
    case SYS_SLS_DATABASE_LIST:
        database_list(); return 0;
    case SYS_SLS_DATABASE_GRANT_UID:
        return sys_sls_database_grant_uid((struct SLSDatabaseGrantUidRequest*)arg);
    case SYS_SLS_DATABASE_GRANT_GROUP:
        return sys_sls_database_grant_group((struct SLSDatabaseGrantGroupRequest*)arg);
    // ── Database Gap Analysis §2.1: revoke (266-267) ─────────────────────
    case SYS_SLS_DATABASE_REVOKE_UID:
        return sys_sls_database_revoke_uid((struct SLSDatabaseRevokeUidRequest*)arg);
    case SYS_SLS_DATABASE_REVOKE_GROUP:
        return sys_sls_database_revoke_group((struct SLSDatabaseRevokeGroupRequest*)arg);
    case SYS_SLS_DATABASE_CHECK:
        return sys_sls_database_check((struct SLSDatabaseCheckRequest*)arg);

    // ─── Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2 (270-271) ──
    case SYS_SLS_TENANT_CREATE:
        return sys_sls_tenant_create((struct SLSTenantCreateRequest*)arg);
    case SYS_SLS_TENANT_LIST:
        tenant_list(); return 0;

    // ─── Multitenant Isolation Gap Analysis §5 item 6 / §7 item 6 (272) ──────
    case SYS_SLS_USAGE_REPORT:
        sys_sls_usage_report(); return 0;

    // ─── Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8 (273-274) ──
    // §11 Q4's authority rule (see partition_control_may_act() above): a
    // tenant may not retune its own or a neighbour's CPU weight.
    case SYS_SLS_PARTITION_CPU_WEIGHT_SET:
        if (!partition_control_may_act("SCHED", "change a CPU weight"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_cpu_weight_set((struct SLSPartitionCpuWeightSetRequest*)arg);
    case SYS_SLS_PARTITION_CPU_WEIGHT_LIST:
        sys_sls_partition_cpu_weight_list(); return 0;

    // ─── Storage Isolation Roadmap Phase 1 (275-276) ─────────────────────────
    // Same rule: raising or zeroing another partition's disk quota is the
    // control plane's decision, and a tenant zeroing its OWN quota would be
    // a self-inflicted denial it could then blame on the store.
    case SYS_SLS_PARTITION_STORAGE_QUOTA_SET:
        if (!partition_control_may_act("STORAGE-QUOTA", "change a storage quota"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_storage_quota_set((struct SLSPartitionStorageQuotaSetRequest*)arg);
    case SYS_SLS_PARTITION_STORAGE_QUOTA_LIST:
        sys_sls_partition_storage_quota_list(); return 0;

    // ─── Network Fairness Phase 2 (277-278) ──────────────────────────────────
    // POSIX-Environments v0.2 §11 Q4 — who may change a quota. The mechanism
    // shipped with no authority check on this arm at all, so any ring-3 caller
    // could zero its own ceiling or a neighbour's; a default set at the mint
    // would have been decorative. PARTITION_SYSTEM is the operator (init, the
    // system shell, the control plane); kernel context has no current process
    // and is the HTTP route's own path, which requires DB_ADMIN above this.
    case SYS_SLS_PARTITION_CONN_QUOTA_SET:
        if (!partition_control_may_act("TCP-QUOTA", "change a connection quota"))
            return (uint64_t)(int64_t)CAP_EPERM;
        return sys_sls_partition_conn_quota_set((struct SLSPartitionConnQuotaSetRequest*)arg);
    case SYS_SLS_PARTITION_CONN_QUOTA_LIST:
        sys_sls_partition_conn_quota_list(); return 0;

    // ─── Seed Kernel Phase 1: capability layer (289-297) ─────────────────
    // docs/AeroSLS-Capability-SDK-Phase1-Seed-Kernel-Design-v0.1.md. The
    // caller's identity is resolved inside each wrapper via
    // cap_current_pid() (process.c's strong hook), never from a request
    // field -- same trusted-resolution discipline as the IPC cases above.
    case SYS_SLS_CAP_CREATE_MEM:
        return sys_sls_cap_create_mem((struct SLSCapCreateMemRequest*)arg);
    case SYS_SLS_CAP_ARENA_ALLOC:
        return sys_sls_cap_arena_alloc((struct SLSCapArenaAllocRequest*)arg);
    case SYS_SLS_CHAN_CREATE:
        return sys_sls_chan_create((struct SLSCapChanCreateRequest*)arg);
    case SYS_SLS_CAP_SEND:
        return sys_sls_cap_send((struct SLSCapSendRequest*)arg);
    case SYS_SLS_CAP_RECV:
        return sys_sls_cap_recv((struct SLSCapRecvRequest*)arg);
    case SYS_SLS_CAP_REVOKE:
        return sys_sls_cap_revoke((struct SLSCapRevokeRequest*)arg);
    case SYS_SLS_CAP_MAP:
        return sys_sls_cap_map((struct SLSCapMapRequest*)arg);
    case SYS_SLS_CAP_UNMAP:
        return sys_sls_cap_unmap((struct SLSCapUnmapRequest*)arg);
    case SYS_SLS_CAP_LIST:
        sys_sls_cap_list(); return 0;

    // ─── Polyglot Nexus Phase 3: message transport (302-304) ────────────
    // docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md §2.2-2.3. The
    // message syscalls carry an IDL payload plus up to CAP_MSG_MAX_CAPS
    // moved MEM caps; the framing/opcode header is the user library's
    // concern (the kernel envelope is payload-opaque). SYS_SLS_CAP_ARENA_FREE
    // drops a single MEM reference (arena frames return at refcount 0).
    case SYS_SLS_CAP_SEND_MSG:
        return sys_sls_cap_send_msg((struct SLSCapSendMsgRequest*)arg);
    case SYS_SLS_CAP_RECV_MSG:
        return sys_sls_cap_recv_msg((struct SLSCapRecvMsgRequest*)arg);
    case SYS_SLS_CAP_ARENA_FREE:
        return sys_sls_cap_arena_free((struct SLSCapArenaFreeRequest*)arg);

    // ─── Seed Kernel Phase 1, two-party verification (298) ───────────────
    // Child resolves its spawner's pid so it can mint a channel's far-end
    // caps directly into the parent's capability table.
    case SYS_SLS_GETPPID:
        return sys_sls_getppid();

    // ─── Seed Kernel Phase 1.5, immediate wake (300) ─────────────────────
    // Voluntary yield: the entry frame stays on the process's syscall stack
    // and it resumes at .syscall_return on its next schedule — the yield
    // looks to ring-3 like a syscall that took a while. Used to hand the
    // CPU back to an async child so it can exit cleanly.
    case SYS_SLS_YIELD:
        return sys_sls_yield();

    // ─── Phase 3 trampoline capabilities (305-306) ─────────────────────
    // Same-ring, zero-copy, hardware-enforced cross-sidecar calls via MPK.
    case SYS_SLS_TRAMPOLINE_CREATE:
        return sys_sls_trampoline_create((struct SLSTrampolineCreateRequest*)arg);
    case SYS_SLS_TRAMPOLINE_CALL:
        return sys_sls_trampoline_call((struct SLSTrampolineCallRequest*)arg);

    // ─── Phase 5: create_sidecar (310) ─────────────────────────────────
    // Self-hosted boot: accept a packed manifest blob, create a process,
    // map the image, build the initial cap table, write a BIB, and enter
    // ring-3 (async, HELD until parent provisions resources).
    case SYS_SLS_CREATE_SIDECAR:
        return sys_sls_create_sidecar((struct SLSCreateSidecarRequest*)arg);

    // ─── E7 increment 2: the Linux-personality spawn (328) ───────────────
    // The same request struct as 310 (SLSCreateSidecarRequest); the
    // personality rides the syscall NUMBER, never the bytes (§5.1). The E2
    // sidecar_authority gate lives inside the callee, so both numbers are
    // gated identically.
    case SYS_SLS_LINUX_SPAWN:
        return sys_sls_linux_spawn((struct SLSCreateSidecarRequest*)arg);

    /* E7 increment 24: a monotonic clock for the sidecar's timed poll. */
    case SYS_SLS_NOW_NS:
        return (uint64_t)kernel_tick_counter * (uint64_t)KERNEL_TICK_NS;

    // ─── POSIX-Environments E3: allocate a contiguous physical region (320)
    // charged to the caller's partition, for a POSIX environment's private
    // heap / ramdisk storage. Gated to the sidecar creator tree.
    case SYS_SLS_ALLOC_REGION:
        return sys_sls_alloc_region((struct SLSAllocRegionRequest*)arg);

    // ─── POSIX-Environments P3 increment 1: the placement gate (327) ────
    // "Can partition P hold N more frames?" — the pre-flight that lets a
    // create refuse by name (ENV_ERR_CAPACITY) before the first frame is
    // taken, so a refused placement allocates nothing anywhere.
    case SYS_SLS_PLACEMENT_CAN_HOLD:
        return sys_sls_placement_can_hold(
            (struct SLSPlacementCanHoldRequest*)arg);

    // ─── POSIX-Environments E5: release a region (321) and resolve a sidecar
    // name to its pid (322). Together these are what ending an environment
    // needs — the pid to kill and the free to hand the frames back — and the
    // resolve doubles as the liveness test that says when it is safe to free
    // them (process_kill() defers a RUNNING target; see cap.h).
    case SYS_SLS_FREE_REGION:
        return (uint64_t)(int64_t)sys_sls_free_region(
            (struct SLSFreeRegionRequest*)arg);
    case SYS_SLS_SIDECAR_PID:
        return (uint64_t)sys_sls_sidecar_pid(
            (struct SLSSidecarPidRequest*)arg);

    // ─── POSIX-Environments Roadmap v0.2, Phase P1b (323-326) ────────────
    // The environment's durable store: attach at create (init), restore at
    // the ramdisk server's handshake, write-through on every RD_WRITE with
    // first-touch quota charging, release at destroy. Gating lives in the
    // wrappers (cap.c), beside the sys_sls_alloc_region family — trusted
    // caller resolution, never a request field.
    case SYS_SLS_ENV_STORAGE_ATTACH:
        return sys_sls_env_storage_attach((struct SLSEnvStorageAttachRequest*)arg);
    case SYS_SLS_ENV_STORAGE_RESTORE:
        return sys_sls_env_storage_restore((struct SLSEnvStorageRegionRequest*)arg);
    case SYS_SLS_ENV_STORAGE_WRITE:
        return sys_sls_env_storage_write((struct SLSEnvStorageWriteRequest*)arg);
    case SYS_SLS_ENV_STORAGE_RELEASE:
        return sys_sls_env_storage_release((struct SLSEnvStorageRegionRequest*)arg);

    // ─── Phase 5 channel transport (311-315) ───────────────────────────
    // The kabi.rs k_chan_* contract over cap_send_msg/cap_recv_msg
    // (kernel/chan.c, docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md
    // §3-§5). These return the transport's POSITIVE CAP_ERR_* codes, not
    // the negative CAP_E* codes of the Phase-3 message syscalls.
    case SYS_SLS_CHAN_WAIT:
        return sys_sls_chan_wait((struct SLSChanWaitRequest*)arg);
    case SYS_SLS_CHAN_RECV:
        return sys_sls_chan_recv((struct SLSChanRecvRequest*)arg);
    case SYS_SLS_CHAN_SEND:
        return sys_sls_chan_send((struct SLSChanSendRequest*)arg);
    case SYS_SLS_CHAN_CLOSE:
        return sys_sls_chan_close((struct SLSChanCloseRequest*)arg);
    case SYS_SLS_CAP_INFO:
        return sys_sls_cap_info((struct SLSCapInfoRequest*)arg);

    default:
        return 0;
    }
}
