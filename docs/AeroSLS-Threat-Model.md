# AeroSLS Threat Model

**Version:** 1.0  
**Date:** 08-22-2026  
**Author:** Team AeroSLS  
**Status:** Draft for Review

---

## 1. Introduction

This document describes the threat model for the AeroSLS operating system, a capability-based sidecar kernel. The purpose is to identify potential security risks, define the trust boundaries, and guide security engineering and audit efforts. AeroSLS introduces a novel architecture where multiple programming environments (sidecars) coexist within a single kernel-mediated capability framework. This model is intended to evolve as the system matures.

## 2. System Overview

AeroSLS is a research kernel that implements a capability-based sidecar SDK. Instead of a single monolithic personality (e.g., POSIX), AeroSLS hosts multiple sidecars—language runtimes, device drivers, and services—all sharing the same hardware resources through kernel-managed capabilities.

##### Key components:

- **Kernel:** Provides capability tables, channel endpoints, shared memory arenas, and scheduling.
- **Sidecars:** User-space or same-ring personalities (POSIX, WebAssembly, Lisp, device drivers, network stack, etc.) that communicate via capabilities.
- **Capabilities:** Unforgeable tokens granting access to memory, I/O ports, interrupts, channels, and other resources.
- **Channels:** Typed message-passing endpoints that can also transfer capabilities atomically.
- **Shared Memory:** Physical memory arenas mapped into multiple sidecars for zero-copy data exchange.

##### The system is built in phases:

1. Seed Kernel (capability lifecycle)
2. POSIX Sidecar
3. Polyglot Nexus (cross-language calls)
4. Device Driver SDK
5. Self-Hosted System

## 3. Scope

##### This threat model covers:

- The AeroSLS kernel capability manager, channel implementation, shared memory manager, and scheduler.
- All sidecar runtimes and services, including but not limited to POSIX, WebAssembly, Lisp, device drivers, filesystem, and network stack.
- Hardware interfaces exposed via capabilities (MMIO, DMA, interrupts).
- Boot process and initial capability grants.

#### Out of scope:

- Physical attacks requiring disassembly or invasive hardware modifications.
- Side-channel attacks (timing, power, EM) unless specifically noted.
- Supply chain attacks on the hardware itself (e.g., malicious CPU).

## 4. Assumptions

##### The following assumptions are made for this threat model:

- Sidecars may be written in memory-safe languages (e.g., Rust) but may also contain unsafe code or be written in C.
- The kernel enforces capability validity and access control; sidecars cannot directly inspect or modify kernel data structures.
- The hardware provides an MMU and, where applicable, an IOMMU. The kernel configures these correctly.
- Sidecars run in user mode with hardware memory isolation, or in the same privilege ring but protected by memory protection keys (MPK/PKU) or equivalent isolation mechanisms.
- The boot chain includes a trusted firmware (e.g., OpenSBI) and verified boot images.

## 5. Trust Model

### 5.1 Trusted Computing Base (TCB)

##### The minimal TCB includes:

- Kernel capability manager (creation, transfer, revocation).
- Channel implementation (message integrity, ordering, atomicity).
- Shared memory manager (mapping, revocation, TLB shootdown).
- MMU/IOMMU configuration code.
- Device Manager sidecar (if it is responsible for hardware enumeration and driver spawning).

Sidecars, including device drivers and language runtimes, are **untrusted** unless explicitly granted capabilities.

### 5.2 Trust Boundaries

<table class="code-line">
  <tr><th>Boundary</th><th>Description</th></tr>
  <tr><td>Kernel ↔ Sidecar</td><td>Capability syscalls are the only interface. Sidecars cannot access kernel memory or other sidecars&#39; private memory without explicit capability grants.</td></tr>
  <tr><td>Sidecar ↔ Sidecar</td><td>Communication only via channels and shared memory arenas. Capability transfer is explicit and kernel-mediated.</td></tr>
  <tr><td>Sidecar ↔ Hardware</td><td>Device drivers access hardware via IO_PORT and DMA_MEM capabilities. The kernel/IOMMU enforces that drivers can only affect their assigned devices and memory regions.</td></tr>
  <tr><td>Boot ↔ Runtime</td><td>Firmware and bootloader verify the kernel and initial sidecar images. Runtime integrity is maintained by capability isolation.</td></tr>
