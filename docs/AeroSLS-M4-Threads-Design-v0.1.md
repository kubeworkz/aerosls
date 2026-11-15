# AeroSLS M4 — Threads and Thread Groups (Design Spike v0.1)

Status: M4.5 landed; M4.7 gate not evidenced; runtime wall in force.

Date: 2026-11-14. Owner: Gridworkz. Scope: M4 in
`AeroSLS-PASE-Linux-Shape-Decision-v0.1.md`.

**This document does not claim thread support.** M4.1 landed the group
object and the shared-PML4 reference count; M4.2 landed the held thread
create, per-thread syscall stack, post-syscall context, messenger pair and
ptid/ctid writes; M4.3 split the sidecar's task record into shared group
state (fds, heap, dispositions) and per-thread state, with clone tracking and
reference-counted release. M4.4 added the group-keyed private futex wait/wake
queue, FIFO wake, expected-value check and a bounded relative timeout. M4.5
added the real `getpid`/`gettid` identities, per-thread `tkill` matching,
the kernel `exit_group` group sweep, the checked `clear_child_tid` zero at
thread exit and the sidecar wake on that word. The tree still has no
`clone3` (435), no thread-aware checkpoint/restore, and no live M4.7 gate. Until the increments in §5 are implemented and evidenced, **Go, threaded Rust, threaded CPython, Node, and every other threaded runtime remain out**. An undocumented or out-of-order partial implementation is a failure, not a milestone: a `clone` that copies a page table is not threads, and a futex that cannot wake another thread is not futex (the M4.4 live gate proves the wake, not a runtime). M4.1 is allowed to land alone because it adds no user-visible thread surface; M4.2 and M4.3 may land before futex only because they are explicitly staged here, and M4.4 may land before exit/identity and checkpoint only because it is likewise staged; all of them still leave every runtime out.

## 0. The wall, measured in the tree

The M4 gate is structural, not a syscall-table omission. Each row below was
checked against the current source rather than inferred from a document.

| Component | Current fact | Why it blocks threads |
|---|---|---|
| `clone` / `clone3` | M4.2 routes Linux 56 to `SYS_SLS_LINUX_CLONE_THREAD` (330) after the sidecar validates the documented thread subset; `clone3` (435) remains absent and unknown numbers reach the default `ENOSYS` | the tree can now create a second execution context in an existing group, but not a new address space and not the full clone state model |
| Address space | M4.2's `proc_create_linux_thread()` assigns `parent->cr3` and never calls `user_clone_page_table()`; `cap_create_sidecar_in_personality()` still clones for a NEW task | a thread shares one PML4; an independently created task still does not |
| Page-table lifetime | M4.1 makes the group own the PML4 (`kernel/linux_threads.c`), and process_exit/process_kill/deferred teardown destroy it only on the last reference; M4.2 attaches the clone child before release and its live probe faults one child while the parent still runs | M4.5's `exit_group` sweep and checked `clear_child_tid` zero run before the last reference; thread-aware checkpoint remains out |
| Process identity | `struct ProcessDescriptor` carries `linux_tgid`, `linux_tid`, `linux_clear_child_tid`, and `linux_is_thread` (`kernel/process.h`); M4.2's `linux_thread_group_attach()` writes the child's TID and the clone syscall writes ptid/ctid | M4.5 makes `getpid` return the TGID, `gettid` the calling TID, `tkill` match that real TID, and `exit_group` sweep by TGID; group-directed signal fan-out remains M4.8 |
| Kernel stacks | M4.2's create allocates a dedicated `syscall_stack_top` for the child and its `ring3_ctx` resumes on the caller's stack | two threads have independent kernel stacks and saved user contexts; M4.5's `clear_child_tid` zero+wake supplies the join rendezvous, while thread-aware checkpoint remains out |
| Sidecar task state | M4.3 adds `LinuxGroup` beside `LinuxTask`: the group owns the Linux fd table, the §5.4 heap ledger, the signal dispositions and (M4.4) the bounded futex wait queue; each thread owns its channel, name, VFS task (sharing the VFS table under `CLONE_FILES`), mask/pending/tid/alt stack and parked request. `track_thread` joins a clone to the parent's group; the last `release_task` retires the group. M4.5's exit/signal/close paths wake the stored clear-tid word and `exit_group` reaps every group record; a parked wait is not yet a checkpoint record | group state is shared and reference-counted, the group can be swept, and identity is real, but without a thread-aware checkpoint and the live M4.7 gate the runtime wall remains |
| `futex` | M4.4 lands private `FUTEX_WAIT`/`FUTEX_WAKE` only: the kernel copies the observed word and optional relative timespec, the sidecar owns a group-keyed FIFO queue, and timeout is `ETIMEDOUT` | `FUTEX_WAIT_BITSET`, requeue, PI, robust lists, process-shared futexes and signal-interrupted waits are named follow-ons; `clear_child_tid` zero+wake is M4.5, and a joinable runtime still needs M4.6's checkpoint |
| `exit_group` | M4.5 routes 231 to the kernel's TGID sweep (`SYS_SLS_LINUX_GROUP_SWEEP`, 331), which refuses a runnable member with `CAP_EAGAIN`, kills only non-running members, and reports success only after the group's live count and descriptor scan reach zero; the sidecar retries/yields, verifies every record is gone, then wakes each stored clear-tid and releases every group record | the group's address space is released once, on the last detach; `exit(60)` is the single-thread path |
| `set_tid_address` | stores the user pointer and uses it as the sidecar's tid (`user/sidecar/src/linux.rs`) | a real TID must be a kernel identity, distinct from the clear-child-tid address |
| Checkpoint record | v2: `ENV_CKPT_MAX_TASKS` is four; the Linux task slot owns the shared pages and `EnvCkptLinuxGroup` names up to eight TIDs, while payload v3 carries one `EnvPayloadThread` per context (`kernel/env_ckpt.h`, `kernel/env_payload.h`) | parked futex waiters are restored with their captured park frame and woken by the sidecar's restored queue; the runtime gate is still M4.7 |

