# AeroSLS Static-Binary Linux ABI Shim — Design v0.1 (Phase E7)

**Status:** Draft for review.
**Scope:** The design pass Phase E7 is gated on (`AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §10, §13), plus the two items that document parks here — its Q5 (§14) and dynamic linking (§12, "Belongs to E7's design pass").
**Resolves:** (1) **syscall routing** — kernel-side Linux-personality dispatch, or forwarding to the owning POSIX sidecar core; (2) **architecture** — x86-64 or ARM64 first; (3) dynamic linking; (4) the shape of the inherited gate.
**Does not do:** implement anything. This document is the pass; §10 says nothing is built before it.

---

## 0. Executive summary

- **D1 — Routing: forward, do not dispatch.** The kernel is a **trap demultiplexer, an argument marshaller and a parker**. It never grows a Linux semantic engine. POSIX/Linux semantics — file descriptors, pathnames, permissions, signals-as-process-state, the process model — stay in exactly one place, the POSIX sidecar core, where they already live. The kernel's job is to recognise a Linux-personality task at the trap, copy its registers into a request, hand that request to the task's owning environment over the channel it already has, park the task, and put the reply into `rax`. §3.
- **D2 — Architecture: x86-64 first.** This is a **dependency, not a preference**. E7 cannot run on ARM64 today because ARM64 has no userspace to run *in*: `arch/arm64/` is a translation-and-scheduling smoke harness (boot, GIC, MMU, PL011, an EL0 excursion for SIMI-translated blobs), not an OS with a process table, an ELF loader, partitions or capability tables. The ARM64 kernel is H2 item #1's work, not E7's. §4, including the amendment this proposes to H2 §2 item #2, which currently says ARM64.
- **D3 — Dynamic linking: not in the shim, and the reason is structural.** A dynamic linker is a userspace program. If the shim runs enough syscalls to run `ld.so`, it runs `ld.so` — there is no `dlopen` to add in the kernel. Refused for v1 under the H2 constraint ("static musl or nothing"); the trigger that would reopen it is named, and it reopens as *more syscalls*, not as a new shim feature. §6.
- **D4 — Target order, sharpened.** The H2 roadmap says "static Rust or Go first". The design pass sharpens that: **static, single-threaded Rust first**, because it needs no thread creation at all. **Go is second and gated on thread-group support**, because Go's runtime unconditionally creates OS threads and this kernel gives every process its own page table with no sharing path (`user_clone_page_table()`, `kernel/process.c`) and has no `clone`/`futex`/thread-group machinery anywhere. That is a deliverable, not a detail. Static CPython as H2 scopes it — conditional on the first user; Node stays out. §7.
- **D5 — The gate is two halves, and only one is E7's.** "A static binary doing the first user's actual work" (E7's) **and** "surviving a checkpoint/restore cycle" (v0.2's, §12 item 1). §8 states what "surviving" has to mean, and finds that the forwarding design *reduces* the checkpoint problem: a task parked in a Linux syscall is already in a fully-captured, well-defined saved state (`struct CapParkCtx`, `kernel/process.h`), so it can be checkpointed parked and restored parked.
- **D6 — The census target is a decision, not a discovery.** `AeroSLS-Roadmap-2026H2-v0.1.md` §5 states it in bold: "**There is no named first user.**" Verified against the tree, no candidate binary exists either — there is no Go source anywhere in the repository, no `std`-linked Rust binary, and every real operational workload is bash or Python. So E7's first deliverable cannot be a measurement of something that exists; it is the *designation* of what gets measured. §5.5.1 evaluates the four operations H2 §7 names against E7's own constraints, recommends the backup operation, and leaves the one-line decision where it belongs.

Three facts shape everything above, all measured rather than assumed (§2):

- **The ELF64 loader already loads a static Linux binary's segments.** `kernel/loader.c` parses `PT_LOAD`, honours `p_flags`, zero-fills BSS, quota-charges frames to the partition, and returns `e_entry`. A non-PIE static musl or Go binary is an `ET_EXEC` with absolute `p_vaddr`s; that is exactly what this loader maps. **No loader work is required for the first two targets.**
- **The syscall number spaces are not disjoint, so a personality is required under *either* option.** SLS claims 105–151, 160–162, 289–297 and 307–320; Linux x86-64 populates 0–~450. `105` is both `SYS_SLS_ALLOCATE` and Linux `setuid`; `107` is both `SYS_SLS_CHMOD` and Linux `geteuid`. "Dispatch on the number" is therefore not even a well-formed question — which is why Q5 is a question about *where semantics live*, not about whether the kernel reads the number.
- **165 `SYS_` numbers are defined and 165 `case` arms exist** (`#define SYS_` and `case SYS_` over `kernel/`). The H2 roadmap's "131" is stale; the head start has grown. The design deliberately does **not** spend it.

---

## 1. The question, restated, and what counts as an answer

`AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §10 fixes E7's placement and leaves two questions:

> 1. **Syscall routing.** A static binary's `syscall` instruction traps to the kernel. Either the kernel dispatches Linux-personality syscalls itself (the H2 roadmap counts 131 existing `SYS_` handlers as a head start), or it forwards them to the owning POSIX sidecar's core over a channel, keeping POSIX semantics in one place. These have very different security and performance profiles.
> 2. **Architecture.** The H2 roadmap targets native ARM64; the sidecar world and the live cluster are x86-64 today.

An answer is only worth recording if it settles these against criteria that are checkable, not against taste. This document uses four, and the rest of §3 and §4 argue against them explicitly:

- **C1 — one implementation.** POSIX semantics must exist in exactly one place. This repo already treats two implementations of one thing as a defect, not a style preference: E6 §9.1 deleted `net/http.c`'s local `env_status_str()` switch in favour of `env_status_name()` in `kernel/env_proto.h` on exactly this ground ("two switches over one set of codes is exactly the drift this file's own header warns about").
- **C2 — no new authority for the Linux task.** §2 of the roadmap's design principles: tenant environments get **zero hardware capabilities**, kernel-enforced. Whatever runs a tenant's binary must not acquire authority the tenant profile denies, and must not *reach* authority through a confused deputy.
- **C3 — no widened kernel attack surface.** E2 and E3 spent real effort restricting `SYS_SLS_CREATE_SIDECAR` to the sidecar-creator tree and denying hardware capabilities to tenants. A routing decision that hands the Linux path the kernel's `SYS_` surface would spend that effort backwards.
- **C4 — every claim ships with a tooth.** §2's principle, applied literally: each of D1–D4 has a named, cheap arm that reddens for the clause it names (§10).

---

## 2. The ground this design stands on

Everything in this section is a reading of the tree, with the file named. Nothing here is inherited from a roadmap paragraph.

### 2.1 The trap door, and the ABI on the other side of it

Ring-3 executes `syscall`; the CPU lands in `syscall_entry_stub` (`arch/x86/syscall.asm`), which `swapgs`es, saves the user RSP and loads the kernel RSP through the per-CPU block, pushes the callee-saved registers and `rcx`/`r11`, then routes on **`rax`** with a hand-written compare chain for the dense SLS block (105, 110–124, 130–134, 140–145, 150, 151) and falls through to `.unknown_syscall`:

```
    mov  rsi, rdi          ; arg was in RDI
    mov  rdi, rax          ; syscall number from RAX
    call do_syscall
```

So the **SLS ring-3 ABI is: `rax` = number, `rdi` = a single pointer to a request struct, one return value in `rax`.** `do_syscall(uint64_t num, void* arg)` (`kernel/syscall_dispatch.h`) is the C half. Both sides are already written down: `_sls_syscall(num, arg)` in `user/libsls/sls.h` (C), `sls_syscall(num, arg)` in `user/aerosls/src/syscall.rs` (Rust), whose own comment states the convention and the clobber list.

Two properties of this stub matter for §3 and are the reason the Linux path is a *new* path rather than a widened one:

1. **The stub does not save the caller-saved argument registers.** Its own comment says so: "Caller-saved regs (R8-R10, RSI, RDX, RDI) are NOT preserved here — user programs must list them as clobbers in their inline asm." Linux's six syscall arguments live in `rdi, rsi, rdx, r10, r8, r9`. **A Linux-personality task's arguments are destroyed by the entry path as it stands today.** This is the single most concrete implementation consequence of D1 (§3.4).
2. **The registers that *are* saved are saved to the per-CPU block, before any call, on purpose.** `arch/x86/user_paging.h` documents `syscall_regs[8]` at `[gs:0x10..0x48]` and states the reason: the `-O2` call chain "may reuse `[top-64..top-8]`" on the syscall stack for its own locals, so a late read captured garbage, and the failure was observed live. Any Linux-path capture inherits that rule verbatim.

The gate that installs all of this is `syscall_gate_init()` in `arch/x86/user_paging.c`: `EFER.SCE|NXE`, `MSR_STAR` (so `SYSRETQ` yields CS `0x23`, SS `0x1B`), `MSR_LSTAR` = the stub, `MSR_SFMASK` clearing IF, and the two GS base MSRs. **`MSR_IA32_FS_BASE` is not touched anywhere in the tree** — see §5.3.

### 2.2 The number space, and why a personality is required either way

Counted in the tree: `#define SYS_` and `case SYS_` are both **165**. The ranges in use are 105–151 (the legacy/`SLS_` block the shell uses, some of it routed in asm), 160–162 (`SYS_SLS_PROC_*`, `kernel/process.h`), 289–297 (`SYS_SLS_CAP_*`, per `kernel/syscall_dispatch.c`), and 307–320 (`SYS_SLS_IO_*`/`DEV_MMAP`/`IRQ_*`/`BOOT_GEN` at 307–318, routed explicitly ahead of the legacy switch, and `SYS_SLS_ALLOC_REGION` 320, `kernel/cap.h`).

Linux x86-64 native numbers run 0–~450 over the same register. The spaces overlap in the SLS ranges, and two of the overlaps are numbers the shell already uses every boot:

| Number | SLS meaning | Linux x86-64 meaning |
|---|---|---|
| 105 | `SYS_SLS_ALLOCATE` (`kernel/syscall_dispatch.c`, `case 105`) | `setuid` |
| 107 | `SYS_SLS_CHMOD` | `geteuid` |
| 110–124 | `VALLOC`, `GRANT`, `TX_*`, … | `getppid`, `setsid`, `setreuid`, `getresuid`, `getsid`, … |
| 160 | `SYS_SLS_PROC_CREATE` | (unassigned in x86-64 native) |
| 320 | `SYS_SLS_ALLOC_REGION` | `kexec_file_load` |

**Consequence for Q5:** whichever option is chosen, the kernel must know *what kind of process is trapping* before it can interpret `rax` at all. A personality is not an optimisation of one option; it is a precondition of both. What Q5 actually decides is **where the semantics after that branch live** — the C `SYS_` switch, or the sidecar core.

### 2.3 The ELF64 loader already loads a static Linux binary's segments

`kernel/loader.c`'s ELF path (§ "ELF64 loader") does the following and nothing else:

- rejects non-64-bit and header-less images;
- for each `PT_LOAD` with `p_filesz != 0`, allocates `ceil(p_memsz/4096)` frames via **`allocate_physical_ram_frame_for_partition(partition_id)`** (quota-charged to the partition — the LPAR Phase 13 note in the code calls this out as deliberate, because `memsz` is attacker-controlled);
- zero-fills each frame, copies the file bytes, and maps it at `p_vaddr` with `USER_PTE_PRESENT|USER`, plus `WRITE` from `PF_W` and `NOEXEC` when `PF_X` is absent;
- returns `ehdr->e_entry`.

A static musl or Go binary is an `ET_EXEC` ELF with absolute `p_vaddr`s (typically `0x400000`-ish) and no `PT_INTERP`. **That is precisely this loader's contract.** So the first two targets need no loader change: the image contract in §5.1 is not a hope, it is a description of code that already runs.

The loader also handles flat binaries and SIMI objects, so a third format is already in the dispatch. Adding a Linux *personality* is orthogonal to adding a format — which is useful, because it means the design never has to touch the format dispatch.

### 2.4 The park/resume machinery, which is what makes forwarding cheap

`kernel/process.h` already carries everything a *blocking syscall* needs:

- `struct CapParkCtx` — the eight values the entry stub pushed (`r11`, `rcx`, `r15..rbp`) plus `user_rsp`, with the field order documented as load-bearing;
- `park_ctx`, `park_req`, `park_syscall` — the saved user state, the request pointer the resume re-runs, and the syscall the resume re-enters through `do_syscall`;
- `resume_kernel` / `resume_sysret` — the two resume modes, where `resume_sysret` returns to Ring-3 through the stub's **`syscall_return_path`** (a global label precisely because `cap_recv_resume()` jumps to it) "as if its syscall (send, yield) had simply taken a while";
- `cap_maybe_handoff` / `handoff_target` — immediate wakeup of the peer instead of waiting for the next tick.

Forwarding a Linux syscall to the sidecar is *the same shape as a blocking `cap_recv`*: park on a channel, be woken by the reply, return to Ring-3. **No new scheduling machinery is required.** This is the strongest single argument for D1, and it is a fact about existing code rather than a prediction.

### 2.5 The capability posture the shim must not weaken

- Tenant environments get a profile with `budget`, `console` and `ramdisk` and no network/uart/IRQ/NIC-BAR (`user/init/src/posix_manifest.rs`; E3 in the POSIX roadmap).
- `sidecar_authority` (`kernel/process.h`, E2) is the existing gate on *who may create sidecars*, and it is deliberately not inherited by HTTP/shell-spawned programs.
- Every kernel syscall handler resolves the caller through `cap_current_pid()` and its capability table — e.g. `sys_sls_dev_mmap` (`kernel/chan.c`) passes `cap_current_pid()` into `k_dev_mmap()`. Authority is per-process and already checked; nothing in this design may bypass it.
- The POSIX sidecar is explicitly **one trust domain**: its Phase 2 design (§7.4, and §0) states that a malicious program sharing its address space can corrupt the core, and that malicious-process isolation is deferred to a later per-task MMU model. §3.5 turns this into a hard constraint on where the Linux binary may run.

### 2.6 `checkpoint_trigger()` — what is persisted, measured

`kernel/checkpoint_mgr.c:127` builds a dirty-mask-driven batch and calls, in order: `persist_catalog`, `_records`, `_schemas`, `_programs`, `_partitions`, `_rowstore_headers`, `_row_constraints`, `_row_index_defs`, `_vecstore_headers`, `_vec_index_defs`, `_row_journal`, `_databases`, `_views`, `_tenants`, `_services`, `_workloads`, then `qemu_sls_tcache_sync()`, then writes a header and a `state_tree_build()` snapshot. Callers: the periodic/`net/http.c:6624` route and `checkpoint_mgr.c:260`.

**There is no process state, no address space, no capability table, no channel, and no ramdisk content anywhere in that list.** The roadmap's §10 claim is confirmed by reading the function, and §8 makes it a gate.

---

## 3. Decision 1 — syscall routing: forward to the owning environment's core

### 3.1 The dichotomy is false, and saying so is the point

"Either the kernel dispatches, or it forwards to the sidecar" reads as a choice about whether the kernel is involved. It is not. **The `syscall` instruction's only destination is `MSR_LSTAR`, which is `syscall_entry_stub`.** There is no route that avoids the kernel. The real choice is narrower and more useful:

> After recognising a Linux-personality task at the trap, does the kernel **interpret** the syscall, or **describe** it and hand it to the environment?

Interpreting means growing a Linux semantic layer in C, beside a sidecar that already implements the same semantics in Rust. Describing means the kernel builds a fixed request, uses machinery it already has to deliver it and park, and keeps every semantic decision in one process.

### 3.2 Option A — kernel-side Linux-personality dispatch. Rejected.

The H2 roadmap's "131 `SYS_` handlers … which is a real head start" is true about the count (now 165) and misleading about the reuse, and the reason is worth stating in this repo's own terms:

- **The head start is the wrong kind of head start.** An existing `SYS_` arm is a *kernel-object* operation (`SYS_SLS_ALLOC_REGION`, `SYS_SLS_IO_OUT`, `SYS_SLS_QUERY`, …). A Linux binary's syscalls are *POSIX* operations — `openat`, `read`, `fstat`, `brk`, `mmap`, `arch_prctl`, `rt_sigaction`, `exit_group`. Almost none of the 165 is a Linux syscall's implementation; the overlap is `write`-class I/O onto a console, at best. What Linux needs is the layer the sidecar already has: fd tables, path resolution, permissions, pipes, `fork`/`exec` semantics (`AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md` §3–§4).
- **It forks the semantics (violates C1).** fd ownership, pathname resolution and permission checks would then exist twice — once in `kernel/*.c` for Linux tasks and once in the sidecar core for POSIX tasks — and the two would have to be kept in step by hand. This repo's culture calls that out explicitly (E6 §9.1, quoted in C1). The whole architectural claim of the POSIX sidecar is that *"the kernel never implements `open()`, `read()`, `fork()`, etc. natively — every POSIX operation is synthesized inside the sidecar"* (Phase 2 design, header). Option A retracts that claim for the subset Linux binaries use.
- **It creates authority the tenant profile denies (violates C2, C3).** A task that reaches kernel `SYS_` arms is a task whose calls are checked against a capability table it must therefore *hold* something in. To make `openat` work kernel-side, the kernel must either hold the process's caps for it or grant the process caps that mean something — and every one of those is authority the E3 tenant profile exists to withhold. In the limiting case the Linux task becomes a second, less-reviewed path into the kernel's object model, which is the opposite of what E2 and E3 built.
- **It is more surface, not less (violates C3).** Roughly 200 Linux syscall arms, each with its own validation, in a ring-0 C switch that any process with the personality can reach. Option B's kernel-side addition is one branch and a fixed request struct.
- **Its one genuine advantage is real and is deferred, not denied.** A kernel-resident path is faster for syscalls the kernel can answer alone. §3.7 keeps that option and attaches it to a measurement instead of a preference.

### 3.3 Option B — forwarding, step by step. Chosen.

The mechanism, in the order the code would run it:

1. **Trap.** `syscall` lands in `syscall_entry_stub` as today.
2. **Capture, before any call.** The Linux path writes the six argument registers to per-CPU scratch *immediately*, next to the existing eight, for the reason §2.1(2) records. Nothing else runs first.
3. **Recognise.** `process_find_current()->personality == PROC_PERSONALITY_LINUX`? If not, fall into today's chain unchanged — byte for byte, so no existing path is perturbed.
4. **Describe.** Build a fixed-size `SLSLinuxSyscallRequest { uint64_t num; uint64_t args[6]; uint64_t ret; int32_t status; }` in kernel space. This is a description, not an interpretation: the kernel knows the Linux argument *positions* (they are ABI, not policy) and knows nothing about what `num` means.
5. **Deliver and park.** Post it on the task's single channel to its environment's POSIX sidecar and park the task on that channel (`park_req`/`park_syscall`, the same fields a `cap_recv` park uses, with `park_syscall` extended to name the Linux-trap resume).
6. **Answer.** The sidecar core performs the operation — through its own fd table, its own paths, its own permissions, and its own **SLS** syscalls, which are checked against *its* capability table by the code that already checks them — and replies with the return value or a `CAP_E*`.
7. **Return.** The park resume writes `ret` into `rax`, maps the status through the errno table (§5.7), and returns through the existing `syscall_return_path` so the task `sysret`s as if the syscall had simply taken a while.

Note what step 6 does to C1: **there is exactly one POSIX implementation, and the kernel is not a second one.** The sidecar's answer is expressed in the syscalls the sidecar was already authorised to make.

### 3.4 The detail that decides feasibility: six arguments, captured before any call

This is the part that a design pass is for, and it is easy to miss:

- **Linux passes six argument registers** (`rdi, rsi, rdx, r10, r8, r9`); SLS passes one pointer. The stub's push list (`rbp, rbx, r12–r15, rcx, r11`) and the per-CPU block (`syscall_regs[8]`) **do not contain `rdi/rsi/rdx/r10/r8/r9`**, and the stub's own contract says the user program must treat them as clobbered. `struct PerCPUData` (`arch/x86/user_paging.h`) is `{ user_rsp, kernel_rsp, syscall_regs[8] }` — nothing else.
- **Therefore the Linux path needs its own capture, and the existing offsets must stay frozen.** `syscall_regs[8]` at `[gs:0x10..0x48]` is read by the park/handoff/yield captures, and its comment records that a live failure came from reading that data late. The addition is a sibling block (`linux_args[6]`), written by step 2 above, before any call — not a widening of the existing block, and not a late read of the syscall stack.

This is also the honest boundary of the "forwarding is nearly free" claim in §2.4: forwarding is free *once the arguments are captured*, and capturing them is a real, small, testable change to the trap path with an existing precedent for its failure mode.

### 3.5 Where the Linux task lives: its own address space, holding one capability

The Linux binary **must be its own ring-3 process** — its own `cr3`, its own frames (charged to the partition), its own capability table — and **not** a task inside the POSIX sidecar's address space, which is how the sidecar's own BusyBox-class applets run today (Phase 2 design; `user/sidecar/src/applets.rs`).

The reason is §2.5's trust-domain statement, applied as a rule rather than a caveat. A program in the sidecar's address space can corrupt the core, and the core holds the environment's *capabilities* — budget, console, the ramdisk channel. A tenant's binary sharing that space would reach every one of them through the core, which is exactly the confused deputy C2 forbids, and it would make "zero hardware capabilities, kernel-enforced" untrue in practice while remaining true on paper. The kernel-enforced boundary has to be an MMU boundary, so the Linux task gets its own.

The consequence is a clean, assertable property:

> **A Linux-personality task's capability table contains exactly one entry: the channel to its environment's POSIX sidecar.**

No hardware caps, no `sidecar_authority`, no MEM caps of its own, no console — nothing else. Every effect it has on the world is a request to the sidecar, and the sidecar's reply is checked by the same code that checks every other sidecar syscall. This is the property §10.2's guard asserts, and it is toothable (§10.3).

The task is spawned by the environment's POSIX sidecar, whose `sidecar_authority` it already inherits (the E4 finding in the roadmap records that tenant sidecars in partition 1 hold it). Frames are quota-charged to the partition by `allocate_physical_ram_frame_for_partition()`, so the spawn is bounded by E3's budget without new machinery.

### 3.6 Security analysis

- **The kernel's Linux entry point is one branch, one fixed struct and a park.** It cannot reach `do_syscall`'s `SYS_` arms, because the personality branch selects a path that does not contain them. `do_syscall` stays reachable exactly by the processes that reach it today.
- **The Linux number space cannot be used to reach SLS numbers.** With D1 this is structural, not a filter: a Linux-personality task's `rax = 105` is *Linux 105* — it is described as `setuid` and forwarded; the sidecar answers `EPERM` under its own policy. §10.3 is the arm that proves the branch is what stops the other reading, by deleting it and watching an SLS effect appear.
- **No new syscall validation surface.** The kernel still validates everything it validates today, on the syscalls it dispatches today. The Linux path's validation is: is the caller's personality Linux, is its channel present, is the request well-formed. The semantic validation is the sidecar's, on the operations the sidecar already exposes.
- **Denial of service is bounded by the channel, not by trust.** A task that traps in a tight loop generates a message per trap. The sidecar channel's existing flow control and the park/wake path bound what the kernel will hold; the design's requirement is that a flooded environment *parks*, and cannot saturate a neighbour's partition share. **This must be measured, not asserted** (§10.2 and §10.4); E6's console guard already establishes the pattern — "every send must be accepted by the kernel's flow control (`queued` > 0, so an environment that never reads cannot exhaust the payload pool for the others)".
- **The honest residual.** The sidecar core becomes the security-critical translator for a language with more edge cases than the SLS syscall set. A bug there is a bug in a tenant's own environment (contained by the partition boundary), not a kernel bug — which is the correct blast radius, and is the argument for putting it there rather than in ring 0.

### 3.7 Cost, honestly, and the fast path deferred to a measurement

A forwarded syscall costs a channel round trip plus a park/wake, where a direct dispatch costs a call. For the first user — a static binary doing work, as H2 §2 item #2 defines it — that is acceptable, and the design refuses to argue about it from first principles. Instead:

- **The design requires one measurement before the gate:** a round-trip micro-benchmark of `getpid`-class forwarded syscalls (N ≈ 10⁵) against a direct `do_syscall` of the same shape, reported as a number in the E7 findings. This is a *required artifact*, not a nice-to-have.
- **The fast path is deferred, and its trigger is the census.** If the syscall histogram from §5.5 shows one or two numbers dominating (the likely candidates are memory growth and clock reads), the documented response is a **kernel fast path for those numbers only**, performed under the same personality branch and the same capture — an optimisation of D1's mechanism, not a reversal of D1. It is explicitly *not* "reopen Option A", because D1's rejection rests on C1–C3, none of which a faster dispatch addresses.

### 3.8 What would overturn Decision 1

- The measurement in §3.7 coming back so badly that the first user's actual work misses its gate — and a *documented* fast path (still within the personality branch, still one semantics implementation) failing to close it. The design would then revisit A for a **named, enumerated** syscall list, and would have to answer C1: where the two implementations are reconciled, and what keeps them from drifting.
- The sidecar core being unable to express an operation the census says is required — e.g. an operation whose semantics genuinely are kernel objects (page-table permission changes). The response is a *narrow* kernel-side SLS-level capability the sidecar can ask for, not a Linux semantic layer.

### 3.9 Rejected alternatives, in one line each

- **A — kernel-side interpretation.** Rejected on C1/C2/C3; §3.2.
- **Linux binary inside the sidecar's address space.** Rejected on C2: it reaches every capability the core holds; §3.5.
- **A separate "Linux sidecar" personality.** Rejected as premature: it duplicates the environment's fd/path/permission state, and the roadmap places E7 *inside* an environment (§10). If a later user needs a Linux environment with no POSIX environment, that is a new phase, and D1's mechanism survives it unchanged.
- **Translating the binary (reuse QEMU-SLS).** Rejected: it does not answer E7's question and does not run natively; §4.5.

---

## 4. Decision 2 — architecture: x86-64 first

### 4.1 The ARM64 dependency, measured

The claim "E7 runs on ARM64 in the H2 roadmap" is true of the *H2 roadmap* and does not survive contact with the tree. `arch/arm64/` and `kernel/kernel_arm64.c` (943 lines) are a **translation-and-scheduling smoke harness**, self-described in its own header:

- `boot_arm64.S` (353 lines), `gic.c` (152), `mmu.c` (138), `uart_pl011.c` (38);
- a boot gate that runs SIMI smoke programs at EL1 and EL0 — "smoke EL1 (MISS), smoke EL1 (HIT), LCG EL1 (MISS), LCG EL0 ×2 (HIT, HIT), mem EL0 ×2 … three translations total, each program's EL0 runs re-executing the same cached bytes";
- a bespoke `svc-from-EL0` handshake (`arm64_svc_from_el0`) that stores a user result and `eret`s to a continuation address, with a `VBAR_EL1` vector table;
- a `TTBR0` switch, a fresh user tree, `ELR_EL1`/`SPSR_EL1 = EL0t`, `eret` — an *excursion*, not a process.

What is absent is everything E7 needs to have a customer: a process table and partitions, a capability table, an ELF loader (the loader is `kernel/loader.c` + `user_map_page()`, both x86), a ring-3 spawn/exit path, channels, and any syscall ABI. **This is not "porting the marshaller"; it is the ARM64 port itself**, which the H2 roadmap budgets separately as item #1 (2.5 months, and its gate is memory-model-consistency under 10,000 injected crashes — a prerequisite for trusting *any* concurrent code on ARM64, including a shim).

### 4.2 Why x86-64 is the right first target, not merely the available one

- **Everything E7 depends on exists only there:** the trap stub and its gate, the ELF loader, the park/resume machinery, the process/capability/partition model, the sidecar world, the CI runners (`polyglot-e2e`, `kernel-guards`) and the live cluster.
- **It is the only architecture where a real first user's real binary can be run today** — H2 §7's "dogfood it" argument, applied: the first user is on x86-64 this window.
- **Native execution, not translation, is preserved.** A Linux x86-64 static binary runs *the same instructions*; only the ABI personality differs. This is a genuine and unusual property: the shim is an ABI layer, never an ISA layer.
- **The gate is reachable.** "A static binary doing the first user's actual work … surviving a checkpoint/restore cycle" is reachable on x86-64 as soon as v0.2's persistence lands; on ARM64 it is behind item #1 *and* v0.2.

### 4.3 The bounded ARM64 delta, so this is a decision and not a deferral

The design keeps ARM64 cheap by construction, and the seams are named:

- **What is per-arch and small:** the trap marshaller (a vector entry + `ESR_ELx` decode + the same six-argument capture, in `arch/arm64/`), the syscall **number table**, and the initial-stack builder. These are the only pieces that know the ISA.
- **What is arch-neutral by construction:** the personality, the request struct, the parking, and the entire semantic layer — the sidecar is already Rust and already the translation point. The Linux personality's *semantics* do not change between x86-64 and AArch64; only the numbers and the register names do.
- **What is neither, and is the real cost:** the ARM64 userspace OS. That is item #1's budget, and E7 should not be counted against it.
- **The binary, not the shim, changes target:** `x86_64-unknown-linux-musl` today, `aarch64-unknown-linux-musl` later. H2's "statically linked binaries only" makes this a *build* choice, not a shim feature. Note the pleasant consequence: the shim never needs a translator on either architecture.

### 4.4 This deviates from H2 §2 item #2, and the deviation is stated rather than buried

H2 §2 item #2 says, in bold, "**a Linux syscall ABI shim on the ARM64 kernel, for statically linked binaries only**", with "**Native execution on ARM64. Not translated.**" This design proposes an amendment:

> **Proposed amendment to `AeroSLS-Roadmap-2026H2-v0.1.md` §2 item #2:** the shim targets **x86-64 first**, with ARM64 as its immediate follow-on once H2 item #1's ARM64 port provides a userspace to run in. The constraint set is unchanged (static only; `strace`-measured surface; Rust → Go → conditional CPython; Node out); only the target order moves, and it moves because it is a dependency: on ARM64 today there is no process, no ELF loader and no syscall ABI for a shim to live in. The gate is unchanged, and it is *closer* on x86-64 because the first user is there.

The reasoning for stating it this way rather than silently choosing x86-64 is the same reasoning H2 §7 gives for publishing the verification documents as-is: the roadmap's ARM64 framing is load-bearing for the north star, and a design pass that quietly contradicts its parent document is worth less than one that says which sentence changes and why.

### 4.5 Positioning against QEMU-SLS — the tension H2 asks to resolve deliberately

H2 §2 item #2 flags two routes to foreign binaries and asks that they be positioned. They are not competitors, and the shim's positioning is clean:

- **The shim is native, per-architecture, ABI-only.** It runs *same-ISA* code and changes what the code's syscalls mean. It never translates an instruction.
- **QEMU-SLS translates**, works cross-ISA, and is the route for an x86-64 binary on an ARM64 node. It is slow and it is a differentiator, not a blocker.
- **They compose exactly where it matters:** on ARM64 the shim runs `aarch64-linux-musl` binaries natively while QEMU-SLS runs `x86_64` ones by translation — which is IBM i's own split, as H2 says.
- **E7 does not shrink QEMU-SLS's scope and QEMU-SLS cannot pass E7's gate** (no native execution, and E7's gate is native work). Conversely the shim **must not grow a translator**; that is the line that keeps them from competing by accident.

### 4.6 What would overturn Decision 2

The first user needing ARM64 specifically *and* item #1 landing first. In that case the shim's order flips and §4.3's seam list becomes the port plan — the decision is cheap to reverse precisely because the per-arch surface is small and this section names it.

---

## 5. The executable image contract

### 5.1 What "static binary" means here, exactly

The contract is what `kernel/loader.c` already implements, stated positively so the census can be run against it:

- **`ET_EXEC`, statically linked, no `PT_INTERP`.** Absolute `p_vaddr`s; mapped as-is; `e_entry` is the entry.
- **Built non-PIE — and that is a build flag, not a default.** A **static-PIE (`ET_DYN`) image is refused in v1**: it needs a load bias and self-relocation, which is loader work this pass deliberately does not open (§2.3's "no loader work" claim depends on this). **v0.1 of this document claimed `x86_64-unknown-linux-musl` produces non-PIE static executables. Measured, that was wrong**, and the first census run is what measured it: the target emitted `ET_DYN (Position-Independent Executable file)` and the census tool refused to trace it (`is ET_DYN (PIE)`) — the behaviour §5.5 asks that tool for. `-C relocation-model=static` is the fix (measured: `Type: EXEC`, no `PT_INTERP`, no `DT_NEEDED`, runs), and `backup/aerobackup/.cargo/config.toml` pins it for the musl triple so the crate owns its own contract. Note the mechanism, because it is a trap: cargo reads `.cargo/config.toml` relative to the **current directory**, not the manifest, so the `--manifest-path` form from the repo root silently builds a PIE that this section refuses. The census tool re-checks the contract on every run rather than trusting the config. Go's `exe` buildmode is non-PIE by default; that claim is **not** measured here and should be re-checked the same way if a Go candidate is ever designated.
- **Refused with a named reason, not a crash:** dynamic (`PT_INTERP` present), wrong class, no program headers — all refused up front with the loader's existing messages extended to name the personality requirement.
- **The image is not an SLS program and is not treated as one.** Personality is explicit at spawn (§5.6/§3.5), never inferred from bytes — inference would make the personality a function of an attacker-controlled file.

### 5.2 The initial stack and auxv

The native sidecar path enters `_start` with no arguments; a Linux binary expects the System V initial stack (argc/argv/envp/auxv) and a static libc start-up that reads it. This is a bona-fide deliverable and the kernel must build it:

- **argv/envp are the environment's to define** — the sidecar knows the command line and the environment (it is the thing that ran `exec`, Phase 2 §4.3); they cross on the spawn request. **Landed** as the manifest's `ARGENV` record (`SIDECAR_TAG_ARGENV`, format reference `user/proto/src/manifest.rs`): `argc u16, envc u16`, then the argv and envp strings each as `len u16 + bytes`, bounded (8/16 entries, 512 NUL-terminated bytes — the kernel's fixed parse arena) so over-bound or malformed records are refused BY NAME (`CAP_ERANGE`/`CAP_EINVAL`), never truncated. An **absent** record is the v1 answer — `argv = { name }`, an empty environment — so every manifest built before the record exists runs byte-identically; a record with no argv entries falls back to `{ name }` as argv[0]. The spawn sites that first cross it are the system POSIX sidecar's three Linux children (the probe, the census candidate and the designated first user — the pair crosses the same record, so image format stays its only difference), whose envp is this environment's default (`PATH=/bin`, `HOME=/root` — the shell's own default env, honest about time: these spawns run at boot, before any `export` exists).
- **auxv entries required by static musl/glibc start-up:** `AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_ENTRY`, `AT_BASE`(0), `AT_PAGESZ`(4096), `AT_HWCAP`(0), `AT_HWCAP2`(0), `AT_CLKTCK`(100), `AT_RANDOM`(16 bytes from `kernel/entropy.c` — absent, stack canaries are worthless), `AT_SECURE`(0), `AT_PLATFORM`("x86_64"), `AT_EXECFN`. `AT_SYSINFO_EHDR` is **absent**, which is correct and must be explicit: there is no vDSO, so `clock_gettime`-class calls are real syscalls and must be in the census (§5.5). This list is not prose to be transcribed later — `tools/e7_stack_launcher.c` (§5.5) is the same list as code, which is how the vDSO declaration gets measured instead of believed.
- **The stack size is a real question, not a formality.** `PROC_USER_STACK_PAGES` is 8 (32 KiB) for ring-3 processes today, and its own comment records that it grew once already for a large ABI struct. A static libc's start-up plus argv/envp/auxv may exceed 32 KiB of headroom on a real workload; the design requires the Linux task's stack to be a **separately-sized, partition-charged allocation** decided by measurement, not by inheriting the native constant.

### 5.3 TLS — the one kernel change that cannot be avoided

A static musl or glibc binary sets up thread-local storage early and does it via **`arch_prctl(ARCH_SET_FS, ...)`**, which writes `MSR_IA32_FS_BASE` (0xC0000100). **Nothing in the tree writes that MSR.** `syscall_gate_init()` (`arch/x86/user_paging.c`) programs `EFER`, `STAR`, `LSTAR`, `SFMASK`, `MSR_KERNEL_GS_BASE` and `MSR_GS_BASE` — FS is untouched, and `swapgs` swaps the GS base, not FS, so the kernel's own trap mechanism neither provides nor conflicts with it.

So the deliverable is explicit and small:

- a per-process `fs_base` (a field on `struct ProcessDescriptor`, beside `cr3`), set by the forwarded `arch_prctl(ARCH_SET_FS)` — the sidecar decides *whether* the task may, the kernel does the write;
- `wrmsr(MSR_IA32_FS_BASE, ...)` on context switch and on syscall return, so a switch between two Linux tasks does not carry the wrong TLS base;
- a user-addressable data selector for `%fs` (the GDT arrangement must be confirmed as part of this deliverable rather than assumed — `MSR_STAR` currently yields a user CS of `0x23` and SS of `0x1B`, and the flat data selector `%fs` needs is a GDT question, not a STAR question);
- zeroing on process create/teardown, in the same place the cap table is torn down, so a reused slot cannot inherit a stale base.

`arch_prctl` is one syscall; the mechanism it needs is one MSR and one field. It is called out as its own section because it is the single place where the *kernel* — not the sidecar — is unavoidably part of a Linux semantic, and pretending otherwise would make the "the kernel interprets nothing" claim false in the one case a reader would check.

**Measured, not predicted.** The census of the designated candidate contains exactly this call, with its argument: `arch_prctl(ARCH_SET_FS, 0x47a1a8) = 0` (syscall 158, `tools/linux_syscall_census.txt`). So the one kernel-side Linux semantic in this design is not a hypothetical a reader has to take on trust — it is in the first user's first hundred syscalls, and it is the earliest thing the shim must implement.

### 5.4 Memory growth → the environment budget

A static Rust or Go binary grows its heap: `brk` and `mmap`/`munmap`/`mprotect`. These are not file operations and they are not sidecar-local: the memory has to be real.

- The design maps them onto **E3's environment budget** — the contiguous region the environment already owns (`SYS_SLS_ALLOC_REGION` 320, `kernel/cap.h`) — with `brk` as a bump pointer inside a reserved span and `mmap` as sub-allocations of the same budget, both refused with `ENOMEM` when the budget is exhausted. The hard ceiling is the partition's frame quota, kernel-enforced, because the frames are charged by `allocate_physical_ram_frame_for_partition()`.
- **`W^X` is a census question, not an assumption.** `mprotect(PROT_WRITE|PROT_EXEC)` is where H2 §2 item #2 draws the Node line ("V8 needs a JIT, which needs W^X page flipping"), so the shim's `mprotect` must **refuse simultaneous write+execute** and the census must show whether the first user's binary ever asks. Refusing it is also the honest place to prove H2's Node exclusion rather than restating it.
- The sidecar decides sizes (policy); the kernel provides frames (mechanism) — the same split as everywhere else.

**Both of those are measurements now, not assumptions.** The candidate's only `mprotect` is `mprotect(0x723f031ee000, 4096, PROT_NONE)` — it never asks for `W|X`, so §5.4's refusal can be enforced without blocking E7's gate, and H2's Node exclusion is provable at that line rather than restated. Its heap grows by `brk(NULL)` → `brk(0x3f968000)`, so the bump-pointer mapping is on the path rather than an optimisation for a case that might not arise.

### 5.5 The syscall census — the required artifact

H2 §2 item #2's constraint is "chosen by what the target binary actually calls — measured with `strace`, not guessed". So the first deliverable of E7 is **not code**; it is the measurement, and the instrument for it now exists: `tools/linux_syscall_census.sh`, driven by `tools/linux_abi_candidate.conf`, which

- **refuses to measure anything until a candidate is designated** — and a designation is not a name but a name plus a **provenance**, the real existing operation in this tree the binary is a rewrite of. A placeholder has no provenance, so a census of hello-world, or of whatever binary happened to be lying around, is not a run that is quietly wrong; it is a run that does not happen. Twenty teeth (the tool's `--selftest`) observe each refusal firing, including the negative controls — an artifact whose sha256 *does* match the designated binary passes, and a declaration that *is* backed by a proof passes;
- **enforces §5.1's image contract on the real file** before tracing it — `ET_EXEC`, no `PT_INTERP`, no `DT_NEEDED`. A PIE or dynamic candidate is refused with the reason rather than censused;
- traces with a **full `strace -f` trace**, not `-c`'s summary table (whose column layout shifts with the `errors` column, so a parser that misreads it yields a plausible, wrong histogram — the failure this tool exists to prevent), and keys each name to its **number** from the kernel's own `unistd_64.h`, because the shim dispatches on numbers;
- records the candidate's identity in the artifact's header — name, provenance, target and **sha256 of the traced binary** — so `--check` re-verifies that the artifact still describes the binary it claims to. **A stale census is refused**, which is what makes "re-run it when the binary changes" a guarantee rather than advice;
- **refuses a declared vDSO backstop that has no measurement beside it.** The declaration is the part of the census the trace structurally cannot produce, so it is the part most able to be wrong without looking wrong; `--check` therefore requires a proof (§5.5, `tools/linux_syscall_census_novdso.txt`), and the proof's own row set has to be the census's minus one `execve` plus the declared calls — so a declaration *and* a claim that nothing else moved are both machine-checked, on a host with no `strace` at all.

Then the shim's dispatch table is built **from that artifact**, and a host test asserts that **every number in the artifact has a table entry** (§10.1) — delete one entry and the test reddens by name. The artifact and its consuming test arrive together.

**The census has now run, and the first real run caught two wrong assumptions — which is the argument for having an instrument instead of a prediction.** The artifact is `tools/linux_syscall_census.txt`: 26 traced syscalls from `aerobackup`'s static musl build, plus one declared vDSO backstop.

- **The musl target builds static-PIE by default**, so §5.1's image contract refused the first attempt outright, before any table could be built on top of it. §5.1 now carries the correction and the flag.
- **The trace contained no clock syscall although the binary demonstrably read the clock** — its own output filename is a timestamp (`sls_storage-20260928-234732.img`) — so the read happened in the vDSO. That is a gap in the *measurement*, not in the binary: the shim has no vDSO by §5.2's own decision, so those calls become real traps there, and a **missing** requirement is indistinguishable from a requirement nobody needed. The tool therefore requires `CANDIDATE_VDSO_SYSCALLS` to be set — with the literal `none` as a distinct, explicit answer, because an unset key cannot be told apart from an oversight — and writes the set into the artifact as `# vdso-backstopped: <number> <name>` lines that `--check` validates against the kernel header and against the traced rows. `clock_gettime` (228) is the one declared; `gettimeofday` and `time` deliberately are not, since declaring a syscall with no evidence would be inventing a requirement rather than measuring one.

**And the declaration is measured now, not inferred.** Refusing to guess was the right response, but the evidence behind the one entry was weaker than the rest of this design accepts: a filename that is a timestamp proves the binary read a clock, not that the read went through the vDSO, and a declaration resting on it is the same "plausible and wrong" artefact the tool refuses everywhere else. So `--prove-vdso` starts the same candidate a second time with **no `AT_SYSINFO_EHDR` in the auxv** — the host stops behaving like a host with a vDSO — and the declared syscall has to appear in the trace as a real call while nothing else moves.

Removing one auxv entry is not something `PTRACE` can do here: the run is already traced by `strace`, and `PTRACE_TRACEME` while already traced fails with `EPERM`. So `tools/e7_stack_launcher.c` loads the `ET_EXEC` itself and builds the initial stack — maps the `PT_LOAD` segments, copies them in, applies their protections and `GNU_RELRO`, fills `argc`/`argv`/`envp`/`auxv` at the top of a fresh stack, and jumps to `e_entry`. Its auxv is an explicit list in its source, and the list simply does not contain `AT_SYSINFO_EHDR`. A single `write(2)` marker before the jump is the boundary in the trace, so the loader's own opens and mmaps stay out of the proof's rows and the two runs are comparable row for row.

**What the proof does and does not establish, precisely.** The launcher does not remove the vDSO *image* — the kernel mapped that when it exec'd the launcher, and nothing unmaps it. It removes the auxv *entry*, which is the only handle a static libc has on one: musl reaches the vDSO through `__vdsosym`, which scans `libc.auxv` for `AT_SYSINFO_EHDR` and has no second route. So the run presents AeroSLS's condition as musl experiences it, which is what the declaration is about — but it is not "a machine with no vDSO", and a libc that found the vDSO by scanning `/proc/self/maps` would still find one there. That residual is stated rather than papered over, because it is the boundary of what this measurement can see. The result:

| Run | Rows | The clock | The start |
|---|---|---|---|
| census (`strace`, kernel-built stack) | 26 | **absent** | `59 execve 1` — the target's own start |
| no-vDSO run (`--prove-vdso`) | 26 | **228 `clock_gettime` 1** | absent — the launcher is what was exec'd |

The other 25 rows are identical, counts and all. `clock_gettime(CLOCK_REALTIME, …)` appears exactly once, and it is unambiguously the target's: the launcher never reads the clock itself, and the call is below the marker. The mechanism is visible in the binary rather than argued from it — `__clock_gettime` calls through a `vdso_func` pointer and only falls back to `syscall 228` when that pointer is null, while `cgt_init` resolves it with `__vdsosym`, which scans `libc.auxv` for `AT_SYSINFO_EHDR`. That is the entry the launcher omits, so the fallback is the path taken, and it is now an observation instead of an inference about a filename.

**The proof then found a third defect, in the census's own fixture.** Comparing two row sets is only meaningful if both runs saw the same input, and they did not: `CANDIDATE_SETUP` created its six backups with `mkdir -p`, so a second `--run` inherited whatever the previous run had left behind — and this workload's whole tail (how many entries are `lstat`ed, how many are pruned, how many lines are printed) depends on how many files are already in the directory. The artifact first checked in recorded `lstat 8 / unlink 4 / write 103`; a clean fixture gives `lstat 7 / unlink 3 / write 102`, and the artifact has been regenerated to match (`CANDIDATE_SETUP` now clears its workspace). The 26-row **set** is unaffected — which is the part that matters, since the set is what the table is built from (§5.6) — but a setup that depends on leftovers is not a setup, and two censuses of the same binary on the same machine should not have been able to disagree.

**And the check is now a guard, which it could not honestly be before.** Its `--run` and `--check` modes both refuse to proceed until a candidate is *designated*, and there is no honest `# GUARD-KIND:` classification for "no first user has been designated yet" — that missing input is a business decision, not a build artefact and not a live cluster. So wiring it in earlier would either have reddened CI for a reason nothing in CI could fix, or added a permanent silent skip. With the designations, the census and the proof in the tree, `tests/linux_abi_census_check.sh` now runs in the guard set (`run_checks.sh`, and through it `deploy.sh`'s gate), and it is source-only: three checked-in files plus the kernel's syscall-number header, no `strace`, no cargo, no built binary. It carries no `GUARD-KIND` marker on purpose, because as written there is nothing it can legitimately be unable to do on a Linux runner.

Two things it adds beyond execing `--check`, both of them failures this repo has paid for before. **A vacuity control:** a pass requires *both* `census: ok` and `proof: ok` in the output, because a check that silently stopped checking would still exit 0 — the `commands_doc_check.sh` failure, where a missing `python3` made every capture empty and every check report `ok`. **And a broken instrument is not a missing prerequisite:** exit 2 means "cannot check" and is a skip, but bash exits 2 for a syntax error too, so a typo in the census tool would present as an owed skip and CI would stay green. Only an `ABORT:` line earns the skip; anything else exiting 2 is a failure.

`tests/linux_abi_census_check_smoke.sh` proves those teeth on every push, with three violations of deliberately different kinds — the proof losing its `# vdso-proved:` line (a declaration with no measurement), one ordinary row disappearing from the proof (the two runs of the same candidate disagreeing about something that is not declared), and a declaration naming a syscall the census already traced (two claims about one call) — then a restore check that both artifacts are byte-identical again. It edits committed artifacts and restores them with `git checkout --`, so it aborts if either is dirty at entry, and it is a `*_smoke.sh` precisely because it must never run in the guard gate that `deploy.sh` blocks on.

#### 5.5.1 The designation: what the constraints leave standing

The one input this design cannot supply is the census target — and that is not a gap in the design, it is what the roadmap says. `AeroSLS-Roadmap-2026H2-v0.1.md` §5 states it in bold: "**There is no named first user.**" It calls that the single largest risk in the roadmap, says the choice it feeds — "which language, which syscalls, which binary" — is unanswerable without one, because "`strace` on their actual workload is the specification", and gives the answer the project can act on: dogfood it, because "if Gridworkz runs something real on it, the first user exists on day one and is you" (§5, §7).

Verified against the tree, **no candidate binary exists yet either.** There is no Go source anywhere in the repository and no `std`-linked Rust binary; every real operational workload is bash or Python (`backup/backup.sh`, `backup/restore.sh`, `monitor/health_check.sh`, `deploy/deploy.sh`, `utils/*.py`); and the only static ELFs in the tree are the kernel images themselves (`my_sls_kernel.bin`, `sls_arm64_kernel.elf`), which are not Linux-ABI candidates at all. So the target must be **built**, not found — and §7 of H2 names the four operations to choose from. Evaluated against E7's own constraints:

| Named operation | What exists in the tree | Verdict |
|---|---|---|
| **logs** | rotation is `pm2-logrotate` plus a `logrotate` template (`AeroSLS-Operational-MVP-Roadmap-v0.1.md`) | **No candidate.** That is configuration of existing tools — there is no workload to write, so nothing to census. |
| **billing** | the only billing is `SlsUserPortal.tsx`, which `AeroSLS-Navigator-Parity-Gap-Roadmap-v0.1.md` §118 calls "a complete, self-aware fictional SaaS billing" demo; usage metering already exists in-kernel (`kernel/usage_metering.c`) | **No candidate.** Fictional, and a TSX web app is not a static binary. |
| **monitoring** | `monitor/health_check.sh` — real, running every two minutes against `https://aerosls.kubeworkz.io/api/health` | **Eliminated by E7's own scope**, and this is the useful elimination: it needs outbound TCP/TLS and a webhook POST — `socket`/`connect`/`sendto` — which §9 and POSIX-Environments v0.2 §12 item 2 place *after* E7. A census target must be reachable with the v1 core. |
| **backup** | `backup/backup.sh` (Operational-MVP Phase D) — real, hourly and daily via cron, with a retention policy; its core loop copies the storage image, then prunes to a retention count | **The recommendation.** No sockets, nothing inherently threaded, and its work is verifiable (a snapshot appears and the retention count holds). It exercises exactly §5.6's core — `openat`/`read`/`write`/`fstat`/`rename`/`unlink`/`getdents64`/`fsync`, plus `getrandom` for integrity — and it is the one named operation that can be **finished** inside a static single-threaded binary on that core. |

**Recommendation:** a static, single-threaded Rust rewrite of `backup/backup.sh`'s snapshot-and-retain core for `x86_64-unknown-linux-musl`, non-PIE, with the `pm2 stop`/`start` and the health poll left out — those are host orchestration, and the health poll is precisely the networking that eliminated the monitoring candidate. Its `CANDIDATE_PROVENANCE` is then `backup/backup.sh`: a real, running production operation, which is the anti-placeholder property the tool checks.

**The decision is recorded as taken: the backup operation is the designated target.** Which workload becomes the first user is a business decision with a product consequence — H2 §3's whole point is that the platform is proven against the first user's shape — and the reason the tool exists is so the decision is *explicit* rather than assumed. `tools/linux_abi_candidate.conf` now carries `CANDIDATE_STATE=designated` with `CANDIDATE_PROVENANCE=backup/backup.sh`, which leaves E7's remaining blockers mechanical rather than editorial: the candidate crate must be written, `x86_64-unknown-linux-musl` added to the toolchain, and `--run` executed on a host with `strace`. **Until that run happens, `--check` reports a named skip — "designated but not yet censused" — and never a pass.**

The census is also the input to two other decisions this document deliberately leaves as measurements: the fast-path list (§3.7) and the stack size (§5.2).

### 5.6 The syscall subset — measured

v0.1 of this section carried a prediction "offered so the census can falsify it". The census has run, so this is the measurement: `tools/linux_syscall_census.txt`, 26 traced syscalls from `aerobackup`'s static musl build plus the one declared vDSO backstop. **Four of v0.1's predictions were falsified**, and they are the reason the table is now an artifact rather than prose:

- **The `*at` forms are not what this candidate calls.** It uses the legacy `open` (2), `stat` (4), `lstat` (6), `unlink` (87), `rename` (82), `mkdir` (83) — *not* `openat`/`newfstatat`/`unlinkat`/`renameat2`/`mkdirat`. A shim built from the v0.1 prediction would have implemented the wrong nine syscalls and none of the right six, and it would have looked finished doing it.
- **No `futex`, no `clone`, no `clone3`.** Measured absent — D4's single-threaded-first ordering confirmed for this candidate rather than argued.
- **No `getrandom`, `nanosleep`, `ioctl`, `pipe2`, `dup`, `readlink`, `uname` or `gettid`.** Absent. §5.2 predicted `getrandom` would matter; it does not, because musl takes its entropy from the `AT_RANDOM` the kernel supplies in the auxv rather than from a syscall — the §5.2 decision `AT_RANDOM` must be present is what makes the syscall unnecessary.
- **`clock_gettime` is absent from the trace and is nevertheless required**, for the vDSO reason §5.5 records — and that reason is now measured rather than inferred: re-run with no `AT_SYSINFO_EHDR` in the auxv, the candidate makes the call as real syscall 228 and nothing else in its row set changes (§5.5).

The measured set, by destination:

| Numbers | Syscalls | Destination |
|---|---|---|
| 0, 1 | `read`, `write` | the copy's bulk plus the console; sidecar fd table |
| 2, 3, 8, 217 | `open`, `close`, `lseek`, `getdents64` | sidecar fd table + paths (Phase 2 §3–§4) |
| 4, 6 | `stat`, `lstat` | sidecar (retention reads mtimes; the tier listing stats every entry) |
| 74, 77, 82, 83, 87 | `fsync`, `ftruncate`, `rename`, `mkdir`, `unlink` | sidecar + §5.4's budget; `rename` is what makes the `.img.partial` → final swap atomic |
| 9, 10, 11, 12 | `mmap`, `mprotect`, `munmap`, `brk` | §5.4, budget-backed; `W|X` refused |
| 13, 14, 131 | `rt_sigaction`, `rt_sigprocmask`, `sigaltstack` | accepted and recorded; **never delivered** — the environment owns lifecycle (E5: `env destroy` is the kill path) |
| 158 | `arch_prctl(ARCH_SET_FS)` | §5.3's kernel MSR — the one kernel-side semantic, measured with its argument |
| 218, 231 | `set_tid_address`, `exit_group` | sidecar-local; on exit the sidecar reaps and the frames return to the partition |
| 59, 7, 72 | `execve`, `poll`, `fcntl` | the spawn path and std's own plumbing |
| 228 | **`clock_gettime`** (declared, vDSO-backstopped) | sidecar-local; there is no vDSO, so on AeroSLS this one is a real trap — measured, not inferred: §5.5's proof run without `AT_SYSINFO_EHDR` is what shows it |

Counts are deliberately absent from this table: they shift with the tier's population (a run with more backups to prune makes more `lstat` and `unlink` calls) and the *set* is what the table is built from. The artifact keeps the counts for the run that produced it.

**What this table means for the phase boundary:** 27 syscalls is small enough to implement deliberately one at a time with a tooth each, and the two hardest are already named — `arch_prctl(ARCH_SET_FS)` (§5.3) and the vDSO-backstopped `clock_gettime` (§5.5). It also means the *file* half of the shim is mostly the sidecar's existing fd table and path resolution (Phase 2 §3–§4) rather than new kernel semantics, which is the strongest available confirmation of D1: the syscalls this candidate makes are the syscalls the sidecar already answers for its own applets.

### 5.7 errno and the return convention

The two conventions already agree in **shape** and disagree in **numbering**, which makes this small and testable rather than open-ended:

- both put a negative value in `rax`; Linux requires `[-4095, -1]`, SLS returns the `CAP_E*` family as negatives widened through `u64`;
- the family is enumerable — `CAP_EINVAL` (−1) … `CAP_EPERM` (−12) plus `CAP_ERR_PROTO` (`kernel/cap.h`), with `CAP_NONE` (0xFFFF) used as an out-param sentinel in handlers such as `sys_sls_dev_mmap` (`kernel/chan.c`) — so the mapping is a **pure function with a host test over every code** (§10.1), and the sentinel must be translated, never leaked to a Linux task as a return value.

---

## 6. Decision 3 — dynamic linking: not in the shim

§12 of the POSIX roadmap parks dynamic linking here ("E7's design document decides it"). **Decision: refused for v1, and it is refused in a way that does not need a second decision later.**

The reason is structural rather than budgetary. A dynamic binary is a `PT_INTERP` image plus ELF objects; the thing that performs symbol resolution and relocation is **`ld.so`, a userspace program**. There is no `dlopen` for the kernel to implement, because there is no `dlopen` anywhere in a dynamic-link scenario that is not ordinary userspace code:

- H2's constraint is the policy: "No dynamic linker, no `.so` loading, no `dlopen`. Static musl or nothing." That remains the v1 contract, and §5.1 refuses `PT_INTERP` up front.
- **If dynamic linking is ever needed, the work is more syscalls, not a shim feature** — enough of `openat`/`mmap`/`read`/`fstat` that `ld.so` can load itself, find its objects, and relocate them (plus `AT_PHDR`/`AT_BASE` in the auxv and `mmap(PROT_EXEC)` in the budget, both already in §5). The shim would then run the *guest's* `ld.so` exactly as it runs the guest's binary.
- **Falsifiable trigger to revisit:** a named first user brings a dynamically linked binary *and* a static rebuild is impossible (a vendored blob, a licence, a toolchain that cannot link static). The response is then an `ld.so`-capable syscall set on a written schedule — and note that the answer is never "give the Linux task more authority", because `ld.so` needs no authority the static case lacks.

---

## 7. Decision 4 — the target order, sharpened: Rust, then Go behind threads

H2 §2 item #2's order is "static Rust or Go first", with the reason "no runtime to speak of, no dynamic linking, small syscall footprint". The design pass sharpens it, because the two are not equivalent in this kernel:

- **Static Rust, single-threaded — first.** Its start-up path is small, its syscall footprint is the §5.6 core, and it does not need thread creation. This is the proof the shim works, and it exercises every mechanism in §3 and §5 except threads.
- **Static Go — second, and gated on thread-group support.** Go's runtime creates OS threads unconditionally (`runtime.newm`/`sysmon`), which needs `clone`, true per-thread kernel stacks, `futex`, and a *shared* address space among threads. This kernel gives every process its own page table via `user_clone_page_table()` and has **no `clone`, no `futex`, and no thread-group concept anywhere** in `kernel/`. So "Rust or Go first" cannot be read as "either, now": Go requires a deliverable that has not been scoped, and it should be scoped rather than discovered.
- **Static CPython — conditional, as H2 scopes it, and the catch stands.** H2's own honest caveat is quoted in the roadmap: without `dlopen` there are no binary C extensions, "which removes most of why people choose Python. Scope this honestly or not at all." Nothing in this design changes that; the census decides it.
- **Node — out.** H2's grounds (JIT → W^X page flipping → threads → a scheduler contract this kernel does not have) are unchanged, and §5.4's refusal of `mprotect(W|X)` is where the exclusion becomes an enforced property rather than a paragraph.

---

## 8. The gate: two halves, one of which is v0.2's

The inherited gate is "a static binary doing the first user's actual work, running natively, **surviving a checkpoint/restore cycle**". §10 makes the second half a hard prerequisite because `checkpoint_trigger()` persists no environment state (§2.6, measured). The design's contribution is to say what "surviving" must mean, and to show that D1 makes it *smaller* than it looks:

- **What must be captured and restored:** the environment's sidecar address space, its capability table, its channels, its ramdisk contents (v0.2 §12 item 1), **plus** the Linux task's address space, its register/stack state, its `fs_base` (§5.3) and its single channel endpoint. E3's per-environment region is contiguous and partition-charged, which helps: it is one describable span, not a scattered set of allocations.
- **Why forwarding reduces the problem:** a task parked in a forwarded syscall is already in a fully-saved, well-defined state — `struct CapParkCtx` plus `park_req`/`park_syscall`/`resume_sysret` are exactly the values needed to re-run that syscall after restore. A checkpoint does not have to invent a way to save a task mid-operation: it can **serialize a parked task and restore it parked**, and the resume path that already exists finishes the syscall. A blocking-syscall design would have to solve this from scratch; a park-based one inherits the solution.
- **The gate test, concretely:** the first user's static binary runs its actual work inside an environment; the system checkpoints; the environment is restored (reboot or reload); the binary's work **continues and completes**, with its open descriptors still open and its heap still consistent. Passing means both halves — the work and the cycle — observed in one run, in the same spirit as E6's guard, which refused to accept "two empty streams" as isolation.
- **Honest note:** this half is **not E7's to build** and E7 cannot pass its gate before v0.2 lands (§12, §13). E7's own deliverable list stops one step short of it, and §10.4 is written so the two halves are reported separately rather than blurred.

---

## 9. Scope, and explicitly not in scope

**In scope** (E7's own work): the personality field and its spawn plumbing; the trap-path Linux branch and the six-argument capture; the request struct, channel delivery and park/resume; the initial stack and auxv; `fs_base`/TLS; error mapping; the `brk`/`mmap` budget mapping; the census artifact and the table built from it; the host teeth and the boot guard; the round-trip measurement.

**Explicitly not in scope:**

- **Dynamic linking** (§6) — refused, with a named trigger and a scoped path.
- **Threads, `clone`, `futex`, thread groups** (§7) — refused in v1, named as Go's gate.
- **The ARM64 rig** (§4.3) — it is H2 item #1's, not E7's.
- **Environment checkpoint/restore** (§8) — v0.2 item 1, E7's gate but not E7's code.
- **A translator** (§4.5) — QEMU-SLS's, and the line that keeps the two from competing.
- **Nested environments** — already closed in the POSIX roadmap §12 for the same reason LPAR §9 closed nested partitions.
- **Signal delivery to Linux tasks** (§5.6) — accepted and recorded, never delivered; E5's `env destroy` is the lifecycle.
- **Networking syscalls** (`socket`, `connect`, …) — v0.2 item 2's, and H2 §2 item #2 does not require them for the first user.

---

## 10. Verification plan

Every claim above with a teething arm, in the repo's existing shape: host tests in `tests/run_all.sh`, a boot guard in `tests/run_checks.sh`, and a smoke file that asserts each arm reddens **for the clause it names** — the pattern E5 and E6 established, including the vacuity control ("two empty streams are not isolation").

### 10.1 Host teeth (no boot) — cheap, and they cover the parts that are pure functions

- `tests/linux_errno_map_host_test.c` — the §5.7 table, asserted **exhaustively over every `CAP_E*` code** including the `CAP_NONE` sentinel, in both directions where the reverse is defined. *Tooth:* change one mapping; the test reddens by name.
- `tests/linux_syscall_table_host_test.c` — **every number in `tools/linux_syscall_census.txt` has a dispatch-table entry**, and every entry names a class. *Tooth:* delete one entry; the test reddens naming the missing number. This is the tooth that makes §5.5's census load-bearing instead of decorative. It arrives with the artifact, since there is no table to check it against until the census has run — and `tests/linux_abi_census_check.sh` (the `--check` guard, §5.5) lands at the same moment.
- `tests/linux_abi_census_check.sh` — execs `--check`, which re-verifies the census against the binary's sha256 **and requires each declared vDSO backstop to be proved** by `tools/linux_syscall_census_novdso.txt`, whose rows must be the census's minus one `execve` plus the declared calls. *Teeth (in `tests/linux_abi_census_check_smoke.sh`, three of different kinds):* drop a `# vdso-proved:` line, delete an ordinary row from the proof, or declare a syscall the census already traced; each reddens naming the violation, and a restore check proves both artifacts are byte-identical afterwards. No boot, no `strace`, no toolchain — which is why it can be a guard at all, and why it runs in `verify` on every push rather than only on a census host.
- `tests/linux_initial_stack_host_test.c` — the §5.2 builder produces a well-formed argc/argv/envp/auxv image: required `AT_*` present, `AT_RANDOM` non-zero and non-repeating, pointers consistent, alignment per the psABI. *Tooth:* drop `AT_PHDR`; the test reddens on the required-entry assertion.
- `tests/linux_personality_host_test.c` — the request struct's size/ABI and the personality-check predicate. *Tooth:* invert the predicate; the test reddens on the "SLS number must not be interpreted as SLS" case.

### 10.2 The boot guard — proposed `tests/linux_abi_shim_check.sh`

On the unified boot, create one environment, run the first user's static binary inside it, and assert, in one run:

- **the binary actually ran**, on its own evidence — the same in-band-identity discipline E6 used, so echoing the request cannot fake it;
- **the capability census**: the Linux task's capability table contains **exactly one entry, the channel to its environment's sidecar** (§3.5), read from the same kernel source the process listing reads;
- **isolation**: two environments, each with its own Linux task, each stream containing only its own markers — with the cross-partition witness E6 established (same index in a second partition, proven live, then paused);
- **attribution**: the sidecar's reply is the only path by which the task's output appeared;
- **flow control under a flood**: a task issuing syscalls in a tight loop parks and does not disturb its neighbour's partition share (`queued > 0`, the E6 pattern);
- **the round-trip number** (§3.7) is printed and recorded.

### 10.3 The named sabotage tooth — routing

The decisive arm, because it tests the security property rather than the feature:

- **`E7_TOOTH=personality-fallthrough`** — make a Linux-personality task's trap fall through to the number chain instead of the forwarding path. **Expected: red**, and specifically on an *SLS effect*: `rax = 105` reaching `sys_sls_allocate` (or another SLS number reaching its arm), observed as a value a Linux `setuid` could not produce. Every other assertion stays green, so the failure is the branch's, not a blanket crash. This is the arm that proves §3.6's "structural, not a filter".
- **`E7_TOOTH=no-cap-channel`** — spawn the Linux task without its channel; red on the capability-census clause and on the first forwarded syscall **only**, with the boot clauses still green (the vacuity control: a guard that never forwarded anything and still claimed forwarding is the failure this arm exists to catch).
- **`E7_TOOTH=leak-caller-regs`** — capture the argument registers *after* a call rather than before. **Expected: red on a wrong-argument clause**, and this arm exists because `arch/x86/user_paging.h` records a live failure of exactly this shape. It converts §3.4 from a note into a check.
- **`E7_TOOTH=fs-base-unswitched`** — skip `fs_base` restore on switch; red with two Linux tasks whose TLS regions differ.
- **`E7_TOOTH=w+x-allowed`** — permit `mprotect(PROT_WRITE|PROT_EXEC)`; red on the §5.4 refusal clause, which is H2's Node exclusion as an enforced property.

### 10.4 The gate run — reported in two halves

The gate is only "passed" when both halves are observed **in one run**, at which point the report says so in one line. Until v0.2 lands, the run reports the halves separately — work-only (green or red) and cycle (not attempted, with the reason) — so a partially-capable shim is never described as gated.

### 10.5 What has landed — increment 1 (the contracts)

E7 starts where this tree always starts: the record layer, before any
mechanism. Three of §9's in-scope items are pure functions with no
dependency on the trap path, the sidecar or a boot — the errno map (§5.7),
the syscall classification table built from the census (§5.6), and the
initial stack/auxv builder (§5.2) — and they are the contracts every later
increment consumes. They landed together as `kernel/linux_abi.h` /
`kernel/linux_abi.c` (in the x86 image's `X86_C_SRC`), each with the host
test §10.1 named:

- `tests/linux_errno_map_host_test.c` — 51 checks, exhaustive over both
  families (all twelve `CAP_E*`, all fourteen `CAP_ERR_*` including the
  `CAP_ERR_OK` success arm), the `CAP_NONE` sentinel's translation to
  `-EIO` (§5.7's "translated, never leaked"), the range law (every result
  is 0 or inside `[-4095, -1]`, musl's errno window), and the reverse map
  **exactly where it is defined**: the twelve unique-preimage codes
  round-trip, and the five collapse errnos (`EPERM`, `ENOMEM`, `EINVAL`,
  `EIO`, `EBUSY`, which two or three CAP codes each map onto) return 0
  rather than guess which kernel-side truth the errno no longer carries.
- `tests/linux_syscall_table_host_test.c` — 42 checks, parsing
  `tools/linux_syscall_census.txt` itself (traced rows *and* the declared
  vDSO backstop) so the artifact is the specification rather than a
  transcription: every one of the 27 numbers has a non-UNKNOWN class, the
  class of each is §5.6's destination asserted by name, `mprotect` carries
  `LSC_F_REFUSES_WX` (§5.4's W^X refusal pinned against the census's own
  row — H2's Node exclusion as a table property), and the number-space
  overlaps 105/160/320 are UNKNOWN: no entry, no semantics, no path to a
  `SYS_` arm. The table's length is pinned to the row count, so a future
  census run that adds a syscall reddens here until the table grows.
- `tests/linux_initial_stack_host_test.c` — 40 checks over a poisoned
  region: the canonical psABI image (argc lowest, strings highest — the
  first draft of the builder was inverted, and the pointer-walk is what
  caught it), §5.2's thirteen required `AT_*` each present exactly once
  with the value named, `AT_SYSINFO_EHDR` **absent** by decision (the vDSO
  honesty requirement asserted at the source, not at the trace),
  `AT_RANDOM` copied verbatim from the caller's entropy, a 16-byte-aligned
  entry rsp checked on the ABSOLUTE address, and the undersize/NULL
  refusals returning 0 rather than a plausible address.

All three run in `tests/run_all.sh` on every push; the module compiles
freestanding (`-ffreestanding -mno-red-zone`); teeth were verified by
sabotage (a flipped table class reddens naming `clock_gettime (228)`; a
flipped errno mapping reddens on the exact forward clause *and* the
round-trip that redefines the reverse).

What this increment is NOT: there is no personality field, no trap-path
branch, no six-argument capture, no request struct and no forwarding. The
Linux task does not exist yet, so the §8 checkpoint half of the gate is
not closer than it was — the extra-tasks hook cannot capture a task that
has not been built. Increment 2 is §3.3's mechanism (the personality, the
branch, the capture), increment 3 is §3.3's delivery (steps 5–7 — the
channel forward, the park/resume, the errno map applied to the reply, and
the sidecar core's first answers, §10.7), increment 4 is §5.3's `fs_base`,
and the checkpoint/restore integration is the increment AFTER the task can
park in a forwarded syscall — because that parked state is exactly what
§8 says a checkpoint serializes.

### 10.6 What has landed — increment 2 (the personality, the branch, the capture)

The first mechanism, and the whole of §3.3's steps 1–4 — with step 6's
sidecar deliberately still owed. What exists now:

- **The personality, stamped at spawn.** `PROC_PERSONALITY_SLS` (0) /
  `PROC_PERSONALITY_LINUX` (1) live in `kernel/linux_abi.h`; the field is
  `ProcessDescriptor.personality` (`kernel/process.h`), reset by
  `proc_clear_resume_state()` so a reused slot can never inherit one, and
  stamped inside `cap_create_sidecar_in_personality()` — before the HELD
  release, so a schedulable task never carries the wrong personality. The
  spawn is `SYS_SLS_LINUX_SPAWN` (328, unique across every `SYS_SLS_*`
  define): the same manifest path and the same request struct as 310, one
  argument different, and therefore the same E2 `sidecar_authority` gate —
  which lives inside the create both syscalls share (§11 Q3's answer, as
  proposed). An unknown personality value is refused `CAP_EINVAL` rather
  than stored.
- **The capture.** `PerCPUData` gains the sibling block `linux_args[6]`
  at `[gs:0x50..0x80)` — appended, with the existing offsets frozen by
  `_Static_assert`s (`syscall_regs` at 0x10, `linux_args` at 0x50, size
  0x80) so a C-side reorder is a build error instead of a silent desync
  with the asm's literals. `syscall_entry_stub` writes all six registers
  after its pushes and BEFORE any call — the §3.4 rule, inherited from
  the live failure that produced `syscall_regs`. The capture is
  unconditional for the honest first cut: six stores on the SLS path,
  nothing read; a personality-gated capture needs the personality in a
  register at trap time and is deferred with §3.7's fast path.
- **The branch.** `do_syscall()`'s FIRST act is the personality
  predicate, ahead of the driver-SDK switch and the legacy chain: a Linux
  task's trap reads the capture (`proc_read_linux_args()`), builds the
  §3.4 request through the pure `linux_syscall_describe()`, logs the
  number and its census class by name, and refuses with `-LINUX_ENOSYS` —
  a Linux errno, never a CAP-family value (§5.7). A non-Linux caller
  takes exactly the path it always took.

**Host test** (`tests/linux_personality_host_test.c`, 19 checks): the
predicate's truth table including the "an unrecognised personality
intercepts nothing" case (§10.3's inversion is its sabotage tooth),
`PROC_PERSONALITY_SLS == 0` (BSS zero-init is the safe default), the
request struct's ABI as literals (72 bytes; num 0, args 8, ret 56,
status 64 — increment 3's channel post and the Rust mirror build against
these), and `describe()` as the pure copy it claims (poisoned in, ret and
status zeroed out, NULL capture defined).

**Source guard** (`tests/linux_personality_check.sh`, 18 clauses, no
`GUARD-KIND` — source-only, so it runs everywhere) because none of the
wiring is observable without a boot: the six captures at their exact
offsets and above the stub's first call; the predicate before the first
`switch (num)`; the refusal value; the slot reset; the create's stamp;
`sys_sls_linux_spawn` passing LINUX; the E2 gate still present; 328's
uniqueness across the number space. Its smoke
(`tests/linux_personality_check_smoke.sh`) proves eight teeth, each
mutating a COPY (the real tree is never touched) and each required to
redden naming its clause: a capture deleted, a capture redirected onto a
frozen `syscall_regs` offset, the static assert dropped, the predicate
inverted to SLS, the branch dead-coded, the spawn's LINUX argument
dropped, the slot reset deleted, the authority gate neutralised — plus a
restore check on the real tree. Verified: guard 18/18 (exit 0); smoke
9/9 including the restore (exit 0); the five touched TUs
(`cap.c`, `process.c`, `syscall_dispatch.c`, `linux_abi.c`,
`user_paging.c`) syntax-check clean under freestanding flags.

**What this increment is NOT.** No forwarding: a Linux task's every
syscall is refused `-ENOSYS`, because the channel delivery and park /
resume (§3.3 steps 5–7) are increment 3 — and so is the sidecar core
that answers. No initial stack or ELF exec path yet (the spawn creates
the task through the sidecar manifest path, not through a Linux
`execve`); no `fs_base` (increment 4); no `brk`/`mmap` budget (5); no
boot guard — `tests/linux_abi_shim_check.sh` and its
`E7_TOOTH=personality-fallthrough` arm need a running Linux task, which
needs increment 3 to be anything but an `ENOSYS` machine. Until then the
guard above is what holds the routing, and it is deliberately source-
level: it can prove the branch is WIRED correctly, not that it FIRES —
and this document says which rather than letting a green guard imply
both.

### 10.7 What has landed — increment 3 (forward, park/resume, the map applied, the sidecar's first answers)

§3.3's steps 5–7, end to end in source, plus step 6's Rust half. The
branch stops refusing and starts delivering — while still interpreting
nothing. What exists now:

- **The post, and the arm that must precede it.** `cap_linux_find_channels`
  (`kernel/cap.c`) resolves the task's single sendable `CHAN_W` and single
  recv-able `CHAN_R` against `linux_channel_shape_ok` — exactly one of
  each, not "at least one": two candidates is a routing decision the
  kernel is not allowed to make, so it is refused rather than guessed
  between. The first trap then arms `cur->park_syscall =
  SYS_LINUX_TRAP_RESUME` **before** the post, because `cap_maybe_handoff`
  reads that state: a sysret handoff returned MID-forward would land back
  in ring-3 with its hardcoded `rax=0` and the reply would be lost.
  `k_linux_forward` (`kernel/chan.c`) resolves through the reply handle
  first (a broken or revoked channel is an errno, not a hang),
  `cap_send_msg`s the 72-byte description, then parks in
  `cap_wait_chans` — it returns **only** on a refusal and never reads a
  reply, by construction. Every refusal on this side leaves as a Linux
  errno: `-LINUX_ENOSYS` for a pre-delivery refusal (no channel, stray
  resume), `linux_errno_from_cap(status)` for the post's CAP status.
- **The resume, the only reader.** `SYS_LINUX_TRAP_RESUME` (0x4C494E52
  — "LINR", outside Linux's number space and outside every `SYS_SLS_*`)
  routes through the pure `linux_forward_resume_is_live`
  (`kernel/linux_abi.c`): a sentinel with no forward in flight — no arm,
  or a personality that intercepts nothing — is refused **by name**,
  never parked, the increment-2 discipline applied to the resume number.
  Armed, `k_linux_deliver` recvs into resume-frame locals (the first
  trap's frame died with it; park-time state travelled as values in
  `park_req`) and hands the payload to `linux_syscall_apply_reply`, which
  enforces the 72-byte size and the `num` echo **before** trusting
  anything — a mismatch is `-EPROTO`, "no answer that was not an
  answer" — and maps a nonzero `status` through §5.7 at the last
  possible moment, so a CAP-family value never crosses into `rax`.
  Status 0 means the sidecar's `ret` IS `rax`, passed through untouched
  (the brk/mmap contract). A spurious wake re-parks with the same
  expected `num`, so the echo check is identical on the third wake as on
  the first. The resulting `rax` rides the existing `cap_recv_resume` →
  `syscall_return_path` back to ring-3 as if the syscall had simply taken
  a while.
- **Step 6 — the sidecar core's answers (`user/sidecar/src/linux.rs`).**
  The spawn path registers each child's two channel handles
  (`linux::track`); the entry loop calls `linux::service` every pass —
  the same non-blocking poll shape as its console arm — and the forward
  channels join the quiesce **wait set** (`linux::append_wait`), so a
  child's request WAKES the sidecar rather than waiting for an idle
  pass. The answers are the census's, answered honestly: `set_tid_address`
  (218) is stored-and-ignored — single-threaded, so the register echo is
  the complete POSIX answer; `exit_group` (231) **reaps** — the task is
  parked awaiting exactly this exchange, so `proc_kill` is an immediate
  teardown and the frames return to the partition, and no reply is owed
  because the task never runs again (a pid that cannot be resolved is
  answered rather than left parked forever); everything else — every
  number outside the census — answers `-ENOSYS` with the number logged,
  §11 Q5's recommended policy: visible and cheap, never a silent success.
  The reply is the same 72-byte `#[repr(C)]` struct, `num`/`ret`/`status`
  at the offsets the host test pins, with a compile-time size assert as
  the last thing between a kernel/sidecar layout drift and an eternity of
  `-EPROTO`.
- **Why three of these are pure functions.** `linux_forward_resume_is_live`,
  `linux_channel_shape_ok` and `linux_syscall_apply_reply` live in
  `kernel/linux_abi.c` with the increment's own reason written down:
  every decision the forward path makes *before it parks* — "is this
  resume ours?", "is this table shape ours?", "is this payload an
  answer?" — is a decision a source guard can only hold if the source
  holds it as a named contract, and a host test can only bite if it is
  pure.

**Host test** (`tests/linux_personality_host_test.c`, 19 → **42
checks**): the sentinel's value as `"LINR"` and its fit inside
`park_syscall`'s `u32` (a truncation would strand a parked forward on a
different number); the in-flight truth table including both refusals
(SLS at the sentinel, a disarmed Linux task, an unrecognised
personality — each asserts nothing); the census table has no row for
the sentinel (`LSC_UNKNOWN`), so it can never be mistaken for a
describable syscall; the channel shape as a pure rule — only (1,1)
passes, and (0,1)/(1,0) is §10.3's `no-cap-channel` tooth as a function;
and `apply_reply` exhaustively: status-0 passthrough of a brk-class
64-bit value un-mangled, the in-place overlay the dispatcher reads back,
seven status rows mapped through §5.7 across both families plus the
`CAP_NONE` sentinel and an out-of-family value (both → `-EIO`), and the
refusals — undersize payload, zero size, a `num` echo from a different
syscall, NULL — each defined as `-EPROTO` rather than returning a
plausible value.

**Source guard** (`tests/linux_personality_check.sh`, 18 → **29
clauses**, still no `GUARD-KIND` — source-only, so it runs everywhere).
Clauses I–K hold increment 3 with one consequence per clause: the
forward/deliver pair exists at all; the post precedes the park *inside*
`k_linux_forward` (scoped to the function — `k_chan_send` parks on the
same shape for its own send); the arm is stamped before the call and
un-armed on the refusal paths; the resume arm checks the in-flight state
before it can deliver; both forward-failure exits cross
`linux_errno_from_cap`; `apply_reply` maps the status and refuses by
size and echo; the entry loop services **and** wait-qualifies the Linux
children; the answer arms are the census's shape; the wire size is
pinned twice; the reply fills `num`/`status` at the pinned offsets.
Because A–H (routing) and I–K (delivery) are one branch, they are one
guard: each half could be green alone while the seam between them is
broken.

Its smoke (`tests/linux_personality_check_smoke.sh`) proves **17 teeth
+ the restore check (18/18, exit 0)** on copies — the nine new ones,
each a plausible future edit: the post deleted (a park nothing wakes),
the arm stamp deleted (the mid-forward sysret handoff), the in-flight
gate inverted, the reply's status returned raw instead of mapped, the
num-echo check dead-coded, the entry loop's service call unhooked, the
default answer arm flipped into a silent success (the answer §11 Q5
forbids), the wire size moved 72 → 71, and the `num` echo never written
(every reply then dies `-EPROTO`). Baseline and restore controls as
before; the real tree is never touched.

**Also landed with it:** `tests/env_storage_durable_check.sh`'s S10
ceiling moved 327 → 328 with three new wiring clauses (S10g–S10i: the
define, the `case`, the handler — the ceiling moving WITH the wiring,
which its own comment already says is the rule), and each
cap-family host test's documented build command now also links
`kernel/linux_abi.c`, because a refusal can be produced anywhere and
the map must therefore be linkable everywhere. Verified in this
increment: guard 29/29 (exit 0), smoke 18/18 (exit 0), the personality
host test 42/42, `tests/run_all.sh` over every `*_host_test.c` green,
`tests/env_storage_durable_check.sh` green standalone, and the touched
TUs syntax-check clean under freestanding flags; the sidecar archives
were re-packed after the `user/` changes invalidated their stamps.

**What this increment is NOT.** Everything above is still source-level.
No qemu run has executed a real forward round trip: `tests/linux_abi_shim_check.sh`
(§10.2's boot guard — one environment, one Linux task, the capability
census, the attribution and flood clauses) still cannot run, and §10.3's
`E7_TOOTH` arms still have nothing live to sabotage. The sidecar answers
two syscalls for real and refuses the rest honestly, so a binary walking
past the census's edge errors loudly at the boundary rather than
receiving a fabricated value — but "a static binary doing the first
user's actual work" is not closer to the observed-work half of the gate
until a boot runs one. No `fs_base` (increment 4), no `brk`/`mmap`
budget (5), no checkpoint half (§8, v0.2's). And §3.7's round-trip
measurement is now *possible* rather than owed-in-principle: there is a
round trip to measure, still unmeasured.

**Superseded since — see §10.8:** a real forward round trip has since run
on QEMU twice per boot and its logs are committed; the boot guard
described above remains unwritten, so the observation is one real round
trip, not yet the full gate.

### 10.8 What has landed — the boot proof (the round trip runs on the real machine)

The one thing §10.7 said did not exist. A QEMU run now executes §3.3's
steps 5–7 for real — spawn, forward, answer, resume, reap — and the
serial logs are committed as evidence (`evidence/e7_phase5_shipped.log`,
`evidence/e7_unified_entry3.log`, `evidence/e7_e3_multi_env.log`).
Committed as `e7: increment 4 — the round trip is proven live on a real
boot`, taking the number the plan had reserved for `fs_base` (§5.3); the
number lives on the commit, the plan's sequence lives here — this
addendum records the proof, and `fs_base` remains the next increment.

**What runs.** The probe: `user/sidecar/fixtures/e7_linux_probe.S`
assembled to a 70-byte flat image (one `.text`, `_start` at file offset
0, the fixture's `build_probe.sh` carrying the exact build line) and
embedded by `user/sidecar/src/e7_probe.rs` with `include_bytes!`. Before
its event loop, the POSIX sidecar plants the payload at the TAIL of its
own budget region — identity-mapped, because `rust_entry` already stands
the heap there, so the write travels the heap's own mapping — builds a
zero-capability manifest (§3.5's shape as a construction: the child's
table gets only the messenger channel pair the create inserts;
`image_kaddr` footnoted as the tail), and spawns through the real **328**
(`k.linux_spawn` → `sys_sls_linux_spawn` →
`cap_create_sidecar_in_personality`). The sidecar is the design-true
creator: §3.5 places the task's single channel with *its environment's
sidecar*, and `linux::service` — the answers — lives in that same
binary.

**What the serial log shows** (verbatim; shipped ISO, default Phase-5
entry — the sequence recurs when the watchdog respawns the POSIX):

```
[SIDECAR] PID 105 'e7.linux.probe.0' released — runs on next schedule (BIB at 0x0000400000005000, entry 0x0000400000000000)
[E7] probe: spawned ch_r0000000D
[CAP] woken PID 104 (channel 11)
[E7] linux: num=000000DA  ok
[CAP] send handoff: PID 104 -> 105
[CAP] woken PID 105 (channel 13)
[E7] linux: num=00000027  -> ENOSYS
[E7] linux: num=00000028  -> ENOSYS
[E7] linux: num=000000E7  exit_group
[PROC] PID 105 terminated.
```

Each `[E7] linux:` line is one completed round trip: the child's trap
described from the `[gs:0x50..0x80)` capture, resolved to its one
channel pair, posted, parked; the sidecar's answer (218 echoed, two
outside-census numbers refused `-ENOSYS` per §11 Q5's policy, 231
reaped) rode `k_linux_deliver` → `linux_syscall_apply_reply` → §5.7 back
to ring-3 — and the NEXT line firing is the resume's proof: a task that
never came back would issue no further traps. Across both boots: zero
`refused` / `not delivered` / `stray trap` lines — those refusals exist
and are guarded (clauses I–K); they simply did not fire.

**The runs and their verdicts.** Shipped ISO default entry (the proof
above); `tests/e3_multi_env_boot_check.sh` → 0 — three live rootfs at
distinct storage regions, probe round trip inside the same boot;
`tests/unified_boot_check.sh` → 18/18 — which by ITS OWN design contains
no POSIX sidecar ("it would fight the kernel for the NIC and COM1"),
hence no §3.5 endpoint, hence no Linux task can exist *there*; the proof
lives on the Phase-5 entry of the same image, and the unified guard's
absence-assertion is itself the evidence that this is architectural
placement, not an omission. Alongside: `tests/linux_personality_check.sh`
29/29 and its smoke 18/18 unchanged — a boot adds an observation, not a
new clause.

**Two environment findings, each with a boot's worth of confusion
behind them.** (a) The kernel Makefile tracks no header dependencies, so
a `process.h` change rebuilt only the files git rewrote; the resulting
mixed-layout link manufactured phantom panics (`parent pid=0`, a
`personality` field reading the impossible `4`) that a full
`rm kernel/*.x86.o arch/x86/*.o` deleted outright. (b) The boot image is
staged at physical `0x20000000` — past the end of a 512 MiB guest — so
a `-m 512M` boot loads init into unbacked phys and dies at ring-3 entry
on a `#PF` no code change explains; the harnesses' `-m 4G` is
load-bearing, and every manual `-m 512M` run manufactures that fault.

**What this proof is NOT.** `tests/linux_abi_shim_check.sh` (§10.2's
boot guard — capability census, attribution and flood clauses) is still
unwritten and §10.3's `E7_TOOTH` arms still have nothing to sabotage:
what the logs prove is ONE honest forwarded round trip per spawn, not
the census boundary, not the flood bound, not "the first user's actual
work". The probe rides the sidecar-manifest spawn path with
`image_entry = 0` by construction — no ELF/execve path exists (none was
claimed). §3.7's round trip remains unmeasured: serial lines carry no
timing, so "possible to measure" is still the honest status. And
`fs_base` (§5.3, the plan's increment 4) and the `brk`/`mmap` budget (5)
are untouched by this proof.

### 10.9 What has landed — the plan's increment 4 (`fs_base`, §5.3)

The one kernel-side Linux semantic, in the four places §5.3 names —
landed, guarded, host-tested and PROVEN on a live boot (the thing §10.7
said could not yet be done for anything, which §10.8 did for the round
trip; this increment has both).

- **The field, cleared where personality is cleared.**
  `ProcessDescriptor.fs_base` sits beside `cr3`, with `fs_base_stage` and
  `fs_stage_valid` carrying the one-forward-at-a-time staged grant;
  `proc_clear_resume_state()` zeroes all three beside its personality
  reset, so a reused slot never hands its predecessor's TLS to the next
  occupant.
- **The write, single-sited.** `user_write_fs_base()`
  (`arch/x86/user_paging.c`, declared beside the gate) is the tree's only
  writer of `MSR_IA32_FS_BASE`: gate init zeroes it once (the invariant
  "FSBASE == current task's fs_base" is TRUE from the first ring-3
  instruction), `proc_build_resume_frame()` loads `next->fs_base` on
  every switch-in (the single frame builder every switch honours), and
  the resume arm performs the granted SET — no personality branch, the
  invariant instead of a special case.
- **The seam: stage, grant, apply.** The first trap stages a real
  `ARCH_SET_FS` through the shared predicate
  `linux_arch_prctl_is_set_fs(num, args[0])` before the arm; the sidecar's
  158 arm is the POLICY half (grant only `0x1002` at a user-canonical
  address — `0x0000_7FFF_FFFF_FFFF` — `E_INVAL` for a bad address, ENOSYS
  otherwise: SET_GS has no GSR, GET_FS's value lives in the kernel's
  field); the reply arm re-checks `linux_fs_addr_is_user_canonical()`
  and writes — the grant decides WHETHER, the mechanism still refuses a
  value it cannot write, as -EINVAL in rax.

**Host teeth.** `tests/linux_personality_host_test.c` 42 → **52 checks**
(the census's own measured `0x47a1a8` must pass its own gate; the
all-ones value must not; the predicate needs BOTH words). Guard 29 →
**36 clauses** (set L: single writer, gate baseline, slot clear,
switch-in, the stage/apply ordering *with the write strictly after the
deliver*, the pure bounds, the sidecar arm) and smoke 18 → **25 passed**
(seven new teeth t18–t24 — gutted writer, dropped baseline, deleted
clear, deleted switch-in, deleted apply, cmd word dropped, bound widened
to any u64). Three host tests that link `process.c` gained the weak
`user_write_fs_base` stub in `tests/process_host_stubs.h` (the
established pattern — their compile failure was the mechanism working).

**Live proof** (`evidence/e7_fs_base_shipped.log`, shipped ISO, twice per
boot — initial POSIX and its watchdog respawn):

```
[E7] linux: num=0000009E  ok          ← 158 arch_prctl granted by the sidecar
[E7] linux: num=000000BB  -> ENOSYS   ← the LATCH: %fs:0 == the direct read, so the MSR moved
[E7] linux: num=000000E7  exit_group  ← clean reap — not the segfault the failure path leaves
```

with zero MARKER 393 lines (the wrong-place tooth) and the only FAULT
in the log being irqtest's own storm kill. **The live boot taught the
fixture two lessons worth their own lines:** the resume clobbers
caller-saved registers (the first run held `_start` in r8 and faulted on
the kernel's leftover scratch — fault addr 0x16, then 0x10: two boots,
two garbage values), so the address now rides r15, park_ctx's preserved
set; and the %fs operand is displacement 0, because linear = FS_BASE +
disp — adding `_start` twice would ask for the non-canonical
`0x8000_0000_0000` by construction.

**One build-system defect, fixed at the root this time.** The header
edit repeated the `checkpoint_delta.h` incident from the Makefile's own
comment: only the four edited `.c` files rebuilt, `cap.c` still read the
OLD `ProcessDescriptor` layout, and init's first create panicked
`[CS] CAP_EINVAL parent pid=0` — the same phantom that cost this session
its first diagnosis. The Makefile now carries named prerequisites for
`kernel/process.h`, `kernel/linux_abi.h` and `arch/x86/user_paging.h`
over every kernel object (`$(X86_KERNEL_OBJS)`), in the same spirit as
the `checkpoint_delta.h` line above it; `touch kernel/process.h` +
`make -n kernel/cap.x86.o` proves the dependency fires.

**What this increment is NOT.** `ARCH_GET_FS`/`SET_GS` are answered
ENOSYS honestly (not in the census; a GET's value would have to travel
back through a reply only the kernel can fill — a future arm, not a
silent gap). The GDT question §5.3 asks to confirm rather than assume is
answered by construction for the measured candidate: user data is `0x1B`
(GDT slot 3), there is NO descriptor at `0x2b` (slot 5 is the TSS), and
the census binary's musl TLS path loads no FS selector — `%fs`
addressing runs off the MSR, which is what this increment writes. No
`brk`/`mmap` budget (§5.4 — the plan's 6, since the fd class took 5), no
§8 checkpoint half, no
boot-guard script — and §3.7's round trip remains unmeasured: serial
lines still carry no timing.

### 10.10 What has landed — the plan's increment 5 (the fd class: the kernel copy + the sidecar's fd table)

§5.6's first row — "`read`, `write` | the copy's bulk plus the console;
sidecar fd table" — has been the one row whose obstacle was named but
never built: the bytes live in the CHILD's address space (a separate cr3
by §3.5), so the sidecar that owns every fd decision cannot read them,
and the kernel that can must not decide anything. This increment is
exactly that crossing, in both halves, with the split §3.3 step 6
requires: **the kernel copies, the sidecar's fd table performs.**

**The plan (kernel/linux_abi.c, pure — the host test drives every
branch).** `linux_fd_write_plan(num, args)` is arithmetic over the
captured registers: a non-write number plans nothing (the rest of the fd
class still forwards as the plain 72-byte description it was),
`count == 0` plans nothing *before* canonicality is consulted —
`write(fd, NULL, 0)` is legal and inventing `-EFAULT` there would be a
rule no kernel the candidate was compiled against enforces, the same
discipline that lets 0 pass §5.3's bound — and a span above
`LINUX_FD_WRITE_CHUNK` (1024) clamps. **The clamp rides IN the
description**: `args[2]` is rewritten to the byte count *before*
`linux_syscall_describe`, so header, payload and the rax the task will
read are one truth rather than three, and a short write — POSIX-legal,
and handled correctly by std's `write_all` loop — is what the task sees.
A span that leaves the user-canonical range (or wraps) refuses
`-LINUX_EFAULT` **before the post**: a first-trap refusal with no channel
and no arm, and EFAULT 14 is returned directly rather than through §5.7,
because it is the task's truth about the task's own pointer, not a CAP
code.

**The copy (same file, also pure over a root the caller supplies).**
`linux_user_copy_out(dst, root, va, n)` walks four levels **read-only**:
present at every level, the USER bit at every level, no huge page (PS at
the PDPTE/PDE — bit 7 of a 4 KiB PTE is PAT and is not refused), then
copies from the identity-mapped frame (boot.asm's own claim: "the
physical addr = virtual addr"), re-walking all four levels from the PML4
for each page so a span crossing a page boundary validates and copies
both. The walker this deliberately is *not* is
`arch/x86/walk_page_tables_x86.c`, whose `get_or_create` ALLOCATES a
missing table — over a tenant-supplied address that would be a memory
lever, not a copy; guard clause M4 pins "allocates nothing" against the
source. The USER bit is the security argument, stated as a property:
**the copy can read exactly what ring 3 could read itself**, so
forwarding the bytes to the sidecar exposes nothing the task did not
already hold, and every refusal lands as an errno into rax — never a page
fault in ring 0 over a tenant pointer.

**The message widened by exactly one dimension.** `k_linux_forward`
now takes `(const void *msg, uint32_t msg_len)` — the 72-byte header
plus attached bytes — validates `msg_len >= sizeof(request)` as
`CAP_ERR_PROTO`, reads the park's wake cookie byte-wise (the same
no-alias discipline `apply_reply` applies to channel payloads), and posts
`msg_len` rather than a hard-coded size. The arithmetic that keeps this
inside the channel: `1024 + 72 <= CAP_MSG_MAX_PAYLOAD (4096)`, pinned by
guard clause M1 as a computation across two headers, so a future chunk
edit that oversizes the pool reddens the guard instead of failing
silently at post time — only for big writes, where nothing else would
notice.

**The fd table (sidecar, linux.rs).** `FdSlot { used, kind }` per task,
`LINUX_MAX_FDS` = 16, **born with the task** in `track()`: fds 0, 1, 2
are the console (a tty is bidirectional — Linux does not make `write(0)`
an EBADF). `answer()`'s new `1 =>` arm resolves the descriptor through
`fd_kind` (pure over the slice — guard M5's) and answers:
console slot → `Answer::Write`, unknown descriptor → `EBADF` with
`CAP_EBADF` pinned at −2 so §5.7 delivers 9. That EBADF is the *whole
truth*, not a shortcut: `open` is still ENOSYS, so no slot beyond the
born three can exist. `service()` grew the second half of the honesty:
the recv buffer carries `72 + 1024`, and for exactly one number the
attached length must equal `args[2]` **and** sit under the chunk's mirror
`FD_PAYLOAD_CAP` (1024) — a violation is the malformed arm's num=0 reply
the kernel's echo check refuses `-EPROTO`, so bytes the task did not
write can never be performed. The perform half is `console.write` on the
entry loop's own CharNode (`service(&RealKernel, &booted.console)`), and
the loop's existing pump drains it to the kernel console channel — no
second path to serial.

**The probe proves the whole seam on the real machine.**
`fixtures/e7_linux_probe.S` (now 213 bytes) writes a fixed string to fd 1
— `E7FD-WRITE-OK` — and requires `rax == len`; then writes the same
buffer to descriptor 0x77 (which cannot exist) and requires `rax == -9`
exactly. Either wrong issues **MARKER 394** (0x18A), asserted ABSENT in
the 391/392/393 discipline. Evidence, committed as
`evidence/e7_fd_write_shipped.log` (the shipped ISO, default entry): the
string appears on serial twice — the bytes walked child→copy→message→
fd table→console→pump→kernel→serial — `num=00000001 write ok` is the
sidecar's perform line, the bad-fd attempt's `num=00000001 ok` is its
answered EBADF reply, and **markers 391–394 occur zero times** (the
absence of 394 *is* the proof that both rax checks passed; the bad-fd
line is still num=1 because the syscall was write — 0x77 was its fd
argument). Two complete probe cycles appear in the log, every marker
clean in both, with §5.3's latch (`9E ok` → `BB -> ENOSYS`) and
`exit_group` intact behind the new block.

**Teeth.** Host test 67 checks (+15: seven plan cases including the
count-0-before-canonicality rule and the wrap-around span, eight copy
cases over fake 4 KiB-aligned tables — each flag refusal named).
Guard clause set M, 43 clauses total (+7: contracts, the cross-header
chunk arithmetic, the dispatcher's plan→describe→copy→arm→forward
order, the forward's message path, the copy's three boundaries plus
"allocates nothing", the fd table's born stdio and pinned EBADF, the
sidecar's payload invariant and console hand-off). Smoke 32 (+7 teeth:
oversized chunk, detached copy, dropped clamp, a send that hard-codes 72,
the USER check deleted, drifted E_BADF, dead-coded payload check), plus
the updated I1/K teeth for the widened forward and the handed console.

**What this increment is NOT.** `read` — the copy-IN — has deliberately
not landed: its bytes travel the other way, which means payload on the
REPLY, and the reply is exactly 72 bytes as a pinned security property
(`apply_reply`'s size check, host-tested and smoke-toothed as the
property "a payload that is not exactly 72 bytes is not a reply").
Loosening that for one syscall is a §10.7-class decision, so read gets
its own increment with its own teeth, alongside the console-input
buffering that decides what a blocking stdin even means. `open`,
`close`, `lseek` and the rest of the fd class remain ENOSYS — the file
half is Phase 2 §3-§4's VFS work, and until `open` lands the fd table
cannot contain anything but its born stdio, which is what makes the
EBADF arm above complete rather than provisional. No mem class (§5.4,
now superseded by §10.11), no `clock_gettime` child-buffer write, and
§3.7's round-trip measurement remains unmeasured.

### 10.11 What has landed — the plan's increment 6 (the mem class: brk's budget span + the W|X refusal)

§5.4's first half — "**`brk` as a bump pointer inside a reserved span**
and `mmap` as sub-allocations of the same budget; both refused with
`ENOMEM` when the budget is exhausted. The hard ceiling is the
partition's frame quota, kernel-enforced" — is now mechanism. The split
is the design's own: the sidecar decides *whether* the break moves (the
budget is its policy — it authored the manifest's `BUDGET` record), the
kernel provides the frames, and the kernel re-checks the grant *at the
map* against the span it stamped itself — the §5.3 posture one level up,
where the `wrmsr` is replaced by a page-table write.

**The span, stamped at create.** `cap_create_sidecar_in_personality()`
(`kernel/cap.c`) stamps three fields on the descriptor for a
`PROC_PERSONALITY_LINUX` child, beside the personality itself:
`linux_heap_base` (directly above the child's stack),
`linux_heap_limit` (base + the `BUDGET` record's `mem_bytes`,
page-rounded) and `linux_brk_cur` (starts at the base — nothing above
the stack is mapped until a `brk` grants it). `mem_stage_{num,a0,valid}`
carry the requested break across the park (the first trap's frame is
abandoned by the park — the increment-4 lesson: the stage lives on the
descriptor), and `proc_clear_resume_state()` zeroes all six beside
personality and TLS. An SLS child gets the zero span, so a brk-shaped
stage on an SLS slot has no span to grant against.

**The verdict, at the reply.** `linux_mem_brk_verdict()`
(`kernel/linux_abi.c`, pure, host-tested) decides what the resume arm
does with a reply, over the kernel's own span, the staged request and
the reply's `rax`:

- `rax == brk_cur` — the denial echo, which is **Linux's own brk
  failure shape**: a denied brk returns the *current break*, never
  `-ENOMEM`, and libc compares the return against the request. Not
  malformed; nothing maps. Over-budget asks and shrinks both arrive
  here, and so does the no-op `brk(cur)`.
- `rax == req` inside `(brk_cur, limit]` — a grant: the verdict
  computes `[align_up(brk_cur), align_up(rax))`, the exact pages no
  grant has backed yet — the page *containing* the old break was mapped
  by whatever moved the break into it, and a re-map would leak the
  frame underneath and zero the bytes on it.
- anything else — malformed: the break stays, nothing maps, the reply
  is logged. A "granted" shrink and a grant that escaped the stamped
  span both land here; the kernel never maps a page because a grant
  said so.

The arm then maps each page with
`allocate_physical_ram_frame_for_partition()` — the hard ceiling §5.4
names, quota-checked and owner-tagged — **zero-filled before the task
can read it** (the pool recycles frames across tasks and partitions, so
an unzeroed frame would carry its last tenant's bytes into a fresh
heap) and non-executable (`USER_PTE_NOEXEC`: W^X by construction —
the mprotect refusal below is the policy half of the same rule). Frame
exhaustion mid-batch *unwinds*: `linux_user_page_frame()` reads each
mapped leaf back under the copy's own boundaries (present, USER, no
huge page; allocating nothing — deliberately not the allocating walker
`arch/x86/walk_page_tables_x86.c` is), the pages are unmapped and
returned to the partition, and the break does not move. x86-64 never
caches a not-present translation (SDM Vol. 3A §4.10.4.1), so the fresh
maps need no `INVLPG` for the task resuming on this same cr3 — the
posture `cap_arch_map_page`'s runtime maps already rely on.

**The sidecar's ledger, and both W|X ends.** `linux.rs` grew
`MemBrk { base, limit, cur }` per task, born in `track()` from the span
`e7_probe.rs` derives from the manifest it authors — the mirror of
cap.c's layout (image pages, guard page, stack pages, then
`mem_bytes`), pinned by guard clause N2 because the Rust side cannot
include `kernel/linux_abi.h`. `answer()`'s new 12 arm grants a growth
inside the span and answers the unchanged current break for everything
else (the denial shape is a *value*, never an errno). The new 10 arm
refuses write+execute (`prot & 0x2 & 0x4`) `EPERM` — `CAP_EPERM`
pinned at −12 so §5.7 delivers 1 — the twin of the kernel's first-trap
refusal, which lands before the post like the write plan's `-EFAULT`:
a prot word is a property of the request, not of any object, so no
channel is needed to say no, and both ends exist so neither alone is
the load-bearing refusal.

**The probe proves the span on the real machine.**
`fixtures/e7_linux_probe.S` (now 420 bytes): `brk(0)` must return a
page-aligned, nonzero break; `brk(base+0x2000)` must echo the request;
the fresh page must **read zero**; `"E7MEM-BRK-OK\n"` built *in the
heap* and `write(1, page, 13)` must return 13 — the bytes crossing
through increment 5's seam *from the newly-mapped heap itself*, which
is what proves the mapping is real; `brk(base+128 KiB)` (past the
64 KiB budget) must come back as the **unchanged break**, not a
negative; `mprotect(heap, 0x1000, PROT_WRITE|PROT_EXEC)` must be
exactly −1. Markers 395–398 issue on each wrong outcome and are
asserted ABSENT (the 391/392 discipline). Evidence, committed as
`evidence/e7_mem_brk_shipped.log` (the shipped ISO, default entry): two
complete probe cycles, each with `E7MEM-BRK-OK` on serial — the bytes
built in a page the kernel mapped for this task at its own brk grant,
then crossed child→copy→message→fd table→console→pump→kernel→serial
through increment 5's seam; `num=0000000C ok` three times per cycle
(the query, the granted growth, the over-budget denial — a *value*, so
an ok line and never an errno line); the kernel's own
`mprotect prot=0x6 refused before post` line once per cycle with **no**
`num=0000000A` behind it — the refusal is first-trap, before any post,
so the sidecar never sees the request; and `E7FD-WRITE-OK`, §5.3's
latch (`9E ok` → `BB -> ENOSYS`) and `exit_group` intact behind the
new block. **Markers 391–398 occur zero times** — the absence of 397
and 398 is the proof that the denial returned the unchanged break and
the W|X refusal returned −1; no malformed-grant or frame-exhaustion
line appears, so every grant the sidecar made was one the kernel's own
span agreed with.

**Teeth.** Host test 86 checks (+19: the W|X predicate's total
branches, the verdict's every arm — denial echo, no-op, grant, the
exact-limit grant, the unaligned grant, the live-page-never-remapped
rule, past-limit, shrink, foreign `rax`, below-base — and the frame
reader against fake tables). Guard clause set N, 52 clauses (+9: the
contracts; the slot-reuse clear; the span's two derivations keyed on
the personality; W|X at both ends; the stage/apply seam through the
verdict; partition-charged, zero-filled, NX frames; the unwind; the
refusal shape; the fixture's own teeth). Smoke 42 (+10: an
unconditional stamp, a dead-coded first-trap refusal, a dead-coded
sidecar twin, a dead-coded malformed branch, un-charged frames,
unzeroed frames, a gutted unwind, an errno-shaped refusal, a deleted
positive marker, a dropped slot-reuse clear).

**What this increment is NOT.** `mmap`/`munmap` — §5.4's
sub-allocation half — remain ENOSYS by name: they need the sidecar to
*choose* addresses (and `munmap` to free frames back through the
partition), a second seam worth its own teeth. `mprotect` implements
only the W|X refusal: a real permission change would rewrite PTEs the
sidecar cannot see. `read`'s copy-IN is still the named next increment
alongside them (§10.10's reasoning unchanged). brk shrink is refused —
the unchanged break, v1 grows only, and no page is ever unmapped by a
grant. And §3.7's round trip remains unmeasured: serial lines still
carry no timing.

---

### 10.12 What has landed — the plan's increment 7 (the image class: the real candidate, §5.1/§5.2)

§5.1's "**the kernel parses [the ELF image], maps segments at their
linked virtual addresses, and enters at `e_entry`**" and §5.2's initial
stack are now mechanism — and, the whole point of the increment,
OBSERVED rather than argued: a real `ET_EXEC` binary is loaded by the
tree's ONE mapper and runs its entire syscall census on the machine.

**One mapper (§5.1).** The ELF64 structs, the gates and the PT_LOAD loop
moved out of `loader.c` into `kernel/elf64_load.{h,c}`, so the uploaded-
program path and the Linux-personality create share one truth about
segment flags, BSS zero-fill, partition charging and the image contract.
Gates refuse by name BEFORE the first frame: not-ELF64, `ET_DYN` (there
is no bias to give it), bounds, non-user-canonical vaddrs. The magic —
and only the magic — picks the path (image format is an image property,
never a personality inference); the entry becomes `e_entry`, never the
flat mapper's `base+offset`.

**The §5.2 stack, and what the measurement caught.** The Linux child's
stack is ONE contiguous partition-charged region;
`linux_initial_stack_build()` writes the argc/argv/envp/auxv image into
it, with a new `region_vaddr` parameter translating every pointer in the
image — and the returned `%rsp` — into the CHILD's address space. The
image stops one page below the region's top so step 9's unconditional
BIB write cannot clobber it (both truths survive: the probe still finds
its BIB at the stack-top page and never reads `%rsp`; a libc child owns
everything below it), an 8 KiB floor names the BIB-page-plus-image
minimum, and an unseeded entropy pool REFUSES the create
(`CAP_ECONFLICT`, region returned) rather than shipping a zero-filled
`AT_RANDOM`.

Two real bugs this increment's own measurement found, both pinned:

1. **The untranslated image** — pointers built from the host/phys region
   address. The `.flat` probe never reads `%rsp`, so it could not see
   this; a libc dereferences `argv[0]` on its first line. Caught by the
   cap host test before boot (`region_vaddr` is the fix, byte-identical
   under identity, pinned by clause O3 / tooth t44).
2. **The unsafe walk** — a real static binary links its segments LOW
   (0x400000), squarely inside the child's SHARED kernel identity map
   (supervisor 2 MiB huge pages, cloned by pointer). `user_map_page()`'s
   walk treated the huge-page PD entry as a table pointer and wrote the
   leaf into the kernel's own frames while the child's fetch still
   resolved through the supervisor page. Measured on the FIRST census
   boot: `[FAULT] Ring-3 #PF error=0x15 addr=0x400000 rip=0x400000`
   with `[LOADER] ELF segment ... flags=R-X` printed above it — the
   mapper entered the right address and the very first instruction
   fetched non-executable. The mapper now routes segments through
   `cap_arch_map_page()`'s safe walk (the fix create_sidecar's MEM caps
   already had: re-point the CHILD's own path, replicate the shared
   huge page at 4 KiB so the kernel keeps visibility of the rest).
   Clause O1 / tooth t48 pin the routing.

**The measurement — the census run.** The fixture
(`user/sidecar/fixtures/e7_candidate.elf`, the probe source linked
non-PIE; `fixtures/build_candidate.sh` reproduces it byte-exactly) is
embedded and planted by `user/sidecar/src/e7_candidate.rs` and spawned
beside the `.flat` probe — a CONTROLLED PAIR: identical machine code,
identical sequence, only the image format differs. On the E3 ISO
(`make x86-iso-e3`, 150 s serial capture):

* **first run** (before the safe-walk fix): both candidates ENTER —
  `rip=0x400000` proves the magic dispatch, the mapper and `e_entry` —
  and die at their first instruction fetch (bug 2 above); the two
  `.flat` probes complete the census (2× each marker);
* **second run** (after): **4/4 children complete the whole census** —
  `E7MEM-BRK-OK` ×4, `E7FD-WRITE-OK` ×4, the `0xBB` `fs_base` latch ×4,
  `218` echoed, `39`/`40` mapped to `-ENOSYS`, `arch_prctl` applied and
  read back through `%fs:0`, the fd-class byte crossing (success and
  exact `-EBADF`), brk's span (query/grant/heap-write/over-budget echo),
  `mprotect(W|X)` refused as `-EPERM`, `exit_group` reaped — and MARKER
  391–398 all ABSENT (the 391/392 discipline), zero E7 Ring-3 faults.

(One `[FAULT] Ring-3 ... error=0x0` in the same boot belongs to the
irqtest applet suite's own p4-storm phase deliberately killing its
target (observed: the system posix, code=134) — independent of E7; every
census sequence completed around it.)

**Where the census was too loud — measured, and the scope it earned.**
The first scope (a pair from EVERY POSIX environment) turned the
census into serial load, and the E3 multi-environment boot measured
it: four POSIX instances, each pair's `[LOADER]` lines, markers and
`[E7]` klogs riding the one UART — and riding it DURING the irqtest's
serial-loopback window, whose deferred-TX ring is bounded. The kernel
said so on the wire: `[serial] loopback window overflow: 0x16e0
byte(s) dropped` — and what it dropped included the tenants' own
drop-proof `[POSIX] aero state=` lines. `e3_multi_env_boot_check.sh`
then failed `live rootfs: 2/3` on boots where every environment had
actually come up (all spawn lines and all four census completions
present in the same log): a regression this increment introduced, not
the check's old lossy-marker flake. The pair is now spawned only by
the environment that OWNS hardware — the system POSIX profile mints
`nic0.bar0` (`CAP_TYPE_DEV`) while the tenant profile carries no
hardware caps at all (`env_manager`'s tests pin `uart`, `irq.timer.0`
and `nic0.bar0` absent from every tenant manifest) — so one boot
carries one census (the system POSIX, plus its watchdog respawn),
each tenant's wire stays as quiet as it was before increment 7, and
the runs recorded above are exactly this scope.

**Teeth.** Clause set O in `tests/linux_personality_check.sh` — O1 the
gates + the safe walk, O2 the magic dispatch, O3 the §5.2 contract
(child-vaddr pointers / BIB-page reservation / 8 KiB floor), O4 the
entropy refusal, O5 the candidate's wiring — with teeth t42–t48 in the
smoke (gate inverted, dispatch deleted, phys pointers, BIB clobber,
entropy dead-coded, unsafe walk, spawn arm deleted). Host side:
`cap_create_sidecar_host_test.c` gained L1 (the image through the real
create, read back THROUGH the child's page table), L2 (the unseeded
refusal returns the region to the partition) and E7c (the candidate's
segments, flags, entry and AT_PHDR/PHENT/PHNUM truthfulness) — 377
checks, plus the builder's 40 under `region_vaddr` identity.

**Verified in this increment:** guard 57 clauses (exit 0) with clause
set O added; smoke 49/49 including O's seven teeth (gate inverted,
dispatch deleted, phys pointers, BIB clobber, entropy dead-coded,
unsafe walk, spawn arm deleted), each reddening the clause it names;
the cap host test green end to end (375+ checks, including the three
new sections); the stack-builder host test 40/40 under the
`region_vaddr` translation; `tests/run_all.sh` over every
`*_host_test.c` green (the cap-linking family's build lines now also
link `kernel/elf64_load.c`/`kernel/entropy.c`/`kernel/sha256.c`,
`simi_info` its `linux_abi.c` half — the loader delegation and the
entropy gate must be linkable everywhere cap.c/loader.c is);
`tools/syntax_check_all.sh` 129/129; the conventions guard green after
fixing the index-level debt it surfaced (four scripts committed
non-executable; the `.flat`, the candidate `.elf` and the raw
`evidence/*.log` serial captures now carry the `binary` attribute they
always needed); `env_recycle_boot_check.sh` green standalone; the
sidecar archives re-packed (`make selfhost-bootimage`) after the
`user/` changes invalidated their stamps. The e3 batch failures were
NOT absorbed as flake: they reproduced the census's serial overflow
(the measurement above), and scoping the census to the owning
environment is what fixes them — the re-run with that scope in place
is the verification of this paragraph: `tests/run_checks.sh` over the
whole tree exited 0 with **46 PASS / 0 FAIL / 7 SKIP**, the seven
skips being the runtime guards that need a live cluster a build host
does not have (`run_checks.sh` itself names them), and
`e3_multi_env_boot_check.sh`, `linux_personality_check.sh` (clause
set O), `sidecar_stamp_check.sh` and `stack_frame_budget_check.sh`
among the 46.

**What this increment is NOT.** The candidate is the probe's SOURCE
linked as an `ET_EXEC` ELF — a controlled pair, not the designated
first user: `aerobackup`'s musl binary has still never executed under
the shim, and its census stays host-measured (`strace`,
`tools/linux_syscall_census.txt`) while every surface beyond this
sequence (`open`/`read`/`mmap`/…) remains ENOSYS by name. argv/envp
cross on §5.2's environment-crossing record (`SIDECAR_TAG_ARGENV`) —
landed after this increment: the record carries argv and envp, the
create copies its strings into the stack image with child-vaddr
pointers beside the v1 argv, and an ABSENT record keeps the v1
fallback (`argv = { name }`, an empty environment) so every pre-record
manifest runs byte-identically. Pinned by guard clauses O6/O7 (teeth
t49–t52), the cap host test's L3 (the crossing read back through the
child's own page table) and L4 (every over-bound or malformed record
refused by name, no process consumed), and `manifest.rs`'s
`argenv_wire_bytes_golden`. `ET_EXEC` only: no PIE, no interpreter, no dynamic linking
(§12 untouched; §5.1 refuses the rest by name). The 8 KiB stack floor
is a floor, not a measurement — §11's Q2 still stands; there is no
vDSO by decision, and §11's Q5 (rseq/membarrier at start-up) is still
open. `mmap`/`munmap` and `read`'s copy-IN remain the named gaps of
§10.10/§10.11.

### 10.13 What is planned — the plan's increment 8 (the reply payload and the file class), scoped

**Nothing in this section has landed.** It is written to increment 1's own
discipline — the contracts before any mechanism (§10.5) — against the
seams as they exist in the tree today, so the increment can be judged as
a scope before a line of it exists. The two gaps §10.10 named — `read`'s
copy-IN and the fd class that stayed ENOSYS beside it — are one
increment because they are one boundary: increment 5 taught the forward
to carry bytes OUT of the child, and this increment makes the reply
carry bytes BACK and the paths out. (§10.11's `mmap`/`munmap` join only
conditionally, as step 1 below — the diagnosis decides.)

**What the increment must produce.** Two seams and one measurement: (1)
the reply payload, host-tested on both sides; (2) the file class
answered end to end; (3) the designated first user running with its
REAL argv — `--storage /tmp/e7-census/sls_storage.img --backup-dir
/tmp/e7-census/backups --kind hourly --retain 4`, `CANDIDATE_ARGS` as
`tools/linux_abi_candidate.conf` records it — crossing on §5.2's ARGENV
record, through its file work to `exit_group(0)`, every trap line of
the run attributable and any remaining refusal named by the log line
that proves it. That run is the gate's first half (§8).

**The reply payload (the copy-IN), seam by seam.** Every place is named
before it exists:

- **`linux_fd_reply_plan(num, args)`** — the first-trap twin of
  `linux_fd_write_plan` (`kernel/linux_abi.c`): per copy-IN number it
  validates the CHILD's destination span (canonical, inside the user
  range — the `(span_end - 1) > MAX` shape the write-plan host tests
  already pin), clamps the count to a per-number chunk no larger than
  `FD_PAYLOAD_CAP`, and refuses `-LINUX_EFAULT` before any post.
  `read`'s count-0 rule is write's own, mirrored: `read(fd, NULL, 0)`
  plans nothing BEFORE canonicality, because inventing -EFAULT there
  would be a rule no kernel the candidate was compiled against has.
- **The staged destination.** The copy-in aims where the FIRST trap
  said, never where the reply says: the plan's (buffer VA, clamped
  len) are staged in `struct ProcessDescriptor` beside `fs_base_stage`
  (`kernel/process.h`), set before `k_linux_forward`, consumed at the
  apply, cleared at every refusal and at slot reuse beside
  `proc_clear_resume_state()`'s existing clears. `park_req` cannot
  carry it — it holds the expected num as a VALUE precisely because
  the first-trap frame died (§10.7), and `CapParkCtx` saves only
  sysret-state registers. The stage is the value-shaped path, and its
  lifecycle is four places where a stale destination would aim a
  sidecar payload at a reused slot's successor — which is what makes
  it clause work, not implementation detail.
- **`linux_user_copy_in(dst, root, va, n)`** — the mirror of
  `linux_user_copy_out` over the same four-level walk, and STRICTER
  where it must be: every page present, USER **and writable**, in two
  phases (validate the whole span, then write), allocating nothing.
  Writable, not merely user: copy-out only needs readable, but copy-in
  must never write a byte into a page the child itself could not have
  written — or the sidecar would launder writes through the child's
  read-only pages. That asymmetry is the new security property, and
  its host test is M4's family extended to refusals-before-mutation.
- **`k_linux_deliver`'s recv** (`kernel/chan.c`) — today
  `k_chan_recv(… sizeof reply)` and `out.len != sizeof reply →
  -CAP_ERR_PROTO`; it becomes a local of `72 + FD_PAYLOAD_CAP`
  accepting exactly three shapes: 72 bytes (unchanged), `72 + n` where
  `n` is echoed by the header's `ret` AND bounded by the staged len
  (apply, copy in, `rax = n`), and anything else `-CAP_ERR_PROTO`.
  The frame grows ~1 KiB on the resume path, so
  `tests/stack_frame_budget_check.sh` is the teeth — and the fallback
  if it reddens is NOT a static buffer: deliver runs concurrently on
  SMP, each on its child's own resume frame, and the per-frame local is
  what keeps that safe.
- **`service()`'s bound** (`user/sidecar/src/linux.rs`) — the mirror
  over the answer: a reply-payload number's payload must equal the
  forwarded `args[2]`'s clamp (the clamp rides IN the description, as
  in increment 5), returned through a new `Answer::ReplyBytes(ret,
  bytes)` beside `Answer::Write`.

**The paths (the forward's second shape).** `open`, `stat`, `lstat`,
`unlink`, `mkdir` and `rename` carry a pathname OUT of the child:
`linux_fd_path_plan` mirrors the write plan — copy-out of a
NUL-terminated string bounded by `LINUX_PATH_CAP`, the twin of
`FD_PAYLOAD_CAP` (1024), a span with no NUL within the cap refusing
`-LINUX_ENAMETOOLONG` at the first trap. Named divergence: Linux's
`PATH_MAX` is 4096; this shim refuses above 1024 by name, and the
trigger to raise it is raising `FD_PAYLOAD_CAP` and both recv buffers
together — not a quiet edit. `rename`'s two paths ride back to back,
two NUL-terminated strings in one payload, split at the first NUL by
the sidecar.

**The §10.7 property changes here, by widening and not by relaxing.**
"A payload that is not exactly 72 bytes is not a reply" stays TRUE for
every number that planned no reply payload; the new truth is a second
legal shape — 72 bytes plus a payload this request's own first trap
allowed, bounded by the child's own clamp, echoed by `ret`, copied only
where the child could write. The M-clause teeth that pin "exactly 72"
are rewritten, not deleted, to pin both shapes, and every new refusal
arm gets its own: payload with `n` beyond the staged len → `-EPROTO`;
`ret != n` → `-EPROTO`; payload on a number with no reply plan →
`-EPROTO`; a stage surviving an apply, a refusal or a slot reuse → its
clause reddens.

**The file class: what lands, and on whose code.** §5.6's class as the
census measured it — `read`(0), `open`(2), `close`(3), `stat`(4),
`lstat`(6), `lseek`(8), `fcntl`(72), `fsync`(74), `ftruncate`(77),
`rename`(82), `mkdir`(83), `unlink`(87), `getdents64`(217): thirteen
numbers. The kernel semantics are this increment's two seams and
nothing more; every DECISION is the sidecar's existing VFS — which is
D1 surviving its own file class: `user/vfs` already implements
`open(task, path, flags, mode)`, `close`, `lseek`, `read`, `write`,
`stat(path)` and `read_dir(path)` over `/` (aerofs-lite), `/dev` and
`/tmp` (ramfs), mounted at boot (`user/sidecar/src/boot.rs`) and
already exercised by the shell's redirects
(`user/sidecar/tests/boot_tests.rs`). The new work is the BRIDGE, and
it is small by construction:

- `FdSlot` grows the File kind carrying the VFS fd and the open flags
  it must echo (`F_GETFL`); `answer()` routes thirteen arms to the VFS
  under the Linux task's VFS task id. An unknown kind keeps answering
  ENOSYS — increment 5's rule, unchanged.
- The flags need almost no translation: `user/vfs/src/vfs.rs` defines
  `O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_EXCL/O_TRUNC/O_APPEND` at
  Linux's own octal values. What does not fit is named — the flag
  word is `u16`, and `O_DIRECTORY`/`O_CLOEXEC` cannot fit in it — so
  those bits are refused at `open` by name (trigger: widen the flag
  word), never silently dropped.
- `fcntl` answers exactly four commands — `F_GETFD`/`F_SETFD` (the
  fd's own cloexec, which the VFS already tracks for `close_cloexec`)
  and `F_GETFL`/`F_SETFL`; every other command is ENOSYS with the cmd
  word logged, because a fabricated answer to a command nobody
  measured is the placeholder failure in a new shape.
- The struct forms are format references in `user/proto`, with golden
  tests beside `argenv_wire_bytes_golden`: `struct stat` at musl's
  x86-64 layout (144 bytes, field offsets pinned golden and proved
  on-target by the probe reading back the fields it wrote) and
  `struct linux_dirent64` (`d_reclen` 8-aligned, `d_name` NUL-padded,
  records bounded by the reply payload).
- `fsync` on a ramfs-backed slot answers 0 — the shmem-faithful
  answer, since Linux's own tmpfs fsync is a no-op; durability of core
  memory across a host failure is claimed by neither, and across
  restore it is v0.2's CONTENTS question, not this one. A
  device-backed slot answers ENOSYS until the cache has a flush entry
  point to call — named, and unreachable from the census's single
  `fsync` row, which lands on `/tmp`.

**Step 0 — the diagnosis the first half is blocked on.** The firstuser
still dies at `rip=0x28` before its file work, and no seam below
matters until it does. What the E3 log already settles: the three
children each earned the startup `arch_prctl`/`set_tid_address`/`brk`/
`write` lines (the counts fit three of each) but the `num=` lines carry
NO child name — so step 0.5 is that name on `service`'s log line, one
line of Rust, after which every measurement is attributable. What else
it settles: no `mmap`/`munmap` line exists at all (the census's 8/7
rows are mid-run, never reached) and only two `exit_group` lines for
three children — the death precedes both. A call whose RIP is 0x28 is
a call THROUGH a NULL object's slot 0x28 — the shape of a failed
allocation or a skipped initialization, not of an ENOSYS, which
answers rather than jumps. The diagnosis decides step 1: if the
evidence names `mmap`, anonymous `mmap`/`munmap` over the budget lands
as step 1 — §5.4's second half minus file backing, which stays refused
by name — and this section is amended with what was measured, not with
a mechanism guessed.

**Sequence.** 0: the attribution instrument, then the `rip=0x28`
diagnosis. 1: whatever the diagnosis names (expected: anonymous
`mmap`/`munmap`). 2: the reply payload, host teeth first —
`linux_user_copy_in`, `linux_fd_reply_plan`, the three deliver shapes,
the `service` bound. 3: the path plan, the VFS bridge, the thirteen
arms. 4: the two format goldens. 5: the fixture probe on-target — a
`fixtures/` file-class sequence (mkdir, open, write, lseek, read-back,
stat, getdents, rename, unlink, close) ending in a marker line in the
E7MEM/E7FD family, beside the probe/candidate pair. 6: the real-flags
run — the measurement. Guard clause set **P** with teeth **t53+** in
`tests/linux_personality_check.sh`/`_smoke.sh`; the census guard
re-verified — its 27 rows stay classified, because classes are
dispatch and the reply-payload capability is the plan's own
per-number table, host-tested, not a new column the census check must
understand.

**The measurement, and its named risks.** The fixture sizes scale to
the partition budget — the census stays the host measurement at its
64 MiB fixture — and the gate reads the OPERATION, not a byte-identical
histogram: the snapshot appears under `--backup-dir`, the rename
lands, the retention count holds, the directory lists, exit 0 with
zero unattributed traps. Two risks are named NOW rather than
discovered. `clock_gettime` (228): the no-vDSO proof row (§5.5) says
the first user makes this call once, and NO time source exists across
the sidecar's ABI (verified against `kabi.rs` and `linux.rs` — there
is none), so if that call panics the workload, the run's last ENOSYS
line IS the evidence and the time seam is the NEXT increment — a
shared change (`aerosls-proto`'s `kabi` is shared by every sidecar,
with the gate that implies) whose three candidate sources are a
kernel-anchored manifest record beside ARGENV, CPUID+rdtsc for the
monotonic half, or widening ARGENV itself. `mmap`(8)/`munmap`(7)
mid-run: if the run reaches malloc's large allocations, the same
anonymous-map question answers itself from the run's own ENOSYS
lines. `poll`(1) and `execve`(1) stay refused by name, as below.

**What this increment is NOT.** `poll` — its revents are a copy-in of
a REWRITTEN array plus console-input state this shim has never had; it
belongs to the input class, unscoped. `execve` — the spawn class.
`clock_gettime` — the time seam named above. File-backed `mmap`.
Permissions reported but never enforced — mode comes from the VFS's
own metadata, and the first refusal a real workload meets is where
enforcement gets scoped, with its trigger. Symlinks — neither
filesystem has them. `chmod`/`chown`/`utimens`/`flock`/`dup`/`pipe` —
ENOSYS, and absent from the census, which is the census's own
discipline for staying out. Sockets — `devfs`'s refusal unchanged.
brk shrink — still §10.11's refusal. Threads, signal delivery and the
checkpoint half of the gate keep their named destinations (Appendix
B). §11's Q2, Q4 and Q5 are untouched here.

---

## 11. Open questions for review

1. **The first user: designated, built, and censused.** `AeroSLS-Roadmap-2026H2-v0.1.md` §5 says in bold that there is no named first user, and the tree agreed — no Go source, no `std`-Rust binary, every real operational workload bash or Python (§5.5.1) — so the target was a *decision*. It is now made and carried out: the backup operation, as `backup/aerobackup/` (a static single-threaded Rust rewrite of `backup/backup.sh`'s snapshot-and-retain core, 27 tests, no dependencies), designated in `tools/linux_abi_candidate.conf`, with the census measured and checked in at `tools/linux_syscall_census.txt` (§5.6). **The open question is no longer about the first user; it is whether this candidate is narrow enough to gate on.** Its measured surface is 27 syscalls, and the two that are structurally hardest — `arch_prctl(ARCH_SET_FS)` (§5.3) and the vDSO-backed `clock_gettime` — are both now named rather than discovered. Whether the other three operations follow, and in what order, is a product question this design does not answer.
2. **Stack sizing for Linux tasks.** §5.2 proposes measurement rather than inheriting `PROC_USER_STACK_PAGES`. Is a separate, partition-charged Linux stack accepted, or should the constant simply grow (which touches every ring-3 process)?
3. **Where the personality field's setter lives.** §5.6/§3.5 propose the spawn request, gated on the existing `sidecar_authority`. Confirm that reusing E2's authority is preferred to a new authority bit.
4. **The flood bound.** §10.2 requires a measured bound on a syscall-flooding task. Is "parks, and cannot reduce a neighbour's partition share" the right bar, or should the environment have an explicit syscall-rate ceiling as part of its E3 budget?
5. **`rseq` and the other modern start-up syscalls.** musl as built by common toolchains may call `rseq`, `membarrier`-class calls, or `prlimit64` at start-up; the census settles which, but a policy decision is needed now: `ENOSYS` with a logged name (recommended — it is visible and cheap) or silent success.

---

## Appendix A — Evidence index

Everything this document asserts about the tree, with its source:

| Claim | Source |
|---|---|
| Trap entry, `swapgs`, per-CPU scratch, asm number chain, `.unknown_syscall` → `do_syscall` | `arch/x86/syscall.asm` |
| The errno map is total over both families and the `CAP_NONE` sentinel, with the reverse defined only on unique preimages | `kernel/linux_abi.h`/`.c`; `tests/linux_errno_map_host_test.c` (51 checks, §10.5) |
| Every census number — traced and vDSO-declared — has a dispatch class, and the SLS overlaps 105/160/320 are UNKNOWN | `tests/linux_syscall_table_host_test.c` (42 checks) over `tools/linux_syscall_census.txt` |
| The initial stack builder emits the canonical psABI image with §5.2's AT_* set, no `AT_SYSINFO_EHDR`, and refuses by returning 0 | `tests/linux_initial_stack_host_test.c` (40 checks) |
| A Linux-personality task's trap describes-and-refuses before any number chain, from arguments captured before any call | `kernel/syscall_dispatch.c` (`do_syscall`'s first branch); `arch/x86/syscall.asm`'s `[gs:0x50..0x80)` stores; `tests/linux_personality_check.sh` clauses C/C1/D |
| The personality is explicit at spawn (328), stamped inside the create, gated by E2's `sidecar_authority`, and reset at every slot reuse | `kernel/cap.c` — `cap_create_sidecar_in_personality()`, `sys_sls_linux_spawn()`; `kernel/process.c` — `proc_clear_resume_state()`; guard clauses E/F |
| The frozen per-CPU offsets are a BUILD error to move, and the request ABI is pinned as literals | `arch/x86/user_paging.h` static asserts; `tests/linux_personality_host_test.c` (19 checks) |
| SLS ABI: `rax` = num, `rdi` = one request pointer | `kernel/syscall_dispatch.h`; `user/libsls/sls.h`; `user/aerosls/src/syscall.rs` |
| `syscall_regs[8]` at `[gs:0x10..0x48]`, and the live late-read failure | `arch/x86/user_paging.h` |
| Gate: `EFER`, `STAR` (`0x23`/`0x1B`), `LSTAR`, `SFMASK`, GS bases; **no FS base** | `arch/x86/user_paging.c`, `syscall_gate_init()` |
| 165 `SYS_` defines, 165 `case` arms | `kernel/` (counted) |
| `SYS_SLS_ALLOCATE` = 105, `SYS_SLS_CHMOD` = 107, `SYS_SLS_SET_USER` = 108 | `kernel/syscall_dispatch.c` |
| `SYS_SLS_PROC_*` 160–162 | `kernel/process.h` |
| `SYS_SLS_ALLOC_REGION` = 320 | `kernel/cap.h` |
| `CAP_EINVAL`…`CAP_EPERM` (−1…−12), `CAP_ERR_PROTO`, `CAP_NONE` sentinel | `kernel/cap.h`; `kernel/chan.c` (`sys_sls_dev_mmap`) |
| ELF64 image loading (gates, `PT_LOAD`, `p_flags`, zero-fill, partition-charged frames, `e_entry`) — the tree's ONE mapper, shared by the uploaded-program loader and the Linux create | `kernel/elf64_load.{h,c}` (extracted from `loader.c`, increment 7); guard clauses O1/O2 (§10.12) |
| A real `ET_EXEC` candidate enters at `e_entry` and completes the whole census; the first measurement caught the unsafe low-vaddr walk (`#PF error=0x15 rip=0x400000`) it now routes around | `user/sidecar/src/e7_candidate.rs`; `user/sidecar/fixtures/e7_candidate.elf` + `build_candidate.sh`; §10.12's two runs; clause O1, tooth t48 |
| Park/resume: `CapParkCtx`, `park_ctx`/`park_req`/`park_syscall`, `resume_sysret`, `cap_recv_resume`, `syscall_return_path` | `kernel/process.h`; `arch/x86/syscall.asm` |
| Per-process address space (`user_clone_page_table`), no thread-group sharing | `kernel/process.c` |
| `sidecar_authority` (E2), tenant profile, zero hardware caps | `kernel/process.h`; `user/init/src/posix_manifest.rs` |
| Sidecar personality tag exists (`SIDECAR_TAG_PERSONALITY 0x0001`; `"aerosls.posix.v1"`) | `kernel/cap.h`; `user/bootimage/src/builder.rs` |
| The sidecar is one trust domain; malicious-process isolation deferred | `AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md` §0, §7.4 |
| `checkpoint_trigger()` persists 16 data regions + tcache; **no process/sidecar state** | `kernel/checkpoint_mgr.c:127` |
| ARM64 is a translation/scheduling smoke harness, with a bespoke `svc-from-EL0` excursion | `kernel/kernel_arm64.c`; `arch/arm64/{boot_arm64.S,gic.c,mmu.c,uart_pl011.c}` |
| E7's constraints and gate; H2's "on the ARM64 kernel" and Node exclusion | `AeroSLS-Roadmap-2026H2-v0.1.md` §2 item #2 |
| E7's placement, its two questions, the checkpoint prerequisite, the dynamic-linking park | `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §10, §12, §13, §14 Q5 |
| No named first user; `strace` on their workload *is* the specification; dogfooding is the answer | `AeroSLS-Roadmap-2026H2-v0.1.md` §5 (bold), §7 |
| The four named operations: billing, monitoring, logs, internal tooling | `AeroSLS-Roadmap-2026H2-v0.1.md` §7 |
| No static Linux binary exists in the tree; real ops workloads are bash/python; no Go, no std-Rust | `backup/backup.sh`, `backup/restore.sh`, `monitor/health_check.sh`, `deploy/deploy.sh`, `utils/*.py`; only static ELFs are `my_sls_kernel.bin`, `sls_arm64_kernel.elf` |
| The monitoring candidate needs outbound TLS plus a webhook POST | `monitor/health_check.sh` |
| The billing candidate is a *fictional* TSX demo, and metering is already in-kernel | `AeroSLS-Navigator-Parity-Gap-Roadmap-v0.1.md` §118; `kernel/usage_metering.c` |
| Log rotation is configuration of existing tools, not a workload | `AeroSLS-Operational-MVP-Roadmap-v0.1.md` §"Log rotation" |
| The census instrument and its anti-placeholder designation contract | `tools/linux_syscall_census.sh`, `tools/linux_abi_candidate.conf` |
| The census: 26 traced syscalls, and the one declared vDSO backstop | `tools/linux_syscall_census.txt` |
| The vDSO backstop measured: the same binary with no `AT_SYSINFO_EHDR`, row for row | `tools/linux_syscall_census_novdso.txt` |
| The census and its proof, gated on every push, with the guard's own teeth proven | `tests/linux_abi_census_check.sh`; `tests/linux_abi_census_check_smoke.sh` |
| The auxv a static binary is started with, as a literal list that omits `AT_SYSINFO_EHDR` | `tools/e7_stack_launcher.c` |
| Steps 5–7 delivered: the post-then-park forward, the arm-before-post resume, a stray resume refused by name | `kernel/chan.c` (`k_linux_forward`/`k_linux_deliver`); `kernel/syscall_dispatch.c`; `kernel/cap.c` (`cap_linux_find_channels`); guard clauses I–K (§10.7) |
| A reply's status reaches `rax` only through the §5.7 map, after size and num-echo are enforced | `kernel/linux_abi.c` (`linux_syscall_apply_reply`); `tests/linux_personality_host_test.c` (42 checks, §10.7) |
| The sidecar's first answers: 218 echoed, 231 reaped, everything else ENOSYS with its number logged | `user/sidecar/src/linux.rs`; `user/sidecar/src/entry.rs` (`service`/`append_wait`) |
| The §5.2 image through the real create: child-vaddr pointers (`region_vaddr`), BIB-page reservation, 8 KiB floor, `AT_RANDOM` = entropy or `CAP_ECONFLICT` with the region returned | `kernel/cap.c` (steps 6–7); `kernel/linux_abi.{h,c}`; host tests L1/L2/E7c (377 checks); clauses O3/O4, teeth t44/t45/t46 (§10.12) |
| §5.2's argv/envp cross on the spawn request as the `ARGENV` record, with the empty-environment v1 fallback when it is absent and by-name refusals instead of truncation | `kernel/cap.h`/`cap.c` (`SIDECAR_TAG_ARGENV`, the parse arena, the §5.2 site); `user/proto/src/manifest.rs` (`ArgEnv`, `argenv_wire_bytes_golden`); `tests/cap_create_sidecar_host_test.c` L3/L4; guard clauses O6/O7, teeth t49–t52 |

## Appendix B — Named gaps, each with its destination

| Gap | Consequence if unaddressed | Destination |
|---|---|---|
| No Linux personality field on `struct ProcessDescriptor` | numbers cannot be interpreted at all (§2.2) | E7, §3.3 |
| Six argument registers not captured by the trap path | every forwarded syscall gets wrong arguments (§3.4) | E7, §3.4 |
| No initial stack/auxv builder | static libc start-up fails before `main` (§5.2) | E7, §5.2 |
| No `MSR_IA32_FS_BASE` handling | TLS setup fails; musl/glibc binaries die early (§5.3) | E7, §5.3 |
| No `brk`/`mmap` → budget mapping (`brk` landed, increment 6, §10.11; `mmap`/`munmap` remain) | sub-allocating heaps and `munmap` still refuse (§5.4) | E7, §5.4 — scoped as increment 8's step 1 if its step-0 diagnosis demands anonymous maps (§10.13); otherwise the next increment |
| No `CAP_E*` → Linux errno table (and a `CAP_NONE` sentinel in returns) | silent wrong errors (§5.7) | E7, §5.7 |
| Target designated, but no candidate binary and therefore no artifact | the table is still unmeasured; `--check` reports a named skip rather than a pass | write the crate, add the musl target, run `--run` |
| No thread groups / `clone` / `futex` | Go and every threaded runtime are out (§7) | named as Go's gate; unscoped |
| No ARM64 userspace OS | E7 cannot run there (§4.1) | H2 item #1 |
| No environment checkpoint/restore | the inherited gate cannot be passed (§8) | POSIX roadmap v0.2 §12 item 1 |
| No reply payload: `read` and the file class never cross back (§10.10's named gap) | the first user cannot open, read, stat, list, rename or prune — its file work never leaves the sidecar (§5.6) | E7 increment 8, §10.13 (scoped) |
| No time source across the sidecar's ABI | `clock_gettime` (228) refuses; the no-vDSO proof (§5.5) shows the first user makes this call | named §10.13: a shared change, its own increment after 8, decided by the run's evidence |
| No console-input state, so `poll` cannot answer | `poll` (census ×1) refuses; a blocking read never waits | the input class, unscoped (named §10.13) |
