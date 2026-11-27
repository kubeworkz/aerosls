# AeroSLS P1 Remediation Plan v0.1 (draft)

Turns the five P1 audit findings into sequenced, gate-backed work. Every phase
below follows the shape this repo already uses for the E7 line: **one kernel
change + one sidecar change + a host test + a guard clause with teeth + one
live arm + a CI step + a doc update**, and every kernel/sidecar change also
forces a `sidecars.cpio` re-pack and an ISO boot (the archive is a tracked
artifact; see `git log` for what that costs).

Scope note: the audit's P0 list is not in this tree, so this document takes
"P0 complete" as the given and records below the landed state it can verify
from the index, guards and this session's runs.

---

## 0. Where the code actually stands (verified, not assumed)

| Claim | Evidence in the tree |
| --- | --- |
| The E7 thread-group line is landed and guarded | `docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md` status table (all LANDED through TG-2d); `tests/thread_group_ckpt_check.sh` T1–T7 pass; `tests/e7_gate_check.sh` G1–G7 pass |
| The two-thread replay runs on every push | `.github/workflows/ci.yml` `kernel-guards` step "TG-2d group checkpoint live arm"; pinned by guard clause T7 |
| The shipped archive matches `user/` and the ISO boots | `sidecars_stamp_check` full sweep green; `make x86-iso` → ISO service smoke `status=ok uptime_ticks=458`; Phase 5 initrd boot `ALL PHASES PASS` + shell prompt |

What is *not* verifiable from the repo: the audit's own P0 numbering. If your
P0 list has an item this branch never touched, it is invisible here.

### The five P1 lines, located in source

| P1 | Where it lives today |
| --- | --- |
| No async preemption, siginfo queueing, fpstate, or signals past `tkill` | **Partly closed** — increments 26–29c landed real delivery: `linux_sig_frame_build`/`linux_sig_frame_restore` and the park bridge (`kernel/linux_abi.c`), `linux_sig_reply_apply`/`linux_sig_return_apply` (`kernel/process.c`), and the sidecar's `Answer::SignalFrame` arm with `sig.handler_active`/`sig.saved_mask` (`linux.rs`), so `tkill` enters a ring-3 handler that returns through `rt_sigreturn`, proven on a boot by `tests/linux_signal_probe_check.sh`. **Increment 31 (shim doc §10.26) added the fault path's own delivery**: a ring-3 #PF with a handler builds a frame with `si_addr` from CR2 and enters it, so the trap path is no longer kill-only. **Increment 32 (§10.27) gave that handler a return**: `rt_sigreturn` from a fault-delivered frame restores the interrupted context through the kernel's own gate (`linux_sig_fault_return_ok`, which takes over exactly the sidecar's truthful `-EINVAL` for a delivery it never made), so a handler that fixes its fault — rewriting the faulted register in its ucontext — resumes the faulting instruction. **Increment 33 (§10.28) ended the mask split that 32 named**: for the duration of a trap-delivered handler the kernel answers `rt_sigprocmask` itself out of `linux_sig_mask` — a delivery that had no round trip can never be reported to the sidecar — so a query reads the handler's set and BLOCK/UNBLOCK use it as their base, with the tkill path untouched. **Increment 34 (§10.29) gave that window the reveal an unmask owes**: a call that frees a blocked bit forwards a tagged control probe, so the sidecar's pending set and dispositions decide — inside the handler — whether the signal is discarded, terminates the task, or is refused, while the kernel keeps the mask and commits it only on the sidecar's consent; every 14 also stages its new mask, so the kernel's copy follows the thread's own blocks and a signal blocked before a fault is in the frame's mask at all. **Increment 35 (§10.30) closed the window's other exit**: a handler that leaves by `longjmp` never sends the 15 that closed it, so a 14 made with `rsp` above the recorded frame closes it instead — the mask goes back to the frame's own `uc_sigmask` (the number the sidecar still holds, since it never moved during the window) and the frame is dropped; a tkill-delivered frame is out of that branch because `linux_sig_reply_apply` clears `fault_active` for it (its address *is* recorded — both paths store `frame_rsp`), and the predicate's one false positive is named in §10.30: a handler running on a stack above its frame (`makecontext`/`swapcontext`) reads as an abandonment and would close the window early. Its live arm (`e7.fault.abandon.0`, `tests/linux_fault_abandon_check.sh`) blocks SIGTERM before the fault (the frame records 0x4000), blocks SIGUSR1 inside the window and leaves by popping rsp above the frame: the first 14 afterwards must log `fault handler abandoned the frame … mask window closed at 4000` and answer 0x4000, and the following 15 must be refused. Still open: **no async preemption** (a signal is delivered only on the reply to a syscall the task itself made — `tkill` — or on the 14-unblock reveal; a task spinning in ring 3 is never signalled); **no siginfo queueing** (`SigState.pending` is a `u64` bitmask, so real-time signals collapse to one bit and `sigqueueinfo` is absent — **self-signals only via `tkill`**, with no `kill`/`tgkill` row); **no fpstate restore** (the frame's fpstate pointer is NULL, so a handler that clobbers xmm leaves it clobbered); and no restart window inside the restorer |
| General thread semantics | **Mostly closed by increments 30/30b** — `CLONE_CHILD_CLEARTID` is in `LINUX_CLONE_ALLOWED` (`kernel/linux_thread.h:75`) and honoured at clone (`kernel/linux_thread.c:79`); `set_tid_address` (218) is not inert: the sidecar answers the tid echo and the kernel captures the word at the first trap and clears it at exit (`kernel/linux_abi.c:167`); the glue is consumed once per death (`linux_thread_take_exit_words`), so exit(60), `exit_group`, a sibling's fault and the deferred-kill sweep all clear. Still open: the parent/child `*SETTID` flags, `list_op_pending`, and `clone3` (435) |
| Per-thread blocking I/O | `kernel/chan.c` `k_linux_forward`: post-then-park on **the group's** channel pair; the single-writer invariant in its comment *is* the lease ("this channel's ONLY writer is the one task forwarding on it, and that task is parked between post and reply"). `LinuxTask` carries one `ch_r`/`ch_w`, `MAX_LINUX_TASKS = 8` |
| Static binaries only | `kernel/elf64_load.c` refuses `ET_DYN` (PIE) and every non-`ET_EXEC` by name, maps only `PT_LOAD`, and never reads `PT_INTERP` — an interpreter request is simply not honoured, the image loads and jumps to its own entry. The refusal is structural in the guest loader; `tests/no_hosted_link_check.sh` pins the separate property that the *kernel* image carries no `.dynamic`/`.interp`/`PT_INTERP` |
| No general process lifecycle | `execve` (59) is classed `LSC_SPAWN` but only the kernel/init spawn path uses it; no clone-without-VM, no fork, no `wait4` row in the census table |