</table>

## 6. Assets

##### Primary assets to protect:

- **Capability integrity**: Unforgeable, non-duplicable, revocable.
- **Memory isolation**: Sidecar private heaps, stacks, and code.
- **Data confidentiality/integrity**: User data processed by POSIX, WASM, Lisp, and other sidecars.
- **System availability**: Kernel and service sidecars remain responsive.
- **Hardware integrity**: Prevent misuse of DMA, interrupts, and MMIO.
- **Boot integrity**: Ensure only authorized code executes from reset.

## 7. Attacker Profiles

<table class="code-line">
  <tr><th>Attacker</th><th>Goal</th><th>Primary Attack Surface</th></tr>
  <tr><td>Malicious Sidecar Author</td><td>Escape capability table, read/write other sidecars&#39; memory, forge capabilities.</td><td>Capability syscalls, channel parser, shared memory manager.</td></tr>
  <tr><td>Compromised POSIX Application</td><td>Break out of POSIX sidecar, reach kernel or other sidecars.</td><td>POSIX VFS, fd/cap mapping, fork/exec inheritance, libc stubs.</td></tr>
  <tr><td>Remote Attacker (Network)</td><td>Compromise NIC driver or network stack, inject malicious packets.</td><td>Driver sidecar, TCP/IP sidecar, packet parsing, DMA buffers.</td></tr>
  <tr><td>Malicious/Buggy Device</td><td>DMA attacks, spoofed interrupts, MMIO abuse.</td><td>IOMMU, IRQ channel, driver capability validation.</td></tr>
  <tr><td>Local Physical Attacker</td><td>Boot chain tampering, memory probing, JTAG.</td><td>Firmware, secure boot, hardware root of trust.</td></tr>
  <tr><td>Supply Chain Attacker</td><td>Insert malicious driver/sidecar into the system image.</td><td>Manifest parser, initial capability grants, build process.</td></tr>
</table>

## 8. Security Invariants

##### The architecture must maintain the following invariants. Violation of any invariant is a security breach.

<table class="code-line">
  <tr><th>#</th><th>Invariant</th><th>Description</th></tr>
  <tr><td>I1</td><td>Capability unforgeability</td><td>A sidecar cannot create a capability it was not granted.</td></tr>
  <tr><td>I2</td><td>Atomic capability transfer</td><td>Capabilities cannot be duplicated or lost during send/receive.</td></tr>
  <tr><td>I3</td><td>Total revocation</td><td>After revocation, no alias or copy continues to grant access.</td></tr>
  <tr><td>I4</td><td>Memory isolation</td><td>A sidecar can access only memory mapped by a valid MEM cap.</td></tr>
  <tr><td>I5</td><td>Channel integrity</td><td>Messages cannot be spoofed, replayed, or reordered by others.</td></tr>
  <tr><td>I6</td><td>DMA confinement</td><td>Devices can only access physical memory granted via DMA_MEM.</td></tr>
  <tr><td>I7</td><td>Interrupt directed</td><td>IRQ messages only delivered to the holder of the IRQ cap.</td></tr>
  <tr><td>I8</td><td>Resource limits</td><td>A malicious sidecar cannot exhaust kernel resources.</td></tr>
</table>

## 9. Threat Scenarios by Attack Surface

### 9.1 Capability Syscalls

- **Double-free of capability slot** → use-after-free → potential privilege escalation.
- **Capability index confusion** → referencing wrong capability type.
- **TOCTOU race** between checking and using a capability.
- **Incomplete TLB shootdown** after revocation → stale mapping remains.
- **Integer overflow** in size/offset calculations → out-of-bounds access.