A runtime is not unblocked by making the number stop returning `ENOSYS`.
It is unblocked only when two threads can share one address space, enter the
kernel independently, block on the same futex word, wake each other, exit
independently, and survive a checkpoint/restore cycle without losing the
shared address space.

## 1. Decisions

**D1 — The thread group is a kernel-owned object, not a convention.**
One `ThreadGroup` owns the shared address space and a live-thread count. Every
thread descriptor names a group index and its own TID. The group, not a
leader process slot, owns `cr3` lifetime, so a leader can exit while its
siblings continue.

**D2 — The address space is shared by reference, never copied.**
A thread create takes a reference on the leader's PML4. The last thread to
exit drops the final reference and only then calls
`user_destroy_page_table()`. No thread creation path may call
`user_clone_page_table()`.

**D3 — Each thread gets its own kernel syscall stack and saved user context.**
The existing per-process `syscall_stack_top`, `park_ctx`, `ring3_ctx`,
`fs_base`, `sigreturn_frame`, and stage fields become per-thread state. The
heap/image/mmap ledgers move to the group because they describe one address
space, not one execution context.

**D4 — `clone` is mediated by the sidecar; the kernel owns mechanism.**
A Linux `clone`/`clone3` trap is forwarded like every other Linux syscall.
The sidecar validates flags, group membership, and policy; the kernel creates
the child execution context, channel endpoints, and shared-address-space
reference. The sidecar never writes a child's user memory and the kernel never
invents a Linux policy answer.

**D5 — Upstream `clone` return semantics are preserved exactly.**
The child starts at the parent's post-`syscall` instruction with `rax = 0`,
the caller-provided child stack pointer, and the requested TLS base. Callee-
saved registers come from the parent's saved park context; caller-saved
registers are not promised by the x86-64 ABI. `CLONE_PARENT_SETTID`,
`CLONE_CHILD_SETTID`, and `CLONE_CHILD_CLEARTID` are validated and performed
by the kernel on the shared address space.

**D6 — `futex` is a sidecar wait queue keyed by `(group, uaddr)`.**
`FUTEX_WAIT` copies the 4-byte expected value out of the child before the
forward, compares it in the sidecar, and parks the thread only on equality;
mismatch is `EAGAIN`. `FUTEX_WAKE` wakes up to N waiters FIFO. The first
increment supports private wait/wake; requeue, priority inheritance, robust
lists, and `FUTEX_WAIT_BITSET` are named follow-ons, not silently accepted.

**D7 — Shared and per-thread sidecar state are separate.**
One group owns the fd table, the VFS task, `MemBrk`, signal dispositions, and
the futex queues. Each thread owns its TID, signal mask, pending bits,
alternate-stack state, channel endpoints, and parked request. `getpid`
returns the group leader's TID (the TGID); `gettid` returns the calling
thread's TID. `tkill` matches the per-thread TID; `exit_group` sweeps the
group.