---

## 1. What "done" means for P1

Each line gets **one guest program that fails today and passes at the end** —
not a claim in a doc. These are the acceptance fixtures:

| P1 | Acceptance program (static musl unless stated) |
| --- | --- |
| #1 signals | installs a `SIGSEGV` handler, faults, handler `longjmp`s, exits 0; a second run installs `SIGUSR1` and raises it; `abort()` still dies with the attributed kill. **Go**: heap growth with the default `GOGC` and `GODEBUG=asyncpreemptoff=0` completes GC cycles inside a tight loop that calls no functions |
| #2 threads | `pthread_create`/`join`/`detach` × 8, per-thread TLS and `errno` isolation, one thread exits while another runs; a raw `clone` with `CLONE_CHILD_CLEARTID` clears its TID word and wakes the joiner — **this half already passes** (`tests/linux_thread_death_check.sh`, increments 30/30b), the pthreads half does not |
| #3 per-thread I/O | two threads: one blocks in `read` on an empty descriptor, the sibling forwards a syscall and exits; the sibling's completion latency must be independent of the blocked thread's wait (measured, not asserted qualitatively) |
| #4 static only | *product decision* — see §7. If taken as "support dynamic", the fixture is a dynamically linked `hello` under `ld.so` |
| #5 lifecycle | `fork` + `execve` of a second stage, `wait4` observes exit code 7, `SIGCHLD` handler reaps; `system("true")`-shaped path works |

---

## 2. The sequencing rule

Two constraints dominate, and they are not chronological preference:

1. **Signal frames are the deepest change** (user-stack layout, return-to-user
   path, FPU state, `FSBASE` restoration rules). They are also what Go's
   *default* runtime needs. Highest risk × highest payoff → start first.
2. **Per-thread forwarding is orthogonal** (channel topology + park state, no
   shared file with the frame builder) → runs in parallel with (1).

Everything else waits on those two: async preemption needs frames; `wait4`
needs `SIGCHLD`; general pthreads need both `clone3`/`CLEARTID` and per-thread
channels.