### 9.2 Channel Implementation

- **Malformed message** (bad length, invalid capability index) → parser memory corruption.
- **Channel queue overflow** → denial of service.
- **Capability transfer race** → duplicate capability or lost update.

### 9.3 Shared Memory Manager

- **Mapping overlap** → one sidecar maps another's private memory.
- **Revocation race** → memory freed while still mapped.
- **DMA buffer reuse** without proper unmapping.

### 9.4 POSIX Sidecar

- **File descriptor ↔ capability confusion** → leaking capabilities.
- **Path traversal** in VFS leading to arbitrary file access.
- **Fork/exec inheritance** grants more capabilities than intended.
- **Race conditions** between open/read and capability revocation.

### 9.5 Polyglot Nexus (WASM/Lisp Interop)

- **IDL deserialization type confusion** → memory corruption.
- **Buffer overread/overflow** in generated marshalling code.
- **Cross-language GC mismatch** → use-after-free across sidecars.
- **Forged capability** in a channel message from a malicious runtime.

### 9.6 Device Drivers

- **DMA outside granted region** if IOMMU absent/misconfigured.
- **MMIO access outside device BAR** due to malformed IO_PORT cap.
- **IRQ spoofing** from malicious device.
- **Freeing DMA buffer while device still uses it** → use-after-free.

### 9.7 Boot Process

- **Unverified kernel image** → attacker loads malicious kernel.
- **Insecure initial capability grants** → sidecar gets too much authority.
- **Rollback attack** → boot older vulnerable version.

## 10. Potential Vulnerabilities by Phase

<table class="code-line">
  <tr><th>Phase</th><th>Component</th><th>Likely Vulnerability Class</th></tr>
  <tr><td>1</td><td>Capability table</td><td>Use-after-free, double-free, TOCTOU</td></tr>
  <tr><td>1</td><td>Channel</td><td>Message parsing, queue overflow, race</td></tr>
  <tr><td>2</td><td>POSIX VFS</td><td>Path traversal, fd/cap confusion</td></tr>
  <tr><td>2</td><td>Fork/exec</td><td>Excessive capability inheritance</td></tr>
  <tr><td>3</td><td>Polyglot Nexus</td><td>Marshalling bugs, type confusion</td></tr>
  <tr><td>4</td><td>DMA/IOMMU</td><td>Missing IOMMU mappings, DMA confinement failure</td></tr>
  <tr><td>4</td><td>IRQ handling</td><td>IRQ spoofing, interrupt storm</td></tr>
  <tr><td>5</td><td>Boot chain</td><td>Image verification bypass, insecure grants</td></tr>
  <tr><td>5</td><td>Service discovery</td><td>Malicious sidecar masquerading as service</td></tr>
</table>

## 11. Mitigations and Controls

<table class="code-line">
  <tr><th>Area</th><th>Mitigation</th></tr>
  <tr><td>Capability lifecycle</td><td>Use opaque capability handles with refcounts; validate all syscall arguments; kernel-level fuzzing.</td></tr>
  <tr><td>Memory isolation</td><td>Hardware page tables with per-sidecar address spaces; MPK/PKU for same-ring isolation; rigorous TLB shootdown.</td></tr>
  <tr><td>Channel security</td><td>Bounded queues; strict message validation; atomic capability transfer with kernel locks.</td></tr>
  <tr><td>POSIX sidecar</td><td>VFS path canonicalization; capability-based fd table; fork/exec capability filtering.</td></tr>
  <tr><td>Polyglot Nexus</td><td>IDL compiler with bounds checks; memory-safe marshalling; capability validation in stubs.</td></tr>
  <tr><td>Device drivers</td><td>IOMMU mandatory for DMA; IO_PORT range checking; IRQ delivery via kernel-mediated channels.</td></tr>
  <tr><td>Boot integrity</td><td>Verified boot with hardware root of trust; signed manifests; secure initial capability grants.</td></tr>
  <tr><td>Resource exhaustion</td><td>Per-sidecar quotas; channel backpressure; kernel watchdog.</td></tr>