**D8 — Checkpoint/restore is thread-aware by construction.**
The record carries one shared-address-space entry (pages, permissions,
heap/image cursors, group id) and N thread entries (RIP/RSP, registers, TLS,
mask/pending, channel, parked request). Restore maps the shared pages once,
recreates N held tasks, and releases them together. A group checkpoint that
loses a parked FUTEX_WAIT must be refused, not restored as a silent hang.

## 2. Architecture

### 2.1 Kernel objects

```
struct LinuxThreadGroup {           /* M4.1, kernel/linux_threads.h */
    uint64_t cr3;          /* one PML4, freed at the last reference      */
    uint32_t tgid;          /* leader PID, stable for the group            */
    uint16_t cr3_refs;      /* one reference per attached thread          */
    uint16_t live_threads;  /* members not yet detached                   */
    uint8_t  used;
    /* M4.1 is group identity + cr3 lifetime; the shared Linux heap/mm/    */
    /* image ledger moves here in M4.2, where a second task can use it.   */
};

struct ProcessDescriptor {
    /* existing fields ... */
    uint32_t linux_tgid;      /* 1-based group id, not a PID             */
    uint32_t linux_tid;       /* this thread's kernel TID                */
    uint64_t linux_clear_child_tid; /* user address, 0 = none            */
    uint8_t  linux_is_thread; /* 1 for a non-leader member               */
    /* per-thread fs_base, stack identity, park/ring3/sigreturn state ... */
};
```

The group table is static and bounded with the process table. A group entry is
reused only after `cr3_refs == 0 && live_threads == 0`, so a stale `cr3` can
never be adopted by a later task. In M4.1 the two counters start and move
together; `detach` refuses to mutate a half-torn group (one zero or the two
disagreeing), clears only the descriptor, and returns no PML4 — a leak is
preferable to freeing an address space a live member still uses.
`proc_clear_resume_state()` clears the thread fields on slot reuse, and a host
test walks every reuse site rather than trusting one.

### 2.2 Sidecar objects

`LinuxTask` is split into a group record and a thread record. The group
record holds `fds`, `vfs_task`, `MemBrk`, signal dispositions, and futex
queues. The thread record holds `ch_r`, `ch_w`, `tid`, `tgid`, `sig.mask`,
`sig.pending`, `sig.alt_*`, `clear_child_tid`, and `pending`/parked state.
A group is destroyed when its last thread slot is released. File offsets and
`O_CLOEXEC` are group state because Linux threads share them; a per-thread
copy is the exact bug that makes a `pthread` file descriptor look duplicated.

### 2.3 `clone` / `clone3` flow

1. The parent traps 56 (or 435) and the kernel builds the existing 72-byte
   description. It validates the child stack pointer and the `ptid`/`ctid`
   user ranges before any post, exactly as the M1/M3 copy plans do.
2. The sidecar validates the flag word. M4.2's implemented path accepts
   `CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES | CLONE_FS |
   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
   CLONE_CHILD_CLEARTID`. It refuses `CLONE_NEW*`, `CLONE_VFORK`,
   `CLONE_PIDFD`, `CLONE_PARENT`, and `CLONE_THREAD` without `CLONE_VM` by
   name. Unsupported flags are `ENOSYS`; malformed combinations are
   `EINVAL`; neither is silently dropped.
3. The sidecar calls `SYS_SLS_LINUX_CLONE_THREAD` (330, next free after
   `SYS_NOW_NS`). The request carries the parent's sidecar CHAN_W slot,
   child stack, TLS, ptid/ctid, and the accepted flag subset. The kernel
   resolves the parent from that channel, checks the parent is Linux and
   belongs to a live group, and refuses a cross-partition or cross-sidecar
   parent.
4. The kernel allocates a process slot and a dedicated syscall stack, takes a
   reference on the group's `cr3`, copies the parent's saved callee-saved
   state, sets `rax = 0`, `rip = parent's post-syscall RIP`, `rsp = child
   stack`, and the requested `fs_base`, and writes ptid/ctid if requested.
   The child is born `PROC_HELD` with only its two messenger channel caps.