```
P1-C signals ─────────► P1-D async preemption ──► Go GC/preempt
    (frames,             (timer delivery)
     rt_sigreturn)
                              ▲
P1-A per-thread I/O ──────────┘   (delivery to a *blocked* thread)
        │
P1-B thread semantics ───────► P1-E process lifecycle
   (CLEARTID, clone3)            (fork/execve/wait4/SIGCHLD)
                                      ▲
                                      └── needs P1-C for SIGCHLD

P1-F dynamic linking: independent, expensive, decision-gated (§7)
```

---

## P1-C — Signal frames and `rt_sigreturn` (audit #1, synchronous half)

**Retires:** "handlers are never invoked". **Size:** 3–4 increments. **Started 2026-11-25: increment 26 landed** (commit `ffcb2d5`) — the rt_sigframe builder, its restore, the default-action table, the delivery predicate and the EINTR rule, with a 64-check host test, guard clauses P–P4 and four new teeth; the layout is pinned by twelve `_Static_asserts`. **Increment 27 landed** (commit `4d552dd`): the delivery's mechanism — the 80-byte request decoder, `si_addr` from CR2/RIP per fault class, `linux_sig_deliver` writing the frame through the validated copy, and the sidecar's `-EINTR` park wake on `CAP_EINTR (-33)`. **Increment 28 landed** (commit `0225df4`): the wire is connected — `LINUX_STAGE_SIGFRAME` in the plan, exactly-80 in the shape rule, chan.c routing to `linux_sig_reply_apply`, the sidecar answering `Answer::SignalFrame` with its mask moved first, and `cap_resume_linux_sig` entering the handler from the park context; the boot after it entered a handler in ring 3 (`SIGHAND-OK` written by the handler's own bytes). **Increment 29 landed** (census + return trip): `tools/linux_rt_sigreturn_census.sh` traces a handler that returns and writes `tools/linux_rt_sigreturn_census.txt` (`15 rt_sigreturn 1`, same provenance columns as the candidate artifact), so the table row exists by measurement; `uc_sigmask` now carries the PRE-handler mask (the frame had stored the handler's — the defect that would leave a signal blocked forever); `linux_sig_return_apply` reads the frame at the handler's own `rsp` through `linux_user_copy_out`, restores registers + mask, clears the active flag and resumes; the sidecar's 15 arm puts back its `saved_mask` and refuses a 15 with no handler; RAX became park word 15 so the resume stops forcing 0. Guard P2/P4 rewritten from absence to presence, P13–P15 added, teeth t154–t157, host tests 65 + 45 checks, sidecar 78 tests, and a boot shows `[E7] rt_sigreturn: restored rip 400000000415 rsp 400000004e60 rax 0 mask 0` followed by the probe's `SIGRET-OK` — the handler RETURNED into the interrupted context. **Named remainders:** the synthetic restorer (not the guest's trampoline), `ucontext` fields beyond `uc_sigmask`, restartable delivery inside the restorer window, `linux_cr2` left uncaptured, and the `rt_sigprocmask` reveal path still refusing.

The seam is no longer a stub: `rt_sigaction` records the act (`SigState.act`,
returned verbatim as `oldact`), `rt_sigprocmask` keeps a real mask with pending
bits, and `tkill`'s answer can BE a delivery — for a handler disposition the
kernel stages an 80-byte act request (`kernel/linux_abi.c:973`) and the frame
runs in ring 3 (increment 28). Default actions still terminate through one
attributed `Answer::SignalKill`. What is open is listed in the work below.

Work already spent (items 1–3 of the original plan): the frame builder
(`linux_sig_frame_build`, `kernel/linux_abi.c:1805`), the delivery through the
park bridge (`cap_resume_linux_sig`, `kernel/process.c:3204`), `rt_sigreturn`
with its kernel row (`kernel/linux_abi.c:148`) and the interruptible park
(`CAP_EINTR`, `kernel/cap.h`). What remains:

