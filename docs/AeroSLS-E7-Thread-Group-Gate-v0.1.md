# AeroSLS E7 — Thread-Group Gate

**Status:** Design and scoping pass. The TG-0 Go census is measured
(2026-11-09); the committed counts artifact is the 2026-11-22 regeneration.
The record-only `rt_sigaction` path and the four netpoll startup arms have
since landed in the tree. TG-1 I1 (group lifecycle), I2 (kernel-local `clone`,
`getpid`/`gettid`/`exit`, group-aware teardown), I3 (private `futex`
wait/wake with a relative timeout) and I4 (`openat`'s measured path open and
the per-thread forward lease) are landed, with host tests and passing native
two-thread, futex and sibling-forward openat live probes. I5 adds the paired
source guard and mutation teeth and re-runs the E7 gate's live/teeth and the
E3 multi-instance regression; the Go gate (G1.1–G1.8) remains the TG-1
acceptance run. Current dispatch status and the TG-0 gap list: §7.
**Date:** 2026-11-09 (measurement), 2026-11-22 (counts regeneration),
2026-11-23 (I2–I5 live probes, teeth and regressions); current-tree status
noted 2026-11-23.
**TG-0 artifact:** `tools/go_thread_gate_census.txt` (Go 1.25.1, pinned)
**Parent:** `docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §7, §8, §9 and
Appendix B.
**Scope:** the minimum Linux thread-group substrate a static Go target needs,
and the gate that decides whether it is real. It does not implement the Go
gate, the checkpoint extension, or signal delivery.

---

## 0. Bottom line

Go cannot be enabled by adding `clone` and `futex` to the syscall table.
The current E7 shim is structurally single-task in four different places:

1. one address space per `ProcessDescriptor`, with no sharing or reference
   count;
2. one writable set of Linux memory cursors (`brk`, `mmap_top`, image range,
   directory position) per descriptor;
3. one capability table and one sidecar channel pair per pid, with an
   invariant that **exactly one task is between post and reply**; and
4. one `LinuxTask` row per environment in the sidecar, carrying one fd table,
   one heap ledger, one signal state and one pending exchange.

A thread group has to move each of those to group ownership, not duplicate
them. The gate therefore has three levels:

- **TG-0 — measurement and compatibility.** A pinned static Go binary is
  run under `strace -f` on the host, its syscall surface is recorded, and its
  startup path is checked against the shim. This was the host-only first task.
  At the 2026-11-09 measurement Go installs signal handlers and the then-current
  sidecar refused handler installation; the current tree has the record-only
  path. See §7 for the current status.
- **TG-1 — live substrate gate.** A static Go program creates at least two
  OS threads, passes a value through shared memory, performs a real futex
  wait/wake, copies deterministic bytes through the environment's file/console
  path, and exits cleanly. No checkpoint cycle yet.
- **TG-2 — E7 parity gate.** The same program survives a checkpoint/restore
  cycle: every thread's context comes back, the shared address space stays
  shared, the fd/heap state comes back, and the work completes. Until this
  passes, Go is not a second E7 target.

A native C/Rust probe is useful as a development instrument for TG-1, but it
is **not** the acceptance gate: a native probe can pass while Go still dies at
`openat` or another still-missing row. At the 2026-11-09 measurement the signal
seam was also fatal; see §7 for the current status. The acceptance gate is the
Go binary.

TG-0 is now measured with the pinned Go 1.25.1 toolchain:
`tools/go_thread_gate_census.txt` records the exercised set and
`tools/go_thread_gate/` is the two-thread fixture. The measurement changes
the scope in one important way: Go's netpoll initializes even without a
`net` import, so `epoll_create1`, `epoll_ctl`, `epoll_pwait` and `eventfd2`
are hard startup requirements, not optional networking calls. §3.5 is the
measured delta; §3.1–§3.4 remain the source-level contract.

---

## 1. Decisions

**D1 — The gate is Go, not a native thread test.** The question "can this
kernel run a threaded Go runtime?" is only answered by a Go runtime. The
native probe exists to isolate kernel faults during development.

**D2 — Share the address space, not the process descriptor.** Threads are
separate `ProcessDescriptor` entries because the scheduler and trap path are
per-descriptor, but they share one `cr3`, one group capability table, one
forward channel pair, and one Linux memory/fd policy object. Per-thread state
is limited to the kernel/user stack, `fs_base`, register context, trap stages,
signal mask/pending state and identity.

**D3 — One forwarder at a time, first.** The existing channel invariant is
kept for v1: a group has one sidecar channel pair and a group-level forward
lease. A thread holds the lease from post to reply; a second thread that needs
a forwarded syscall parks until the lease is released. This is smaller than a
per-thread channel/demux design and preserves the sidecar's single `pending`
exchange. The cost is named in §5.4.

**D4 — The sidecar keeps one row per group, not one per thread.** The row
owns the fd table, VFS task id and heap ledger. Per-thread signal state is an
array keyed by tid. A thread request carries its tid; `clone` itself is
kernel-local and does not allocate a sidecar row.

**D5 — Futex is kernel-local.** The sidecar cannot read the child's page
table and is not the scheduler. `clone` and `futex` therefore join the
kernel-owned classes, beside the existing `fs_base` write. Everything that
needs fd/VFS/brk policy remains forwarded.

**D6 — Checkpoint is a separate increment.** The current checkpoint record
has four task slots and one Linux task; the payload captures one context and
one page walk for it. A thread group needs a schema change, a group stop
protocol, shared-page deduplication and a multi-thread replay. TG-1 can land
without pretending TG-2 is solved. TG-2 is required before Go is called a
second target.

**D7 — Do not promise full Go signal delivery.** The first gate runs with GC
and asynchronous preemption disabled, no `os/signal`, and no expected signal.
The minimum compatibility change is to accept a handler **installation** and
record it without ever invoking it (or to prove a narrower route); actual
signal frames and `rt_sigreturn` remain out of scope. If Go cannot start
without a delivered signal, the gate stops at that named wall rather than
calling a recorded handler a delivered one.

**D8 — Pin the Go version.** The syscall surface and clone flags are versioned
runtime behavior. The fixture records `go version`, the binary's sha256, and
the exact `strace` artifact. A later Go upgrade re-runs TG-0; it does not
inherit the previous green.

---

## 2. What the current tree actually provides

### 2.1 One address space per process

`struct ProcessDescriptor` (`kernel/process.h`) has one `cr3` and one
`fs_base`. `user_clone_page_table()` (`arch/x86/user_paging.c`) creates a
fresh PML4 with the kernel half shared by pointer and the user half empty.
There is no page-table reference count and no "share this root" helper.

`user_destroy_page_table()` frees every user-half intermediate table and
every owned leaf. It is called by `process_exit()`, `process_kill()` and
`proc_deferred_teardown()` (`kernel/process.c`), each of which assumes it owns
the address space. There is no path that tears down one thread without
tearing down the group.

The scheduler already has the pieces a thread needs: `ring3_ctx`,
`park_ctx`, `kernel_rsp`, `syscall_stack_top`, and a switch-in that writes
`fs_base`. What it does not have is a group identity or a per-thread teardown
path.

### 2.2 Per-descriptor Linux memory state

`linux_heap_base`, `linux_heap_limit`, `linux_brk_cur`, `linux_mmap_top`,
`linux_image_lo/hi`, `linux_dir_pos` and the initial `linux_stack_*` fields
are all on `ProcessDescriptor`. The brk/mmap/mprotect apply arm in
`kernel/syscall_dispatch.c` reads and writes those fields directly.

Two threads that keep private copies would be able to choose overlapping
addresses or disagree about the current break. This is not a policy detail:
the sidecar's `MemBrk` is the policy twin of those fields, and the kernel's
comment already says the two must never drift. A group needs one object with
one set of cursors; the descriptors must not remain writable owners.

`fs_base` is the exception: it is genuinely per-thread, is already written on
every switch-in, and Go's clone path sets it with `CLONE_SETTLS`.

### 2.3 The forward channel is one-in-flight by construction

`cap_linux_find_channels()` (`kernel/cap.c`) requires exactly one usable
`CHAN_W` and one `CHAN_R` in the caller's capability table.
`k_linux_forward()` posts a request and parks; `k_linux_deliver()` receives
the next reply and checks the syscall-number echo. The header is explicit:

> this channel's ONLY writer is the one task forwarding on it, and that task
> is parked between post and reply, so the inbound queue can never be full at
> post time.

`ProcessDescriptor.park_syscall == SYS_LINUX_TRAP_RESUME` is the in-flight
marker. A second thread posting on the same channel before the first reply
would let either thread receive the other's reply; the echo catches many such
cases as `-EPROTO` but cannot make a same-number exchange correct. This
invariant must be made a group invariant before any second thread can trap.

### 2.4 The sidecar has one task row per environment

`LinuxTask` (`user/sidecar/src/linux.rs`) holds one `ch_r`/`ch_w` pair, one
fd table, one `MemBrk`, one `SigState`, one VFS task id, and one pending
read/poll exchange. `MAX_LINUX_TASKS` is 8, but that is eight **environments**,
not eight threads in one group. The sidecar is a single-threaded event loop;
the `pending` field deliberately means "no further request can arrive" while
a read/poll is unresolved.

A group therefore cannot get one row per thread without also solving channel
demultiplexing, per-thread fd-table identity, and multiple pending exchanges.
The smaller design is one row per group with per-thread signal entries.

### 2.5 Exit is group-aware (TG-1 I2)

I1 moved the shared teardown behind `linux_group_destroy()`, and I2 added the
distinct lifecycle paths around it. `exit` (60) is `LSC_TASK` and releases ONE
thread: its syscall stack, trap state and descriptor identity go, while the
group's cr3, cap table and sidecar row stay while siblings live. The last live
thread (or `exit_group`, a fault, or a kill that lands on any member) reaches
`linux_group_release_all()`, which stops every member and then runs the group's
ONE cap-table/page-table destroy. `process_kill()` on a member is a group kill;
a RUNNING target defers every member to the scheduler sweep.

The leader is v1's named exception: if it exits while siblings live, its
descriptor stays ACTIVE-but-ZOMBIE so the sidecar's name-to-pid lookup keeps
resolving until group teardown. It is never scheduled again.

**Measured 2026-11-23.** `tests/linux_thread_probe_check.sh` boots the
ordinary ISO and the sidecar-spawned native probe creates one sibling: the
kernel logs `clone: TID 100 joins group 106 (leader 106)`, both threads read
that group id, the child publishes its own TID and a shared word through the
group's one heap, and `exit(60)` logs a thread-local exit for TID 100 before
the parent's `exit_group` produces exactly one `group exit: group 106` line.
The probe deliberately makes no forwarded syscall from the child: a forward
parks the calling descriptor, and v1's channel owner is the group leader, so
the reply would wake the leader and leave the child parked. Per-thread
park/wake is I3/I4 work; the probe's post-clone path is kernel-local
`getpid`/`gettid`/`exit` plus shared memory only.

### 2.6 Checkpoint has one Linux task slot

`ENV_CKPT_MAX_TASKS` is 4 (`kernel/env_ckpt.h`: the two sidecars, one Linux
tenant, one spare). `EnvCkptTask` carries one name/kind/entry/stack record.
`EnvPayloadTask` carries one Linux register context, one `fs_base`, one set of
brk/mmap/image fields, and a per-task page count/block. The restore creates
one Linux child and holds it for the pour; `env_payload` refuses a task that
is not in the one restorable form.

`ep_linux_page()` assigns pages by the task's own image/heap/stack ranges.
Two threads sharing a heap would produce duplicate page entries unless one
owner is made responsible for the shared range. `ENV_PAYLOAD_REC_VERSION` is
3; any group layout change is a format bump, not a silent reuse of the spare
task slot.

### 2.7 The syscall table: Rust census plus netpoll rows

`kernel/linux_abi.c` started as the 27-row Rust census table. The current
table adds the four netpoll startup rows (`epoll_ctl` 233, `epoll_pwait` 281,
`eventfd2` 290, `epoll_create1` 291), `clone` (56, `LSC_THREAD`), and I2's
three task rows — `getpid` (39), `exit` (60) and `gettid` (186), all
`LSC_TASK` and kernel-local. I3 added `futex` (202, `LSC_FUTEX`), also
kernel-local: the sidecar owns no page table and is not the scheduler, so the
wait/wake state lives in the kernel. `openat` (257, `LSC_FD`) is now the one
remaining TG-1 table gap. `linux_syscall_class()` answers `LSC_UNKNOWN` for
everything else, and the first-trap branch refuses an unknown number before
any post.

The table also has no row for `tkill` (200), although the sidecar has a
`LINUX_TKILL` answer arm. As read in this pass, the first-trap classification
would never let 200 reach that arm. That is a pre-existing mismatch to settle
during the Go census; the thread-group gate must not be built on an
unreachable signal path.

### 2.8 The sidecar's signal seam at measurement time

**Measurement-time finding (2026-11-09).** At that measurement the seam
refused non-default handlers. The current tree has the record-only path; §7 is
the current status.

At the 2026-11-09 measurement the E7 signal seam recorded
`SIG_DFL`/`SIG_IGN`, masks and pending bits, and refused a non-default handler
by name because the shim had no signal frames and no `rt_sigreturn`. Go's
runtime does not start without installing handlers: `setsig()` calls
`rt_sigaction` with the Go trampoline, and `sysSigaction()` throws when the call
fails. That was the first concrete blocker for any Go binary, independent of
threads.

`sigaltstack` is already accepted (query and install), and
`rt_sigprocmask` is recorded. The missing piece at measurement time was
acceptance of a handler address, with no promise to invoke it. The current
tree's record-only path accepts and stores the address and never invokes it;
see §7. It remains an explicit compatibility decision, not a side effect of
adding `clone`.

---

## 3. The Go runtime contract this gate must satisfy

The following is read from the Go 1.25.1 Linux runtime sources (`os_linux.go`
and `sys_linux_amd64.s`). The fixture will pin its own Go version; the values
below are the contract to verify, not a claim that 1.25.1 is the shipped
version.

### 3.1 Clone

Go's `newosproc` builds these flags:

```text
CLONE_VM       0x00000100
CLONE_FS       0x00000200
CLONE_FILES    0x00000400
CLONE_SIGHAND  0x00000800
CLONE_SYSVSEM  0x00040000
CLONE_THREAD   0x00010000
              ----------