5. The kernel writes the requested ptid/ctid, releases the child, and
   returns both sides' channel slots and the child TID. The sidecar tracks
   its own ends (`sidecar_r`/`sidecar_w`) and replies `0` to the parent. The
   child cannot run before its group membership and channel exist; a
   tracking failure kills it. M4.2's sidecar record is the minimal one —
   group/thread state sharing is M4.3.

### 2.4 Exit and join

A non-leader `exit` releases its channel, syscall stack, and group reference,
zeroes `clear_child_tid` if set, and wakes one futex waiter on that address.
The shared page table and fd table stay alive. `exit_group` walks every live
thread in the group, performs the same per-thread release, and only the last
release destroys the address space. A fatal signal follows the group rule:
the first increment may terminate the group, but it must not leave a thread
executing in a freed PML4.

`pthread_join` is then ordinary userspace: the joiner waits on the child's
`clear_child_tid` futex, the exiting child zeroes the word, and the wake
returns the joiner. No `waitpid`, no zombie, and no separate join syscall is
invented for M4.1.

### 2.5 Signals and TLS

Dispositions are group state. Masks, pending bits, alternate-stack state,
and the current handler depth are per-thread. `tkill` matches the calling
thread's TID; `kill`/`tgkill` group-directed delivery is a named follow-on.
`fs_base` is per-thread and is written only by the existing canonicality-
checked `ARCH_SET_FS` path or the clone request's `CLONE_SETTLS`. A thread
must never inherit a stale `fs_base` from a reused slot.

### 2.6 Checkpoint/restore

The M4 record extends the existing environment record rather than inventing a
second format. It adds a group record and a variable-length thread array. The
shared address space is captured once; each thread's context, TLS, mask,
pending, channel, and parked request are captured separately. A restored
group recreates the same group id, maps the shared pages once, spawns all
threads held, and releases them together. A group with a parked futex waiter
is restored parked with the same `(uaddr, expected)` key; if the key cannot be
represented, the capture refuses with a named code.

## 3. Why the alternatives are refused

- **User-space fibers in one task.** Go's runtime creates OS threads through
  `clone`; a fiber library cannot satisfy `runtime.newm` or `sysmon`.
- **Copy the address space and pretend.** Two copied PML4s do not share a
  mutex word. A `pthread_mutex_lock` in one thread would never see the other's
  unlock, and a Go heap write would be invisible to its scheduler.
- **One sidecar `LinuxTask` multiplexing all threads.** Each kernel thread has
  its own channel and parks independently. Multiplexing would require a second
  scheduler inside the sidecar and would still leave the kernel's per-thread
  kernel stack and signal context unbuilt.
- **Kernel-side `futex` only.** The existing Linux policy split (D1) puts fd,
  heap, and signal policy in the sidecar. A kernel futex queue would duplicate
  that policy and give the kernel a Linux-specific scheduler contract it does
  not otherwise have.
- **Claim Go as soon as `clone` returns 0.** A clone that copies the page
  table, a futex that cannot wake, or an `exit_group` that frees a live PML4
  all pass a shallow probe and fail a real runtime. The gate is a threaded
  candidate completing work and a checkpoint cycle, not a syscall number
  appearing in a log.

## 4. Security and isolation

- A thread group is confined to one partition, one owner UID, and one
  sidecar. The clone request is accepted only from that sidecar and only for
  a parent in its own group.
- No `CLONE_NEW*`, no cross-partition `clone`, no thread creation from an
  unprivileged program. The existing `sidecar_authority` gate remains the
  only door.
- All user pointers (`child_stack`, `ptid`, `ctid`, futex word, TLS) are
  validated by the kernel's two-phase user-copy walk. The sidecar never
  dereferences a child address.
- The shared address space is reference-counted. A teardown path that frees
  `cr3` while `as_refs > 0` is a fatal guard failure, not a recoverable bug.

## 5. Increments

Each increment carries a source guard, a teeth/smoke arm where a boot is not
required, and a live arm where it is. No increment may claim the next one.

- **M4.0 — Design and wall guard (this document).** Record the wall, the
  decisions, and the source guard that keeps Go/threaded runtimes refused
  while `clone`, `futex`, and thread groups are absent or partial.