1. ~~**`SIGSEGV` intersects the trap path.**~~ **Landed: increment 31**
   (shim doc §10.26). `handle_page_fault` captures `pd->linux_cr2` and asks
   `linux_fault_sig_plan`; a handler plus a restorer is a frame built by the
   same builder every other delivery uses, the trap frame is rewritten and the
   stub's `iretq` enters the handler, while a fault with no handler still dies
   by the attributed kill. The    kernel keeps its own copy of the four fault
   signals' acts, recorded at `rt_sigaction`'s first trap. **Increment 32**
   (§10.27) landed the return that 31 named as its remainder: a fault-delivered
   handler's `rt_sigreturn` restores the interrupted context, so a handler can
   fix the fault and the faulting instruction resumes. **Increment 33**
   (§10.28) closed the mask split 32 named: inside a trap-delivered handler
   the kernel answers `rt_sigprocmask` itself out of `linux_sig_mask`
   (`linux_sig_mask_fault_owned`/`linux_sig_mask_step`), because a trap has no
   round trip to carry the mask to the sidecar — so the query reads the set
   the frame installed and BLOCK/UNBLOCK compute on that base, while the tkill
   path (whose mask the sidecar moved before answering) keeps its forward.
   **Increment 34** (§10.29) closed the second half of that window: an unmask
   there now forwards a tagged control probe so the sidecar's own reveal
   ladder runs over its pending set inside the call (a pending signal is
   discarded, terminated or refused then and there), while a tkill-delivered
   handler's unmask is untouched; every 14 also stages its new mask so the
   kernel's copy finally follows the thread's own blocks, which is what a
   fault frame needs. Named remainder: a nested delivery still nests two
   frames.
2. **siginfo queueing and the other producers.** `SigState.pending` is a `u64`
   bitmask, so real-time signals collapse to one bit, there is no `sigqueueinfo`,
   and `tkill` is the only producer — no `kill`/`tgkill` rows. A real queue lands
   with them.
3. **fpstate.** The frame's fpstate pointer is NULL, so a handler that clobbers
   xmm leaves it clobbered.
4. **The restorer window.** A signal arriving inside the restorer, and the
   `rt_sigprocmask` reveal path, which still refuses.
5. **altstack and the wider `ucontext`.** `sigaltstack` (131) is recorded and
   never delivered (`kernel/linux_abi.c:162`); `ucontext` beyond `uc_sigmask` is
   not filled.