cloneFlags     0x00050F00
```

The amd64 clone wrapper adds `CLONE_SETTLS` (`0x00080000`) when it supplies a
TLS pointer, so the effective request is **`0x000D0F00`**. It passes:

- `rdi` flags;
- `rsi` the new thread's stack pointer (`mp.g0.stack.hi`);
- `rdx` parent-TID pointer: 0;
- `r10` child-TID pointer: 0;
- `r8` TLS pointer: `&mp.tls + 8` on amd64;
- `r9` the `*g` value the child trampoline uses (not a syscall argument).

No `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` or `CLONE_CHILD_CLEARTID` is
used by this path. The child returns from the syscall with `rax == 0`, sets
`SP = rsi` in the assembly after the syscall, calls `gettid`, stores it in
`m.procid`, and enters `mstart`. The parent receives a positive TID.

The kernel's clone implementation therefore must:

- create a new descriptor without cloning the page table;
- set the child's `cr3` to the group root;
- set `fs_base` from the TLS argument when `CLONE_SETTLS` is present;
- preserve the parent's return RIP and the child-stack value in the child's
  first ring-3 context, with `rax = 0`;
- give the child its own kernel syscall stack and scheduler entry; and
- return the child's TID to the parent.

### 3.2 Futex

Go uses only the private operations by default:

```text
FUTEX_WAIT_PRIVATE = 128   (0 | 128)
FUTEX_WAKE_PRIVATE = 129   (1 | 128)
```

`futexsleep` passes a relative timeout in a `timespec`; `futexwakeup` wakes at
most `cnt` waiters. A v1 gate needs `FUTEX_WAIT`/`WAKE` and the private bit
with a relative timeout. It does not need PI futexes, robust-list ownership,
requeue, or process-shared futexes.

### 3.3 Other runtime calls the census measured

The measured gate did call `gettid`, `rt_sigaction`, `rt_sigprocmask`,
`sigaltstack`, `sched_getaffinity`, `openat`, `madvise`, `exit` and
`exit_group`. It did **not** call `getpid`, `tgkill`, `mincore`,
`clock_gettime`, `munmap`, `mprotect` or `brk` in this run. The complete,
classified set is §3.5; the source-level names in §3.1–§3.4 remain useful for
the next Go version, but they are not a substitute for the pinned artifact.

The census also found four calls that are not in the source-level list:
`epoll_create1`, `epoll_ctl`, `epoll_pwait` and `eventfd2`. They are Go's
netpoll initialization and are fatal if missing; §3.5 and §5.1 treat them as
startup requirements even though the gate imports no `net`.

### 3.4 The startup signal wall

**Measurement-time finding (2026-11-09).** At that measurement the shim
refused handler installs; the current tree has the record-only path. See §7
for the current status.

`setsig()` is called during runtime initialization for the signals Go
installs. It constructs a `sigaction` with `SA_SIGINFO|SA_ONSTACK|SA_RESTORER|
SA_RESTART`, a Go trampoline and `sigreturn__sigaction`, and calls
`rt_sigaction`. `sysSigaction()` throws `"sigaction failed"` if the kernel
returns an error. This happens before the gate's `main`.

Therefore, at measurement time, the shim's "handler installs refused by
name" was a hard blocker. TG-0 had to prove one of:

1. a **record-only handler acceptance** path (recommended): store the handler
   address, return success, never invoke it, and run the gate with no signal
   expected; or
2. a full signal-frame path, which is a separate, larger project and is not
   scoped here.

The thread-group gate must not claim to have delivered a signal merely because
it stored a handler address.

### 3.5 Measured TG-0 result (2026-11-09)

**Measurement-time findings.** The counts and the three findings below record
the 2026-11-09 tree and trace. They are not the current dispatch status; §7
records what has since landed and the six remaining table gaps.

**Counts are that run's:** the table below is the 2026-11-09 measurement, not the regenerated artifact — the committed `tools/go_thread_gate_census.txt` is the 2026-11-22 regeneration, and its numbers differ by run.

The fixture in `tools/go_thread_gate/`, built with the pinned Go 1.25.1
toolchain (tarball sha256
`7716a0d940a0f6ae8e1f3b3f4f36299dc53e31b16840dbd171254312c41ca12e`), is a
static `ET_EXEC`. It created two distinct TIDs (1242243/1242245), completed
a real futex wait/wake (`wait=0`, `wake=1`), and copied 65,536 bytes
byte-identically. The full trace is represented by
`tools/go_thread_gate_census.txt`; the counts below are one run and are
scheduling-sensitive.

| Class | Syscalls (number, count) |
|---|---|
| **Hard startup/runtime** (forced `ENOSYS` fatal or corrupts the run) | `epoll_create1` (291, 1), `epoll_ctl` (233, 3), `epoll_pwait` (281, 2), `eventfd2` (290, 1), `fcntl` (72, 14), `clone` (56, 6), `futex` (202, 54), `rt_sigaction` (13, 114), `rt_sigprocmask` (14, 21), `sigaltstack` (131, 15), `arch_prctl` (158, 1), `mmap` (9, 22), `openat` (257, 6), `exit_group` (231, 1) |
| **Required by the gate** | `read` (0, 6), `write` (1, 3), `close` (3, 5), `fsync` (74, 1), `gettid` (186, 15), `exit` (60, 1). `gettid` is tolerated by the runtime, but its failure makes the gate's TID evidence invalid. |
| **Optional/tolerated** (forced `ENOSYS` still completed) | `copy_file_range` (326, 2), `pread64` (17, 2), `sched_yield` (24, 2), `madvise` (28, 2), `nanosleep` (35, 51), `uname` (63, 1), `prctl` (157, 2), `sched_getaffinity` (204, 2), `prlimit64` (302, 2) |
| **Bootstrap only** | `execve` (59, 1) — the kernel starts the binary; not a gate call |

The three scope-changing findings at measurement time:

1. **Netpoll is not optional.** `epoll_create1`, `epoll_ctl`, `epoll_pwait`
   and `eventfd2` failed fatally under fault injection even though the
   fixture imports no `net`. At measurement time the table had none of them;
   Go cannot reach `main` without a minimal event substrate.
2. **`rt_sigaction` was the hard wall.** 114 calls, all handler
   installations; at measurement time the sidecar refused them. The
   record-only compatibility path was not optional.
3. **`fcntl` is a startup requirement.** It checks standard fds 0–2 at
   runtime init; a failure is fatal. It is already in the Rust table, but its
   Go role is startup, not only file policy.

`copy_file_range` is what `io.Copy` chose first; forcing it to `ENOSYS`
caused the expected `read`/`write` fallback and the copy still matched.
`nanosleep`, `madvise`, `prctl`, `prlimit64`, `uname`, `sched_yield` and
`pread64` are exercised but not required for this gate. No `mprotect`,
`munmap` or `brk` appeared in this trace.

---

## 4. Target design

### 4.1 The ownership split

| State | Owner | Why |
|---|---|---|
| PML4/root, user page tables | group | one shared address space; one destroy |
| heap base/limit, brk, mmap top | group | one allocator policy, one sidecar ledger |
| image range, directory cursor | group | shared image and fd table |
| capability table, channel pair | group | threads share fds and the sidecar |
| forward in-flight lease | group | preserve one-writer/one-reply invariant |
| fd table, VFS task, heap ledger | sidecar group row | shared Linux process state |
| `fs_base`, user/kernel stack, register context | thread | per-thread execution |
| signal mask/pending, clear-TID pointer | thread | per-thread Linux state |
| staged trap words, park context | thread | one trap at a time per thread |
| exit status / group lifetime | group | last thread owns final teardown |

The concrete C layout is an implementation choice. The contract is that no
thread keeps a writable private copy of a group-owned field, and no thread
tears down a group-owned object.

### 4.2 Proposed objects

This is a shape sketch, not compiled code:

```c
#define LINUX_THREADS_MAX 8