</table>

## 12. Residual Risks

- Side-channel attacks (e.g., cache timing) are not fully mitigated.
- If a sidecar runs in the same address space and uses only language safety, memory corruption in an unsafe language can break isolation.
- Hardware errata or misconfigurations may undermine IOMMU or MPK protections.
- The Device Manager sidecar, if compromised, could grant excessive capabilities to malicious drivers.

## 13. Audit and Testing Alignment

##### This threat model should be used to drive:

- Static analysis (Rust Clippy, Clang-tidy, CodeQL)
- Dynamic fuzzing (libFuzzer, AFL++, syzkaller-style harness)
- Isolation penetration testing
- Formal verification of capability lifecycle
- External security review

A detailed security audit plan can be derived from the threats and invariants listed here.

## 14. Document History

<table class="code-line">
  <tr><th>Version</th><th>Date</th><th>Changes</th></tr>
  <tr><td>1.0</td><td>08-22-2026</td><td>Initial threat model</td></tr>
</table>

---

*This document is a living artifact. Update it as the architecture evolves and new threats are identified.*

---

## AeroSLS Security Audit Plan

### 0. First, Pin Down the Trust Model

Before any audit, you need to answer these about the actual implementation:

- Do all sidecars share **one virtual address space**, or does each sidecar have its own address space with shared arenas mapped in?
- If they share one address space, is isolation enforced by **MPK/PKU**, **CHERI**, or only language memory safety?
- Do sidecars run in **user mode** or in the **same privilege ring** as the kernel?
- Is an **IOMMU** present and enabled for all DMA?
- What languages are the sidecars written in? Rust? C? Lisp? WASM?

These answers determine whether your isolation boundary is page tables, protection keys, or software fault isolation — and that changes the whole audit.

---

### 1. Define the Security Invariants

##### The audit should prove these properties hold:

<table>
  <tr><th>#</th><th>Invariant</th></tr>
  <tr><td>I1</td><td>Capabilities are unforgeable. A sidecar cannot create a capability it was not granted.</td></tr>
  <tr><td>I2</td><td>Capability transfer is atomic. A capability cannot be duplicated or lost during cap_send/cap_recv.</td></tr>
  <tr><td>I3</td><td>Revocation is total. Once a capability is revoked, no alias or copy can continue to grant access.</td></tr>
  <tr><td>I4</td><td>Memory isolation holds. A sidecar can read/write/execute only memory mapped by a MEM cap it holds.</td></tr>
  <tr><td>I5</td><td>Channels preserve integrity and ordering. Messages cannot be spoofed, replayed, or reordered by another sidecar.</td></tr>
  <tr><td>I6</td><td>DMA is confined. Devices can only access physical memory granted via DMA_MEM caps.</td></tr>
  <tr><td>I7</td><td>Interrupts are directed. An IRQ message is delivered only to the sidecar holding the corresponding IRQ cap.</td></tr>
  <tr><td>I8</td><td>Resource limits are enforced. A malicious sidecar cannot exhaust kernel resources or starve others.</td></tr>
</table>

---

### 2. Threat Model

##### Assume these attackers:

