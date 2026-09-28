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

- **argv/envp are the environment's to define** — the sidecar knows the command line and the environment (it is the thing that ran `exec`, Phase 2 §4.3); they cross on the spawn request.
- **auxv entries required by static musl/glibc start-up:** `AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_ENTRY`, `AT_BASE`(0), `AT_PAGESZ`(4096), `AT_HWCAP`(0), `AT_HWCAP2`(0), `AT_CLKTCK`(100), `AT_RANDOM`(16 bytes from `kernel/entropy.c` — absent, stack canaries are worthless), `AT_SECURE`(0), `AT_PLATFORM`("x86_64"), `AT_EXECFN`. `AT_SYSINFO_EHDR` is **absent**, which is correct and must be explicit: there is no vDSO, so `clock_gettime`-class calls are real syscalls and must be in the census (§5.5).
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

- **refuses to measure anything until a candidate is designated** — and a designation is not a name but a name plus a **provenance**, the real existing operation in this tree the binary is a rewrite of. A placeholder has no provenance, so a census of hello-world, or of whatever binary happened to be lying around, is not a run that is quietly wrong; it is a run that does not happen. Ten teeth (the tool's `--selftest`) observe each refusal firing, including the negative control that an artifact whose sha256 *does* match the designated binary passes;
- **enforces §5.1's image contract on the real file** before tracing it — `ET_EXEC`, no `PT_INTERP`, no `DT_NEEDED`. A PIE or dynamic candidate is refused with the reason rather than censused;
- traces with a **full `strace -f` trace**, not `-c`'s summary table (whose column layout shifts with the `errors` column, so a parser that misreads it yields a plausible, wrong histogram — the failure this tool exists to prevent), and keys each name to its **number** from the kernel's own `unistd_64.h`, because the shim dispatches on numbers;
- records the candidate's identity in the artifact's header — name, provenance, target and **sha256 of the traced binary** — so `--check` re-verifies that the artifact still describes the binary it claims to. **A stale census is refused**, which is what makes "re-run it when the binary changes" a guarantee rather than advice.

Then the shim's dispatch table is built **from that artifact**, and a host test asserts that **every number in the artifact has a table entry** (§10.1) — delete one entry and the test reddens by name. The artifact and its consuming test arrive together.

**The census has now run, and the first real run caught two wrong assumptions — which is the argument for having an instrument instead of a prediction.** The artifact is `tools/linux_syscall_census.txt`: 26 traced syscalls from `aerobackup`'s static musl build, plus one declared vDSO backstop.

- **The musl target builds static-PIE by default**, so §5.1's image contract refused the first attempt outright, before any table could be built on top of it. §5.1 now carries the correction and the flag.
- **The trace contained no clock syscall although the binary demonstrably read the clock** — its own output filename is a timestamp (`sls_storage-20260928-234732.img`) — so the read happened in the vDSO. That is a gap in the *measurement*, not in the binary: the shim has no vDSO by §5.2's own decision, so those calls become real traps there, and a **missing** requirement is indistinguishable from a requirement nobody needed. The tool therefore requires `CANDIDATE_VDSO_SYSCALLS` to be set — with the literal `none` as a distinct, explicit answer, because an unset key cannot be told apart from an oversight — and writes the set into the artifact as `# vdso-backstopped: <number> <name>` lines that `--check` validates against the kernel header and against the traced rows. `clock_gettime` (228) is the one declared, on that filename evidence; `gettimeofday` and `time` deliberately are not, since declaring a syscall with no evidence would be inventing a requirement rather than measuring one.

It is deliberately a `tools/` tool and not yet a `tests/*_check.sh` guard. CI runs `run_checks.sh --require-all`, where an exit-2 guard carrying no `# GUARD-KIND:` classification is a **failure**, and there is no honest classification for "no first user has been designated yet" — the missing input is a business decision, not a build artefact and not a live cluster. Wiring it in before an artifact exists would either redden CI for a reason nothing in CI can fix, or add a permanent silent skip. The guard and its smoke land with the artifact, as a short `tests/linux_abi_census_check.sh` that execs `--check`.

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
- **`clock_gettime` is absent from the trace and is nevertheless required**, for the vDSO reason §5.5 records.

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
| 228 | **`clock_gettime`** (declared, vDSO-backstopped) | sidecar-local; there is no vDSO, so on AeroSLS this one is a real trap |

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
| SLS ABI: `rax` = num, `rdi` = one request pointer | `kernel/syscall_dispatch.h`; `user/libsls/sls.h`; `user/aerosls/src/syscall.rs` |
| `syscall_regs[8]` at `[gs:0x10..0x48]`, and the live late-read failure | `arch/x86/user_paging.h` |
| Gate: `EFER`, `STAR` (`0x23`/`0x1B`), `LSTAR`, `SFMASK`, GS bases; **no FS base** | `arch/x86/user_paging.c`, `syscall_gate_init()` |
| 165 `SYS_` defines, 165 `case` arms | `kernel/` (counted) |
| `SYS_SLS_ALLOCATE` = 105, `SYS_SLS_CHMOD` = 107, `SYS_SLS_SET_USER` = 108 | `kernel/syscall_dispatch.c` |
| `SYS_SLS_PROC_*` 160–162 | `kernel/process.h` |
| `SYS_SLS_ALLOC_REGION` = 320 | `kernel/cap.h` |
| `CAP_EINVAL`…`CAP_EPERM` (−1…−12), `CAP_ERR_PROTO`, `CAP_NONE` sentinel | `kernel/cap.h`; `kernel/chan.c` (`sys_sls_dev_mmap`) |
| ELF64 loader: `PT_LOAD`, `p_flags`, zero-fill, partition-charged frames, returns `e_entry` | `kernel/loader.c` |
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

## Appendix B — Named gaps, each with its destination

| Gap | Consequence if unaddressed | Destination |
|---|---|---|
| No Linux personality field on `struct ProcessDescriptor` | numbers cannot be interpreted at all (§2.2) | E7, §3.3 |
| Six argument registers not captured by the trap path | every forwarded syscall gets wrong arguments (§3.4) | E7, §3.4 |
| No initial stack/auxv builder | static libc start-up fails before `main` (§5.2) | E7, §5.2 |
| No `MSR_IA32_FS_BASE` handling | TLS setup fails; musl/glibc binaries die early (§5.3) | E7, §5.3 |
| No `brk`/`mmap` → budget mapping | Rust/Go heaps cannot grow (§5.4) | E7, §5.4 |
| No `CAP_E*` → Linux errno table (and a `CAP_NONE` sentinel in returns) | silent wrong errors (§5.7) | E7, §5.7 |
| Target designated, but no candidate binary and therefore no artifact | the table is still unmeasured; `--check` reports a named skip rather than a pass | write the crate, add the musl target, run `--run` |
| No thread groups / `clone` / `futex` | Go and every threaded runtime are out (§7) | named as Go's gate; unscoped |
| No ARM64 userspace OS | E7 cannot run there (§4.1) | H2 item #1 |
| No environment checkpoint/restore | the inherited gate cannot be passed (§8) | POSIX roadmap v0.2 §12 item 1 |