struct LinuxThreadGroup {
    uint8_t  used;
    uint8_t  exiting;
    uint16_t live_threads;
    uint32_t group_id;          /* boot-local; equals the leader's tgid */
    uint32_t leader_pid;
    uint32_t cap_owner_pid;     /* descriptor whose cap table the group uses */
    uint64_t cr3;               /* the one root; threads point at this */

    /* Shared Linux memory policy, mirroring MemBrk. */
    uint64_t heap_base, heap_limit;
    uint64_t brk_cur, mmap_top;
    uint64_t image_lo, image_hi;
    uint64_t dir_pos;

    /* Forward lease. */
    uint8_t  fwd_busy;
    uint32_t fwd_owner_tid;
    uint32_t fwd_wait_head;     /* descriptor index, or NONE */
};

struct ProcessDescriptor {
    /* existing fields ... */
    uint32_t tid;               /* gettid(), unique per descriptor */
    uint32_t tgid;              /* getpid(), the leader's group id */
    uint32_t group_index;       /* index into linux_groups[] */
    uint8_t  is_group_leader;
    /* existing fs_base, user_rsp, ring3_ctx, park_ctx, kernel stack ... */
};
```

A static `linux_groups[]` pool of at most `PROC_MAX` is sufficient; this
kernel does not allocate descriptors dynamically. The group index, not a raw
pointer, is safer across a future descriptor-table move and is easy to print.

`PROC_MAX` is 16. Two sidecars plus a leader plus eight threads is eleven
slots; the gate will use two or three threads. A second concurrent environment
will exhaust the table quickly, so the gate must state its tenant budget and
the group's thread cap must be enforced with a Linux errno (`EAGAIN`) rather
than overcommitting.

### 4.3 Clone semantics

The kernel-local `clone` path accepts only the Go thread shape in v1:

- `CLONE_VM`, `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`, `CLONE_SYSVSEM`,
  `CLONE_THREAD` and `CLONE_SETTLS` may be set;
- `CLONE_VFORK`, `CLONE_PARENT`, namespace flags, `CLONE_PTRACE` and any
  unrecognised flag are refused with `EINVAL`;
- `CLONE_THREAD` requires `CLONE_VM` and `CLONE_SIGHAND` (Linux's own
  consistency rules);
- the child must have a user-canonical non-zero stack, and the stack is not
  mapped by the kernel: Go allocated it in the shared heap before calling
  clone;
- the caller must be a Linux task whose group has room; an SLS personality
  cannot use this path.

On success the parent returns the child's TID. The child is left
`PROC_SUSPENDED` with `has_ring3_ctx = 1`, a synthesized 20-word context
(`rax=0`, `rip=parent's return RIP`, `rsi=child_stack`, `rsp=child_stack`,
`fs_base=tls`), and a fresh syscall stack. It becomes runnable on the next
scheduler pass. No sidecar request is sent: the group already owns the
channel and fd table.