<table>
  <tr><th>Attacker</th><th>Goal</th><th>Primary Attack Surface</th></tr>
  <tr><td>Malicious sidecar author</td><td>Escape capability table, read other sidecars’ memory, forge caps</td><td>Cap syscalls, channel parser, shared memory manager</td></tr>
  <tr><td>Compromised POSIX application</td><td>Break out of POSIX sidecar, reach kernel or other sidecars</td><td>POSIX VFS, fd/cap mapping, fork/exec inheritance</td></tr>
  <tr><td>Remote attacker via network</td><td>Compromise NIC driver or TCP/IP sidecar, inject malicious packets</td><td>Driver sidecar, packet parsing, DMA buffers</td></tr>
  <tr><td>Malicious or buggy device</td><td>DMA attacks, spoofed interrupts, MMIO abuse</td><td>IOMMU, IRQ channel, driver capabilities</td></tr>
  <tr><td>Local user with physical access</td><td>Boot chain tampering, memory probing, JTAG</td><td>Firmware, secure boot, hardware root of trust</td></tr>
  <tr><td>Supply chain attacker</td><td>Malicious driver/sidecar shipped in image</td><td>Manifest parser, initial capability grants</td></tr>
</table>

---

### 3. Attack Surface by Phase

Each phase you built has its own weak points.

#### Phase 1 — Seed Kernel

- Capability table index reuse after revoke → use‑after‑free of a capability slot.
- Double‑free or capability duplication in `cap_send`/`cap_recv`.
- Missing TLB shootdown after revocation → stale memory mapping.
- Channel queue overflow / unbounded message queuing → DoS.
- Integer overflow in memory size or offset calculations.

#### Phase 2 — POSIX Sidecar

- File descriptor ↔ capability mapping confusion.
- Path traversal in the VFS layer.
- `fork`/`exec` inheriting more capabilities than intended.
- Race conditions between `open`, `read`, and capability revocation.

#### Phase 3 — Polyglot Nexus

- IDL deserialization type confusion.
- Buffer overread/overflow in generated marshalling code.
- Cross‑language garbage collection mismatches → use‑after‑free across sidecars.
- A malicious WASM/Lisp function passing a forged capability via a channel message.

#### Phase 4 — Device Drivers

- DMA outside granted `DMA_MEM` regions if IOMMU is absent or misconfigured.
- MMIO access outside the device’s BAR through a malformed `IO_PORT` cap.
- IRQ spoofing or interrupt storms from a malicious device.
- Freeing a DMA buffer while the device still has it queued.

#### Phase 5 — Self‑Hosted System

- Boot chain tampering if firmware verification is weak.
- Filesystem image replacement or rollback.
- Network service spoofing: a malicious sidecar advertising itself as `net.0`.
- Crash/restart of a driver sidecar leaving dangling capability references in clients.

---

### 4. Audit Phases

#### Phase A — Threat Modeling & TCB Definition

- Produce a formal threat model document.
- Define the exact trusted computing base:
  - Kernel capability manager
  - Channel implementation
  - Shared memory manager
  - MMU/IOMMU configuration
  - Device Manager (if trusted)
- Identify which components must be correct for security and which can fail safely.

**Deliverable:** TCB document + attack trees.

---

#### Phase B — Code Audit & Static Analysis

- Review all unsafe code paths, especially in the kernel capability subsystem.
- Check for:
  - Capability slot reuse
  - Reference counting errors
  - TOCTOU races in `cap_send`/`cap_recv`
  - TLB invalidation on revocation
  - Bounds checks in channel message parsing
  - Integer overflows
- Run static analyzers: `cargo clippy` if Rust, `clang-tidy` if C, plus formal linters.

**Deliverable:** Findings list with severity ratings.

---

#### Phase C — Dynamic Fuzzing

- Build a fuzzing harness for:
  - Random `cap_create_mem` / `cap_send` / `cap_recv` / `cap_revoke` sequences.
  - Malformed channel messages with bad lengths, invalid capability indices, truncated headers.
  - Malformed sidecar manifests.
  - Malformed IDL files and generated marshalling inputs.
  - POSIX sidecar path inputs and fd operations.
- Use a syzkaller‑style approach adapted to AeroSLS syscalls.
- Stress test concurrent cap transfer and revocation to find races.

**Deliverable:** Fuzzing harnesses + crash reports + regression tests.

---

#### Phase D — Isolation & Penetration Testing

