# AeroSLS POSIX Environments Roadmap v0.1 — on-demand POSIX environments inside partitions (the PASE equivalent)

## 0. Where this picks up

This project's north star has been IBM i since SIMI/SLIC were first modeled. One piece of that picture has never been assembled: **IBM PASE** — the ability to run a POSIX/UNIX workload *inside* a running IBM i instance, beside native programs, started on demand. Two of the three ingredients now exist here, built and verified, but in separate worlds:

- **Partitions.** `AeroSLS-LPAR-Roadmap-v0.1.md` Phases 8–14 built in-kernel multi-tenant partitions: partition-tagged catalog objects, gated spawn, a cross-partition IPC boundary, partition-fair scheduling, frame quotas with real per-frame reclamation, pause/resume, and destroy with full teardown (binary store, SIMI activation cache). The cluster work added partition replication, owned-set GC, write leases, failover adoption and migration across nodes. The live server runs this today as a 4-node `run-cluster`.
- **A POSIX environment.** Phase 5 (`AeroSLS-Self-Hosted-Phase5-Design-v0.1.md`, `AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`) built a POSIX sidecar — aerofs VFS over a ramdisk driver, `fork`/`exec`, pipes and redirects, applets, an interactive `sh` — verified end-to-end in QEMU.
- **Binary compatibility** — running existing binaries, which is what PASE is actually *for* — is planned (`AeroSLS-Roadmap-2026H2-v0.1.md` §2 item #2, a static-binary Linux ABI shim) and not built.

**The target.** On a node that is also serving the HTTP control plane and participating in the cluster:

```
partition create acme          # exists today
env create acme                # new: a POSIX environment inside partition acme
env attach <env>               # new: a shell in that environment
  $ echo hi | cat
env destroy <env>              # new; partition destroy acme also tears it down
```

This is a planning document, not a findings document — nothing below has been built. It follows the LPAR roadmap's phase shape (why, scope, explicitly not in scope, verification plan) so each phase's findings can land as an addendum in place.

### 0.1 The IBM i mapping, stated precisely

It is worth being exact, because the naming has misled this project once already (LPAR roadmap §9):

| IBM i | What it is | AeroSLS equivalent |
|---|---|---|
| **LPAR** | A hypervisor-carved hardware slice running its own complete OS and SLIC instance | A separate kernel boot — one `run-cluster` node. ✅ exists |
| Work isolation inside one OS | Subsystems, jobs, user profiles and object authority | In-kernel partitions (LPAR roadmap Phases 8–14). ✅ exists |
| **PASE** | An integrated runtime *inside* a running IBM i instance: AIX binaries run natively as jobs, their system calls served by the OS, started on demand (`QP2TERM`, `Qp2RunPase`) | **A POSIX environment inside a partition, created on demand** — this document |
| The PASE runtime | Ships with the OS; the programs it runs are what vary | The POSIX sidecar image ships in the boot image; environments instantiate it |

PASE is not layered *into* an LPAR by the hypervisor; it is a runtime within an OS instance. That makes the target here smaller than it first sounds: no new isolation layer is needed, only a way to instantiate the existing POSIX runtime, on demand, inside the isolation layer that already exists.

## 1. What exists, and why it does not compose yet

Each gap below was confirmed against the code, not inferred from phase names.

| # | Gap | Evidence |
|---|---|---|
| G1 | **The two boot worlds are mutually exclusive.** With an initrd, `launch_init_sidecar()` enters `init` in ring 3 and never returns — "the sidecar system IS the boot" — so the HTTP server and shell, where partitions are managed, never start. Without an initrd there is no sidecar world. The live cluster's nodes boot kernel-only, so production has partitions and no POSIX environment at all. | `kernel/boot_image.c` (step 6 comment, `launch_init_sidecar`); `kernel/kernel.c` step 7d; `run-cluster.sh` `build_node_iso()` (no Multiboot2 module line) |
| G2 | **The ring-0 control plane is not schedulable.** The timer ISR only calls `schedule_ring3()` when it interrupted ring-3 code; a tick that lands in the ring-0 HTTP/shell loop runs housekeeping and returns. `/api/program/spawn` is synchronous and blocks the HTTP loop until the child exits; an async child only runs while some other ring-3 context is current. A never-exiting sidecar and the ring-0 loop cannot share the CPU. | `arch/x86/interrupt.asm` `isr32_stub` (`cmp qword [rsp+8], 0x23` → `.ring0_timer`); `kernel/process.c` `program_spawn_common()` (async=0 blocks); `kernel/loader.c` `program_load()` → `program_spawn()` |
| G3 | **A sidecar cannot be created into a chosen partition.** `SLSCreateSidecarRequest` has no partition field; the child inherits its parent's partition and charges frames to it. `init` lives in `PARTITION_SYSTEM`. | `kernel/cap.h` `struct SLSCreateSidecarRequest`; `kernel/cap.c` `cap_create_sidecar()` (`pd->partition_id = parent->partition_id`) |
| G4 | **The POSIX runtime is a singleton.** Its manifest has fixed names (`aerosls.posix.0`, peers `drv.ramdisk.0`, `drv.network.0`), a 4 MiB heap at a fixed boot-layout physical address, and hardware capabilities meant for the Driver SDK test applets (COM1 port I/O, timer and serial IRQ binds, the NIC's BAR0). | `user/init/src/posix_manifest.rs` |
| G5 | **Sidecar names are global and silently re-pointed.** The name→pid registry is one 16-entry table for the whole kernel, and registering an existing name updates it in place. A second instance — or a tenant manifest that names `drv.ramdisk.0` — takes over another environment's peer. | `kernel/cap.c` sidecar registry (`sidecar_registry_register`: "Re-registering an existing name UPDATES it in place"); `kernel/cap.h` `SIDECAR_REGISTRY_MAX 16` |
| G6 | **Capacity is sized for one system, not many tenants.** 16 process slots for the whole kernel; 16 registry entries; the catalog binary store holds 16 KiB per object, while `init.bin` alone is 114,704 bytes. | `kernel/process.h` `PROC_MAX 16`; `kernel/loader.h` `LOADER_MAX_BINARY_SIZE 16384` |
| G7 | **There is one terminal.** `sh` is wired to `kernel.debug.console` (the serial console, pid 0 on the kernel side). N environments need N terminals. | `posix_manifest.rs` `console` cap; `kernel/console_service.h` |
| G8 | **No binary compatibility.** The POSIX sidecar implements POSIX on a sidecar-local ABI for its own applets; unmodified binaries do not run. | `AeroSLS-Roadmap-2026H2-v0.1.md` §2 #2 (planned); no shim code in the tree |

What *does* compose already, and should be reused rather than rebuilt: `cap_create_sidecar()` already stamps the partition and charges frames to it; `process_kill_partition()` already kills every process in a partition, sidecars included; a dying sidecar already leaves the registry (`sidecar_registry_remove_pid`); `partition_reclaim_all_frames()` already does real per-frame reclamation; Phase 12's partition CPU weights (`SYS_SLS_PARTITION_CPU_WEIGHT_SET`) already exist to trade control-plane latency against environment CPU.

## 2. Design principles

The LPAR roadmap's four disciplines carry forward unchanged — ground truth first, reuse the choke points, backward compatible by construction, audit every call site — plus three specific to this work:

1. **Tenant environments get zero hardware capabilities, enforced by the kernel.** A PASE program cannot touch hardware; neither may an environment. The kernel rejects port-I/O, IRQ and device capabilities in any manifest whose target partition is not `PARTITION_SYSTEM`, rather than trusting the manifest builder to omit them.
2. **The runtime ships with the OS; the programs vary.** The POSIX sidecar image stays in the boot image, shared and read-only, instantiated per environment — the same shape as the SIMI activation cache's shared code frames. Tenants do not upload runtimes (and the 16 KiB catalog store could not hold one anyway).
3. **Every new check ships with its tooth.** Each phase names a sabotage that must turn its guard red, following the project's existing smoke discipline.

Backward compatibility is concrete here: the kernel-only boot (the cluster today) and the Phase 5 initrd boot must behave byte-for-byte as before unless the new unified mode is selected, and no environment exists until one is created.

## 3. Roadmap

Phase numbers carry an `E` prefix so they cannot be confused with the LPAR roadmap's 8–15 or the self-hosted Phases 1–6.

| Phase | Deliverable | Depends on | Risk / lift |
|---|---|---|---|
| E1 | Unified boot — the ring-0 control plane and the sidecar world share one boot | — | **High** — scheduler core, ring-0/ring-3 handoff |
| E2 | Partition-scoped sidecar registry and a kernel-enforced tenant capability profile | — | Medium — likely host-testable |
| E3 | Multi-instance POSIX runtime — per-environment names, storage, and budget memory charged to the partition | E2 | Medium |
| E4 | Partition-targeted creation and an environment manager in `init` | E1, E3 | Medium |
| E5 | Environment lifecycle and teardown | E4 | Medium |
| E6 | Per-environment terminals and the control-plane surface (HTTP, shell, `aeroslsctl`) | E4 | Low-medium |
| E7 | Static-binary Linux ABI shim — the binary-compatibility half of PASE | E3 (runs inside an environment); environment checkpoint/restore from the v0.2 plan, to pass its gate (§10, §12) | **High** — its own design pass first |

Milestone **M1 — on-demand POSIX environments** is E1–E6. Milestone **M2 — PASE parity** is E7, and it also needs environment checkpoint/restore, which is not an M1 phase (§10, §12).

## 4. Phase E1 — Unified boot

**Why this is first, and hardest.** Every later phase needs a node where the HTTP control plane and long-lived ring-3 sidecars both make progress (G1, G2). Today the kernel has two modes precisely because they cannot: the timer's ring-0 path never schedules, and the Phase 5 boot resolves that by giving the CPU to `init` permanently.

**Options.**

- **(a) Preempt the ring-0 control plane from the timer.** Extend `isr32_stub`'s ring-0 path to switch to runnable ring-3 work. Rejected for the first cut: the kernel's ring-0 code was written assuming it is never preempted (single CPU, `IF=0` inside the timer ISR, lock-free structures that assume one writer), and preemption at arbitrary ring-0 instructions would have to be audited across the whole kernel.
- **(b) Cooperative yield at the control plane's safe points — recommended.** The HTTP loop and the shell loop already poll. At their idle points they call a new `kernel_yield_to_ring3(budget_ticks)`, which records the control plane's kernel context as a schedulable pseudo-process in `PARTITION_SYSTEM` and switches to the next runnable ring-3 process. The ring-3 timer path already preempts ring-3 code; when the budget expires, `schedule_ring3()` resumes the control plane through the existing kernel-context resume path — the same `resume_kernel` iretq path it already uses for processes woken from a blocking `cap_recv` park. Preemption happens only at points the control plane chose, so no ring-0 audit is needed.
- **(c) Dedicate an application processor to ring-3 sidecars.** Rejected for the first cut: ring-3 scheduling assumes a single CPU, and cluster nodes are autosized to few vCPUs.

**Scope.**
- A third boot mode, selected explicitly (a kernel command-line flag carried by a new grub entry, or by `run-cluster.sh` when environments are enabled). Kernel-only and Phase 5 boots keep their current behavior.
- In unified mode `launch_init_sidecar()` creates `init` as a runnable process and **returns**; boot continues into the HTTP server.
- `kernel_yield_to_ring3()` plus the control-plane pseudo-process, wired into `schedule_ring3()`'s rotation and into the HTTP and shell poll loops.
- Device ownership in unified mode: the kernel keeps the NICs (HTTP on the management NIC, DSPP on the cluster NIC) and the serial console. `init` does **not** spawn the Device Manager's e1000 driver or the network sidecar, and gets no console input — its output goes to the kernel log. Environments get terminals in E6.
- Latency policy uses the existing partition CPU weights: the control plane's share of the rotation is tunable, not hard-coded.

**Explicitly not in scope.** Ring-0 preemption; SMP ring-3 scheduling; any environment creation (E4).

**Verification plan.** A new boot check, `unified_boot_check.sh`, boots the unified entry and asserts both worlds make progress over the same window: `/api/health` answers repeatedly **and** a ring-3 heartbeat from `init` keeps advancing between requests. **Tooth:** remove the yield call from the HTTP loop — `/api/health` still answers but the heartbeat freezes, and the check fails. The existing kernel-only and Phase 5 boot checks run unchanged, proving the other two modes did not move.

## 5. Phase E2 — Partition-scoped registry and tenant capability profile

**Why.** G5 is a cross-tenant hazard independent of everything else: peer names resolve globally, and a colliding name silently re-points. It has to be closed before a second environment can exist, and it does not depend on E1.

**Scope.**
- Each registry entry records the partition of the process that registered it. Peer resolution matches within the caller's partition, plus an explicit allowlist of kernel-owned names (`kernel.*`, e.g. `kernel.debug.console`).
- Registering a name held by a **different** partition fails. Re-registering within the same partition keeps today's restart semantics (the watchdog-respawn path depends on them).
- Names may then repeat across partitions: `drv.ramdisk.0` in partition 5 and in partition 7 are different sidecars. This removes most of E3's renaming work.
- `cap_create_sidecar()` rejects `CAP_IO`, `CAP_IRQ` and `CAP_DEV` records when the target partition is not `PARTITION_SYSTEM`.
- Revisit `SIDECAR_REGISTRY_MAX` with the sizing in §11.

**Explicitly not in scope.** Changing how channels are minted, or cross-partition channels (the IPC boundary from LPAR Phase 11 stays as the rule).

**Verification plan.** The registry is freestanding, in the same idiom as `service_registry.c`, so this should reach real execution in a host test (`sidecar_registry_host_test.c`): cross-partition resolve denied; cross-partition name collision rejected; same-partition restart still re-points; a tenant manifest carrying an IO, IRQ or DEV cap rejected while the same manifest in `PARTITION_SYSTEM` is accepted. **Tooth:** drop the partition comparison — the cross-partition resolve assertion fails. The Phase 5 boot checks still pass (every Phase 5 sidecar is in `PARTITION_SYSTEM`).

## 6. Phase E3 — Multi-instance POSIX runtime

**Why.** G4: today's manifests describe exactly one POSIX sidecar, with its memory at fixed physical addresses and hardware capabilities attached.

**Scope.**
- A **tenant profile** for the POSIX manifest: `budget`, `console`, `ramdisk`; no `network`, no `uart`, no IRQ binds, no NIC BAR. The system profile (today's manifest) stays for the Phase 5 boot and the Driver SDK acceptance demos.
- **Per-environment storage.** Each environment gets its own ramdisk driver instance and backing memory, so one environment cannot read another's filesystem. Alternative to decide in review: a ramfs inside the POSIX sidecar, which costs one process slot per environment instead of two (§11).
- **Budget memory allocated, not fixed.** The heap and ramdisk storage come from the frame pool and are charged to the environment's partition (`allocate_physical_ram_frame_for_partition`), instead of `posix_manifest.rs`'s boot-layout physical addresses. Open question: whether MEM capabilities need contiguous regions (the ramdisk's `storage_base`/`storage_len` suggests yes) or can carry a frame list.
- **Image by reference.** Environments instantiate the POSIX image already in the boot image (`posix.image`), mapped read-only and shared (principle 2).

**Explicitly not in scope.** Networking inside environments; persistent environment storage (§10).

**Verification plan.** In the Phase 5 boot — which does not need E1 — `init` starts two tenant-profile instances in `PARTITION_SYSTEM`, each with its own ramdisk. Each runs a boot script that writes a file and lists the root. Assert both reach `System ready` and each sees only its own file. **Tooth:** point both instances at the same ramdisk — the isolation assertion fails.

## 7. Phase E4 — Partition-targeted creation and the environment manager

**Why.** G3: nothing can place a sidecar into partition N, and nothing on the control plane can ask for one.

**Scope.**
- `SLSCreateSidecarRequest` gains `target_partition` (0 = inherit, today's behavior). The kernel honors a nonzero value only when the caller is in `PARTITION_SYSTEM`, the target partition is active and not paused, and the budget fits the target's frame quota. The child's `partition_id` and every frame it allocates are charged to the target.
- **Environment manager in `init`.** A kernel-held request channel, resolved through the registry the same way the console service is, carries `ENV_CREATE { partition, name }` and `ENV_DESTROY { env }`. `init` builds the tenant-profile manifests (E3), creates the ramdisk and POSIX sidecars with `target_partition`, and replies with an environment id and its console channel. Keeping manifest construction in Rust, in `init`, avoids a second manifest builder in kernel C.
- Control plane: `POST /api/partition/{id}/env` and a shell `env create`, authorized by the existing role model (partition administrators only).
- Placement: an environment is created on the node that currently owns the partition (its write-lease holder). Cross-node placement is §10.

**Explicitly not in scope.** Destroy semantics (E5); terminals (E6).

**Verification plan.** `env_create_boot_check.sh` (unified boot): create a partition over HTTP, create an environment, and assert its processes appear with that `partition_id`, its frames are charged to that partition's quota, and a non-system process's `create_sidecar` with `target_partition` set is denied. **Tooth:** remove the `PARTITION_SYSTEM` authorization — the non-system create succeeds and the check fails.

## 8. Phase E5 — Lifecycle and teardown

**Why.** A create path without a leak-free destroy path is worse than no create path (the LPAR roadmap's Phase 14 lesson).

**Scope.**
- `env destroy`: `init` kills the environment's sidecars; the registry drops their names (existing `sidecar_registry_remove_pid`); channels close.
- `partition destroy` already kills every process in the partition. `init` must treat the resulting peer-closed channels as an environment ending, clear its environment table, and **never** restart a tenant environment — the Device Manager watchdog's respawn policy must not apply to it.
- `partition pause` already removes the partition from scheduling; confirm environments stop and resume cleanly.
- Frames are reclaimed by the existing `partition_reclaim_all_frames()` on partition destroy. `env destroy` without a partition destroy must free the environment's budget and storage frames individually.

**Explicitly not in scope.** Graceful in-environment shutdown (signalling processes inside the POSIX sidecar before the kill).

**Verification plan.** A recycle program in the style of `part_recycle.c`: N cycles of partition create → environment create → run a command → destroy, where N exceeds `PROC_MAX` and `SIDECAR_REGISTRY_MAX`. Assert process slots, registry entries and the system frame count are stable across all N. **Tooth:** skip the registry cleanup — the cycle that exceeds the registry size fails with registry full.

## 9. Phase E6 — Terminals and the control-plane surface

**Why.** G7: an environment you cannot reach is not an environment.

**Scope.**
- Each environment's console is a kernel-brokered channel pair instead of `kernel.debug.console`.
- HTTP attach: stream output and forward input, within the constraints already worked out in `AeroSLS-Web-Terminal-Plan-v0.1.md`.
- `env list`, `env attach`, `env destroy` in the shell, HTTP and `aeroslsctl`, documented in `docs/COMMANDS.md` (which `commands_doc_check.sh` enforces).

**Explicitly not in scope.** Terminal multiplexing; authentication beyond the existing token and role model.

**Verification plan.** An HTTP end-to-end check: create two environments, attach to both, send `echo hi-$ENV | cat` to each, and assert each stream contains only its own output. **Tooth:** cross-wire the two consoles — the isolation assertion fails.

## 10. Phase E7 — Static-binary Linux ABI shim

`AeroSLS-Roadmap-2026H2-v0.1.md` §2 item #2 already scopes this, and its constraints stand: statically linked binaries only (static musl or nothing, no `dlopen`), a syscall surface measured with `strace` rather than guessed, and a strict order — static Rust or Go first, static CPython only if the first user needs it, Node explicitly out of scope. Its gate: a static binary doing the first user's actual work and surviving a checkpoint/restore cycle.

This document adds only where it runs: **inside an environment**, as a Linux-personality task of the POSIX sidecar, so it inherits E1–E6's partition isolation, lifecycle and terminals. Two questions must be settled in a dedicated design pass before anything is built:

1. **Syscall routing.** A static binary's `syscall` instruction traps to the kernel. Either the kernel dispatches Linux-personality syscalls itself (the H2 roadmap counts 131 existing `SYS_` handlers as a head start), or it forwards them to the owning POSIX sidecar's core over a channel, keeping POSIX semantics in one place. These have very different security and performance profiles.
2. **Architecture.** The H2 roadmap targets native ARM64; the sidecar world and the live cluster are x86-64 today.

**A prerequisite outside M1.** The inherited gate requires surviving a checkpoint/restore cycle, and no environment state is checkpointed today. `checkpoint_trigger()` (`kernel/checkpoint_mgr.c`) persists kernel data regions — the catalog, program store, partitions, row and vector stores, services, workloads — but no process or sidecar state: not a sidecar's address space, not its capability table or channels, not its ramdisk contents. E7 therefore cannot pass its gate until environment checkpoint/restore exists. That is the first item of the follow-on plan in §12, and that plan must be written before E7 starts.

## 11. Capacity for the first cut

| Resource | Per environment (ramdisk-sidecar option) | Per environment (in-process ramfs option) | Kernel limit |
|---|---|---|---|
| Process slots | 2 (POSIX + ramdisk) | 1 | `PROC_MAX` 16, shared with `init` and any ring-3 programs |
| Registry entries | 2 | 1 | `SIDECAR_REGISTRY_MAX` 16 |
| Memory | 4 MiB heap + ramdisk storage, charged to the partition | 4 MiB heap + ramfs, charged to the partition | Partition frame quota |

With the ramdisk option, `PROC_MAX` allows at most seven environments beside `init`, fewer once ring-3 programs run. **M1 targets four concurrent environments per node.** Raising `PROC_MAX` touches many fixed-size arrays; it becomes its own audited phase when a user needs more, not a side effect of E3.

## 12. Not in this plan

Each excluded item has a stated destination, so none is silently dropped.

**Closed — not applicable.**

- **Nested environments.** IBM i has no environment-inside-an-environment, for the same structural reason LPAR roadmap §9 closed nested partitions. Closed, not deferred.

**Belongs to E7's design pass.**

- **Dynamic linking.** The H2 roadmap limits the shim to static binaries. If the first user needs dynamic linking, E7's design document decides it; it does not need a plan of its own.

**Deferred to a follow-on plan: POSIX Environments Roadmap v0.2.**

These three are one plan rather than three, because each depends on the one before it:

1. **Persistence first.** Environment checkpoint/restore — a sidecar's address space, capability table, channels and storage — and storage that survives reboot, such as an SLS-backed aerofs in place of RAM-backed ramdisks. It comes first because failover needs it, because E7's gate needs it (§10), and because an environment that survives reboot is the single-level-store property containers do not have.
2. **Networking second.** Kernel-mediated sockets scoped by the existing per-partition connection quotas, with the NICs staying kernel-owned. It precedes migration because connection identity and addressing have to be settled before an environment can move.
3. **Cluster placement, migration and failover last,** building on the first two and on the partition migration and failover that already exist.

**When to write v0.2:** after E4 lands and before E7 starts. By then the environment's shape is fixed — ramdisk or in-process ramfs (§14 Q2), how budget memory is allocated (Q3), where the environment manager lives (Q4) — and persistence and networking attach to exactly those choices; writing v0.2 earlier would build on guesses. The one exception: if Q6 shows the first user needs networking during M1, pull networking forward on its own.

## 13. Suggested execution order

E1 and E2 have no dependency on each other and should start together: E2 is low-risk and host-testable, and E1 is the riskiest phase in the plan, so it should surface its problems early. Then E3, E4, E5 and E6 in order, which completes M1. The follow-on POSIX Environments Roadmap v0.2 (§12) is written once E4 has landed. E7 starts with its own design document once M1's environments exist to run it in, and can pass its gate only after v0.2 delivers environment checkpoint/restore.

## 14. Open questions for review

1. E1: is cooperative yield at the control plane's poll points (recommended) acceptable for HTTP latency under a busy environment, given the partition CPU weights?
2. E3: one ramdisk sidecar per environment, or an in-process ramfs to halve the process-slot cost?
3. E3: contiguous MEM regions or frame-list MEM capabilities for environment budgets?
4. E4: environment manager in `init` (recommended) or in kernel C?
5. E7: kernel-side Linux-personality dispatch, or forwarding to the POSIX sidecar core? And x86-64 or ARM64 first?
6. §12: does the first user need networking during M1? If so, it moves ahead of v0.2's persistence-first order.