The proposed implementation adds no `CLONE_CHILD_CLEARTID` promise. Go does
not request it. `set_tid_address` and clear-child-TID can be a later
compatibility step if a measured runtime requires them.

### 4.4 Futex semantics (TG-1 I3)

The kernel keeps a fixed wait table keyed by `(group_id, user_vaddr)` —
`kernel/linux_futex.c`, 16 records (one blocked descriptor per process-table
slot). A record holds the group, the user address, the waiting tid, and an
ABSOLUTE tick deadline (`0` = forever). The group id, not the tid, is the
process half of the key: two processes that map the same virtual address must
never wake each other.

`FUTEX_WAIT`/`FUTEX_WAIT_PRIVATE`:

1. validate the user span and read 32 bits through the caller's page-table
   root (`linux_user_copy_out`, the same read-only walker the fd class uses);
2. if the value differs, return `EAGAIN` before parking;
3. enqueue and mark the thread `PROC_BLOCKED` inside the SAME trap, then
   switch to the next runnable descriptor. Interrupts are off and the BSP is
   the only CPU, so no wake can run between the value check and the enqueue:
   a wake either finds the record, or the waiter's own re-check returns
   `EAGAIN`. That is the lost-wakeup argument, stated as a property of the
   one-trap window rather than of a lock;
4. a waker returns `0` in rax; the deadline tick returns `-ETIMEDOUT`.

The resume does NOT re-execute the wait. The park captures the syscall-entry
registers into `park_ctx`; the waker rebuilds the 20-word ring-3 frame from
that capture with the result in rax (the mapping `proc_gate_stop_at_sysret`
uses) and marks the descriptor `PROC_SUSPENDED`. `park_syscall` carries its own
sentinel, `SYS_LINUX_FUTEX_RESUME` ("LFXF"); nothing is forwarded and no
sidecar reply is involved.

`FUTEX_WAKE`/`FUTEX_WAKE_PRIVATE` does not dereference the word: it takes at
most the requested count of matching records, wakes the descriptors that still
exist, and returns how many resumed. A count of `0` is a no-op.

A relative timespec is converted to `now + ceil(ns / KERNEL_TICK_NS)` (round
UP, never early; clamped at a day); a zero timespec answers `ETIMEDOUT`
without parking. The timer ISR calls `linux_futex_deadline_tick()` beside
`cap_park_deadline_tick()`, before `schedule_ring3`, so a timed-out waiter is
schedulable on that tick. A waiter with no other runnable descriptor is
withdrawn and answered `EAGAIN`, the same posture `cap_wait_chan` takes for a
lone process; the live probe keeps its sibling runnable so a real block and a
real timeout both occur.

**Measured 2026-11-23.** `tests/linux_futex_probe_check.sh` boots the ordinary
ISO and the sidecar-spawned native probe's parent parks in
`FUTEX_WAIT_PRIVATE`; the kernel logs `[E7] futex wait: TID 100 group 106 ...
parked`, the sibling's wake logs `woken=1` and the parent's wait returns `0`,
a second 20 ms wait logs `[E7] futex timeout: TID 100 resumed with -ETIMEDOUT`,
and the group tears down once. `tests/linux_futex_host_test.c` pins the same
rules without a boot (op allowlist, rounding, group/address keying, count,
expiry).

v1 refuses: PI futexes, robust-list owner death, `FUTEX_REQUEUE`, process-shared
futexes, priority inheritance, `FUTEX_WAIT_BITSET` and `CLOCK_REALTIME`
timeouts. The sidecar's `set_robust_list`/`rseq` posture is unchanged: a
success return is not allowed merely because Linux returns zero.

### 4.5 Forwarding and sidecar state

**One group, one channel.** `cap_table_index(pid)` resolves a thread to its
group's cap-owner descriptor, so every thread sees the same two channel caps
and the same fd-related capability state. `cap_table_teardown()` is a group
operation, not a thread operation.

**Per-thread identity (the two resolutions I4 had to make).** A group can
have more than one RUNNING thread — the leader spins in ring 3 while a
sibling traps — so "first RUNNING descriptor" is not "the caller". The live
openat probe caught both halves:

- `process_find_current()` now matches the per-CPU syscall-stack top (written
  by `schedule_ring3` and `kernel_switch_next` on every entry) before falling
  back to the old scan, so a sibling's trap resolves to the sibling. Without
  it, the forward's `cap_maybe_handoff` read the leader's `park_syscall`
  (zero), handed the CPU away mid-forward, and the reply then had no parked
  waiter to wake;
- `chan_table_for_pid()` (kernel/chan.c) resolves a member to
  `proc_group_cap_owner(pid)` before scanning `cap_table_pid`, exactly as
  `cap_table_index` does. Without it a sibling's slot 0/1 names resolved
  against the KERNEL table — the console channels — so its openat posted to
  chan 5 instead of the group's probe channel and could never be woken.

**One in-flight forward.** Before staging the reply payload or posting,
the calling thread acquires the group lease (`linux_group_fwd_acquire`,
`kernel/linux_thread.c`). If another thread owns it, the caller records its
descriptor bit (`fwd_wait_mask`) and parks WITHOUT posting —
`park_syscall = SYS_LINUX_LEASE_RESUME` ("LLSE"), `park_req = the syscall
number`, entry registers in `park_ctx`. Nothing is owed for a waiter, so no
sidecar state names it and no reply can be applied to it.