- **M4.1 — Kernel thread-group object and address-space references.**
  **Landed 2026-11-14.** `kernel/linux_threads.{h,c}` add the bounded group
  table, `cr3_refs`/`live_threads`, the per-thread fields, and the
  create/attach/detach lifecycle. `process_init()` resets the table; the
  three teardown sites (`process_exit`, `process_kill`,
  `proc_deferred_teardown`) release the group reference before clearing the
  descriptor and destroy the PML4 only on the last reference, after
  `cap_table_teardown()`. A group-table-full create failure routes through
  `process_kill()` rather than a partial cleanup, and `detach` treats
  mismatched counters as half-torn: it clears the descriptor, returns no
  PML4, and leaks the slot rather than freeing a live address space.
  `tests/linux_thread_group_host_test.c` (the M4.1 cases run inside the
  now-177-check binary) drives a leader plus two attached threads, a leader exiting first, a
  non-last exit, the last-reference destroy, double detach, slot reuse,
  table exhaustion, the half-torn detach refusal, and the non-Linux legacy
  path. `tests/cap_create_sidecar_host_test.c` fills the group table and
  proves the resulting create failure reaches `process_kill()` and unbinds
  the failed child's cap table. No user-visible `clone`.
- **M4.2 — Kernel thread creation.** **Landed 2026-11-14.**
  `kernel/linux_clone.c` implements `SYS_SLS_LINUX_CLONE_THREAD` (330):
  flag/address validation before any allocation, the held descriptor and
  per-thread syscall stack (`proc_create_linux_thread`), the post-syscall
  context helper (`linux_thread_clone_context`), group attach, a messenger
  pair from the sidecar to the child, ptid/ctid writes, and release. A
  failure after create tears the child down through `process_kill`/
  `proc_discard_linux_thread`, never the parent's PML4. The sidecar's
  `clone` (56) arm validates the same subset, calls the kernel primitive,
  tracks the child's channel/VFS task, and answers `0`/TID. The raw probe
  (`user/sidecar/fixtures/m4_thread_probe.S`, `M4_THREAD_PROBE=1`) creates
  one thread, writes `M4-THREAD-OK` through its own tracked channel, then
  publishes a shared word; the parent writes `M4-THREAD-PARENT-OK`.
  `tests/linux_thread_group_host_test.c` (177 checks) adds the context and
  flag-verdict teeth, and `tests/m4_thread_probe_check.sh` boots the
  `M4_THREAD_PROBE=1` image and requires the child marker before the parent
  marker with no failure marker. This stage is permitted to land before futex and
  leaves Go and every threaded runtime out: a create without a join or
  shared sidecar state is not a runtime.
- **M4.3 — Sidecar clone and thread registry.** **Landed 2026-11-14.**
  `user/sidecar/src/linux.rs` now has a bounded `LinuxGroup` table and a
  `LinuxTask` table. The group owns the Linux fd slots, the `MemBrk` ledger
  and the signal dispositions; each thread owns its messenger channel,
  registry name, VFS task, signal mask/pending/tid/alternate stack and
  parked read/poll exchange. `track_thread` validates the parent/group and
  joins a clone with `CLONE_FILES` sharing the group table (or a private
  copy without it); a thread born without `CLONE_FILES` owns that private
  copy and M4.3 refuses a further clone from it by name rather than
  pretending it shares the group. `release_task` is the reference-counted
  exit path, and the last thread retires the group. `answer_clone` allocates
  the thread's VFS task with `Vfs::clone_task(CLONE_FILES)` rather than a
  fresh table. `user/sidecar/src/linux.rs`'s host suite adds group-sharing/
  private-fd/last-release teeth (81 sidecar tests). The live
  `M4_GROUP_PROBE=1` fixture opens a file in the leader, clones a thread,
  has the child write that descriptor and a shared stack word, and requires
  `M4-GROUP-CHILD-OK` before
  `M4-GROUP-PARENT-OK` with no `M4-GROUP-FAIL`
  (`tests/m4_group_probe_check.sh`). It is raw assembly deliberately: a real
  pthread probe needs the futex join in M4.4.
