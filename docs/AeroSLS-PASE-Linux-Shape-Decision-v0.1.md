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
| P1 | **M3 — Signal delivery** | 3–4 weeks | Handlers, blocked/pending masks, `sigaltstack` and `rt_sigreturn` work; default actions remain correct; signal state survives checkpoint/restore. |
| P2 | **M4 — Threads and thread groups** | 10–16 weeks | `clone`, `futex`, shared address spaces, per-thread stacks and thread-aware checkpoint/restore; a static Go or threaded Rust candidate survives the gate. A one-week design spike runs during M0; implementation waits for M2. |
| P2 | **M5 — Native data interop, write path** | 8–12 weeks | A Linux task writes through the mediated catalog/Db2 surface with real identity and ACL checks; a native SLS observer sees the change; partition isolation is preserved. IFS-like files and native SIMI reach can follow as a second increment. |
| P3 | **M6 — Capacity and P3 durability** | 8–12 weeks | Capacity target beyond four environments; promoted replicas retain payload across a survivor reboot; partition DB-page movement is implemented or refused by name; the replica adoption branch is exercised live. |
| Deferred | ARM64 userspace OS; any literal AIX track | — | Separate architecture/product decisions. Neither is required for the Linux-shape target. |

**Order.** M0 → M1 → M2 → M3 → M4 → M5 → M6. M5 moves ahead of M4 only if data parity becomes more important than Go/threaded-CPython/Node support. M3 may move earlier as a small standalone increment, but it does not unblock M1/M2.

**MVP boundary.** M0–M3 is the first useful runtime: networked, dynamically linked, signal-capable and single-threaded. M4 is the modern-runtime gate. M5 is PASE data parity. M6 is production capacity and recovery.

**M1 closure (2026-11-12).** The eight socket-class numbers cross the Linux trap through the sidecar to P2's socket service. The kernel-side plan has 27 host checks in `tests/linux_net_bridge_host_test.c`; the live QEMU gate `tests/m1_net_probe_check.sh` boots `sls_operating_system_m1net.iso`, asserts all eight calls, and observed two successful probe executions with no `M1-NET FAIL` (`evidence/m1_net_probe_gate.log`). The live probe's peer is the guest's own 10.0.2.15 address (kernel loopback), not a host/NIC peer; UDP, IPv6, external connect failures, `CLOEXEC`/nonblocking variants and `SHUT_RDWR` remain outside this gate.

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