The owner releases when the reply is applied, when the post is refused, when
the request's channel resolver fails, or when it exits while owning. The
release (`linux_forward_lease_release`) clears the lease, walks the wait mask
for the lowest slot that is still a LIVE lease-parked descriptor, grants that
descriptor the lease, and rebuilds a RESTART frame: `rax = the syscall
number`, `rip = park_ctx.rcx - 2` — the `syscall` instruction itself. Ring 3
re-executes the trap, the first trap runs again with the same registers, and
this time it owns the lease and forwards normally. That is Linux's own
syscall-restart convention (the shape it uses when restarting a syscall after
a signal), not a resumed C frame, so nothing in the dispatcher has to be
extracted into a re-entrant function. A waiter whose two bytes below the
return RIP are not `0F 05` is answered `EAGAIN` instead of being rewound.

A waiter that dies before its turn cancels its own bit at descriptor release;
the release's live-descriptor check is the second net, so the lease can never
be granted to a slot that cannot restart (a reused or dead descriptor), and a
cancelled waiter is skipped rather than wedging the group. A thread blocked
on futex does not hold the lease; threads can still make progress while one
waits on a futex.

**Measured 2026-11-23.** `tests/linux_openat_probe_check.sh` boots the
ordinary ISO and the sidecar-spawned `e7.openat.probe.0` child clones one
sibling; the LEADER never forwards. The child `openat(257, AT_FDCWD, "/",
O_RDONLY|O_DIRECTORY)`s and `close(3)`s the result, and the kernel logs
`[E7] forward lease: TID <child> group <gid> acquired for syscall 257` for the
cloned TID, the probe reports `E7OA-OPEN-OK` (fd >= 3) and `E7OA-CLOSE-OK`
(0), and the group tears down once. `tests/linux_thread_group_host_test.c`
pins the lease state machine without a boot (free/busy acquire, non-owner
release, waiter mask, cancel, dead-waiter skip, grant-while-busy).

**Request identity.** Add a `tid` field to `SLSLinuxSyscallRequest` and its
Rust twin. The kernel fills it from the calling descriptor; the sidecar uses
it for per-thread `set_tid_address`, mask updates and signal targeting. The
syscall-number echo remains; an additional tid check is not required for the
single-in-flight path, but the field is needed for per-thread state.

**Sidecar row.** A `LinuxTask` becomes a group row with:

- one `ch_r`/`ch_w` pair;
- one fd table and one `vfs_task`;
- one `brk`/`mtop` ledger;
- a small per-tid signal array (mask, pending, clear-TID address) and the
  group's handler table;
- the existing `pending` exchange, valid because only one forward is in flight.

`track()` is called once for the group leader. A clone does not call it.
`exit_group` resolves the leader by name and calls a new group-kill operation;
`exit` from a thread is a kernel-local thread teardown and does not touch the
sidecar row.

**The named v1 limitation.** A blocking sidecar exchange (console read, timed
poll) holds the group forward lease until it answers. Other threads cannot
issue forwarded syscalls during that window. The gate must not perform a
blocking read in one thread while another needs a syscall. A real Go workload
that blocks on stdin while another goroutine opens a file will need either
per-thread channels with a sidecar demux or an asynchronous sidecar exchange;
that is a follow-on, not hidden in this gate.

**Why not per-thread channels now.** It needs eight more channel pairs per
group, new channel capability creation at clone time, sidecar rows or a
demultiplexer, and a rule for which thread owns `pending`. It is a larger
security and lifetime change. The lease design is the smaller correct v1.

### 4.6 Lifecycle and teardown

- **Thread exit (`exit`, 60):** remove one descriptor only. Free its kernel
  syscall stack; its user stack is group memory and is not freed separately.
  Remove its futex waiters and signal state. Do not destroy `cr3`, the cap
  table or the sidecar row.
- **Group exit (`exit_group`, 231):** the sidecar answers `Exit`; the kernel
  kills every thread in the group, closes the sidecar row once, tears down the
  group cap table once, destroys the shared page table once, and releases the
  group object.
- **Unexpected faults:** a kernel #PF in one thread currently reaches
  `process_exit`. For a group member this must become a group-fatal path (or a
  named policy decision); it must never run the single-thread teardown.
- **Partition destroy / process kill:** `process_kill(pid)` must resolve a
  group member to the group and kill the whole group. `process_kill_partition`
  remains a list of pids; group-aware kill avoids double teardown.
- **Last-thread rule:** the group's `live_threads` reaches zero before any
  group-owned object is freed. An exiting leader does not free the group while
  a sibling is alive.

The current `process_exit` loop scans for a running descriptor and tears it
down; the group implementation needs a separate `linux_thread_exit` and a
`linux_group_exit` rather than adding branches to the existing teardown.

### 4.7 Signals: the minimum compatibility surface

The first Go gate is intentionally signal-quiet:

- `rt_sigaction` installs are accepted and recorded, but no handler is ever
  invoked;
- `rt_sigprocmask` is per-thread; the group has shared dispositions;
- `sigaltstack` keeps the current accept/query behavior;
- `tkill`/`tgkill` are not used to deliver into the gate; if Go calls them
  during initialization, the census records the call and the gate proves it is
  not required for correctness;
- no `rt_sigreturn`, no signal frames, no asynchronous preemption;
- the gate runs with `GOGC=off` and `GODEBUG=asyncpreemptoff=1`.

The `rt_sigaction` acceptance is a **compatibility prerequisite**, not proof
of signal support. A later Go workload that needs SIGURG preemption, SIGPROF
profiling or a SIGSEGV handler triggers the separate signal-frame project.
The gate's log must never say a handler ran.

The existing sidecar's signal state is per task and `tkill` only matches its
own `sig.tid`. A group needs per-tid masks and a group handler table; the
single `SigState.tid` is not enough. The kernel's `SYS_LINUX_TRAP_RESUME`
reply path already preserves the six argument registers, so a per-tid request
field is the main protocol change.

### 4.8 Checkpoint and restore (TG-2, proposed)

This is the largest unproven part. The current payload is deliberately
single-Linux-task: one `EnvPayloadTask`, one context, one page walk, one held
child. A thread group needs all of the following before the gate can claim
E7 parity:

1. **Schema.** Bump the checkpoint/payload format. `ENV_CKPT_MAX_TASKS` rises
   to at least 8. A Linux task record gains a group id, a thread index, a
   leader flag and a per-thread tid slot. The name-only lookup cannot identify
   two threads of the same group; lookup must use `(group name, thread index)`.
2. **Page ownership.** The group leader owns the shared image and heap page
   blocks; non-leader threads own only pages unique to them. The current
   `ep_linux_page()` predicate gives every thread the same heap range and would
   duplicate entries. The page index needs a group-level dedup rule, and the
   restore must reject two entries for one vaddr with different CRCs or
   permissions.
3. **Per-thread context.** Each thread needs its own `linux_regs`, `fs_base`,
   user stack top, trap form and signal mask. The leader's heap/brk/mmap/image
   fields live once, not once per thread.
4. **Group stop.** The existing gate stops one task at a sysret. A multi-thread
   group must be quiesced consistently. The preferred first form is a
   **futex barrier**: every gate thread waits on a known futex, and the
   payload carries a new `FORM_FUTEX` wait record (address, expected value,
   timeout, saved entry context). The restore re-enqueues those waits before
   releasing the group. If a thread is in a forwarded syscall, the checkpoint
   still refuses it by name; the gate must not create that state.