- **M4.4 — Futex wait/wake.** **Landed 2026-11-14.**
  `kernel/linux_abi.{h,c}` adds the `LSC_FUTEX` row (202) and a pure futex
  copy plan: a private `FUTEX_WAIT` attaches a fixed 24-byte record with the
  observed 4-byte word, a `has_timeout` flag and the relative timespec; a
  private `FUTEX_WAKE` attaches nothing. The sidecar's `LinuxGroup` owns a
  bounded FIFO `FutexWaiter` queue, `answer_futex` compares the observed
  word with the caller's expected value (`EAGAIN` on mismatch), parks a
  matching wait, wakes up to N oldest matching waiters and returns the
  count, and returns `ETIMEDOUT` at the deadline. A zero relative timeout
  is immediate `ETIMEDOUT`; a negative/out-of-range timespec is `EINVAL`.
  A released waiter is removed from the queue before its slot can be
  reused. The raw `M4_FUTEX_PROBE=1` fixture is a join shape: child
  publishes `ready`, parent wakes `go`, child publishes `done`, parent
  wakes it, then a no-waker timed wait must return `-ETIMEDOUT`; the markers
  are `M4-PTHREAD-OK`, `M4-FUTEX-OK`, `M4-FUTEX-TIMEOUT-OK` and no
  `M4-FUTEX-FAIL` (`tests/m4_futex_probe_check.sh`). This is not a libc
  pthread join: it is a raw shared-word handshake. M4.5 adds the
  `clear_child_tid` zero+wake join arm (the exit probe), while signal
  interruption of a parked futex wait remains a named follow-on; M4.4's
  interruptible arm is the bounded timeout and closed-channel teardown.
- **M4.5 — Identity, exit, and signals.** **Landed 2026-11-14.**
  `kernel/linux_abi.{h,c}` classifies `getpid` (39), `exit` (60), `gettid`
  (186) and `tkill` (200); the dispatch table answers 39/60/186/200 through
  the sidecar. `ProcessDescriptor`'s `linux_tid` is the kernel PID, the
  group's TGID is the leader PID, and `set_tid_address` (218) now returns
  the real TID while recording the user word for exit. `kernel/linux_clone.c`
  adds `SYS_SLS_LINUX_GROUP_SWEEP` (331): it resolves the Linux TGID through
  `linux_thread_group_id_by_tgid`, preflights the member snapshot and refuses
  a RUNNING member with `CAP_EAGAIN` before any kill (the caller yields and
  retries), then revalidates each PID after teardown. It does not treat a
  return from `process_kill` as death: success is reported only after both
  `linux_thread_group_live` and a fresh descriptor scan are zero, and a
  deferred or incomplete sweep returns `CAP_EAGAIN` instead of a count. The
  caller must hold `sidecar_authority`. `SYS_SLS_LINUX_SET_CLEAR_TID` (332)
  validates and stores the word.
  `linux_thread_group_detach` zeroes `linux_clear_child_tid` through
  `linux_user_copy_in` before the descriptor leaves the group; an unmapped
  word is silent and never strands the exit. The sidecar stores real
  TID/TGID/clear-tid on each `LinuxTask`, answers 39/186 and 218's real-TID
  return, routes 60 to one task and 231 to the group sweep, wakes a queued
  futex waiter on the stored ctid before releasing a task (exit, signal kill
  or CLOSED channel), and matches `tkill` against the sibling's real TID.
  The exit_group arm retries a `CAP_EAGAIN` sweep after `sched_yield`, and
  only reaps a record after the kernel success AND a fresh
  `sidecar_pid(name)` check says the task no longer resolves. A sweep that
  cannot prove death replies `EBUSY` to the still-parked caller, retires only
  records already dead, and logs `group sweep FAILED live=N not reaped`.
  Sibling signals currently answer exactly for existence, ignored and
  blocked (pended) arms; handler/default delivery to a sibling is `ENOSYS`,
  named, because it needs that thread's frame context. Group-directed
  fan-out remains M4.8. The raw `M4_EXIT_PROBE=1` fixture requires the
  leader's `getpid == gettid`, the child's real TID to equal `ptid` and be
  reachable by `tkill`, `set_tid_address` to return that TID, the join to
  observe `clear_child_tid` zero and wake after `exit(60)`, and finally
  `exit_group` to produce the kernel's `killed=2 live=0` sweep and the
  sidecar's `group swept=2 dead=2 live=0` with markers `M4-CHILD-OK`,
  `M4-IDENTITY-OK`, `M4-TKILL-OK`, `M4-JOIN-OK`, `M4-EXITGROUP-CHILD-OK`,
  `M4-EXITGROUP-BEFORE` and neither `M4-EXIT-FAIL` nor
  `M4-EXITGROUP-AFTER` (`tests/m4_exit_probe_check.sh`). The fixture's C2
  spins on the leader's own clear-tid word and faults if it observes the
  sweep's zero, so the gate sees a live post-sweep execution as a
  `m4.exit.probe.0.tN` exit/deferred-teardown line after the sweep. This
  still does not unlock a runtime: M4.6's checkpoint and the M4.7 gate are
  absent.
