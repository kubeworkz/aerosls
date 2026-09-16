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

### 4.1 Findings (2026-09-17) — E1 landed; the unified boot is live

Branch `feat/e1-unified-boot`. Option (b) was taken as written: a cooperative yield at the control plane's idle points, no ring-0 preemption.

**What landed.**

- **The boot mode.** `unified=1` on the kernel command line, parsed once and cached by `boot_params_unified_mode()` (with the same "asked for badly" warning `node=` carries, so a typo cannot silently produce the old boot). grub.cfg gained a third menu entry, `AeroSLS — unified (control plane + sidecars)`, deliberately *appended* so entries 1 and 2 stay where every existing boot check expects them; `tests/grub_select_kernel_only.sh` grew an optional 4th argument (the 1-based menu position) to reach it, defaulting to the entry it has always selected.
- **The pseudo-process.** `proc_control_plane_init()` plants PID 1 `kplane` in `PARTITION_SYSTEM`, `PROC_BLOCKED`, owning no frames, no syscall stack and no cap table. `kernel_yield_to_ring3(budget_ticks)` records the Ring-0 continuation into it and switches to the next runnable Ring-3 process; `proc_control_plane_tick()` (timer ISR, immediately beside `cap_park_deadline_tick`) flips it back to runnable once the budget (2 ticks, ~20 ms) expires, and the next `schedule_ring3()` resumes it through a kernel-context iretq — the fourth resume shape, added to `proc_build_resume_frame()` so it cannot drift from the three that already worked (`resume_sysret`, `resume_kernel`, plain ring-3). The switch itself is `kernel_yield_switch`/`kernel_resume_control_plane` in `arch/x86/process_enter.asm`.
- **`launch_init_sidecar()` returns** in unified mode, leaving `init` `PROC_SUSPENDED` with the synthetic `ring3_ctx` `cap_create_sidecar()` already built — no hand-crafted context, and the kernel proceeds into `http_server_run()` instead of `kernel_enter_sidecar()`.
- **"Is this the kernel?" stayed answered the same way.** `process_find_current()` skips the control plane, so `cap_current_pid() == 0`, the park hooks still report "kernel context, cannot park", and a send from the control plane still does *not* hand off the CPU (the woken receiver runs at its next schedule). The control plane is also excluded from `process_kill_partition()`, and `proc_runnable()` learned the new resume flag.
- **Device ownership.** The kernel keeps the NICs (step 7's roles, unchanged) *and* the console: `console_service_set_input_forward(0)` stops the console service from also delivering typed lines to sidecars, and the BIB (now v2, with a `flags` word) carries `SIDECAR_BIB_FLAG_UNIFIED` so `init` reads `BootInfo::is_unified()` and keeps to the software-only part of its spawn chain — Device Manager and its own ramdisk driver, no network driver and no system POSIX sidecar. `read_line()`'s wait and `http_server_run()`'s sweep tail are the two yield sites.

**Two deviations from §4, both deliberate.**

1. *The tooth is not "remove the yield call".* That remains the sharpest theoretical tooth, but it needs a second ISO built from patched sources. `tests/unified_boot_check_smoke.sh` instead points the *same* guard at grub entry 2 (the kernel-only boot), which has neither a yield nor `init`, requires it to fail, and then runs the guard unmodified and requires it to pass. The guard's own assertions still bite on a yield regression: heartbeats counted *after* yields and yields counted *after* heartbeats are separate assertions, so a control plane that stops yielding freezes both counters (the guard fails at the boot-marker phase) even though `/api/health` keeps answering.
2. *Two knobs, not one.* §4 asks for the control plane's share of the rotation to ride the existing partition CPU weights. It does — the pseudo-process is an ordinary `PARTITION_SYSTEM` member — but the yield budget itself (`PROC_CONTROL_PLANE_BUDGET_TICKS`) is a separate, smaller knob: it bounds the HTTP latency the control plane can suffer while Ring-3 work is runnable (~20 ms worst case), which the partition weights cannot express.

**The bug the boot found, for the record.** The control plane's first plant took "the first free slot" (0) and `boot_plant_parent()` — which `memset`s `proc_table[0]` unconditionally, by design — erased it a few hundred lines later. The boot log said it plainly (`[SIDECAR] boot parent planted: slot0 pid=99 …` landing on top of `[E1] control plane planted as PID 1`), and the symptom was a boot that looked healthy while `kernel_yield_to_ring3()` silently returned every time (no `[E1] yield` lines, no `[INIT]` output, HTTP serving). `proc_control_plane_init()` now plants into the **highest** free slot and skips slot 0, and both sides carry a comment stating the reservation, so the collision is impossible rather than avoided.

**The bug the guard found, for the record: two writers, one UART.** The unified boot is the first boot with two concurrent console writers — the BSP's Ring-0 control plane and the AP's console-service drain, which prints the Ring-3 sidecars' log lines. Both bottom out at `kernel_serial_putchar()`, and two writers inside that loop emit byte-interleaved text (observed: `[INIT] UNIF<kernel line>IED boot`). It is not cosmetic: the boot guards read exactly that log, and the guard's BIB assertion (`[INIT] UNIFIED boot`) failed in one smoke run purely because its marker had arrived split. The fix is at the writers: `kernel_serial_tx_lock()`/`kernel_serial_tx_unlock()` (`kernel/kernel_io.h`) bracket `kernel_serial_print`/`printf`/`print_hex64` and the console drain (`kernel/console_service.c`), so one line — or one sidecar console message — is written whole. The wait is **bounded** on purpose: a caller that cannot acquire prints anyway and releases nothing, so no core can park on a printer and a print from interrupt context (where the same core may already hold it) degrades to the pre-E1 unlocked behaviour instead of self-deadlocking. The guard's BIB assertion now reads the kernel's own line (`[E1] BIB v2 flags=0x1 (UNIFIED) …`), which is kernel TX and therefore whole; init's *having read* the flag stays asserted from the other side, by Phase 3's requirement that the POSIX sidecar and the network/e1000 drivers be unspawned.

**The lock's own second bug, for the record: the wait hammered the machine.** That first lock shipped as a compare-and-swap retry loop, so a writer waiting on a slow console line issued `lock cmpxchg` on one cacheline up to the whole spin budget (200 000) per failed acquisition — and under QEMU's multi-threaded TCG each of those is a helper call taking a lock the other vCPU thread needs in order to make progress at all. The wait now reads the flag with a **plain load** and issues one atomic only when the load reports the port free, so a *held* window contains no atomic read-modify-write while an *uncontended* acquire still costs exactly one. Both halves are asserted on the host by `tests/serial_tx_lock_host_test.c` through the `kernel_serial_tx_try_acquire` seam (`tests/kernel_serial_tx_seam.h`), and a swap-per-iteration wait built from the same sources fails it (2 checks: the waiter's atomic count goes from 0 back to 200 000).

The obvious cheaper shape — give up after a **single** attempt, so a waiter never spins at all — was built and measured as a third arm, and is **not** a substitute for it. Whole-line output is the lock's entire purpose, and with the loser printing immediately two writers interleave byte by byte: over 14 interleaved boots its torn-line count was **57**, indistinguishable from an arm whose lock never acquires at all (**62**), where the test-then-set shape tore **0** in 7/7 boots against a same-window control that tore **24**. (`tryonce` was therefore rejected, not kept.)

**This is not a claim about the boot wedge.** The wait was changed for atomic traffic, not for the wedge, and the wedge still reproduces on this tree: `tests/phase5_boot_flake_guard.sh` failed **2/6** twice in a row on 2026-09-18 with only this change in place, at the same signature as before (`System ready` then `$$`, QEMU alive, log stopped). What is recorded for whoever picks the wedge up next is that the *episode* is the variable — not the build, and not the boot medium: 58 boots run from `/tmp` ISO copies between 10:23 and 11:52 that day were clean, and then three runs between 12:27 and 13:12 wedged **2/6 each** on the same binary (in-tree ISO 12:27, in-tree ISO 12:47, a `/tmp` copy 13:00), i.e. the medium did not separate clean from wedged boots at all. A quiet-window A/B is therefore worth nothing, and any future comparison of a wedge change has to be interleaved inside one reproducing episode. (**Corrected 2026-09-18:** that signature is not a stop — see *The flake guard's false failure* below. Those 2/6 runs were the guard failing on the shell's own prompt bytes.)

**And the wait is not the wedge's cause.** One such interleaved comparison was run inside the 12:27–13:41 episode: the *previous* swap-per-iteration wait against this one, 10 boots each, alternating order. They wedged identically — **2/10 vs 2/10** (Fisher exact p = 1.0), at the same signature and the same spread (boots 2, 3, 5, 6, i.e. not at the start of a run, which is what the guard's two 2/6 runs had suggested). So removing the atomic storm from the print path is worth doing for its own sake, and it is **neutral** on the wedge. Worth noting for the next attempt: one boot of that comparison's control run failed differently — QEMU *exited* at 70 s with the log stopping at `[IRQ] unmask vector 36: pin 4` — which is the same stopping point as the batch wedges seen on 2026-09-17, so if it recurs it is a second, distinct defect rather than the lost wake.

**The flake guard's false failure, for the record: `System ready` then `$$` is a live shell, not a wedge.** The failing signature the two paragraphs above call a wedge — `ALL PHASES PASS`, then `System ready`, then `$$` and silence with QEMU alive — is the **shell's own prompt arriving byte-glued**, and the boot was running the whole time. Three measurements, in order of strength:

1. *Live counter-example.* A socket console plus an injected `echo <token>` after the log went quiet: the boot whose log ended `System ready\n$$ ` **answered the command** — as did all five clean boots where the probe ran in that harness (2 torn+clean boots of 7 were probe-less: one QEMU died at startup under the memory hog, one hit the harness window). A machine that answers an injected `echo` is not a machine that stopped.
2. *The corpus.* The 130 surviving boot logs split into 83 ending `\n\n$ `, 12 ending `\n$ $` and 10 ending `\n$$ ` — the same three bytes, three orders. All ten glued logs are the guard's failures, spanning every serial-lock arm (`locked`, `tryonce`, `nolock`, `testthen`) and both this host and the CI runner (CI's own log shows it: the guard's stderr report runs into the boot tail, printing `$$ FAIL: 1/6 boots missed…`). A failure that follows *every build* is not a build's regression.
3. *The stream is not byte-faithful.* `ALL PHASES PASS` was intact in **162/162** logs; `System ready` was intact in 131, **one byte short** ('ystem ready' — the leading `S` gone) in 27, had **a byte spliced in** ('Sys\ntem ready') in 4, and was **splintered** ('$ $ stem ready') once. So the guard was betting its verdict on the one line in the log the console demonstrably mangles.

**The host-trigger search came up empty, and that is the finding.** The guess was a host episode — reclaim or scheduling stalling a vCPU thread inside the guest's park/wake handshake. It does not hold up: with a hog forcing host memory pressure across a boot, PSI memory `full` averaged **17.5%** (and **33.8%** in the boot right after) with **11 264 pages swapped out**, ~917 k pages scanned and `allocstall` in the hundreds — and those boots still finished and answered. The only torn boot of that run was a **control** boot at PSI 4–5%. Twelve CPU spinners had already moved boot wall-clock only 87 s → 96 s. Neither CPU load, nor memory pressure, nor the ISO's read path (the earlier session's finding), nor QEMU vCPU thread states separates a torn boot from a clean one. What separates them is the console's own plumbing — sampled at a few percent per boot, not a state the host can be blamed for. (**Corrected 2026-09-18:** not two writers racing on the wire — the TX lock serialises those — but two *drainers* sharing one kernel buffer; see *The torn prompt's cause, found and fixed* below.)

**What that means for the genuine flake.** The lost wake the guard exists for is a different event: a boot that stops *mid-chain*, e.g. `phase5_boot_smoke` on `main` (2026-09-18) stopping at `[nettest] client: socket(TCP)` with zero `[FAULT]`/triple-fault reports in the whole job log. Those boots never print the phase marker or the prompt, so they still fail — verified over the corpus: 162/162 end-state logs are accepted and the only rejected boot log is one truncated mid-caps-table.

**What changed (2026-09-18).** `tests/phase5_boot_markers.py` is the one classifier both the guard and its teeth now use: a boot finished when the phase marker and the prompt are both present, the prompt counted as prompt *bytes* in any of its three orders (glued ones reported as `torn`), each marker tolerating **one** damaged byte and reporting that it did, and `System ready` measured but **not** required. `tests/phase5_boot_flake_guard.sh` judges with it, and on failure prints which marker was missing plus the host's PSI/reclaim counters for the boot (so a thrashing runner is visible instead of inferred). `tests/phase5_boot_flake_guard_smoke.sh` is its source-only teeth: eight marker fixtures (intact, glued, one-byte-short ready, spliced ready, truncated, mid-phase, mid-line `$ `, absent log) plus five end-to-end runs of the guard itself against a **stub qemu** that writes the log — `ISO`/`QEMU` are overridable for exactly that — including the glued-prompt boot that must now **pass** and the truncated one that must still fail naming `prompt=0`. Over the 185 recorded logs the classifier accepts **14** the old rule failed and fails **0** it accepted.

**The torn prompt's cause, found and fixed (2026-09-18).** `console_service_tick()` runs on **two cores** — the AP's service poll and the BSP's deferred timer tick — and both halves of it shared kernel state. The RX half had already been given a compare-and-swap single-flight after typed input came back as `ecoh` with h/o swapped and one of two typed lines vanished; the **TX half had none**, and it prints out of one static `console_svc_buf`, holding those bytes for as long as the UART takes to shift them out (a 4 KiB message at 115200 baud is ~356 ms). A second drainer refilling that buffer mid-print makes the first emit the *other* message's bytes — dropped bytes and duplicated runs, i.e. exactly `System ready` → `ystem ready` and the prompt pair arriving as `$$ ` instead of `$ $ `. `tests/console_tx_atomic_host_test.c` (new) links the real `console_service.c` and calls the tick again from inside the UART TX hook — the two-core interleaving without the timing luck — and reproduces the pre-fix wire bytes `$System ready\r\ny` and `[$  AP] objects active=…`; with the single-flight CAS the same re-entrancy prints nothing and both messages come out whole and in order. Teeth: reverting just the CAS fails that test on 4 checks. Boot-level A/B, 12 interleaved boots per arm from ISOs differing by nothing but the CAS (both carrying a diagnostic race window, since the field rate is only a few percent): the pre-fix arm lost a byte from `System ready` in **12/12** boots with **8** glued prompt pairs; the fixed arm **0** and **0**. The field-rate corpus agrees — the unwidened pre-fix ISO glued a prompt in 1 of 12 boots, the unwidened fixed ISO in 0 of 12. So the prompt was never emitted as more than one console message (`user/sidecar/src/applets.rs` writes `b"$ "` in a single `write`): those bytes were being *lost after* they were written, which is why the fix belongs in the drain and not in the shell. `$ $` with both bytes present is the shell's legitimate re-prompt around a command that printed nothing, and stays.

**Verified (2026-09-17, this host).**

- `tests/run_all.sh` — **112/0** with the new `serial_tx_lock_host_test` (25 checks) in the count; `tests/run_checks.sh` 27 passed / 1 failed / 6 skipped, the failure being §6.1's pre-existing `e3_multi_env_boot_check.sh`.
- `tests/unified_boot_check.sh` — 17 assertions green on the unified entry, including the two that only a unified boot can satisfy: `/api/health` answered twice across a heartbeat sample (heartbeats advanced 6 → 57 while the control plane served), and a typed `help` answered by the *kernel* shell (the control plane owns the console). Boot-to-verdict ~15 s.
- `tests/unified_boot_check_smoke.sh` — tooth (kernel-only entry) fails in ~8 s naming the contradiction; restore run passes. ~19 s total.
- The other two modes did not move: `tests/run_all.sh` 111/0; the boot guards the serial-TX change touches all pass on the same ISO — `phase5_boot_smoke.sh`, `phase5_e1000_driver_smoke.sh` (`[e1000] PASS` present), `aerocap_boot_check.sh`, `cap_boot_check.sh` — and the kernel-only boot still reaches `/api/health` (CI's own decoder-build step).
- BIB v2's consumers were all updated with it: `user/proto/src/bootinfo.rs` (parser + a `unified_flag` unit test), the sidecar test's BIB builder (now reading the version from the parser's constant so a future bump cannot desynchronise it), `tests/cap_create_sidecar_host_test.c`'s total-length expectations, and the wire-format note in `kernel/cap.h`.

**Not done here.** `env_create_boot_check.sh` (§7.1) was waiting on exactly this surface — the HTTP control plane and `init` in one boot — so it is now unblocked; the shell `env create` surface (§7) attaches with it. `run-cluster.sh` does not yet offer the unified mode: it generates each node's command line, so the flag would be a few lines there, but no cluster scenario exercises the unified boot yet and §4's selector is the grub entry. SMP ring-3 scheduling and the ring-0 preemption audit stay out of scope, as §4 says.

## 5. Phase E2 — Partition-scoped registry and tenant capability profile

**Why.** G5 is a cross-tenant hazard independent of everything else: peer names resolve globally, and a colliding name silently re-points. It has to be closed before a second environment can exist, and it does not depend on E1.

**Scope.**
- Each registry entry records the partition of the process that registered it. Peer resolution matches within the caller's partition, plus an explicit allowlist of kernel-owned names (`kernel.*`, e.g. `kernel.debug.console`).
- A registry entry is **scoped to the partition that registered it**, so the same name in two partitions is two separate entries — there is no cross-partition collision to reject; the scoping itself is the isolation. Re-registering within the same partition keeps today's restart semantics (the watchdog-respawn path depends on them).
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

### 6.1 Findings (2026-09-17) — `e3_multi_env_boot_check.sh` is red on `main`

Found while validating E1, and **not** an E1 regression: the check fails identically on the pristine tree at `8350ed1` (a detached worktree of `main`, no E1 changes) and on the E1 branch, on the same host, with the same verdict —

```
FAIL  within 120s: init done-marker or 3 live rootfs never seen (live rootfs: 2/3)
```

What the boot shows: both tenants ARE spawned (`[INIT] E3 env 1/2: drv.ramdisk.<i> spawned`, `aerosls.posix.<i> spawned`), the boot completes to `System ready` and the shell prompt, but only two `[POSIX] aero state=00000000` (live rootfs) lines ever appear, and init's `[INIT] E3: tenant environments spawned.` marker is missing from the log even though the line is printed unconditionally after the tenant loop (`user/init/src/entry.rs`) — i.e. at least one tenant POSIX instance does not reach a live rootfs inside the window, and the marker line is lost on the shared console. `x86-iso-e3` is built by no CI job (§5's `kernel-guards` builds only `x86-iso`), which is why this has been invisible.

Left as found — it is E3's to close, not E1's — but recorded here because E1's own guard work now depends on reading that same shared console, and because the check's `done_marker` gate is the fragile-grep shape E1's guard had to stop using (see §4.1).

## 7. Phase E4 — Partition-targeted creation and the environment manager

**Why.** G3: nothing can place a sidecar into partition N, and nothing on the control plane can ask for one.

**Scope.**
- `SLSCreateSidecarRequest` gains `target_partition` (0 = inherit, today's behavior). The kernel honors a nonzero value only when the caller is in `PARTITION_SYSTEM`, the target partition is active and not paused, and the budget fits the target's frame quota. The child's `partition_id` and every frame it allocates are charged to the target.
- **Environment manager in `init`.** A kernel-held request channel, resolved through the registry the same way the console service is, carries `ENV_CREATE { partition, name }` and `ENV_DESTROY { env }`. `init` builds the tenant-profile manifests (E3), creates the ramdisk and POSIX sidecars with `target_partition`, and replies with an environment id and its console channel. Keeping manifest construction in Rust, in `init`, avoids a second manifest builder in kernel C.
- Control plane: `POST /api/partition/{id}/env` and a shell `env create`, authorized by the existing role model (partition administrators only).
- Placement: an environment is created on the node that currently owns the partition (its write-lease holder). Cross-node placement is §10.

**Explicitly not in scope.** Destroy semantics (E5); terminals (E6).

**Verification plan.** `env_create_boot_check.sh` (unified boot): create a partition over HTTP, create an environment, and assert its processes appear with that `partition_id`, its frames are charged to that partition's quota, and a non-system process's `create_sidecar` with `target_partition` set is denied. **Tooth:** remove the `PARTITION_SYSTEM` authorization — the non-system create succeeds and the check fails.

### 7.1 Findings (2026-09-16) — parts 1–3c landed; end-to-end boot check deferred to E1

The E4 build was taken as far as it can go without E1. What landed (branch `feat/e4-partition-targeted-creation`, PR #36):

- **Kernel `target_partition` primitive.** `SLSCreateSidecarRequest` gained `target_partition` (replacing four pad bytes); `cap_create_sidecar_in()` resolves the child partition (0 = inherit the parent's) and, for a nonzero target, requires the caller in `PARTITION_SYSTEM` and the target present and not paused, else `CAP_EPERM`/`CAP_EINVAL`. The child's `partition_id` and its image, stack and syscall-stack frames are all charged to the target; the per-frame partition allocator enforces the quota. `cap_create_sidecar` is now the `target=0` inherit wrapper. Mirrored in `proto/kabi.rs` as `k_create_sidecar_in` and a defaulted `Kernel::create_sidecar_in`.
- **Environment manager in `init` (Rust).** `user/init/src/env_manager.rs`: `create_environment()` allocates the budget/storage/heap regions, builds the E3 tenant-profile manifests (`drv.ramdisk.<i>` + `aerosls.posix.<i>`) and creates both via `create_sidecar_in(manifest, partition)`; `EnvManager::handle_request` dispatches `ENV_CREATE`/`ENV_DESTROY` and replies. Manifest construction stays in Rust, in `init` (§14 Q4, as recommended).
- **ENV_* wire protocol.** `user/proto/src/env_proto.rs` with a C mirror `kernel/env_proto.h`, pinned against each other by `tests/env_proto_host_test.c`.
- **Kernel-held control channel + synchronous kernel→`init` RPC.** `kernel/env_service.c`: the registry wires `kernel.env.control` at boot the way the console service is wired; `env_service_create()` sends `ENV_CREATE` on the kernel end and spins the ring-3 scheduler until `init` replies, bounded by a tick deadline; `console_service_tick` excludes the ENV reply slot so the reply is not drained to serial.
- **`init` event-loop dispatch.** `run_supervisor_loop` waits on the env channel alongside the Device-Manager channels and routes ENV requests to the manager.
- **Control plane.** `POST /api/partition/{id}/env` (partition-admin role) parses `{"index":N}`, calls `env_service_create`, and returns JSON.

**Verified now.** All host/unit suites green (kernel `cap.c` host tests, the `env_proto` host test, the env-manager tests, the bootimage tests, the `init` tests). The control channel is **boot-verified as wired** in the Phase-5 self-hosted boot: `[SIDECAR] … wired chan 'env' to kernel service 'kernel.env.control'` and `[ENV] control channel wired: kernel ends rd=2 wr=3`.

**Deferred — `env_create_boot_check.sh` needs E1.** The verification plan above is explicitly "(unified boot)". E1 was sequenced before E4 (§3, §16), but the build order ran E2 → E3 → E4, skipping it. In the current Phase-5 self-hosted boot the e1000 is owned by the user-space driver sidecar and `http_server_run` does not bind port 3000, so there is no HTTP surface to drive `env_service_create` — the kernel control plane and `init` do not yet share one boot. E4's RPC path is therefore built and channel-verified but not exercised end to end. `env_create_boot_check.sh` and its `PARTITION_SYSTEM` tooth move to **after E1 lands**, run against the unified boot; the E4 code is otherwise complete. The shell `env create` surface (§7 scope) likewise attaches once the control plane and `init` share a boot.

(**Closed 2026-09-21** — E1 landed, the check was written and run against the unified boot, and running it turned up two things this section's plan assumed rather than measured. See §7.2.)

### 7.2 Findings (2026-09-21) — the deferred boot check exists, and it disagrees with two lines of this phase's plan

`tests/env_create_boot_check.sh` is the check §7.1 deferred, run against the unified boot exactly as that section said it would be: it selects grub entry 3 (the E1 entry), drives the API over the loopback `hostfwd`, and asserts the E4 property where a boot is the only place to assert it —

- `POST /api/partitions` defines a partition and the kernel logs it;
- `POST /api/partition/{id}/env` creates an environment IN that partition and reports `ok`, `env_id` and the partition;
- the environment's two sidecars (`drv.ramdisk.<i>`, `aerosls.posix.<i>`) appear in `GET /api/processes` carrying `partition_id` = the target — and the POSIX sidecar is wired to the environment's OWN ramdisk, not the system's identically-named one (see below);
- `GET /api/partitions` shows the TARGET partition charged for the whole environment — its 1344 region frames plus its two sidecars — and the creator's own usage flat across the create;
- a placement the kernel must refuse IS refused, in the kernel's own words, with nothing placed in the refused target.

The check needed one product-side addition: **`GET /api/processes` gained `partition_id`** (the same "nothing exposed it" gap `owner_node` had on `/api/partitions`), because no surface anywhere reported which partition a process is scheduled in — so §7's "its processes appear with that `partition_id`" had nothing to read.

Teeth: `tests/env_create_boot_check_smoke.sh` runs the guard against two inputs that must turn it red — the kernel-only boot (no env manager to drive; the guard's own contradiction check names it in seconds) and `E4_TOOTH=skip-pause`, which withholds the pause the refusal arm depends on, so a vacuous refusal assertion would pass there and the smoke fails it — and then requires the guard to pass on the real boot. It is on `run_source_smokes.sh`'s build-needed exclusion list and therefore runs on a build host through `run_guard_smokes.sh`, i.e. in CI's `kernel-guards` job alongside the other boot smokes.

**Measured (this host, one boot, ~27 s boot-to-verdict).** Partition 1 created; `POST /api/partition/1/env` answered `ok` with `env_id` 1 in **248–855 ms** across runs; partition 1's `frame_usage` **+1520** across the create (its 1344 region frames and its two sidecars) with the creator at **+16**; the refused (paused) target was refused by the kernel's own `[ALLOC_REGION] CAP_EPERM: target partition 2 is paused — cannot charge it`, with **0 frames and 0 processes** in it; the absent target was refused with `CAP_EINVAL`. The first measurement taken, before Finding 1 was closed, read **+176** on the target and **+1360** on the creator — those numbers are the pre-fix shape and are what the charging control below reproduces.

**E2 evidence that fell out of it, for free.** The environment's `drv.ramdisk.0` (partition 1, PID 103) coexists with the boot's own `drv.ramdisk.0` (PARTITION_SYSTEM, PID 102) — the name really is partition-scoped, not merely renamed — and the tenant POSIX's `ramdisk` channel resolves to **PID 103**, its own driver, not the system's under the same name. That is roadmap G5's hazard, closed and now visible in a boot log.

**Finding 1 — §7's "its frames are charged to that partition's quota" was only half true (closed the same day).** The child's own frames were charged to the target: `cap_create_sidecar_in` allocates the image, stack and syscall stack with `allocate_physical_ram_frame_for_partition(child_partition)` — measured as **+176 frames** on partition 1. But the environment's *budget* — 4 MiB heap + 1 MiB storage + ramdisk heap, **1344 frames**, the bulk of an environment — was allocated by `init` with `alloc_region`, and `sys_sls_alloc_region` charged the **CALLER** (`kernel/cap.c`, whose own E3 note read "Charging to the CALLER's own partition keeps it backward compatible — a target-partition variant is E4's job"). E4 did not add that variant: `env_manager::create_environment` called `k.alloc_region(...)` with no target, so those frames landed on PARTITION_SYSTEM — measured as **+1360 frames** on the creator across one successful create. Two consequences beyond the accounting: a tenant could not be quota-bounded for its storage (its disk counted against the system partition's quota, and §11's "Memory … charged to the partition" was not what the code did), and **a refused placement leaked all three regions** — measured: the paused-target create moved the creator's `frame_usage` by exactly **1344 frames** while placing nothing.

Closed by giving `SYS_SLS_ALLOC_REGION` the target-partition variant its own E3 note deferred. The request carries `target_partition` (0 = the caller's own, which is what every E3 caller passes and is unchanged), `init`'s environment manager passes the environment's partition for all three regions, and the kernel honours a nonzero target only for a `PARTITION_SYSTEM` caller into a live, unpaused partition — the same rule `cap_create_sidecar_in` §4a2 applies to its own target, checked **before a single frame is taken**. The refusal leak closes as a consequence rather than as a second fix: the env manager reaches this gate first, so a refused placement allocates nothing anywhere. **Measured after:** partition 1's `frame_usage` **+1520** across the create and the creator **+16**; the refused (paused) create moves the creator by **0 frames** instead of 1344. The check now **gates** on both numbers instead of reporting them, and `tests/cap_create_sidecar_host_test.c` §0c2 pins the charging and all three refusals (caller not `PARTITION_SYSTEM`, target absent, target paused) where the `PARTITION_SYSTEM` branch is reachable — the same structural reason as Finding 2, so the host test is where that branch can be toothed.

Two source controls, each run by hand against its own rebuilt image (the guard takes `E4_ISO`, so neither touches the shipped one). Reverting the charging to the caller reproduces the pre-fix numbers exactly — `partition 1's frame_usage grew by only 176 frames` and `the creator's frame_usage grew by 1360 frames` — with the guard red and every other assertion green. Deleting the region gate's paused check turns the guard red on the missing `[ALLOC_REGION] CAP_EPERM: target partition 2 is paused` line and on `the refused partition was populated anyway (1344 frames, 0 processes)`: with the gate neutered the regions are charged to the paused target and the sidecars are then refused by `cap_create_sidecar_in`, so the frames land in a partition that has an environment's memory and no processes. **Residual, not fixed:** every denial on this path returns 0, so `init`/the API report a gate refusal as `frame pool exhausted` (`ENV_ERR_MEM`) — the message the paused and absent arms now carry is misleading even though the refusal is correct. Distinguishing them needs a nonzero denial code on the ABI, which is a protocol change E3's "0 on failure/denial" contract does not have.

**Finding 2 — the `PARTITION_SYSTEM` tooth in §7's plan cannot be exercised from a boot, and this is measured, not argued.** §7's tooth is "remove the `PARTITION_SYSTEM` authorization — the non-system create succeeds and the check fails". It does not fail: with that branch deleted from `cap_create_sidecar_in` and the ISO rebuilt, this guard is **still green (rc=0)**, and the boot log contains **zero** `may not target partition` lines. The reason is structural: `SYS_SLS_CREATE_SIDECAR` is already restricted to the sidecar-creator tree (`sidecar_authority`, E2), and every process holding that authority in any boot — the boot parent, `init`, the Device Manager, the system ramdisk driver — lives in `PARTITION_SYSTEM`. The tenant sidecars that DO live outside it (this check's environment, `drv.ramdisk.0`/`aerosls.posix.0` in partition 1, authority inherited) are never asked to create anything, so no non-system caller exists to be denied. Reaching that branch from a boot would take a tenant-side caller that nothing builds today.

So the gate is pinned where it is reachable instead: `tests/cap_create_sidecar_host_test.c` §0d drives it directly (a non-`PARTITION_SYSTEM`-partition parent targeting another partition) and, with the same one-line deletion, fails with `FAIL: E4: a non-PARTITION_SYSTEM caller may not target another partition` — verified. The boot check's toothed refusal arms are the placement refusals instead (paused → `CAP_EPERM`, absent → `CAP_EINVAL`), and those are proven the same way — but the gate that answers them is now `sys_sls_alloc_region`'s, since the env manager reaches it before either sidecar is asked for (Finding 1). Deleting *that* paused check, rebuilding, and running this guard turns it red on the missing `[ALLOC_REGION] CAP_EPERM` line and on `the refused partition was populated anyway (1344 frames, 0 processes)`, while every other assertion still passes — so the failure is the refusal arm's, not a blanket crash. The same deletion in `cap_create_sidecar_in` no longer turns this guard red, and that is the point: the region gate refuses before any frame is taken. The guard takes `E4_ISO` so that control can be repeated against a sabotaged image without touching the shipped one.

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