5. **Replay create.** The current restore creates one held child. A group
   replay creates the leader, creates siblings sharing the same root, pours
   shared pages once, installs every thread's context, then releases all of
   them. The sidecar's group row is restored by its own payload; no extra
   `track()` calls per thread.
6. **No double ownership.** One `cap_table_teardown`, one
   `user_destroy_page_table`, one sidecar row close for the group. The payload
   restore's page mapping must target the shared root, not a fresh per-thread
   root.

TG-2's gate must show at least two threads before and after the cycle, one
shared write visible across the cycle, a completed copy/checksum, and exactly
one group teardown. If the group-stop protocol cannot be made deterministic,
the correct outcome is a named refusal, not a checkpoint of some threads and a
restart of the rest.

### 4.9 Scheduling and limits

Threads are scheduled as individual descriptors in v1. The existing partition
round-robin treats every process as one share, so an N-thread group can
consume N shares. That is acceptable for the gate (two threads) but must be
named: a group-level CPU weight/fairness policy is not part of TG-1.

`LINUX_THREADS_MAX` should start at 8 and be enforced at clone. The gate uses
two or three threads. `PROC_MAX` is 16; the gate's environment uses two
sidecars plus the group, so a 4-thread gate needs seven slots and leaves
headroom. If the limit is hit, clone returns `EAGAIN`; the sidecar and kernel
must agree on the refusal.

Each thread has its own kernel syscall stack (two frames, charged to the
partition). Group leaf frames are charged once to the group's partition; a
thread exit must not return shared frames.

---

## 5. Syscall and table changes

### 5.1 Classes

The existing table needs these rows and class assignments for TG-1:

- `LSC_THREAD` — `clone` (56), kernel-local;
- `LSC_FUTEX` — `futex` (202), kernel-local;
- `LSC_TASK` — `gettid` (186), `getpid` (39), `exit` (60),
  kernel/group-local;
- `LSC_FD` — `openat` (257), forwarded; other measured file calls
  (`copy_file_range`, `pread64`) remain optional/tolerated and may stay
  `LSC_UNKNOWN` unless a later gate promotes them;
- `LSC_SIG` — existing 13/14/131; no new signal rows for TG-1;
- `LSC_EXIT` — existing 231; group reap, no new row for `exit_group`.

`set_tid_address` (218) already has an `LSC_LOCAL` row; it is not a TG-1
table gap (per-thread bookkeeping, if a later Go version needs it, is sidecar
work). `sched_getaffinity` (204) remains optional/tolerated and may stay
`LSC_UNKNOWN` unless a later gate promotes it.

**Netpoll startup group (measurement-time requirement).** At the 2026-11-09
measurement the four calls `eventfd2` (290), `epoll_create1` (291),
`epoll_ctl` (233) and `epoll_pwait` (281) were hard requirements even without
`net`, and the table had none of them. The current tree classes all four and
answers the zero-timeout startup probe; a blocking `epoll_pwait` remains a
named decision. See §7. This is separate from the thread-group gate, but it
is a prerequisite for the live fixture.

The first-trap branch must check the class before forwarding. A kernel-local
class never builds a `SLSLinuxSyscallRequest`; it is permitted only for a
Linux personality and only against the caller's own group. The number-space
rule remains: a number absent from the table is `LSC_UNKNOWN` and refused by
name, never interpreted by the SLS switch.

### 5.2 The census artifact

TG-0 produces a separate artifact, not an edit to the Rust census:

```text
tools/go_thread_gate_census.txt
```

with:

- Go version and toolchain sha256;
- gate binary sha256;
- `strace -f` command;
- each syscall number/name, count, and whether it was required or tolerated;
- the exact gate run (two threads, futex, copy, exit);
- a vDSO/no-vDSO note, because this kernel ships no vDSO.

`tests/linux_syscall_table_host_test.c` (or a sibling) must read the new
artifact and fail on any required/startup exercised number with no class, any
`LSC_UNKNOWN` outside the explicitly tolerated set, and any table row whose
class is not handled by the dispatcher. A missing `/sys` openat from Go's
huge-page probe can be a named tolerance; a missing `clone` cannot.

### 5.3 Pointer and identity rules

- `clone` stack and all user pointers are canonical and read/written through
  the group's page table;
- futex reads are read-only user copies; a missing page is `EFAULT`, never a
  kernel fault or an allocation;
- `tid` is the descriptor pid; `tgid` is the leader pid; the group id is
  boot-local and is not a checkpoint identity;
- `gettid`/`getpid` cannot be confused with the SLS pid namespace; the
  personality check comes first;
- a clone is only allowed for a Linux group leader, never for the sidecar or
  an SLS process;
- a group's cap table cannot be reached by another group through a tid.

---

## 6. Verification plan

### 6.1 TG-0 (host only)

**Deliverables:** pinned Go fixture; `go_thread_gate_census.txt`; the
compatibility probe; table entries and source guard.

**Measured host result (2026-11-09):** the fixture built static, produced two
distinct TIDs, completed a real futex wait/wake, and copied 64 KiB
byte-identically. The full classified set is §3.5. The compatibility path has
since landed in the tree (§7); the host checks below are the planned pins.

**Checks:**

- the fixture builds static, no cgo, with the pinned toolchain;
- `strace -f` shows at least two `clone` calls with `CLONE_VM|CLONE_THREAD`
  and the expected TLS flag;
- two distinct `gettid` values are observed;
- a futex wait/wake pair is observed;
- the `rt_sigaction` handler-install path returns success under the
  record-only arm; the TG-1 plan pins this with a host test;
- every required/startup exercised syscall has a table class; optional/
  tolerated calls may remain `LSC_UNKNOWN` and return the named `ENOSYS`;
- the source guard reddens if `clone`, `futex`, `gettid`, `openat` or `exit`
  is removed from the relevant table/path.

No QEMU boot is required for TG-0, but the gate's binary and command line are
recorded.

### 6.2 TG-1 (live substrate)

**Gate clauses:**

- **G1.1** the static Go gate prints a stable start marker before any work;
- **G1.2** at least two distinct TIDs are printed and the kernel log shows at
  least two thread creations under one tgid;
- **G1.3** a value written by one thread is visible to the other through the
  shared address space;
- **G1.4** a direct futex wait and wake both occur; the waiter returns 0 and
  the waker gets 1;
- **G1.5** the deterministic file/console copy produces the expected byte
  count and checksum;
- **G1.6** no `[FAULT]`, no unexpected `ENOSYS` in the required set, and no
  `LSC_UNKNOWN` outside the census's named optional/tolerated set;
- **G1.7** `exit_group(0)` reaps the whole group; exactly one address-space
  teardown and one sidecar row close occur;
- **G1.8** the firstuser's existing single-threaded gate still passes; the
  thread-group change must be additive.

**Failure teeth** (each must redden by name):

| Break | Expected red |
|---|---|
| remove `CLONE_VM` from the accepted set | shared write is lost or clone returns `EINVAL` |
| make the clone child use a copied page table | visibility clause G1.3 fails |
| remove `FUTEX_WAKE` | G1.4 times out |
| make `futex` use a per-thread key | wake misses the waiter |
| make each thread keep its own `brk`/`mmap_top` | overlapping allocation or G1.5 corruption |
| remove the group forward lease | reply mismatch / G1.6 |
| free the page table on a sibling exit | G1.7 double teardown or `[FAULT]` |
| refuse `rt_sigaction` handler installs | Go never reaches `main` |
| remove `openat` | G1.5 cannot open its input |

### 6.3 TG-2 (checkpoint parity)