- **M4.6 — Thread-aware checkpoint/restore.** Extend the record and payload,
  restore one shared address space plus N threads, and prove a parked futex
  waiter survives a reboot.
  **Increment 2 live (2026-11-15): record v2 and payload v3 carry the group.**
  `EnvCkptRecord` now ends in one `EnvCkptLinuxGroup` — `n_threads`, the
  leader's TGID, the shared image/heap/brk/mmap/stack bounds, and up to eight
  TIDs — while the Linux `EnvCkptTask` still owns the pages once. The v3
  payload header adds `n_threads` and indexes an `EnvPayloadThread` array after
  the task descriptors. Each entry carries the complete 20-word context, the
  six argument registers, `fs_base`, both cursors, `clear_child_tid`, the
  captured `CapParkCtx` frame, the expected forwarded reply number, TID, clone
  flags, and the captured sidecar channel handles (the child slots are reserved).
  A healthy group of one to
  eight members is capturable; `EP_REFUSE_THREADS` remains only for a half-torn
  group (zero members, a live/refs disagreement, more members than the array).
  Capture records the group itself through `env_payload_refresh_records()`
  before `persist_environments()` writes the record array; without that pass the
  first checkpoint would persist an empty group.
  Restore recreates the N-1 siblings with
  `proc_create_linux_thread_restore()`/`cap_linux_restore_thread()` as HELD
  tasks sharing the leader's PML4, before the pour. The leader's channel pair
  is the fresh create's pair (verified against the captured sidecar slots), and
  each sibling gets a newly minted pair that must land on the captured sidecar
  slots; a mismatch refuses with `EP_REFUSE_CHANNEL` rather than pouring a
  restored sidecar table that names a channel this boot did not mint. Parked
  threads are re-armed from the captured `CapParkCtx`, `park_num` and
  `park_syscall=SYS_LINUX_TRAP_RESUME`; `waiting_chan` is this boot's channel.
  The restored sidecar's boot-time wait set predates the siblings, so the
  restore wakes it on an unrelated receive channel (console/ramdisk) to rebuild
  `append_wait`; it must not wake a task channel, because that would resume a
  parked forward before its reply exists. All tasks are released together by
  group. A restored park also gets `kernel_rsp = syscall_stack_top` — a parked
  descriptor has neither a saved kernel frame nor a ring3 frame until its first
  wake, and `process_find_current()` must resolve it when `cap_recv_resume`
  re-runs `do_syscall` (the live bug this increment fixed).
  The live half is `tests/m4_ckpt_probe_check.sh`. It builds the gate profile
  with `E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe M4_CKPT_PROBE=1`, which
  swaps the E7 firstuser for `fixtures/m4_ckpt_probe.S`: the child writes
  `M4.6-THREAD-OK`, sets READY and parks in an untimed private FUTEX_WAIT; the
  parent writes `M4.6-CKPT-READY` and parks in a 30-second private FUTEX_WAIT.
  The harness checkpoints only after both sidecars log `wait (futex)`, powers
  off, reboots, and POSTs `/api/env/restore`. Observed on 2026-11-15:
  boot 1 answered `{"status":0,"seq":1,"count":1}` and logged
  `linux group 'm4.ckpt.probe.0' threads=2 tids=105..106 forms=1/1` plus
  `capture: task 2 ... form=1`; boot 2 answered `payloads=1, payload_skips=0`,
  logged `restored group ... 2 held task(s)`, restored both threads PARKED on
  channels 14/15, and printed `M4.6-FUTEX-PARENT-WAKE`, `M4.6-FUTEX-WOKE`, and
  `M4.6-RESTORED-OK`. The E7 live gate was rerun on the same build and passed
  B1–B7 and D1–D2 (`payloads=1, payload_skips=0`; no fault; exit_group swept
  the resumed tenant). The host payload suite is 103/103, including a focused
  `EP_REFUSE_CHANNEL` case where the fresh sidecar pair lands on different
  slots than the capture recorded: the restore refuses before the first page
  moves. The runtime wall is
  unchanged.
- **M4.7 — The runtime gate.** A static Go candidate (or threaded Rust)
  creates threads, joins them, shares a heap and fd, and survives the
  checkpoint/restore cycle. Only this increment may remove the M4 wall from
  the roadmap and Appendix B.
- **M4.8 — Hardening.** `clone3`, robust futex lists, requeue, priority
  inheritance, group-directed signals, and a second-architecture audit.