- Run an untrusted sidecar with minimal capabilities and attempt to:
  - Access kernel memory
  - Read another sidecar’s private heap
  - Forge a channel message
  - Exhaust kernel resources
- Use fault injection:
  - Revoke a capability while a sidecar is actively using it.
  - Kill a driver sidecar during I/O.
  - Trigger interrupt storms.
- Test DMA attacks with and without IOMMU.
- Test boot integrity by modifying the image and checking verification.

**Deliverable:** Red team report with exploits demonstrated.

---

#### Phase E — Formal Verification of Critical Invariants

- Model the capability lifecycle in TLA+ or a proof assistant (Coq, Isabelle, Lean).
- Prove:
  - No capability duplication.
  - No capability forgery.
  - Revocation removes all access.
  - Channel message ordering and integrity.
- Focus only on the kernel capability and channel code — not the whole OS.
- If formal verification is too heavy, at least model‑check the state machine.

**Deliverable:** Formal model + proof sketches or model‑checking results.

---

#### Phase F — External Review

- Engage an external security firm or independent researchers.
- Give them access to:
  - AeroSLS running on QEMU and real hardware.
  - The capability SDK documentation.
  - The sidecar manifests and a sample driver.
- Scope:
  - Escape from POSIX sidecar.
  - Cross‑sidecar capability forgery.
  - Driver sidecar compromise.
  - Network stack exploitation.
  - Boot chain bypass.

**Deliverable:** External audit report.

---

### 5. Tooling Recommendations

<table>
  <tr><th>Area</th><th>Tool</th></tr>
  <tr><td>Static analysis</td><td>cargo clippy, clang-tidy, semgrep, codeql</td></tr>
  <tr><td>Fuzzing</td><td>libFuzzer, AFL++, cargo-fuzz, syzkaller‑style harness</td></tr>
  <tr><td>Concurrency testing</td><td>loom (Rust), ThreadSanitizer, stress harnesses</td></tr>
  <tr><td>Formal modeling</td><td>TLA+, Coq, Isabelle, Lean, miri for Rust</td></tr>
  <tr><td>DMA/IOMMU testing</td><td>QEMU with IOMMU enabled, hardware with VT‑d/IOMMU</td></tr>
  <tr><td>Penetration testing</td><td>Custom harnesses, GDB, QEMU monitor, hardware probes</td></tr>
</table>

---

### 6. Sample 12‑Week Timeline

<table>
  <tr><th>Weeks</th><th>Activity</th></tr>
  <tr><td>1–2</td><td>Threat modeling, TCB definition, security invariants</td></tr>
  <tr><td>3–5</td><td>Code audit + static analysis</td></tr>
  <tr><td>5–7</td><td>Fuzzing harness development and crash fixing</td></tr>
  <tr><td>8–9</td><td>Isolation testing + fault injection</td></tr>
  <tr><td>10–11</td><td>Formal modeling of capability lifecycle</td></tr>
  <tr><td>11–12</td><td>External review scoping, remediation, final report</td></tr>
</table>

---

### 7. Prompt for DeepSeek v4 Flash / Pro

Use this to generate a detailed, actionable security audit plan from the model:

```plaintext
You are a security architect for the AeroSLS operating system, a capability-based sidecar kernel. Phases 1–5 are built: seed kernel with capability tables and channels, POSIX sidecar, polyglot nexus, device driver SDK, and self-hosted system. We need a comprehensive security audit plan.

Produce:
1. Threat model with attacker personas, goals, and attack surfaces.
2. Security invariants that must hold in the kernel and sidecar boundaries.
3. A phase-by-phase audit plan covering static analysis, fuzzing, isolation testing, formal verification, and external review.
4. Specific likely vulnerabilities in each phase and how to test for them.
5. Tooling recommendations.
6. A 12-week audit timeline.
7. A list of deliverables.

Assume sidecars run in user space with hardware memory isolation where possible, but also address the case where sidecars share a single address space using MPK/PKU.

```