The existing `tests/e7_gate_check.sh` remains the model, but a thread-group
gate needs its own clauses:

- **G2.1** every thread's tid/context is captured; the group identity and
  leader are recorded;
- **G2.2** shared pages are captured once and restored into one root;
- **G2.3** each thread's `fs_base`, stack and register context is restored;
- **G2.4** a futex-barrier wait is restored as a wait, not as a busy loop or
  a silent return;
- **G2.5** a write from one thread before the checkpoint is visible to a
  sibling after restore;
- **G2.6** the copy completes with the same checksum after the reboot;
- **G2.7** exactly one group teardown and one frame return count;
- **G2.8** the existing single-task E7 gate replay remains green.

Teeth: drop a thread from the record; duplicate a shared page with a different
CRC; restore a thread with the leader's `fs_base`; restore a per-thread root;
skip the futex re-enqueue; run the old one-task harness against a group. Each
must fail by the clause it names.

### 6.4 Test files

I1–I5 named these; the three live probes are the shipped runtime guards.

- `tests/linux_thread_group_host_test.c` — group ownership, limits, tid/tgid,
  teardown counts, and I4's forward-lease state machine;
- `tests/linux_futex_host_test.c` — pure wait/wake/timeout state, no
  privileged instructions (landed with I3);
- `tests/linux_clone_host_test.c` — flag validation and child-context build;
- `tests/linux_thread_probe_check.sh` — I2's native two-thread live probe;
- `tests/linux_futex_probe_check.sh` — I3's native futex live probe;
- `tests/linux_openat_probe_check.sh` — I4's native sibling-forward openat
  live probe (lease-acquire line for the child's TID, fd, close, teardown);
- `tests/thread_group_check.sh` — source guard for the contracts above
  (group ownership, clone shape, table rows, openat path, identity, lease,
  one teardown; 47 clauses);
- `tests/thread_group_check_smoke.sh` — its mutation teeth: each load-bearing
  field, flag, row, path and lease transition is removed on a seeded copy and
  must make the guard fail by clause name (13 teeth);
- `tests/go_thread_gate_census_check.sh` — TG-0 artifact and table check;
- `tests/go_thread_gate_check.sh` — TG-1 live gate, including teeth;
- `tests/thread_group_ckpt_check.sh` — TG-2, reusing the E7 gate harness shape.

The source guard is not a substitute for the live gate. The live gate is not a
substitute for the host teeth. This is the same two-half discipline E7
already uses.

---

## 7. Implementation order

### TG-0 — measured 2026-11-09, counts regenerated 2026-11-22; compatibility in tree

The fixture is `tools/go_thread_gate/` and the artifact is
`tools/go_thread_gate_census.txt`, regenerated by
`tools/go_thread_gate_counts_census.sh`; the pinned toolchain is Go 1.25.1.
The committed artifact is the 2026-11-22 regeneration, not the 2026-11-09
run: the counts are run-sensitive, while the measured row set is unchanged.
The generator verifies the pinned tarball, pins the fixture source and binary
hashes, traces one full run under the gate's
`GOGC=off`/`asyncpreemptoff` environment, and refuses a measured syscall
outside its classification table or a classified number that never appears.
The build is static `ET_EXEC`, the two TIDs are distinct, the futex handshake
returned `wait=0`/`wake=1`, and the 64 KiB copy is byte-identical. The
measured set is §3.5.

TG-0b's argument-level record is `tools/go_thread_gate_args_census.txt`,
generated by `tools/go_thread_gate_census.sh` and guarded by
`tests/go_thread_gate_args_check.sh`. The generator verifies the pinned Go
1.25.1 tarball SHA-256 and pins the fixture source and binary hashes. The
artifact records the measured argument
shapes: Go's clone flags, only `FUTEX_WAIT_PRIVATE`/`FUTEX_WAKE_PRIVATE`,
zero-timeout `epoll_pwait` calls with a NULL sigmask, `AT_FDCWD` openat
calls for the input/output, and the handler-install action flags. Counts and
addresses vary by run; the argument shape is the contract.

The compatibility half is already in the tree:

1. **Record-only `rt_sigaction` exists.** The sidecar's 13 arm stores the
   32-byte handler installation, returns the old action when asked, and never
   invokes the handler (`user/sidecar/src/linux.rs`). The kernel already
   attaches and returns the 32-byte action. The host run proves Go's
   requirement; the TG-1 plan adds a host test pinning the shim's record-only
   answer.
2. **The four netpoll startup arms exist.** `kernel/linux_abi.c` classes
   `epoll_ctl` (233), `epoll_pwait` (281), `eventfd2` (290) and
   `epoll_create1` (291); `answer_event()` in the sidecar creates/records the
   eventfd and epoll instance and answers the measured zero-timeout
   `epoll_pwait` probe. A blocking `epoll_pwait` remains a named decision; the
   current arm does not claim to serve one.

The table gaps before a live Go start were six. I2 closed four: **`clone`
(56)** (kernel-local thread creation), **`gettid` (186)** (per-thread
identity), **`getpid` (39)** (group identity; it was not in the TG-0 trace and
was added with the group identity surface) and **`exit` (60)** (thread-local
teardown). I3 closed **`futex` (202)** (private wait/wake with a relative
timeout). I4 closed **`openat` (257)** (the measured path open: pathname in
args[1], `AT_FDCWD` only, served by the same VFS open arm; any other dirfd is
refused by name). None of the six TG-1 table gaps remains.

The other required/startup rows already have a class. `copy_file_range`,
`pread64`, `sched_yield`, `madvise`, `nanosleep`, `uname`, `prctl`,
`sched_getaffinity` and `prlimit64` remain optional/tolerated; they may remain
`LSC_UNKNOWN` and return the named `ENOSYS`, and they are not TG-1 table gaps.

The concrete TG-1 implementation plan is the companion **AeroSLS E7 TG-1
Implementation Plan** (2026-11-21; Freebuff doc artifact
`01a2001f-1640-7ae2-838b-508cbf354768`), sections 2–6. It defines the
smallest group/clone/futex slice, the kernel and sidecar changes, the host
and live tests, and the I0–I5 order.

Exit: the six gaps have implementations (or a named refusal), the source
guard covers them, and the existing single-thread E7 gate remains green.
TG-0 itself does not claim a live Go start; that is TG-1's G1.1–G1.8.

### TG-1 — group, clone, futex (kernel + sidecar + host tests)

The detailed plan is the companion **AeroSLS E7 TG-1 Implementation Plan**
(2026-11-21; artifact `01a2001f-1640-7ae2-838b-508cbf354768`). This section
is the order; the plan owns the exact data structures, tests and teeth.

1. Add the group object, tid/tgid, group-aware cap lookup and group lifecycle;
   keep `fs_base` and trap state per thread. **Landed: I1.**
2. Add kernel-local `clone` with the flags and child context in §4.3.
   **Landed: I2.**
3. Add the futex wait table and resume arm. **Landed: I3.**
4. Move the memory cursors and sidecar fd/heap state to group ownership.
   I1 moved the cursors; the sidecar row's per-thread identity is I4's lease
   work.
5. Add the forward lease and per-thread `tid` request field. **Landed: I4.**
6. Add `gettid`, `getpid`, `exit` and the measured `openat` path. The three
   task calls landed with I2; `openat` landed with I4.
7. **Landed: I5.** Host tests, the source guard and its 13 mutation teeth;
   the native I2/I3/I4 probes; the E7 gate's `--live` (B1–B7, D1–D2) and
   `--teeth` (control green, four breaks red by name); and the E3
   multi-instance boot regression, whose storage evidence now rides the
   serial wire and whose image no longer runs the E7 census. The Go gate
   (G1.1–G1.8) is the remaining TG-1 acceptance run.

**Measured 2026-11-23 (I5); E3 determinism rerun 2026-11-24:**
`thread_group_check.sh` passes its 47 clauses with 0 failures and the smoke
bites all 13 mutations. `e7_gate_check.sh --live` is green (B1–B7, D1–D2);
`--teeth` leaves the control green and reddens exactly the declared clauses
(descriptor B2/B3/B4, no-fd B3/B4, no-heap B4/B5, fs_base B2/B4/B5).
`e3_multi_env_boot_check.sh` is green on the FIRST 300 s TCG boot after the
fix (distinct tenant regions 0x12469000 and 0x12b24000, three live rootfs
lines, no serial loss, zero E7 probe lines), and its smoke passes 10/10.

The storage evidence now rides init's `SYS_SLS_SERIAL_WRITE` path as
`[E3-ENV] env N: ... storage @ 0x...`, the same drop-proof wire as
`[POSIX] aero state=`; the `[INIT] E3 env ...` console copy remains for human
reading, and the check no longer parses it. The E3 image's system POSIX also
carries a new `FLAG_E3_PROFILE` manifest bit (copied to the BIB as
`BOOT_INFO_FLAG_E3_PROFILE`), which makes it skip the E7 native-probe and
first-user census block; that block still runs on the plain image. The
300 s window remains for TCG, but the gate now opens as soon as the evidence
arrives (the rerun returned inside 75 s), and lost evidence still fails by
name. The I2/I3/I4 native probes remain green from their increments, and
their plain-image boot is unchanged.

Exit: G1.1–G1.8 pass on a live boot, with the existing E7 gate still green.

### TG-2 — checkpoint

1. Bump the record/payload format and task limits.
2. Add group stamping, shared-page ownership, per-thread contexts and the
   futex-wait form.
3. Add group-aware replay create, hold and release.
4. Add the group gate harness and teeth.

Exit: G2.1–G2.8 pass after a real reboot.

### TG-3 — hardening after the gate

Signal frames and handler delivery; robust futex; `FUTEX_WAIT_BITSET` if
measured; per-thread channels for blocking I/O; group scheduling fairness;
SMP TLB shootdown; migration/checkpoint of groups at scale. None of these is
required to call the first two-thread gate real, but each is required before
a general Go runtime claim.

---

## 8. Out of scope

- `clone3`, `fork`, `vfork`, namespaces, cgroups, PID namespaces;
- process-shared futexes, PI futexes, robust-list owner death, requeue;
- full signal delivery, signal frames, `rt_sigreturn`, async preemption;
- per-thread sidecar channels (deferred; the lease is v1);
- group scheduling fairness;
- checkpoint/migration of a group before TG-2;
- dynamic linking, cgo, networking, `os/exec`;
- ARM64 userspace (H2 item #1, not this gate);
- full Go standard library support. The gate uses `runtime`, `syscall`,
  `unsafe`, `os`/`io` for the copy, and no signals.

---

## 9. Risks and open questions

1. **Go startup vs. signal handlers.** The record-only path exists: handler
   installations are accepted and stored, and handlers are never invoked, so
   Go's `setsig` sees success. See §7 for the current status. Record-only is
   not signal delivery; Go is measured with GC and async preemption disabled,
   and if it needs a real signal frame, TG-1 is blocked.
2. **Forward lease starvation.** A blocking sidecar read holds the lease and
   stops other threads' syscalls. The gate avoids blocking reads; a real Go
   workload may not. Per-thread channels are the follow-on.
3. **Checkpoint stop.** A group can be caught with one thread in a futex wait,
   one in a forwarded syscall and one in user code. The first gate must use
   a deterministic group barrier; arbitrary stop-state capture is not claimed.
4. **Page duplication.** The existing per-task page predicate gives every
   thread the same heap range. TG-2 must assign shared pages to one owner and
   reject conflicts; otherwise the blob doubles and the restore can pour the
   same page twice.
5. **TID and tgid identity.** `tid` = descriptor pid is boot-local; it cannot
   be checkpoint identity. The group record needs a stable thread index and
   leader flag; the sidecar's name-only lookup cannot distinguish siblings.
6. **Frame accounting.** Group pages and kernel stacks are charged to a
   partition. A sibling exit must not free group frames; the teardown counts
   must be asserted.
7. **Go version drift and netpoll.** Clone flags, `openat`,
   `sched_getaffinity`, `madvise`, `fcntl` and signal setup are
   runtime-version facts; the measured `epoll_*`/`eventfd2` group is the one
   the design did not predict. The fixture pins the toolchain and re-runs
   TG-0 on upgrade.
8. **`tkill` table mismatch.** The sidecar has a 200 arm but the current
   census table has no 200 row. The Go census must make the table and the
   dispatch agree; do not build the gate on an unreachable arm.
9. **`PROC_MAX`.** Two sidecars plus a group plus eight threads leaves little
   room. The gate must declare its thread cap and a second environment's
   budget; `EAGAIN` is preferable to a partial clone.
10. **`set_tid_address`/clear-TID.** Go's current clone path does not request
    `CLONE_CHILD_CLEARTID`; a future runtime or library may. Decide from the
    measured runtime, not from the Rust census.

---

## Appendix A — Source map

| Concern | Current source |
|---|---|
| Descriptor, cr3, fs_base, Linux memory fields | `kernel/process.h` |
| Process exit/kill/deferred teardown, scheduler switch-in | `kernel/process.c` |
| Fresh page table and owner teardown | `arch/x86/user_paging.{h,c}` |
| One channel pair rule, cap table per pid | `kernel/cap.c`, `kernel/cap.h` |
| Post/park/reply and in-flight invariant | `kernel/chan.c`, `kernel/linux_abi.h` |
| Linux trap branch and memory apply | `kernel/syscall_dispatch.c`, `kernel/linux_abi.c` |
| Sidecar task row, fd/heap/signal/pending state | `user/sidecar/src/linux.rs` |
| Checkpoint task slots and payload task | `kernel/env_ckpt.h`, `kernel/env_payload.{h,c}` |
| Rust census and table | `tools/linux_syscall_census.txt`, `kernel/linux_abi.c` |
| Go thread-gate fixture, counts census and its generator | `tools/go_thread_gate/`, `tools/go_thread_gate_census.txt`, `tools/go_thread_gate_counts_census.sh` |
| E7 gate and its teeth | `tests/e7_gate_check.sh`, `tests/e7_gate_evidence_check.sh` |
| Go runtime contract (external) | Go 1.25.1 `src/runtime/os_linux.go`, `src/runtime/sys_linux_amd64.s` |

## Appendix B — The measured fixture

The fixture is `tools/go_thread_gate/main.go`, not a sketch. It locks two
OS threads, uses a shared `uint32` and direct futex wait/wake, then copies
the input to the output with `os.Open`/`os.Create`/`io.Copy`; `io.Copy`
chose `copy_file_range`, with the `read`/`write` fallback observed under
fault injection. The measured result is §3.5, and the artifact header records
the exact build and trace commands.

The next step is not another sketch: the record-only `rt_sigaction` path, the
four netpoll startup arms and the clone/thread-group/private-futex/openat
slice are in the tree, with three native probes green. No TG-1 table gap §7
lists remains; the forward lease is what turns a cloned sibling's forwarded
syscall into a correct wake. I5's source guard and mutation teeth hold the
seams, and the E7 gate's live/teeth and E3 regressions are green; the live Go
gate (G1.1–G1.8) is the remaining acceptance run.