## 6. Verification plan

- **Source guard** `tests/m4_threads_wall_check.sh`: pins the wall while
  support is absent, permits only the documented M4.1 (group), M4.2
  (group+clone), M4.3 (group/thread split) and M4.4 (private futex) partial
  stages, refuses an
  out-of-order partial (for example
  `futex` without `clone` or without a group), and requires all three
  components plus the live gate artifact before the docs may stop saying
  "  out". An all-present M4.4 tree never unlocks a runtime at F8; G1 must
  also see the documented M4.4 stage and M4.7 unevidenced, or the guard stays
  red rather than accepting a wall-preserved pass. Its smoke mutates a
  missing decision, a false landed
  claim, a doc-only gate claim, an out-of-order partial, and an all-present
  tree with and without the artifact.
- **Kernel host tests:** address-space reference prevents destruction; child
  context has `rax = 0`, the parent's post-syscall RIP, the requested RSP/TLS;
  `exit_group` sweeps every thread; the last exit frees exactly once; a
  half-torn group returns no PML4; and a group-table-full create reaches
  `process_kill()` with its cap table unbound.
- **Sidecar host tests:** group fd/heap/signal sharing, per-thread mask/TID,
  futex expected-value mismatch (`EAGAIN`), FIFO wake count, timeout and
  release removal, real getpid/gettid identity, set_tid_address's TID return
  and clear-tid record, exit-vs-exit_group answer selection and sibling
  tkill identity (83 sidecar tests). The M4.5 live probe joins through the
  `clear_child_tid` zero+wake and sweeps the group; the M4.4 probe remains
  the raw futex join shape.
- **Live probes:** the M4.2 raw two-thread probe (`M4-THREAD-OK`) and the
  M4.3 shared-group probe (`M4-GROUP-CHILD-OK` after a child writes the
  leader's fd), then the M4.4 join-shaped raw futex probe (`M4-PTHREAD-OK`
  from the child, `M4-FUTEX-OK` and `M4-FUTEX-TIMEOUT-OK` from the parent,
  and no `M4-FUTEX-FAIL`), then M4.5 (`M4-CHILD-OK`, `M4-IDENTITY-OK`,
  `M4-TKILL-OK`, `M4-JOIN-OK`, `M4-EXITGROUP-CHILD-OK`,
  `M4-EXITGROUP-BEFORE`, no `M4-EXIT-FAIL`/`M4-EXITGROUP-AFTER`, the
  kernel's `[PROC] M4.5 group sweep ... killed=2 live=0`, the sidecar's
  `group swept=2 dead=2 live=0`, and no `m4.exit.probe.0.tN` exit or
  deferred-teardown line after the sweep). Each marker is written by a
  tenant, not inferred from a syscall line.
- **Runtime gate:** static Go, `GOMAXPROCS=2`, a shared counter and a
  `sync.Mutex` across two goroutines pinned to two OS threads, then a
  checkpoint/restore cycle with the same result. A clean `exit_group` and a
  named `[TORE]` address-space count are the teardown evidence. The guard
  accepts only a live artifact at `evidence/m4_runtime_gate.log` carrying
  `M4-GO-OK`, `M4-FUTEX-OK`, `M4-CHECKPOINT-OK`, and
  `M4-RUNTIME-GATE-PASS`, with no `M4-RUNTIME-GATE-FAIL`; a roadmap sentence
  alone is not evidence.
- **Vacuity controls:** a `clone` that returns 0 without a second runnable
  context, a futex wake with zero waiters, and a checkpoint that restores only
  the leader must each redden their own arm.

## 7. Open questions, named rather than assumed

- The exact static Go toolchain baseline and whether its first `clone3` shape
  needs `CLONE_PIDFD`/`CLONE_INTO_CGROUP`; the answer comes from a census of
  the designated candidate, not from memory.
- Whether the first pthread probe uses musl or glibc; musl is smaller, but the
  final Go gate decides.
- Whether group-directed signal delivery chooses a thread or fans out; M4.5
  fixes per-thread `tkill` addressing and pends blocked sibling signals, but
  group-directed fan-out is M4.8.
- The checkpoint record's growth from four tasks to a variable thread array;
  the version bump and backward-read rule are M4.6's, not this spike's.
- Whether `clone` (56) or `clone3` (435) is the first user-visible surface.
  The design allows either; the census chooses the first, and the other is
  refused by name until implemented.