Gate: a new guard clause — the single-letter space is now **full** (A–Z), so
new clauses start at `AA` (increment 31's fault delivery took `AA`); teeth
for: frame built at a misaligned RSP, handler mask not applied,
`rt_sigreturn` not clearing the mask, `sigaltstack` still a no-op, a sigreturn
that reaches the MSR (t154–t157 already tooth the return trip). Live arm: the
musl `SIGSEGV`+`longjmp` fixture.

Risk: the frame is written into the *user's* stack — every write must go through
the same validated copy path as the sig copy plan (no supervisor page, no
unmapped destination), or the delivery path becomes a memory lever.

---

## P1-A — Per-thread forward channels (audit #3)

**Retires:** "a blocked read holds the group's lease". **Size:** 2–3 increments.
**Parallel with P1-C.**

Current topology: the group is spawned with one `(ch_w, ch_r)` pair; every
thread forwards on it via `k_linux_forward`, and the sidecar's `LinuxTask` holds
exactly that pair. The single-writer invariant documented above `k_linux_forward`
is not a bug — it is the *lease*, and it becomes false the moment a second
thread of the same group forwards.

Work:

1. **Kernel: N pairs per group.** Allocate a channel pair per thread at clone
   (not per group at spawn), keep the per-thread handles in the descriptor, and
   have the forward path resolve that thread's pair. The invariant becomes "one
   parked forwarder per pair" — same FIFO argument, one pair each.
2. **Sidecar: registry per thread.** `LinuxTask` keyed by tid with its own
   `ch_r`/`ch_w`; `entry.rs`'s pump already joins children's forward channels,
   so the change is per-thread registration plus a larger table
   (`MAX_LINUX_TASKS = 8` is the next hard wall for any 8-thread program).
3. **Wake discipline.** A parked thread must not be the only runnable one; the
   resume arm already re-derives everything from the table, so this is mostly
   the park/wake bookkeeping, not new machinery.
4. **Checkpoint interaction (must not be an afterthought).** The TG-2d payload
   rebuilt one pair for the group. Per-thread pairs mean the payload carries N
   handle pairs and the replay recreates all of them; `assert_group` gains a
   clause. Otherwise the two-thread arm silently degrades to one working thread
   after restore.

Gate: a new guard clause (T is no longer free — T7 pins the TG-2d arm; D, E,
J, Q, S, U, W, Y remain) plus a **measured** live arm: leader parks
in `read` on an empty record, sibling forwards and completes; assert the
sibling's completion time is not a function of the leader's wait. Tooth: revert
to one shared pair → the measurement stalls and the guard reddens.

---

## P1-B — General thread semantics (audit #2)

**Retires:** "Go-shaped clone only, `set_tid_address` inert". **Size:** 3–4
increments. **After P1-A** (same files, same desktop).

1. ~~**`CLONE_CHILD_CLEARTID` + `set_tid_address` (one increment together).**~~
   **Landed in increments 30 and 30b**, since this plan was first drafted: the
   word is stored on the descriptor at clone (`kernel/linux_thread.c:79`),
   `set_tid_address` (218) is captured at the first trap
   (`kernel/linux_abi.c:167`), and the exit glue writes 0 and
   `futex(FUTEX_WAKE, 1)` on it from every death funnel — proven on a boot by
   `tests/linux_thread_death_check.sh`. What is still open inside this item: the
   parent/child `*SETTID` flags and `list_op_pending`.
2. **Accept flags individually.** Replace the all-or-nothing
   `LINUX_CLONE_GO_FLAGS` check with a per-bit table: `VM|FS|FILES|SIGHAND|
   THREAD|SYSVSEM|SETTLS|CHILD_CLEARTID|CHILD_SETTID|PARENT_SETTID` accepted
   with real semantics, and the namespace/cgroup bits
   (`NEWUSER|NEWNS|NEWPID|NEWNET|NEWIPC|NEWUTS|NEWCGROUP`) still refused **by
   name** — they stay out until a namespaces phase exists, and the audit line
   says so rather than pretending coverage.
3. **`clone3` (435).** Bound-copy the 88-byte `clone_args`, validate
   `stack`/`stack_size`/`tls`/`ctid`/`set_tid`, and dispatch into the same
   validator. Note for sequencing: returning `ENOSYS` for `clone3` is a
   *compatible* answer — glibc ≥ 2.34 falls back to `clone` on `ENOSYS` — so
   this increment can land without breaking anything that already works.
4. **`vfork`/`fork`** belong to P1-E, not here; they need address-space copy
   semantics.

Gate: musl 8-thread fixture (create/join/detach, TLS + errno isolation). Teeth:
stop clearing the TID word → `join` hangs and the guard times out with the
thread's own name in the message; accept `CHILD_CLEARTID` without the mechanism
→ clause reddens.

---

## P1-D — Async preemption (audit #1, asynchronous half)

**Retires:** "Go's GC/preemption path does not work". **Size:** 2 increments.
**After P1-C, alongside P1-B.**

Design decision, stated up front: **deliver only at return-to-user**, never
mid-kernel. The timer tick marks the thread's pending set; the frame is pushed
on the path that is already building the return context. This is
indistinguishable from Linux for a user program and avoids injecting a frame
into a half-built kernel stack — the failure mode that would cost days.

Work: tick → pending mark; FPU/SSE state capture into `fpstate` (the lazy-FPU
path is dormant, so this is a straight save/restore of the live state — and the
increment must keep it that way, or name the interaction); nested-signal
handling (`SA_NODEFER`, mask depth); signal during an altstack fault.

Gate: the Go acceptance program — `GODEBUG=asyncpreemptoff=0`, default `GOGC`, a
spin loop with no calls; assert the runtime's own GC log line appears and the
program exits 0. Second live arm: `os/signal` catches `SIGTERM`, prints, exits 0.

Tooth: mark pending but skip the tick path → the Go fixture never runs a GC and
the arm reddens by timeout (which is exactly the audit's symptom).

---

## P1-E — Process lifecycle (audit #5)

**Retires:** "no `os/exec`, no shell-outs". **Size:** 3–4 increments.
**After P1-B (fork) and P1-C (SIGCHLD).**

1. **`execve` from a running task.** Load a new ELF into the *existing* address
   space: unmap everything, reuse the spawn path's initial-stack builder
   (argc/argv/envp/auxv at the top of the new stack), reset dispositions to
   default except ignored ones, honour `O_CLOEXEC`, then return to user at the
   new entry. This needs the loader factored into "load into empty" +
   "teardown" halves; the boot path must keep working unchanged (its own guard
   is the regression test).
2. **`fork`/`vfork`.** No COW in this kernel: fork is an **eager copy** of the
   mapped set into a fresh frame budget, charged to the child's partition.
   Honest limits (documented, measured): fork of a large guest is expensive; say
   so in the doc rather than discovering it under load. `vfork` suspends the
   parent until the child execs or exits.
3. **`wait4` (61) + exit status + zombies.** Exit status plumbing in the process
   table, `wait4`/`waitid` rows, reaping, and `SIGCHLD` delivery (needs P1-C).
   Poll-only `wait4` can land before `SIGCHLD`.

Gate: a static fixture that forks, execs its own second stage, and asserts the
exit code (7) propagates; then `sh`-shaped: fork + exec + wait in a loop.
Tooth: drop the exit-status plumbing → the parent observes 0 instead of 7.

Out of scope, named: multi-process checkpoint/restore (the TG-2d payload is
single-group), and PID namespaces (`/proc`-shaped tools).

---

## P1-F — Dynamic linking (audit #4): a decision, not a task

**DECIDED (2026-11-25): out of scope for now.** Recorded here and in `docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §6.1, so it can be reopened with a number attached instead of re-argued. A static-only target with static-musl and static-Go (non-PIE — the musl target emits `ET_DYN` by default and §5.1 refuses PIE, so `-C relocation-model=static` is the build flag, not a Go `-buildmode`) covers the gate's real workloads today. The syscall surface an `ld.so`-capable shim would need is named and priced at **4–6 increments**: `rseq` (334), `getrandom` (318), `newfstatat` (262), `readlinkat` (267), `prlimit64` (302), `sched_getaffinity` (204), plus `mprotect` outside §5.4's W|X refusal for `RELRO`-shaped calls (`set_robust_list` 273, on this list at drafting time, has since landed in increment 30 — `kernel/linux_abi.c:178`, trace `tools/linux_robust_list_census.txt` — and is off the price). Reopen condition (unchanged, shim doc §6): a named first user brings a dynamically linked binary and a static rebuild is impossible. Because every entry assumes working threads and signals, it stays behind P1-A/B and P1-C/D by construction — before them `ld.so` starts and dies inside pthread/signal start-up, which is a worse state than refusing the image by name.

If it is later required, the honest path is two steps, and the cost is
concentrated in *syscalls*, not in the loader:

- **Load `ld.so`.** Read `PT_INTERP`, map it at `AT_BASE`, build the Linux-style
  initial stack (`AT_PHDR`, `AT_ENTRY`, `AT_BASE`, `AT_PHNUM`, `AT_RANDOM`,
  `AT_EXECFN`), and jump to the interpreter. `ld.so` then does the relocation
  work itself using `openat`/`read`/`mmap`, which already exist.
- **Close the syscall surface glibc/ld.so actually call** and that the census
  does not yet carry: `rseq` (334), `getrandom` (318),
  `newfstatat` (262), `readlinkat` (267), `prlimit64` (302),
  `sched_getaffinity` (204), plus `mprotect` without the W|X refusal for
  `RELRO`-shaped calls. Budget 4–6 increments, and only after P1-B/D, because
  every one of those libraries assumes working threads and signals.
- `dlopen` needs a real `ld.so`; **static musl cannot `dlopen`** — that is a
  musl property, not a gap in this kernel, and the doc should say so.
- **cgo** is a different story: static cgo needs P1-B and P1-A (threads +
  per-thread channels), not the dynamic loader. Gate it there and it comes
  almost free.

---

## 3. Cross-cutting obligations (the things that bite if skipped)

1. **Every phase touches the checkpoint payload.** Per-thread channels, signal
   dispositions/pending, the TID and robust words increments 30/30b added to the
   descriptor, and zombie exit status are all per-thread state that
   `struct EnvCkptTask` does not carry — its 64 bytes hold only name, kind,
   leader, thread_index, group_id, cap_count, entry and the stack bounds. `docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md`'s TG-2d payload
   must grow a versioned extension for them, and `assert_group` must gain a clause
   per field — otherwise the two-thread arm keeps passing while restoring threads
   that are subtly dead (no channels, forgotten TID words). This is the single
   highest-value guard in the whole plan, because the restore path is the product
   differentiator and the least tolerant of silent drift.

2. **Every kernel/sidecar change re-packs and re-boots the artifact.**
   `make selfhost-bootimage` changes `sidecars.cpio` (tracked), so each phase
   lands with the archive, its `.digest`, `.sha256` and `.stamps`, plus one ISO
   service smoke and one Phase 5 initrd boot in the same commit. Skipping this is
   how the branch got into the drift that `sidecar_stamp_check` clauses B/K
   caught this week.

3. **WCET budget for the `LinuxTask` table.** `MAX_LINUX_TASKS = 8` is a
   compile-time cap in a single-threaded sidecar. General pthreads needs
   ~32–64 threads' worth of slack or a table that spills to a fixed-capacity
   array sized from the manifest. **Decided (Appendix A): 32 per group, fixed array**, landed inside P1-A — P1-B is not blocked on it.

4. **One live arm per phase, wired into CI.** The TG-2d precedent is now
   enforced by guard clause T7: a live arm that only runs by hand rots. Each P1
   phase adds one `kernel-guards` step (or extends an existing one) and its own
   guard clause pinning the invocation.

5. **Census changes are code, not comments.** The remaining new syscalls
   (`clone3` 435, `wait4` 61, `fork` 57, `vfork` 58) need a kernel-table row,
   the sidecar's documented refusal list, and a trace of their own. That is how
   `rt_sigreturn` (15) landed — row `kernel/linux_abi.c:148`, trace
   `tools/linux_rt_sigreturn_census.txt` (`15 rt_sigreturn 1`), increment 29 —
   and the same holds for 202, 233, 257 and 273, none of which appear in
   `tools/linux_syscall_census.txt`'s 37-line aerobackup trace. The three
   sources already disagree by design when a number is *deliberately* absent,
   and the guard checks exactly that.

---

## 4. Order, size, and what each phase buys

| Wave | Phase | Retires | Increments (est.) | Blocks |
| --- | --- | --- | --- | --- |
| 1a | **P1-C** signals (frames + `rt_sigreturn` + EINTR) | handlers are never invoked | 3–4 | D |
| 1b (parallel) | **P1-A** per-thread forward channels | blocked sibling stalls the group | 2–3 | B (same files), pthreads |
| 2 | **P1-B** thread semantics (`CLEARTID`, flag table, `clone3`) | Go-shaped clone only | 3–4 | E, cgo |
| 3 | **P1-D** async preemption (tick → pending → frame) | Go GC/preemption | 2 | — |
| 4 | **P1-E** process lifecycle (`execve`, `fork`, `wait4`, `SIGCHLD`) | no `os/exec`, no shell-outs | 3–4 | — |
| — | **P1-F** dynamic linking | static only | **out of scope (decided 2026-11-25, shim doc §6.1)** | — |

If the work must be **serial**, use C → A → B → D → E. If two people work in
parallel, C and A share no file and are the natural split; B then follows A,
and D follows C.

What the ordering buys, stated plainly: after **wave 1** a real crash handler
works and a blocked thread no longer freezes its siblings; after **wave 2** a
musl pthread program runs; after **wave 3** a normal Go program — default GC,
default preemption — runs; after **wave 4** a program can spawn another program.
That is the whole audit list except dynamic linking, which stays a documented
product constraint until someone needs a `.so`.

---

## 5. Decisions this plan needs from you

1. ~~Dynamic linking (P1-F): scope out, or fund it?~~ **Decided 2026-11-25:
   out of scope for now** — recorded in §P1-F above and in the shim doc §6.1
   with the syscall surface (334, 318, 262, 267, 302, 204, plus RELRO
   `mprotect`; 273 has since landed) and its 4–6 increments. No input needed unless the reopen
   trigger fires.
2. ~~Thread cap: 32? 64? Sized from the manifest? Array or paged table?~~
   **Decided 2026-11-25: 32 threads per group, fixed array** — Appendix A has
   the resource chain (`PROC_MAX` 16 → 64, the four 8s → 32/34, channels
   unchanged) and why 32 rather than 8 or 64. Lands inside P1-A, so P1-B
   starts from 32.
3. **CoW or eager copy for `fork`.** Eager copy is simpler and honest; CoW is a
   second memory-manager project. Recommendation: eager copy, measured and
   documented, revisit only if fork latency blocks a real workload.

Everything else (guard letters, increment numbering, which fixture language)
follows the repo's existing conventions and does not need a decision.

---

## Appendix A — Thread-cap decision (closes §5 item 2)

**Decided 2026-11-25: `LINUX_THREADS_MAX = 32`, landed as part of P1-A, not P1-B.**
The number is set by the scarcest resource a thread consumes, not by taste. In
this kernel a Linux thread is charged against five tables, and four of them are
sized 8 today while the fifth is 16:

| Resource | Today | After | Why that value |
| --- | --- | --- | --- |
| `proc_table[PROC_MAX]` (`kernel/process.h`) | 16 | **64** | `MAX_TASKS` (`scheduler.h`) and `FPU_TASK_STATES_MAX` (which mirrors it) are already 64 — that is this machine's task ceiling; 16 was historical, and a 32-thread group in a 16-slot table is impossible |
| `LINUX_THREADS_MAX` (`kernel/linux_thread.h`) | 8 | **32** | the per-group admission cap (see the reasoning below) |
| `ENV_CKPT_MAX_THREADS` / `ENV_CKPT_MAX_TASKS` (`kernel/env_ckpt.h`) | 8 / 10 | **32 / 34** | the record must hold a full group (2 sidecars + 32 threads), or restore silently truncates |
| `MAX_LINUX_TASKS` (`user/sidecar/src/linux.rs`) | 8 | **32** | the sidecar table is global to the environment, so it must hold one full group's threads |
| `CAP_CHAN_MAX` (`kernel/cap.h`) | 64 | **unchanged** | one channel *object* per pair — `cap_chan_create` mints both end caps against one `obj_id` — so 32 threads cost 32 objects and leave ~16 for init's messenger endpoints, consoles and other tasks |

### Why 32, grounded

- **8 is Go's measured number, not a pthreads number.** `kernel/linux_thread.h`'s
  own comment: "Go's measured startup cloned six threads for a two-thread
  fixture" — the current cap is nearly consumed before the program does any
  work of its own.
- **General pthreads spends threads on pools, not on the main program.**
  glibc/musl default worker pools and async runtimes size themselves to the
  vCPU count (4–8 on the gate's QEMU), and build tools use `-j$nproc`. 32 covers
  that shape with real headroom, where 8 does not.
- **Not 48 or 64.** Group threads come out of the same `proc_table` that holds
  init, both sidecars, the control plane, tenants and (after P1-E) every
  `fork`ed child. 32 in a 64-slot table leaves 32 for everything else — and
  leaves half the channel pool free, since channels are the next binding
  resource at one object per thread.
- **The checkpoint stays cheap.** `EnvCkptTask` is 64 bytes, so 34 entries is
  ~2.2 KiB per record against 640 bytes today (× `ENV_CKPT_MAX` 4 ≈ +6 KiB of
  static state), and `EP_MAX_TASKS` follows the constant automatically because
  `kernel/env_payload.c` defines it as `ENV_CKPT_MAX_TASKS`.

### Landing order, and the three traps

1. **Raise `PROC_MAX` first (16 → 64).** `linux_group_table[PROC_MAX]`
   (`kernel/linux_thread.c`) is indexed by it, and
   `tests/linux_clone_host_test.c` plants a descriptor at
   `proc_table[LINUX_THREADS_MAX]` to prove admission refuses past the cap —
   with the cap at 32 and `PROC_MAX` still 16 that write is out of bounds, so
   the guard would be checking memory it does not own.
2. **Then the four thread caps in one commit.** A group that can be created
   but not recorded fails at its first checkpoint, not at clone.
3. **This is a checkpoint-format change.** `n_tasks` now reaches 34 in a
   `struct EnvCkptRecord` whose `version`/`size` fields exist for exactly this:
   bump `version`, teach the reader both sizes, and add an `assert_group`
   clause for a full-group replay. Without it TG-2d's replay keeps passing while
   restoring a truncated group.
4. **Sync the E7 doc's own copies** — `docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md`
   states `#define LINUX_THREADS_MAX 8` (§7) and "should start at 8" (§9); both
   become 32 and say why.

### Why this is P1-A's increment, not separate work

The cap is only worth raising once per-thread channels exist: eight threads
queueing behind one forward lease stall each other exactly as thirty-two would,
so raising the number first would buy capacity nobody can use. The chain change
is therefore one increment inside P1-A, and P1-B may assume 32 threads from its
first line.

**Teeth:** fill a group to 32 and assert the 33rd clone gets `EAGAIN` naming the
cap; assert the channel table still has free objects with a full group resident;
checkpoint and replay a full 32-thread group (or a named skip with the reason).

**Reopen condition:** a workload needing more than 32 threads in one group is a
`CAP_CHAN_MAX` 64 → 128 question first (channels bind at one object per thread),
and only then cap 32 → 64 — decided with the measurement in hand, not ahead of
it.
