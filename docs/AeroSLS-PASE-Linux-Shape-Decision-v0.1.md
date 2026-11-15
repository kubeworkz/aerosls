# AeroSLS PASE Linux-Shape Decision Record (M0)

**Status:** Accepted
**Date:** 2026-11-11
**Owner:** Gridworkz
**Decision:** Linux-shape PASE runtime
**Related:** `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §0.1, `AeroSLS-POSIX-Environments-Roadmap-v0.2.md` §0.1 and Appendix B, `AeroSLS-Linux-ABI-Shim-Design-v0.1.md`

## Context

The project has used “PASE” in two different senses. IBM PASE is an AIX binary-compatibility runtime inside IBM i. The v0.1 POSIX-Environments roadmap explicitly mapped the *shape* of PASE onto an on-demand POSIX environment inside a partition, but the binary-compatibility half arrived later as a **static x86-64 Linux ABI shim** (E7), not AIX compatibility.

That ambiguity is now a product decision, not an implementation detail. Continuing to call the Linux ABI shim “PASE” without qualification would invite expectations it cannot meet: AIX binaries, Power, shared libraries, AIX libc and IBM i user profiles are all different targets.

## Decision

AeroSLS adopts the **PASE-shaped Linux runtime** target.

- The compatibility target is **x86-64 Linux**, not AIX/Power.
- The shipped image contract is static, non-PIE `ET_EXEC` first; dynamic linking is a planned later increment (M2), not part of the current contract.
- The isolation model stays the POSIX environment inside a partition, with the Linux task holding exactly one capability: the channel to its environment's sidecar.
- Native data access is a mediated, partition-scoped surface. The read-only catalog and `CAT_QUERY` are the first cut; a write path, IFS-like sharing, identity/ACL and native-SIMI reach are later increments (M5).
- The cluster/durability work (P1a–P3) remains part of the environment story; it is not an AIX compatibility feature.
- The public and technical description should say **“PASE-shaped Linux runtime”** or **“PASE equivalent”**, never unqualified “IBM PASE.”

This is a Linux-shape decision. It does not claim that AIX binaries run, that Power is supported, or that the project is a drop-in IBM PASE replacement.

## Explicit non-goals

- AIX binary compatibility, AIX libc, Power, or IBM i job/user-profile semantics.
- Nested environments; the v0.1/v0.2 closure stands.
- Hardware capabilities for tenant environments; the zero-hardware-cap tenant profile stands.
- Zero-downtime live migration; P3 pauses the environment for transfer.
- A general POSIX-complete runtime in one step. The measured syscall surface and named milestones define the increments.

## Roadmap

One operator means one active implementation milestone. Owners are Gridworkz throughout; no separate maintainers are assumed.

| Priority | Milestone | Rough effort | Exit gate |
|---|---|---:|---|
| P0 | **M0 — Record the Linux-shape decision** | 1–2 weeks | This record accepted; v0.2 Appendix B corrected; `.github/CODEOWNERS` names the Gridworkz account. |
| P1 | **M1 — Linux network bridge** | 3–4 weeks | `socket`/`connect`/`bind`/`listen`/`accept`/`sendto`/`recvfrom`/`shutdown` forward through the sidecar to P2's socket service; a network-using Linux candidate completes a live TCP round trip. **LANDED 2026-11-12** — see the closure note below. |
| P1 | **M2 — Dynamic linking** | 6–8 weeks | Guest `ld.so` runs: `PT_INTERP`, shared-object mapping, `AT_PHDR`/`AT_BASE`, relocations and the required syscalls; a dynamically linked musl or glibc candidate starts and completes work. |
| P1 | **M3 — Signal delivery** | 3–4 weeks | Handlers, blocked/pending masks, `sigaltstack` and `rt_sigreturn` work; default actions remain correct; signal state survives checkpoint/restore. **LANDED through M3.5, 2026-11-14** — M3.1: ordinary `sa_handler`, `tkill`/unblock, x86-64 frame, `rt_sigreturn`; M3.2–M3.4: `sigaltstack`/`SA_ONSTACK`, `SA_SIGINFO`/ucontext, `SA_RESETHAND`, return-time pending redelivery; M3.5: an EPIPE `sendto` produces SIGPIPE with the saved `-EPIPE` return. Signal state rides the captured sidecar image window and Linux stack. Boundary: cross-task signals are ESRCH, timer/device producers are absent, and no signal-specific live checkpoint arm was run (design §10.15). |
| P2 | **M4 — Threads and thread groups** | 10–16 weeks | `clone`, `futex`, shared address spaces, per-thread stacks and thread-aware checkpoint/restore; a static Go or threaded Rust candidate survives the gate. **M4.1 landed 2026-11-14; M4.2 landed 2026-11-14; M4.3 landed 2026-11-14; M4.4 landed 2026-11-14; M4.5 landed 2026-11-14; M4.6 landed 2026-11-15; M4.7 gate LANDED for the static threaded-Rust gate shape 2026-11-15.** The static threaded-Rust gate is evidenced; std pthread and Go runtime remain out on the PROT_NONE stack reservation; see `AeroSLS-M4-Threads-Design-v0.1.md`. |
| P2 | **M5 — Native data interop, write path** | 8–12 weeks | A Linux task writes through the mediated catalog/Db2 surface with real identity and ACL checks; a native SLS observer sees the change; partition isolation is preserved. IFS-like files and native SIMI reach can follow as a second increment. |
| P3 | **M6 — Capacity and P3 durability** | 8–12 weeks | Capacity target beyond four environments; promoted replicas retain payload across a survivor reboot; partition DB-page movement is implemented or refused by name; the replica adoption branch is exercised live. |
| Deferred | ARM64 userspace OS; any literal AIX track | — | Separate architecture/product decisions. Neither is required for the Linux-shape target. |

**Order.** M0 → M1 → M2 → M3 → M4 → M5 → M6. M5 moves ahead of M4 only if data parity becomes more important than Go/threaded-CPython/Node support. M3 may move earlier as a small standalone increment, but it does not unblock M1/M2.

**MVP boundary.** M0–M3 is the first useful runtime: networked, dynamically linked, signal-capable and single-threaded. M4 is the modern-runtime gate. M5 is PASE data parity. M6 is production capacity and recovery.

**M1 closure (2026-11-12).** The eight socket-class numbers cross the Linux trap through the sidecar to P2's socket service. The kernel-side plan has 27 host checks in `tests/linux_net_bridge_host_test.c`; the live QEMU gate `tests/m1_net_probe_check.sh` boots `sls_operating_system_m1net.iso`, asserts all eight calls, and observed two successful probe executions with no `M1-NET FAIL` (`evidence/m1_net_probe_gate.log`). The gate also carries the default-off `M1_TOOTH=bypass-connect` arm: its live tooth image observed the named `[M1-TOOTH] bypass-connect` refusal before any sidecar post, zero forwarded connects and a stage-5 probe failure (`evidence/m1_net_probe_tooth_refused.gate.txt`, `evidence/m1_net_probe_tooth_pass.gate.txt`); the clean image then reran green. The live probe's peer is the guest's own 10.0.2.15 address (kernel loopback), not a host/NIC peer; UDP, IPv6, external connect failures, `CLOEXEC`/nonblocking variants and `SHUT_RDWR` remain outside this gate.

**M3.5 closure (2026-11-14).** The first asynchronous producer is live: after `shutdown(fd, SHUT_WR)`, a third `sendto` on the same connected stream returns `-EPIPE` and raises SIGPIPE. The network backend's half-close path now reports `NET_ERR_PIPE` instead of `NET_ERR_NOTCONN`; the sidecar maps that to `E_PIPE`, the kernel carries the pending `SIGD` delivery with the saved `-EPIPE`, and the handler runs. The live M1 gate observed `sendto=3`, `close=3`, `pipe=3`, `[E7] linux: ... sig=13 handler -> frame`, `E7PIPE-OK` from the handler, and a clean `exit_group status=0`. The guard clause R13 pins the producer and its saved return. Boundary unchanged: no timer/device producer, no cross-task delivery, no job-control stop/continue, and no signal-specific live checkpoint arm.

**M4 design spike (2026-11-14).** M4 is the wall for modern runtimes, and the missing scope is now recorded rather than deferred: `AeroSLS-M4-Threads-Design-v0.1.md` fixes the thread-group object, shared-`cr3` lifetime, per-thread stacks/contexts, sidecar group-vs-thread state, clone/futex flows, exit/join rules, and thread-aware checkpoint shape. M4.1 landed 2026-11-14: `kernel/linux_threads.{h,c}` add the bounded `LinuxThreadGroup` table, `cr3_refs`/`live_threads`, and the per-thread `linux_tgid`/`linux_tid`/`linux_clear_child_tid`/`linux_is_thread` fields on `ProcessDescriptor`; `process_init()` resets the table, and all three teardown paths release the group reference before destroying the PML4 only on the last reference, after `cap_table_teardown()`. `tests/linux_thread_group_host_test.c` (178 checks including the M4.2/M4.5 cases) drives leader-first exit, non-last exit, last-reference destroy, double detach, slot reuse, table exhaustion, half-torn detach refusal and the non-Linux legacy path; `tests/cap_create_sidecar_host_test.c` fills the group table and proves the create failure reaches `process_kill()` with the cap table unbound.

M4.2 landed 2026-11-14: `kernel/linux_clone.c` implements `SYS_SLS_LINUX_CLONE_THREAD` (330) as the held thread create — flag/address validation, a shared-CR3 child descriptor with its own syscall stack and post-syscall context (`proc_create_linux_thread`, `linux_thread_clone_context`), group attach, a sidecar-to-child messenger pair, ptid/ctid writes and release. The sidecar's `clone` (56) arm validates the documented subset and tracks the child; `tests/linux_thread_group_host_test.c` adds context/flag teeth, and the live `M4_THREAD_PROBE=1` fixture writes `M4-THREAD-OK` from the thread and `M4-THREAD-PARENT-OK` from its parent. M4.3 landed 2026-11-14: `user/sidecar/src/linux.rs` splits the tracked record into a bounded `LinuxGroup` (fd table, heap ledger, signal dispositions) and per-thread `LinuxTask` (channel, name, VFS task, mask/pending/tid/alt stack, parked wait); `track_thread` joins a clone to its parent group and `release_task` retires the group only on the last release. The VFS task is created with `clone_task(CLONE_FILES)` so the child shares the leader's open-file table. The raw `M4_GROUP_PROBE=1` fixture has the child write a descriptor only the leader opened and requires `M4-GROUP-CHILD-OK` before `M4-GROUP-PARENT-OK` with no failure marker. M4.4 landed 2026-11-14: `kernel/linux_abi.{h,c}` classifies futex (202) as `LSC_FUTEX` and copies the observed word plus an optional relative timespec into a fixed 24-byte wait record; `user/sidecar/src/linux.rs` adds a bounded group-keyed FIFO `FutexWaiter` queue, private `FUTEX_WAIT`/`FUTEX_WAKE`, expected-value mismatch `EAGAIN`, wake-count replies and `ETIMEDOUT`. The raw `M4_FUTEX_PROBE=1` fixture is a join shape with child and parent markers and a no-waker timed wait. M4.5 landed 2026-11-14: syscalls 39/60/186/200 are classified and answered per thread, `set_tid_address` returns the real TID while recording its word, `linux_thread_group_detach` zeroes `linux_clear_child_tid` through the checked user copy, and `SYS_SLS_LINUX_GROUP_SWEEP` (331) kills every live member of a TGID group for `exit_group`. The sidecar stores real TID/TGID/ctid, routes 60 to one task and 231 to the sweep, wakes a clear-tid futex waiter before release, and matches `tkill` against the sibling's real TID (blocked signals pend; sibling handler/default delivery is named `ENOSYS`, and group-directed fan-out is M4.8). The `M4_EXIT_PROBE=1` live gate observed `M4-CHILD-OK`, `M4-IDENTITY-OK`, `M4-TKILL-OK`, `M4-JOIN-OK`, `M4-EXITGROUP-CHILD-OK`, `M4-EXITGROUP-BEFORE`, no `M4-EXIT-FAIL`, and the kernel's `[PROC] M4.5 group sweep ... killed=2`. M4.6 landed 2026-11-15: record v2 and payload v3 carry the shared PML4 once and one context per thread; the live two-boot probe restored both parked futex waiters. M4.7 gate LANDED for the static threaded-Rust gate shape 2026-11-15: `tools/thread_runtime_gate` creates a second OS thread with raw `clone(56)`, shares a heap object and the inherited console fd, parks both tasks on the group's private futex, survives checkpoint/restore, and joins; `evidence/m4_runtime_gate.log` carries `M4-GO-OK`, `M4-FUTEX-OK`, `M4-CHECKPOINT-OK` and `M4-RUNTIME-GATE-PASS` with no failure marker. The tree still has no `clone3` (435); std pthread and Go runtime remain out because their `PROT_NONE` stack reservation is not backed and mprotect over an absent page answers `ENOMEM`. The source guard `tests/m4_threads_wall_check.sh` requires the live artifact before a runtime claim.

**M3.2–M3.4 closure (2026-11-13).** The signal mechanism now runs with actions, alternate stacks and `SA_SIGINFO`: `rt_sigaction` 13 records the action; `tkill` 200 and the `rt_sigprocmask` 14 unblock reveal deliver through a kernel-built x86-64 frame; `sigaltstack` 131 records/queries/disables a Linux `stack_t`; `SA_ONSTACK` places the frame in that region; `SA_SIGINFO` supplies siginfo/ucontext; `SA_RESETHAND` resets before entry; `rt_sigreturn` 15 restores the mask/register file and redelivers the next unblocked pending handler. The sidecar owns which action/mask applies; the kernel owns the frame and sysret mechanism. Live E3 boot printed `E7SIG-INFO` from the handler after checking its alternate RSP and siginfo/ucontext, then `E7SIG-OK`, then `E7SIG-RET` after the restored-mask query, with no `405`–`411` marker. The same boot proves the whole SLS/Linux number overlap is personality-routed: Linux 105 returned `-ENOSYS` (`E7SIG-LNX-105-OK`), while the SLS sidecar's raw 105 took the legacy SLS arm (`E7SIG-SLS-105-OK`); the x86 entry stub no longer carries any SLS number chain. Signal state survives a checkpoint by construction (sidecar image window + captured Linux stack). Named boundary: cross-task signals answer ESRCH and no timer/device async producer exists; no signal-specific live checkpoint arm was run.

## Gap-register consequences

The v0.2 register now records:

- the P2 live L5 arm as green in CI run `43072501848` (2026-11-11), removing the old boot-evidence caveat;
- the Linux networking gap as **M1**, now landed, not as an unowned gap;
- native interop as **PARTIAL**, not “LANDED”; the read-only catalog/`CAT_QUERY` slice is real, but writes, IFS-like sharing, identity/ACL and native SIMI remain M5;
- threads as **M4**, dynamic linking as **M2**, signal delivery as **M3**, and capacity/P3 durability as **M6**;
- AIX binary compatibility as **out of scope by this decision**, not as a planned gap;
- the remaining P3 replica-branch caveat as an M6 evidence-closure item.

## Ownership and process

Gridworkz owns the product, architecture, implementation and verification. The absence of separate maintainers is deliberate for the current single-operator phase, not an assumption that review is unnecessary.

M0 adds a `.github/CODEOWNERS` entry naming the Gridworkz account so the ownership is machine-enforced. Every milestone also carries the project's normal evidence discipline: a guard, a sabotage tooth, a live arm where boot is required, an Appendix B update, and a green CI run. A milestone is not complete on source inspection alone.

## Acceptance

Accepted by Gridworkz on 2026-11-11 as the M0 Linux-shape decision. No separate reviewer approval is recorded because the project is currently single-operator.
